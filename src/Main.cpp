#include "Driver.hpp"
#include "Common.hpp"
#include "Stats.hpp"
#include "Exec.hpp"
#include "SignalHandler.hpp"
#include "DagGenerator.hpp"
#include "Profiler.hpp"
#include "DagData.hpp"
#include "NodeResultPrinting.hpp"
#include "DynamicOutputDirectories.hpp"
#include "LeafInputSignature.hpp"
#include "RemoveStaleOutputs.hpp"
#include "AllBuiltNodes.hpp"
#include "ReportIncludes.hpp"
#include "StandardInputCanary.hpp"
#include "Inspect.hpp"

#include <stdio.h>
#include <stdlib.h>

#ifdef TUNDRA_WIN32
#include <windows.h>
#endif

#ifdef _MSC_VER
#define snprintf _snprintf
#endif

#ifdef HAVE_GIT_INFO
extern "C" char g_GitVersion[];
extern "C" char g_GitBranch[];
#endif

namespace OptionType
{
enum Enum
{
    kBool,
    kInt,
    kString
};
}

static const struct OptionTemplate
{
    char m_ShortName;
    const char *m_LongName;
    OptionType::Enum m_Type;
    size_t m_Offset;
    const char *m_Help;
} g_OptionTemplates[] = {
    {'j', "threads", OptionType::kInt, offsetof(DriverOptions, m_ThreadCount), "Specify number of build threads"},
    {'t', "show-targets", OptionType::kBool, offsetof(DriverOptions, m_ShowTargets), "Show available targets and exit"},
    {'v', "verbose", OptionType::kBool, offsetof(DriverOptions, m_Verbose), "Enable verbose build messages"},
    {'Q', "silence-if-possible", OptionType::kBool, offsetof(DriverOptions, m_SilenceIfPossible), "If no actions taken, don't display a conclusion message"},
    {'C', "identifactioncolor", OptionType::kInt, offsetof(DriverOptions, m_IdentificationColor), "Color used to identify progress messages" },
    {'m', "visualmaxnodes", OptionType::kInt, offsetof(DriverOptions, m_VisualMaxNodes), "How much nodes to keep space for in the progress notification message" },
    {'l', "don't use previous results.", OptionType::kBool, offsetof(DriverOptions, m_DontReusePreviousResults), "Builds the requested target from scratch"},
    {'w', "spammy-verbose", OptionType::kBool, offsetof(DriverOptions, m_SpammyVerbose), "Enable spammy verbose build messages"},
    {'D', "debug", OptionType::kBool, offsetof(DriverOptions, m_DebugMessages), "Enable debug messages"},
    {'k', "continue-on-failure", OptionType::kBool, offsetof(DriverOptions, m_ContinueOnFailure), "Build as much as possible after the first error"},
    {'S', "debug-signing", OptionType::kBool, offsetof(DriverOptions, m_DebugSigning), "Generate an extensive log of signature generation"},
    {'e', "just-print-leafinput-signature", OptionType::kString, offsetof(DriverOptions, m_JustPrintLeafInputSignature), "Print to the specified file the leaf input signature ingredients of the requested node"},
    {'c', "stdin-canary", OptionType::kBool, offsetof(DriverOptions, m_StandardInputCanary), "Abort build if stdin is closed"},
    {'s', "stats", OptionType::kBool, offsetof(DriverOptions, m_DisplayStats), "Display stats"},
    {'p', "profile", OptionType::kString, offsetof(DriverOptions, m_ProfileOutput), "Output build profile"},
    {'C', "working-dir", OptionType::kString, offsetof(DriverOptions, m_WorkingDir), "Set working directory before building"},
    {'R', "dagfile", OptionType::kString, offsetof(DriverOptions, m_DAGFileName), "filename of where tundra should store the mmapped dag file"},
    {'I', "report-includes", OptionType::kString, offsetof(DriverOptions, m_IncludesOutput), "Output included files into a json file and exit"},
    {'h', "help", OptionType::kBool, offsetof(DriverOptions, m_ShowHelp), "Show help"},
#if defined(TUNDRA_WIN32)
    {'U', "unprotected", OptionType::kBool, offsetof(DriverOptions, m_RunUnprotected), "Run unprotected (same process group - for debugging)"},
#endif
    {'X', "inspect", OptionType::kBool, offsetof(DriverOptions, m_Inspect), "Inspect the following data files, then exit."},
    };

