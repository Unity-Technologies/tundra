#include "NodeResultPrinting.hpp"
#include "DagData.hpp"
#include "BuildQueue.hpp"
#include "Exec.hpp"
#include "JsonWriter.hpp"
#include "LeafInputSignature.hpp"
#include <stdio.h>
#include <sstream>
#include <ctime>
#include <math.h>
#include "Driver.hpp"
#if TUNDRA_UNIX
#include <unistd.h>
#include <stdarg.h>
#endif




struct NodeResultPrintData
{
    const Frozen::DagNode *node_data;
    const char *cmd_line;
    bool verbose;
    int duration;
    ValidationResult::Enum validation_result;
    const bool *untouched_outputs;
    const char *output_buffer;
    int processed_node_count;
    int amount_of_nodes_ever_queued;
    MessageStatusLevel::Enum status_level;
    int return_code;
    bool was_preparation_error;
};

static bool EmitColors = false;

static uint64_t last_progress_message_of_any_job;
static const Frozen::DagNode *last_progress_message_job = nullptr;
static int total_number_node_results_printed = 0;

static int deferred_message_count = 0;
static NodeResultPrintData deferred_messages[kMaxBuildThreads];

static bool isTerminatingChar(char c)
{
    return c >= 0x40 && c <= 0x7E;
}

static bool IsEscapeCode(char c)
{
    return c == 0x1B;
}

static char *DetectEscapeCode(char *ptr)
{
    if (!IsEscapeCode(ptr[0]))
        return ptr;
    if (ptr[1] == 0)
        return ptr;

    //there are other characters valid than [ here, but for now we'll only support stripping ones that have [, as all color sequences have that.
    if (ptr[1] != '[')
        return ptr;

    char *endOfCode = ptr + 2;

    while (true)
    {
        char c = *endOfCode;
        if (c == 0)
            return ptr;
        if (isTerminatingChar(c))
            return endOfCode + 1;
        endOfCode++;
    }
}

void StripAnsiColors(char *buffer)
{
    char *writeCursor = buffer;
    char *readCursor = buffer;
    while (*readCursor)
    {
        char *adjusted = DetectEscapeCode(readCursor);
        if (adjusted != readCursor)
        {
            readCursor = adjusted;
            continue;
        }
        *writeCursor++ = *readCursor++;
    }
    *writeCursor++ = 0;
}

int s_IdentificationColor;
int m_VisualMaxNodes;

void InitNodeResultPrinting(const DriverOptions* driverOptions)
{
    last_progress_message_of_any_job = TimerGet() - 10000;

#if TUNDRA_UNIX
    if (isatty(fileno(stdout)))
    {
        EmitColors = true;
    }
#endif

#if TUNDRA_WIN32
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD dwMode = 0;
    if (GetConsoleMode(hOut, &dwMode))
        EmitColors = true;
#endif

    char *value = getenv("DOWNSTREAM_STDOUT_CONSUMER_SUPPORTS_COLOR");
    if (value == nullptr)
        return;

    if (*value == '1')
        EmitColors = true;
    if (*value == '0')
        EmitColors = false;

    s_IdentificationColor = driverOptions->m_IdentificationColor;
    m_VisualMaxNodes = driverOptions->m_VisualMaxNodes;
}

static void EnsureConsoleCanHandleColors()
{
#if TUNDRA_WIN32
    //We invoke this function before every printf that wants to emit a color, because it looks like child processes that tundra invokes
    //can and do SetConsoleMode() which affects our console. Sometimes a child process will set the consolemode to no longer have our flag
    //which makes all color output suddenly screw up.
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD dwMode = 0;
    if (GetConsoleMode(hOut, &dwMode))
    {
        const int ENABLE_VIRTUAL_TERMINAL_PROCESSING_impl = 0x0004;
        DWORD newMode = dwMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING_impl;
        if (newMode != dwMode)
            SetConsoleMode(hOut, newMode);
    }
#endif
}

static void EmitColor(const char *colorsequence)
{
    if (EmitColors)
    {
        EnsureConsoleCanHandleColors();
        printf("%s", colorsequence);
    }
}

#define RED "\x1B[91m"
#define GRN "\x1B[32m"
#define YEL "\x1B[33m"
#define BLU "\x1B[34m"
#define MAG "\x1B[35m"
#define CYN "\x1B[36m"
#define GRAY "\x0B[37m"
#define WHT "\x1B[37m"
#define RESET "\x1B[0m"


