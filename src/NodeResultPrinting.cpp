#include "NodeResultPrinting.hpp"
#include "DagData.hpp"
#include "BuildQueue.hpp"
#include "Exec.hpp"
#include "JsonWriter.hpp"
#include <stdio.h>
#include <sstream>
#include <ctime>
#include <math.h>
#if TUNDRA_UNIX
#include <unistd.h>
#include <stdarg.h>
#endif


namespace t2
{

struct NodeResultPrintData
{
  const NodeData* node_data;
  const char* cmd_line;
  bool verbose;
  int duration;
  ValidationResult validation_result;
  const bool* untouched_outputs;
  const char* output_buffer;
  int processed_node_count;
  MessageStatusLevel::Enum status_level;
  int return_code;
  bool was_signalled;
  bool was_aborted;
};


static bool EmitColors = false;

static uint64_t last_progress_message_of_any_job;
static const NodeData* last_progress_message_job = nullptr;
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

static char* DetectEscapeCode(char* ptr)
{
    if (!IsEscapeCode(ptr[0]))
        return ptr;
    if (ptr[1] == 0)
        return ptr;

    //there are other characters valid than [ here, but for now we'll only support stripping ones that have [, as all color sequences have that.
    if (ptr[1] != '[')
        return ptr;

    char* endOfCode = ptr+2;

    while(true) {
        char c = *endOfCode;
        if (c == 0)
            return ptr;
        if (isTerminatingChar(c))
            return endOfCode+1;
        endOfCode++;
    }
}

void StripAnsiColors(char* buffer)
{
   char* writeCursor = buffer;
   char* readCursor = buffer;
   while(*readCursor)
   {
       char* adjusted = DetectEscapeCode(readCursor);
       if (adjusted != readCursor)
       {
           readCursor = adjusted;
           continue;
       }
       *writeCursor++ = *readCursor++;
   }
    *writeCursor++ = 0;
}

void InitNodeResultPrinting()
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

    char* value = getenv("DOWNSTREAM_STDOUT_CONSUMER_SUPPORTS_COLOR");
    if (value == nullptr)
        return;