static int AssignOptionValue(char *option_base, const OptionTemplate *templ, const char *value, bool is_short)
{
    char *dest = option_base + templ->m_Offset;

    switch (templ->m_Type)
    {
    case OptionType::kBool:
        *(bool *)dest = true;
        return 1;

    case OptionType::kInt:
        if (value)
        {
            *(int *)dest = atoi(value);
            return is_short ? 2 : 1;
        }
        else
        {
            if (is_short)
                fprintf(stderr, "option requires an argument: %c\n", templ->m_ShortName);
            else
                fprintf(stderr, "option requires an argument: --%s\n", templ->m_LongName);
            return 0;
        }
        break;

    case OptionType::kString:
        if (value)
        {
            *(const char **)dest = value;
            return is_short ? 2 : 1;
        }
        else
        {
            if (is_short)
                fprintf(stderr, "option requires an argument: %c\n", templ->m_ShortName);
            else
                fprintf(stderr, "option requires an argument: --%s\n", templ->m_LongName);
            return 0;
        }
        break;

    default:
        return 0;
    }
}

static bool InitOptions(DriverOptions *options, int *argc, char ***argv)
{
    int opt = 1;
    char *option_base = (char *)options;

    while (opt < *argc)
    {
        bool found = false;
        const char *opt_str = (*argv)[opt];
        int advance_count = 0;

        if ('-' != opt_str[0])
            break;

        if (opt_str[1] != '-')
        {
            const char *opt_arg = opt + 1 < *argc ? (*argv)[opt + 1] : nullptr;

            if (opt_str[2])
            {
                fprintf(stderr, "bad option: %s\n", opt_str);
                return false;
            }

            for (size_t i = 0; !found && i < ARRAY_SIZE(g_OptionTemplates); ++i)
            {
                const OptionTemplate *templ = g_OptionTemplates + i;

                if (opt_str[1] == templ->m_ShortName)
                {
                    found = true;
                    advance_count = AssignOptionValue(option_base, templ, opt_arg, true);
                }
            }
        }
        else
        {
            const char *equals = strchr(opt_str, '=');
            size_t optlen = equals ? equals - opt_str - 2 : strlen(opt_str + 2);
            const char *opt_arg = equals ? equals + 1 : nullptr;

            for (size_t i = 0; !found && i < ARRAY_SIZE(g_OptionTemplates); ++i)
            {
                const OptionTemplate *templ = g_OptionTemplates + i;

                if (strlen(templ->m_LongName) == optlen && 0 == memcmp(opt_str + 2, templ->m_LongName, optlen))
                {
                    found = true;
                    advance_count = AssignOptionValue(option_base, templ, opt_arg, false);
                }
            }
        }

        if (0 == advance_count)
            return false;

        if (!found)
        {
            fprintf(stderr, "unrecognized option: %s\n", opt_str);
            return false;
        }

        opt += advance_count;
    }

    *argc -= opt;
    *argv += opt;

    return true;
}

static void ShowHelp()
{
    printf("\nTundra Build Processor 2.0\n");
    printf("Copyright (C) 2010-2018 Andreas Fredriksson\n\n");

#ifdef HAVE_GIT_INFO
    printf("Git branch: %s\n", g_GitBranch);
    printf("Git commit: %s\n\n", g_GitVersion);
#endif

    printf("This program comes with ABSOLUTELY NO WARRANTY.\n");

    printf("Usage: tundra2 [options...] [targets...]\n\n");
    printf("Options:\n");

    size_t max_opt_len = 0;
    for (size_t i = 0; i < ARRAY_SIZE(g_OptionTemplates); ++i)
    {
        size_t opt_len = strlen(g_OptionTemplates[i].m_LongName) + 12;
        if (opt_len > max_opt_len)
            max_opt_len = opt_len;
    }

    for (size_t i = 0; i < ARRAY_SIZE(g_OptionTemplates); ++i)
    {
        const OptionTemplate *t = g_OptionTemplates + i;

        if (!t->m_Help)
            continue;

        char long_text[256];
        if (t->m_Type == OptionType::kInt)
            snprintf(long_text, sizeof long_text, "%s=<integer>", t->m_LongName);
        else if (t->m_Type == OptionType::kString)
            snprintf(long_text, sizeof long_text, "%s=<string>", t->m_LongName);
        else
            snprintf(long_text, sizeof long_text, "%s          ", t->m_LongName);

        if (t->m_ShortName != 0)
            printf("  -%c, ", t->m_ShortName);
        else
            printf("       ");

        printf("--%-*s %s\n", (int)max_opt_len, long_text, t->m_Help);
    }
}