static void PrintDiagnosticPrefix(const char *title, const char *color = YEL)
{
    EmitColor(color);
    printf("##### %s\n", title);
    EmitColor(RESET);
}

static void PrintDiagnosticFormat(const char *title, const char *formatString, ...)
{
    PrintDiagnosticPrefix(title);
    va_list args;
    va_start(args, formatString);
    vfprintf(stdout, formatString, args);
    va_end(args);
    printf("\n");
}

static void PrintDiagnostic(const char *title, const char *contents)
{
    if (contents != nullptr)
        PrintDiagnosticFormat(title, "%s", contents);
}

static void PrintDiagnostic(const char *title, int content)
{
    PrintDiagnosticFormat(title, "%d", content);
}

void EmitColorReset()
{
    EmitColor(RESET);
}

void EmitColorForLevel(MessageStatusLevel::Enum status_level)
{
    if (status_level == MessageStatusLevel::Info)
        EmitColor(WHT);
    if (status_level == MessageStatusLevel::Warning)
        EmitColor(YEL);
    if (status_level == MessageStatusLevel::Success)
        EmitColor(GRN);
    if (status_level == MessageStatusLevel::Failure)
        EmitColor(RED);
}

void PrintServiceMessage(MessageStatusLevel::Enum status_level, const char *formatString, ...)
{
    EmitColorForLevel(status_level);
    va_list args;
    va_start(args, formatString);
    vprintf(formatString, args);
    va_end(args);
    EmitColor(RESET);
    printf("\n");
}


static void TrimOutputBuffer(OutputBufferData *buffer)
{
    auto isNewLine = [](char c) { return c == 0x0A || c == 0x0D; };

    int trimmedCursor = buffer->cursor;
    while (isNewLine(*(buffer->buffer + trimmedCursor - 1)) && trimmedCursor > 0)
        trimmedCursor--;

    buffer->buffer[trimmedCursor] = 0;
    if (!EmitColors)
    {
        StripAnsiColors(buffer->buffer);
    }
}

static void EmitBracketColor(MessageStatusLevel::Enum status_level)
{
    if (s_IdentificationColor == 1)
        EmitColor(MAG);
    else if (s_IdentificationColor == 2)
        EmitColor(BLU);
    else
        EmitColorForLevel(status_level);
}

static void PrintMessageMaster(MessageStatusLevel::Enum status_level, int dividend, int divisor, int duration, const char* message, va_list args)
{
    EmitBracketColor(status_level);
    printf("[");
    EmitColorForLevel(status_level);

    int maxDigits = ceil(log10(divisor + 1));
    int visualMaxNodeDigits = ceil(log10(m_VisualMaxNodes + 1));
    int shouldPrint = 2*visualMaxNodeDigits + 2;

    int printed = 0;
    if (dividend >= 0)
    {
        printed += printf("%*d/%d ", maxDigits, dividend, divisor);
    }
    for (int i=printed; i<shouldPrint;i++)
        printf(" ");

    if (duration >= 0)
        printf("%2ds", duration);
    else
        printf("   ");


    EmitBracketColor(status_level);
    printf("] ");
    EmitColor(RESET);
    vprintf(message, args);
    printf("\n");
}


void PrintMessage(MessageStatusLevel::Enum status_level, int duration, const char* message, ...)
{
    va_list args;
    va_start(args,message);
    PrintMessageMaster(status_level, -1, -1, duration, message, args);
    va_end(args);
}
void PrintMessage(MessageStatusLevel::Enum status_level, const char* message, ...)
{
    va_list args;
    va_start(args,message);
    PrintMessageMaster(status_level, -1, -1, -1, message, args);
    va_end(args);
}
void PrintMessage(MessageStatusLevel::Enum status_level, int dividend, int divisor, int duration, const char* message, ...)
{
    va_list args;
    va_start(args,message);
    PrintMessageMaster(status_level, dividend, divisor, duration, message, args);
    va_end(args);
}

void PrintMessage(MessageStatusLevel::Enum status_level, int duration, ExecResult *result, const char *message, ...)
{
    va_list args;
    va_start(args, message);
    PrintMessage(status_level, (int)duration, message, args);
    va_end(args);

    if (result != nullptr && result->m_ReturnCode != 0)
    {
        TrimOutputBuffer(&result->m_OutputBuffer);
        printf("%s\n", result->m_OutputBuffer.buffer);
    }
}

