#pragma once

#include "Common.hpp"

typedef uintptr_t ThreadId;

#if defined(TUNDRA_WIN32)
typedef unsigned int ThreadRoutineReturnType;
#else
typedef void *ThreadRoutineReturnType;
#endif

typedef ThreadRoutineReturnType(TUNDRA_STDCALL *ThreadRoutine)(void *);

ThreadId ThreadStart(ThreadRoutine routine, void *param, const char* name);

void ThreadJoin(ThreadId thread_id);

void ThreadSetName(ThreadId, const char* name);

ThreadId ThreadCurrent();
