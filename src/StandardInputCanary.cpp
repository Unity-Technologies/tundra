#include "StandardInputCanary.hpp"
#include "Thread.hpp"
#include "SignalHandler.hpp"

#include <stdio.h>

static ThreadRoutineReturnType TUNDRA_STDCALL WaitForStdinToClose(void *param)
{
    while (fgetc(stdin) != EOF)
        ;
    SignalSet("stdin closed");
    return 0;
}

void StandardInputCanary::Initialize()
{
    ThreadStart(WaitForStdinToClose, nullptr, "Canary (stdin)");
}