static void PrintNodeResult(const NodeResultPrintData *data, BuildQueue *queue)
{
    PrintMessage(data->status_level, data->processed_node_count, queue->m_AmountOfNodesEverQueued, data->duration, data->node_data->m_Annotation.Get());

    if (data->verbose)
    {
        PrintDiagnostic("CommandLine", data->cmd_line);
        for (int i = 0; i != data->node_data->m_FrontendResponseFiles.GetCount(); i++)
        {
            char titleBuffer[1024];
            const char *file = data->node_data->m_FrontendResponseFiles[i].m_Filename;
            snprintf(titleBuffer, sizeof titleBuffer, "Contents of %s", file);

            char *content_buffer;
            FILE *f = fopen(file, "rb");
            if (!f)
            {

                int buffersize = 512;
                content_buffer = (char *)HeapAllocate(queue->m_Config.m_Heap, buffersize);
                snprintf(content_buffer, buffersize, "couldn't open %s for reading", file);
            }
            else
            {
                fseek(f, 0L, SEEK_END);
                size_t size = ftell(f);
                rewind(f);
                size_t buffer_size = size + 1;
                content_buffer = (char *)HeapAllocate(queue->m_Config.m_Heap, buffer_size);

                size_t read = fread(content_buffer, 1, size, f);
                content_buffer[read] = '\0';
                fclose(f);
            }
            PrintDiagnostic(titleBuffer, content_buffer);
            HeapFree(queue->m_Config.m_Heap, content_buffer);
        }

        if (data->node_data->m_EnvVars.GetCount() > 0)
            PrintDiagnosticPrefix("Custom Environment Variables");
        for (int i = 0; i != data->node_data->m_EnvVars.GetCount(); i++)
        {
            auto &entry = data->node_data->m_EnvVars[i];
            printf("%s=%s\n", entry.m_Name.Get(), entry.m_Value.Get());
        }
        if (data->return_code == 0)
        {
            if (data->validation_result == ValidationResult::UnexpectedConsoleOutputFail)
            {
                PrintDiagnosticPrefix("Failed because this command wrote something to the output that wasn't expected. We were expecting any of the following strings:", RED);
                int count = data->node_data->m_AllowedOutputSubstrings.GetCount();
                for (int i = 0; i != count; i++)
                    printf("%s\n", (const char *)data->node_data->m_AllowedOutputSubstrings[i]);
                if (count == 0)
                    printf("<< no allowed strings >>\n");
            }
            else if (data->validation_result == ValidationResult::UnwrittenOutputFileFail)
            {
                PrintDiagnosticPrefix("Failed because this command failed to write the following output files:", RED);
                for (int i = 0; i < data->node_data->m_OutputFiles.GetCount(); i++)
                    if (data->untouched_outputs[i])
                        printf("%s\n", (const char *)data->node_data->m_OutputFiles[i].m_Filename);
            }
        }
        if (data->return_code != 0)
            PrintDiagnostic("ExitCode", data->return_code);
    }

    if (data->output_buffer != nullptr)
    {
        if (data->verbose)
        {
            PrintDiagnosticPrefix("Output");

            printf("%s\n", data->output_buffer);
        }
        else if (0 != (data->validation_result != ValidationResult::SwallowStdout))
        {
            printf("%s\n", data->output_buffer);
        }
    }
}

static void PrintCacheOperationIntoStructuredLog(ThreadState* thread_state, RuntimeNode* node, const char* hitOrMissMessage)
{
    if (IsStructuredLogActive())
    {
        MemAllocLinearScope allocScope(&thread_state->m_ScratchAlloc);

        JsonWriter msg;
        JsonWriteInit(&msg, &thread_state->m_ScratchAlloc);
        JsonWriteStartObject(&msg);

        JsonWriteKeyName(&msg, "msg");
        JsonWriteValueString(&msg, hitOrMissMessage);

        JsonWriteKeyName(&msg, "annotation");
        JsonWriteValueString(&msg, node->m_DagNode->m_Annotation);

        JsonWriteKeyName(&msg, "index");
        JsonWriteValueInteger(&msg, node->m_DagNode->m_OriginalIndex);

        JsonWriteKeyName(&msg, "leafInputSignature");
        char hash[kDigestStringSize];
        DigestToString(hash, node->m_CurrentLeafInputSignature->digest);
        JsonWriteValueString(&msg, hash);

        JsonWriteEndObject(&msg);
        LogStructured(&msg);
    }
}