    if (*value == '1')
      EmitColors = true;
    if (*value == '0')
      EmitColors = false;
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

static void EmitColor(const char* colorsequence)
{
  if (EmitColors)
  {
    EnsureConsoleCanHandleColors();
    printf("%s", colorsequence);
  }
}

#define RED   "\x1B[91m"
#define GRN   "\x1B[32m"
#define YEL   "\x1B[33m"
#define BLU   "\x1B[34m"
#define MAG   "\x1B[35m"
#define CYN   "\x1B[36m"
#define GRAY   "\x0B[37m"
#define WHT   "\x1B[37m"
#define RESET "\x1B[0m"

static void PrintDiagnosticPrefix(const char* title, const char* color = YEL)
{
    EmitColor(color);
    printf("##### %s\n",title);
    EmitColor(RESET);
}

static void PrintDiagnosticFormat(const char* title, const char* formatString, ...)
{
    PrintDiagnosticPrefix(title);
    va_list args;
    va_start(args, formatString);
    vfprintf(stdout, formatString, args);
    va_end(args);
    printf("\n");
}

static void PrintDiagnostic(const char* title, const char* contents)
{
    if (contents != nullptr)
        PrintDiagnosticFormat(title, "%s", contents);
}

static void PrintDiagnostic(const char* title, int content)
{
    PrintDiagnosticFormat(title, "%d", content);
}

static void EmitColorForLevel(MessageStatusLevel::Enum status_level)
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

void PrintServiceMessage(MessageStatusLevel::Enum status_level, const char* formatString, ...)
{
    EmitColorForLevel(status_level);
    va_list args;
    va_start(args, formatString);
    vfprintf(stdout, formatString, args);
    va_end(args);
    EmitColor(RESET);
    printf("\n");
}

static void TrimOutputBuffer(OutputBufferData* buffer)
{
  auto isNewLine = [](char c) {return c == 0x0A || c == 0x0D; };

  int trimmedCursor = buffer->cursor;
  while (isNewLine(*(buffer->buffer + trimmedCursor -1)) && trimmedCursor > 0)
    trimmedCursor--;

  buffer->buffer[trimmedCursor] = 0;
  if (!EmitColors)
  {
    StripAnsiColors(buffer->buffer);
  }
}

static void PrintLineWithDurationAndAnnotation(int duration, const char* progressStr, MessageStatusLevel::Enum status_level, const char* annotation)
{
  EmitColorForLevel(status_level);

  printf("[");
  if (status_level == MessageStatusLevel::Failure && !EmitColors)
    printf("!FAILED! ");
  printf("%s ", progressStr);
  printf("%2ds] ", duration);
  // for failures, color the whole line red and only reset at the end
  if (status_level != MessageStatusLevel::Failure)
    EmitColor(RESET);
  printf("%s\n", annotation);
  if (status_level == MessageStatusLevel::Failure)
    EmitColor(RESET);
}

static void PrintLineWithDurationAndAnnotation(int duration, int nodeCount, int max_nodes, MessageStatusLevel::Enum status_level, const char* annotation)
{
    int maxDigits = ceil(log10(max_nodes+1));
    char* progressStr = (char*)alloca(maxDigits * 2 + 2);
    snprintf(progressStr, maxDigits * 2 + 2, "%*d/%d", maxDigits, nodeCount, max_nodes);
    PrintLineWithDurationAndAnnotation(duration, progressStr, status_level, annotation);
}

void PrintNonNodeActionResult(int duration, int max_nodes, MessageStatusLevel::Enum status_level, const char* annotation, ExecResult* result)
{
  int maxDigits = ceil(log10(max_nodes + 1));
  char* progressStr = (char*)alloca(maxDigits * 2 + 2);
  memset(progressStr, ' ', maxDigits * 2 + 1);
  progressStr[maxDigits * 2 + 1] = 0;

  PrintLineWithDurationAndAnnotation(duration, progressStr, status_level, annotation);
  if (result != nullptr && result->m_ReturnCode != 0)
  {
    TrimOutputBuffer(&result->m_OutputBuffer);
    printf("%s\n", result->m_OutputBuffer.buffer);
  }
}

static void PrintNodeResult(const NodeResultPrintData* data, BuildQueue* queue)
{
    PrintLineWithDurationAndAnnotation(data->duration, data->processed_node_count, queue->m_Config.m_MaxNodes, data->status_level, data->node_data->m_Annotation.Get());

    if (data->verbose)
    {
        PrintDiagnostic("CommandLine", data->cmd_line);
        for (int i=0; i!= data->node_data->m_FrontendResponseFiles.GetCount(); i++)
        {
            char titleBuffer[1024];
            const char* file = data->node_data->m_FrontendResponseFiles[i].m_Filename;
            snprintf(titleBuffer, sizeof titleBuffer, "Contents of %s", file);

            char* content_buffer;
            FILE* f = fopen(file, "rb");
            if (!f)
            {

              int buffersize = 512;
              content_buffer = (char*)HeapAllocate(queue->m_Config.m_Heap, buffersize);
              snprintf(content_buffer, buffersize, "couldn't open %s for reading", file);
            } else {
              fseek(f, 0L, SEEK_END);
              size_t size = ftell(f);
              rewind(f);
              size_t buffer_size = size + 1;
              content_buffer = (char*)HeapAllocate(queue->m_Config.m_Heap, buffer_size);
              
              size_t read = fread(content_buffer, 1, size, f);
              content_buffer[read] = '\0';
              fclose(f);
            }
            PrintDiagnostic(titleBuffer, content_buffer);
            HeapFree(queue->m_Config.m_Heap, content_buffer);
        }


        if (data->node_data->m_EnvVars.GetCount() > 0)
          PrintDiagnosticPrefix("Custom Environment Variables");
        for (int i=0; i!= data->node_data->m_EnvVars.GetCount(); i++)
        {
           auto& entry = data->node_data->m_EnvVars[i];
           printf("%s=%s\n", entry.m_Name.Get(), entry.m_Value.Get() );
        }
        if (data->return_code == 0 && !data->was_signalled)
        {
          if (data->validation_result == ValidationResult::UnexpectedConsoleOutputFail)
          {
            PrintDiagnosticPrefix("Failed because this command wrote something to the output that wasn't expected. We were expecting any of the following strings:", RED);
            int count = data->node_data->m_AllowedOutputSubstrings.GetCount();
            for (int i = 0; i != count; i++)
              printf("%s\n", (const char*)data->node_data->m_AllowedOutputSubstrings[i]);
            if (count == 0)
              printf("<< no allowed strings >>\n");
          }
          else if (data->validation_result == ValidationResult::UnwrittenOutputFileFail)
          {
            PrintDiagnosticPrefix("Failed because this command failed to write the following output files:", RED);
            for (int i = 0; i < data->node_data->m_OutputFiles.GetCount(); i++)
              if (data->untouched_outputs[i])
                printf("%s\n", (const char*)data->node_data->m_OutputFiles[i].m_Filename);
          }
        }
        if (data->was_signalled)
          PrintDiagnostic("Was Signaled", "Yes");
        if (data->was_aborted)
          PrintDiagnostic("Was Aborted", "Yes");
        if (data->return_code !=0)
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

inline char* StrDupN(MemAllocHeap* allocator, const char* str, size_t len)
{
  size_t sz = len + 1;
  char* buffer = static_cast<char*>(HeapAllocate(allocator, sz));
  memcpy(buffer, str, sz - 1);
  buffer[sz - 1] = '\0';
  return buffer;
}

inline char* StrDup(MemAllocHeap* allocator, const char* str)
{
  return StrDupN(allocator, str, strlen(str));
}


void PrintNodeResult(
  ExecResult* result,
  const NodeData* node_data,
  const char* cmd_line,
  BuildQueue* queue,
  ThreadState* thread_state,
  bool always_verbose,
  uint64_t time_exec_started,
  ValidationResult validationResult,
  const bool* untouched_outputs
  )
{
  int processedNodeCount = ++queue->m_ProcessedNodeCount;
  bool failed = result->m_ReturnCode != 0 || result->m_WasSignalled || validationResult >= ValidationResult::UnexpectedConsoleOutputFail;
  bool verbose = (failed && !result->m_WasAborted) || always_verbose;

  int duration = TimerDiffSeconds(time_exec_started, TimerGet());

  NodeResultPrintData data = {};
  data.node_data = node_data;
  data.cmd_line = cmd_line;
  data.verbose = verbose;
  data.duration = duration;
  data.validation_result = validationResult;
  data.untouched_outputs = untouched_outputs;
  data.processed_node_count = processedNodeCount;
  data.status_level = failed ? MessageStatusLevel::Failure : MessageStatusLevel::Success;
  data.return_code = result->m_ReturnCode;
  data.was_signalled = result->m_WasSignalled;
  data.was_aborted = result->m_WasAborted;

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

    JsonWriteKeyName(&msg, "annotation");
    JsonWriteValueString(&msg, node_data->m_Annotation);

    JsonWriteKeyName(&msg, "index");
    JsonWriteValueInteger(&msg, node_data->m_OriginalIndex);

    JsonWriteKeyName(&msg, "exitcode");
    JsonWriteValueInteger(&msg, result->m_ReturnCode);

    if (failed && data.output_buffer)
    {
      JsonWriteKeyName(&msg, "stdout");
      JsonWriteValueString(&msg, data.output_buffer);
    }

    JsonWriteEndObject(&msg);
    LogStructured(&msg);
  }

  // defer most of regular build failure output to the end of build, so that they are all
  // conveniently at the end of the log
  bool defer = failed && (0 == (queue->m_Config.m_Flags & BuildQueueConfig::kFlagContinueOnError)) && deferred_message_count < ARRAY_SIZE(deferred_messages);
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
    int n_outputs = node_data->m_OutputFiles.GetCount();
    bool* untouched_outputs_copy = (bool*)HeapAllocate(queue->m_Config.m_Heap, n_outputs * sizeof(bool));
    memcpy(untouched_outputs_copy, untouched_outputs, n_outputs * sizeof(bool));
    data.untouched_outputs = untouched_outputs_copy;

    // store data needed for deferred output
    deferred_messages[deferred_message_count] = data;
    deferred_message_count++;
  }

  total_number_node_results_printed++;
  last_progress_message_of_any_job = TimerGet();
  last_progress_message_job = node_data;

  fflush(stdout);
}

void PrintDeferredMessages(BuildQueue* queue)
{
  for (int i = 0; i < deferred_message_count; ++i)
  {
    const NodeResultPrintData& data = deferred_messages[i];
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



int PrintNodeInProgress(const NodeData* node_data, uint64_t time_of_start, const BuildQueue* queue)
{
  uint64_t now = TimerGet();
  int seconds_job_has_been_running_for = TimerDiffSeconds(time_of_start, now);
  double seconds_since_last_progress_message_of_any_job = TimerDiffSeconds(last_progress_message_of_any_job, now);

  int acceptable_time_since_last_message = last_progress_message_job == node_data ? 10 : (total_number_node_results_printed == 0 ? 0 : 5) ;
  int only_print_if_slower_than = seconds_since_last_progress_message_of_any_job > 30 ? 0 : 5;

  if (seconds_since_last_progress_message_of_any_job > acceptable_time_since_last_message && seconds_job_has_been_running_for > only_print_if_slower_than)
  {
    int maxDigits = ceil(log10(queue->m_Config.m_MaxNodes+1));

    EmitColor(YEL);
    printf("[BUSY %*ds] ", maxDigits*2-1, seconds_job_has_been_running_for);
    EmitColor(RESET);
    printf("%s\n", (const char*)node_data->m_Annotation);
    last_progress_message_of_any_job = now;
    last_progress_message_job = node_data;

    fflush(stdout);
  }

  return 1;
}
}
