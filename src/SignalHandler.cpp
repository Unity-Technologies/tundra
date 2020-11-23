#include "SignalHandler.hpp"
#include "Config.hpp"
#include "Mutex.hpp"
#include "Thread.hpp"
#include "ConditionVar.hpp"
#include <stdio.h>

#if defined(TUNDRA_UNIX)
#include <signal.h>
#include <sys/signal.h>
#include <string.h>
#include <pthread.h>
#endif


static struct
{
    bool m_Signalled;
    const char *m_Reason;
} s_SignalInfo;

static Mutex s_SignalMutex;
static ConditionVariable *s_SignalCond;

const char *SignalGetReason(void)
{
    MutexLock(&s_SignalMutex);
    const char *result = s_SignalInfo.m_Reason;
    MutexUnlock(&s_SignalMutex);
    return result;
}

void SignalSet(const char *reason)
{
    MutexLock(&s_SignalMutex);

    s_SignalInfo.m_Signalled = true;
    s_SignalInfo.m_Reason = reason;

    if (ConditionVariable *cvar = s_SignalCond)
        CondBroadcast(cvar);

    MutexUnlock(&s_SignalMutex);
}

#if defined(TUNDRA_UNIX)
static void HandleSignal (int sig)
{
    const char *reason = "unknown";
    switch (sig)
    {
    case SIGINT:
        reason = "SIGINT";
        break;
    case SIGTERM:
        reason = "SIGTERM";
        break;
    case SIGQUIT:
        reason = "SIGQUIT";
        break;
    }
    SignalSet(reason);
}
#endif

#if defined(TUNDRA_WIN32)
BOOL WINAPI WindowsSignalHandlerFunc(DWORD ctrl_type)
{
    const char *reason = NULL;

    switch (ctrl_type)
    {
    case CTRL_C_EVENT:
        reason = "Ctrl+C";
        break;

    case CTRL_BREAK_EVENT:
        reason = "Ctrl+Break";
        break;
    }

    if (reason)
    {
        SignalSet(reason);
        return TRUE;
    }

    return FALSE;
}
#endif

void SignalHandlerInit()
{
    MutexInit(&s_SignalMutex);

#if defined(TUNDRA_UNIX)

    struct sigaction s;

    memset(&s, 0, sizeof(struct sigaction));
    s.sa_handler = HandleSignal;
    if (sigaction(SIGINT, &s, NULL) != 0)
        CroakErrno("sigaction failed.");
    if (sigaction(SIGTERM, &s, NULL) != 0)
        CroakErrno("sigaction failed.");
    if (sigaction(SIGQUIT, &s, NULL) != 0)
        CroakErrno("sigaction failed.");

#elif defined(TUNDRA_WIN32)
    SetConsoleCtrlHandler(WindowsSignalHandlerFunc, TRUE);
#else
#error Meh
#endif
}

#if defined(TUNDRA_WIN32)
static DWORD WINAPI CanaryWatcherThread(LPVOID parent_handle)
{
    WaitForSingleObject(parent_handle, INFINITE);
    SignalSet("Process terminated");
    return 0;
}

void SignalHandlerInitWithParentProcess(void *parent_handle)
{
    SignalHandlerInit();
    HANDLE t = CreateThread(NULL, 16 * 1024, CanaryWatcherThread, parent_handle, 0, NULL);

    if (nullptr == t)
        CroakErrno("Failed to create canary watcher thread");

    CloseHandle(t);
}
#endif

void SignalHandlerShutdown()
{
    MutexInit(&s_SignalMutex);
}

void SignalHandlerSetCondition(ConditionVariable *cvar)
{
    MutexLock(&s_SignalMutex);
    s_SignalCond = cvar;
    MutexUnlock(&s_SignalMutex);
}