void PrintCacheHitIntoStructuredLog(ThreadState* thread_state, RuntimeNode* node)
{
    PrintCacheOperationIntoStructuredLog(thread_state, node, "cachehit");
}

void PrintCacheMissIntoStructuredLog(ThreadState* thread_state, RuntimeNode* node)
{
    PrintCacheOperationIntoStructuredLog(thread_state, node, "cachemiss");
}

void PrintCacheHit(BuildQueue* queue, ThreadState *thread_state, double duration, RuntimeNode* node)
{
    CheckHasLock(&queue->m_Lock);

    PrintCacheHitIntoStructuredLog(thread_state, node);

    char buffer[1024];
    char hash[kDigestStringSize];
    DigestToString(hash, node->m_CurrentLeafInputSignature->digest);
    int written = snprintf(buffer, sizeof(buffer), "%s [CacheHit %s]", node->m_DagNode->m_Annotation.Get(), hash);
    if (written >0 && written < sizeof(buffer))
        PrintMessage(MessageStatusLevel::Success, queue->m_FinishedNodeCount, queue->m_AmountOfNodesEverQueued, duration, buffer);
}



static char *StrDupN(MemAllocHeap *allocator, const char *str, size_t len)
{
    size_t sz = len + 1;
    char *buffer = static_cast<char *>(HeapAllocate(allocator, sz));
    memcpy(buffer, str, sz - 1);
    buffer[sz - 1] = '\0';
    return buffer;
}

static char *StrDup(MemAllocHeap *allocator, const char *str)
{
    return StrDupN(allocator, str, strlen(str));
}

void PrintNodeResult(
    ExecResult *result,
    const Frozen::DagNode *node_data,
    const char *cmd_line,
    BuildQueue *queue,
    ThreadState *thread_state,
    bool always_verbose,
    uint64_t time_exec_started,
    ValidationResult::Enum validationResult,
    const bool *untouched_outputs,
    bool was_preparation_error)
{
    int processedNodeCount = queue->m_FinishedNodeCount;
    bool failed = result->m_ReturnCode != 0 || validationResult >= ValidationResult::UnexpectedConsoleOutputFail;
    bool verbose = (failed && !was_preparation_error) || always_verbose;

    int duration = TimerDiffSeconds(time_exec_started, TimerGet());

    NodeResultPrintData data = {};
    data.node_data = node_data;
    data.cmd_line = cmd_line;
    data.verbose = verbose;
    data.duration = duration;
    data.validation_result = validationResult;
    data.untouched_outputs = untouched_outputs;
    data.processed_node_count = processedNodeCount;
    data.amount_of_nodes_ever_queued = queue->m_AmountOfNodesEverQueued;
    data.status_level = failed ? MessageStatusLevel::Failure : MessageStatusLevel::Success;

    data.return_code = was_preparation_error ? 1 : result->m_ReturnCode;
    data.was_preparation_error = was_preparation_error;

    bool anyOutput = result->m_OutputBuffer.cursor > 0;
    if (anyOutput && verbose)
    {
        TrimOutputBuffer(&result->m_OutputBuffer);
        data.output_buffer = result->m_OutputBuffer.buffer;
    }
    else if (anyOutput && 0 != (validationResult != ValidationResult::SwallowStdout))
    {
        TrimOutputBuffer(&result->m_OutputBuffer);
        data.output_buffer = result->m_OutputBuffer.buffer;
    }

    if (IsStructuredLogActive())
    {
        MemAllocLinearScope allocScope(&thread_state->m_ScratchAlloc);

        JsonWriter msg;
        JsonWriteInit(&msg, &thread_state->m_ScratchAlloc);
        JsonWriteStartObject(&msg);

        JsonWriteKeyName(&msg, "msg");
        JsonWriteValueString(&msg, "noderesult");

        JsonWriteKeyName(&msg, "processed_node_count");
        JsonWriteValueInteger(&msg, data.processed_node_count);

        JsonWriteKeyName(&msg, "amount_of_nodes_ever_queued");
        JsonWriteValueInteger(&msg, data.amount_of_nodes_ever_queued);

        JsonWriteKeyName(&msg, "annotation");
        JsonWriteValueString(&msg, node_data->m_Annotation);

        JsonWriteKeyName(&msg, "index");
        JsonWriteValueInteger(&msg, node_data->m_OriginalIndex);

        JsonWriteKeyName(&msg, "exitcode");
        JsonWriteValueInteger(&msg, result->m_ReturnCode);

        if (data.node_data->m_OutputFiles.GetCount() > 0)
        {
            JsonWriteKeyName(&msg, "outputfile");
            JsonWriteValueString(&msg, data.node_data->m_OutputFiles[0].m_Filename);
        }

        if (data.node_data->m_OutputDirectories.GetCount() > 0)
        {
            JsonWriteKeyName(&msg, "outputdirectory");
            JsonWriteValueString(&msg, data.node_data->m_OutputDirectories[0].m_Filename);
        }

        if (data.output_buffer)
        {
            JsonWriteKeyName(&msg, "stdout");
            JsonWriteValueString(&msg, data.output_buffer);
        }

        JsonWriteEndObject(&msg);
        LogStructured(&msg);
    }

    // defer most of regular build failure output to the end of build, so that they are all
    // conveniently at the end of the log
    bool defer = failed && deferred_message_count < ARRAY_SIZE(deferred_messages);
    if (!defer)
    {
        PrintNodeResult(&data, queue);
    }
    else
    {
        // copy data needed for output that might be coming from temporary/local storage
        if (data.cmd_line != nullptr)
            data.cmd_line = StrDup(queue->m_Config.m_Heap, data.cmd_line);
        if (data.output_buffer != nullptr)
            data.output_buffer = StrDup(queue->m_Config.m_Heap, data.output_buffer);

        if (untouched_outputs != nullptr)
        {
            int n_outputs = node_data->m_OutputFiles.GetCount();
            bool* untouched_outputs_copy = (bool*)HeapAllocate(queue->m_Config.m_Heap, n_outputs * sizeof(bool));
            memcpy(untouched_outputs_copy, untouched_outputs, n_outputs * sizeof(bool));
            data.untouched_outputs = untouched_outputs_copy;
        }
        // store data needed for deferred output
        deferred_messages[deferred_message_count] = data;
        deferred_message_count++;
    }

    total_number_node_results_printed++;
    last_progress_message_of_any_job = TimerGet();
    last_progress_message_job = node_data;

    fflush(stdout);
}