int main(int argc, char *argv[])
{


#if TUNDRA_WIN32
    if (getenv("GIVE_DEBUGGER_CHANCE_TO_ATTACH") != nullptr)
    {
        MessageBox(
            NULL,
            (LPCWSTR)L"Native debugger can attach now",
            (LPCWSTR)L"Tundra",
            MB_OK);
    }
#endif

    InitCommon();

    Driver driver;
    DriverOptions options;

    // Set default options
    DriverOptionsInit(&options);

    // Scan options from command line, update argc/argv
    if (!InitOptions(&options, &argc, &argv))
    {
        ShowHelp();
        return 1;
    }

    DriverInitializeTundraFilePaths(&options);

    if (options.m_Inspect)
    {
        return inspect(argc, argv);
    }

#if defined(TUNDRA_WIN32)
    if (!options.m_RunUnprotected && nullptr == getenv("_TUNDRA2_PARENT_PROCESS_HANDLE"))
    {
        // Re-launch Tundra2 as a child in a new process group. The child will be passed a handle to our process so it can
        // watch for us dying, but not be affected by sudden termination itself.
        // This ridiculous tapdance is there to prevent hard termination by things like visual studio and the mingw shell.
        // Because Tundra needs to save build state when shutting down, we need this.

        HANDLE myproc = GetCurrentProcess();
        HANDLE self_copy = NULL;
        if (!DuplicateHandle(myproc, myproc, myproc, &self_copy, 0, TRUE, DUPLICATE_SAME_ACCESS))
        {
            CroakErrno("DuplicateHandle() failed");
        }

        // Expose handle in the environment for the child process.
        {
            char handle_value[128];
            _snprintf(handle_value, sizeof handle_value, "_TUNDRA2_PARENT_PROCESS_HANDLE=%016I64x", uint64_t(self_copy));
            _putenv(handle_value);
        }

        STARTUPINFOA startup_info;
        PROCESS_INFORMATION proc_info;
        ZeroMemory(&startup_info, sizeof startup_info);
        ZeroMemory(&proc_info, sizeof proc_info);
        startup_info.cb = sizeof startup_info;

        HANDLE job_handle = CreateJobObject(NULL, NULL);

        // Set job object limits so children can break out.
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
        ZeroMemory(&limits, sizeof(limits));
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_BREAKAWAY_OK | JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK;

        if (!SetInformationJobObject(job_handle, JobObjectExtendedLimitInformation, &limits, sizeof limits))
            CroakErrno("couldn't set job info");

        if (!CreateProcessA(NULL, GetCommandLineA(), NULL, NULL, TRUE, CREATE_BREAKAWAY_FROM_JOB | CREATE_NEW_PROCESS_GROUP | CREATE_SUSPENDED, NULL, NULL, &startup_info, &proc_info))
            CroakErrno("CreateProcess() failed");

        AssignProcessToJobObject(job_handle, proc_info.hProcess);
        ResumeThread(proc_info.hThread);

        WaitForSingleObject(proc_info.hProcess, INFINITE);

        DWORD exit_code = 1;
        GetExitCodeProcess(proc_info.hProcess, &exit_code);

        CloseHandle(proc_info.hThread);
        CloseHandle(proc_info.hProcess);
        ExitProcess(exit_code);
    }
    else if (const char *handle_str = getenv("_TUNDRA2_PARENT_PROCESS_HANDLE"))
    {
        HANDLE h = (HANDLE)_strtoi64(handle_str, NULL, 16);
        SignalHandlerInitWithParentProcess(h);
    }
    else
    {
        SignalHandlerInit();
    }
#else
    SignalHandlerInit();
#endif

    uint64_t start_time = TimerGet();

    if (options.m_WorkingDir)
    {
        if (!SetCwd(options.m_WorkingDir))
            CroakErrno("couldn't change directory to %s", options.m_WorkingDir);
    }

    if (options.m_ThreadCount > kMaxBuildThreads)
    {
        Log(kWarning, "too many build threads (%d) - clamping to %d", options.m_ThreadCount, kMaxBuildThreads);
        options.m_ThreadCount = kMaxBuildThreads;
    }

    if (options.m_ShowHelp)
    {
        ShowHelp();
        return 0;
    }

    // Initialize logging
    int log_flags = kWarning | kError;

    if (options.m_DebugMessages)
        log_flags |= kInfo | kDebug;

    if (options.m_SpammyVerbose)
        log_flags |= kSpam | kInfo | kDebug;

    SetLogFlags(log_flags);

    // Protect against running two or more instances simultaneously in the same directory.
    // This can happen if Visual Studio is trying to launch more than one copy of tundra.
#if defined(TUNDRA_WIN32)
    {
        char mutex_name[MAX_PATH];
        char cwd[MAX_PATH];
        char cwd_nerfed[MAX_PATH];
        GetCwd(cwd, sizeof cwd);
        const char *i = cwd;
        char *o = cwd_nerfed;
        char ch;
        do
        {
            ch = *i++;
            switch (ch)
            {
            case '\\':
            case ':':
                ch = '^';
                break;
            }
            *o++ = ch;
        } while (ch);

        _snprintf(mutex_name, sizeof mutex_name, "Global\\Tundra--%s-%s", cwd_nerfed, options.m_DAGFileName);
        mutex_name[sizeof(mutex_name) - 1] = '\0';
        bool warning_printed = false;
        HANDLE mutex = CreateMutexA(nullptr, false, mutex_name);

        while (WAIT_TIMEOUT == WaitForSingleObject(mutex, 100))
        {
            if (!warning_printed)
            {
                Log(kWarning, "More than one copy of Tundra running in %s -- PID %u waiting", cwd, GetCurrentProcessId());
                warning_printed = true;
            }
            Sleep(100);
        }

        Log(kDebug, "PID %u successfully locked %s", GetCurrentProcessId(), cwd);
    }
#endif

    ExecInit();

    const char *buildTitle = "Tundra";

    // Initialize driver
    if (!DriverInit(&driver, &options))
        return 1;

    // Initialize profiler if needed
    if (driver.m_Options.m_ProfileOutput)
        ProfilerInit(driver.m_Options.m_ProfileOutput, driver.m_Options.m_ThreadCount + 1);

    BuildResult::Enum build_result = BuildResult::kSetupError;
    int finished_node_count = 0;

    if (!DriverInitData(&driver))
        goto leave;

    InitializeDynamicOutputDirectories(driver.m_Options.m_ThreadCount);

#if TUNDRA_WIN32
    buildTitle = _strdup(driver.m_DagData->m_BuildTitle.Get());
#else
    buildTitle = strdup(driver.m_DagData->m_BuildTitle.Get());
#endif

    if (driver.m_Options.m_ShowTargets)
    {
        DriverShowTargets(&driver);
        Log(kDebug, "Only showing targets - quitting");
        build_result = BuildResult::kOk;
        goto leave;
    }

    if (driver.m_Options.m_IncludesOutput != nullptr)
    {
        build_result = ReportIncludes(&driver) ? BuildResult::kOk : BuildResult::kSetupError;
        Log(kDebug, "Only reporting includes - quitting");
        goto leave;
    }

    if (options.m_StandardInputCanary)
        StandardInputCanary::Initialize();

    DriverReportStartup(&driver, (const char **)argv, argc);


    RemoveStaleOutputs(&driver);

    build_result = DriverBuild(&driver, &finished_node_count, (const char**) argv, argc);

    if (!SaveAllBuiltNodes(&driver))
        Log(kError, "Couldn't save AllBuiltNodes");

    if (!DriverSaveScanCache(&driver))
        Log(kWarning, "Couldn't save header scanning cache");

    if (!DriverSaveDigestCache(&driver))
        Log(kWarning, "Couldn't save SHA1 digest cache");

leave:

    DestroyDynamicOutputDirectories();

    DriverDestroy(&driver);

    // Dump/close profiler
    if (driver.m_Options.m_ProfileOutput)
        ProfilerDestroy();

    // Dump stats
    if (options.m_DisplayStats)
    {
        printf("output cleanup:    %10.2f ms\n", TimerToSeconds(g_Stats.m_StaleCheckTimeCycles) * 1000.0);
        printf("json parse time:   %10.2f ms\n", TimerToSeconds(g_Stats.m_JsonParseTimeCycles) * 1000.0);
        printf("scan cache:\n");
        printf("  hits (new):      %10u\n", g_Stats.m_NewScanCacheHits);
        printf("  hits (frozen):   %10u\n", g_Stats.m_OldScanCacheHits);
        printf("  misses:          %10u\n", g_Stats.m_ScanCacheMisses);
        printf("  inserts:         %10u\n", g_Stats.m_ScanCacheInserts);
        printf("  save time:       %10.2f ms\n", TimerToSeconds(g_Stats.m_ScanCacheSaveTime) * 1000.0);
        printf("  entries dropped: %10u\n", g_Stats.m_ScanCacheEntriesDropped);
        printf("file signing:\n");
        printf("  cache hits:      %10u\n", g_Stats.m_DigestCacheHits);
        printf("  cache get time:  %10.2f ms\n", TimerToSeconds(g_Stats.m_DigestCacheGetTimeCycles) * 1000.0);
        printf("  cache save time: %10.2f ms\n", TimerToSeconds(g_Stats.m_DigestCacheSaveTimeCycles) * 1000.0);
        printf("  digests:         %10u\n", g_Stats.m_FileDigestCount);
        printf("  digest time:     %10.2f ms\n", TimerToSeconds(g_Stats.m_FileDigestTimeCycles) * 1000.0);
        printf("stat cache:\n");
        printf("  hits:            %10u\n", g_Stats.m_StatCacheHits);
        printf("  misses:          %10u\n", g_Stats.m_StatCacheMisses);
        printf("  dirty:           %10u\n", g_Stats.m_StatCacheDirty);
        printf("building:\n");
        printf("  old records:     %10u\n", g_Stats.m_StateSaveOld);
        printf("  new records:     %10u\n", g_Stats.m_StateSaveNew);
        printf("  dropped records: %10u\n", g_Stats.m_StateSaveDropped);
        printf("  state save time: %10.2f ms\n", TimerToSeconds(g_Stats.m_StateSaveTimeCycles) * 1000.0);
        printf("  exec() count:    %10u\n", g_Stats.m_ExecCount);
        printf("  exec() time:     %10.2f s\n", TimerToSeconds(g_Stats.m_ExecTimeCycles));
        printf("low-level syscalls:\n");
        printf("  mmap() calls:    %10u\n", g_Stats.m_MmapCalls);
        printf("  mmap() time:     %10.2f ms\n", TimerToSeconds(g_Stats.m_MmapTimeCycles) * 1000.0);
        printf("  munmap() calls:  %10u\n", g_Stats.m_MunmapCalls);
        printf("  munmap() time:   %10.2f ms\n", TimerToSeconds(g_Stats.m_MunmapTimeCycles) * 1000.0);
        printf("  stat() calls:    %10u\n", g_Stats.m_StatCount);
        printf("  stat() time:     %10.2f ms\n", TimerToSeconds(g_Stats.m_StatTimeCycles) * 1000.0);
    }

    double total_time = TimerDiffSeconds(start_time, TimerGet());
    bool haveTitle = strlen(buildTitle) > 0;
    if (haveTitle && (build_result != 0 || !options.m_SilenceIfPossible))
    {
        if (total_time < 60.0)
        {
            PrintServiceMessage(build_result == 0 ? MessageStatusLevel::Success : MessageStatusLevel::Failure, "*** %s %s (%.2f seconds), %d items updated, %d evaluated", buildTitle, BuildResult::Names[build_result], total_time, g_Stats.m_ExecCount, finished_node_count);
        }
        else
        {
            int t = (int)total_time;
            int h = t / 3600;
            t -= h * 3600;
            int m = t / 60;
            t -= m * 60;
            int s = t;
            PrintServiceMessage(build_result == 0 ? MessageStatusLevel::Success : MessageStatusLevel::Failure, "*** %s %s (%.2f seconds - %d:%02d:%02d), %d items updated, %d evaluated", buildTitle, BuildResult::Names[build_result], total_time, h, m, s, g_Stats.m_ExecCount, finished_node_count);
        }
    }

    SetStructuredLogFileName(nullptr);

    HeapVerifyNoLeaks();

    return build_result;
}
