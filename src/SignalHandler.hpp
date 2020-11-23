#pragma once
#include "Config.hpp"


struct Mutex;
struct ConditionVariable;

// Return non-null if the processes has been signalled to quit.
const char *SignalGetReason(void);

// Call to explicitly mark the current process as signalled.  Useful to pick
// up child processes dying after being signalled and propagating that up to
// all build threads.
void SignalSet(const char *reason);

// Init the signal handler.
void SignalHandlerInit(void);

#if defined(TUNDRA_WIN32)
// Init the signal handler with a parent canary process to watch for sudden termination.
void SignalHandlerInitWithParentProcess(void *parent_handle);
#endif

// Specify a condition variable which will be broadcast when a signal has
// arrived.
void SignalHandlerSetCondition(ConditionVariable *variable);