void PrintDeferredMessages(BuildQueue *queue)
{
    for (int i = 0; i < deferred_message_count; ++i)
    {
        const NodeResultPrintData &data = deferred_messages[i];
        PrintNodeResult(&data, queue);
        if (data.cmd_line != nullptr)
            HeapFree(queue->m_Config.m_Heap, data.cmd_line);
        if (data.output_buffer != nullptr)
            HeapFree(queue->m_Config.m_Heap, data.output_buffer);
        if (data.untouched_outputs != nullptr)
            HeapFree(queue->m_Config.m_Heap, data.untouched_outputs);
    }
    fflush(stdout);
    deferred_message_count = 0;
}

int PrintNodeInProgress(const Frozen::DagNode *node_data, uint64_t time_of_start, const BuildQueue *queue, const char* message)
{
    uint64_t now = TimerGet();
    int seconds_job_has_been_running_for = TimerDiffSeconds(time_of_start, now);
    double seconds_since_last_progress_message_of_any_job = TimerDiffSeconds(last_progress_message_of_any_job, now);

    if (message == nullptr)
        message = node_data->m_Annotation.Get();

    int acceptable_time_since_last_message = last_progress_message_job == node_data ? 10 : (total_number_node_results_printed == 0 ? 0 : 5);
    int only_print_if_slower_than = seconds_since_last_progress_message_of_any_job > 30 ? 0 : 5;

    if (seconds_since_last_progress_message_of_any_job > acceptable_time_since_last_message && seconds_job_has_been_running_for > only_print_if_slower_than)
    {
        int maxDigits = ceil(log10(m_VisualMaxNodes + 1));

        EmitColor(YEL);
        printf("[BUSY %*ds] ", maxDigits * 2 - 1, seconds_job_has_been_running_for);
        EmitColor(RESET);
        printf("%s\n", message);
        last_progress_message_of_any_job = now;
        last_progress_message_job = node_data;

        fflush(stdout);
    }

    return 1;
}

