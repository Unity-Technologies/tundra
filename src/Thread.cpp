#include "Thread.hpp"

#if defined(TUNDRA_UNIX)
#include <pthread.h>
#endif

#if defined(TUNDRA_WIN32)
#include <windows.h>
#include <process.h>
#endif



ThreadId ThreadCurrent()
{
#if defined(TUNDRA_UNIX)
    static_assert(sizeof(pthread_t) <= sizeof(ThreadId), "pthread_t too big");
    return (ThreadId)pthread_self();
#elif defined(TUNDRA_WIN32)
    return ::GetCurrentThreadId();
#endif
}

void ThreadSetName(ThreadId threadId, const char* name)
{
#if TUNDRA_WIN32

    typedef HRESULT(*PSetThreadDescriptionFn)(HANDLE, PCWSTR);

    HANDLE thread = (HANDLE)threadId;

    HMODULE kernel32 = GetModuleHandle("kernel32.dll");
    PSetThreadDescriptionFn pfnSetThreadDescription = kernel32 != nullptr ? reinterpret_cast<PSetThreadDescriptionFn>(GetProcAddress(kernel32, "SetThreadDescription")) : nullptr;
    if (pfnSetThreadDescription != nullptr)
    {
        int wchars_num = MultiByteToWideChar(CP_UTF8, 0, name, -1, NULL, 0);
        wchar_t* wstr = new wchar_t[wchars_num];
        MultiByteToWideChar(CP_UTF8, 0, name, -1, wstr, wchars_num);

        pfnSetThreadDescription(thread, wstr);

        delete[] wstr;
    }

    else if(IsDebuggerPresent())
    {
        const DWORD MS_VC_EXCEPTION = 0x406D1388;
#pragma pack(push,8)
        typedef struct tagTHREADNAME_INFO
        {
            DWORD dwType; // Must be 0x1000.
            LPCSTR szName; // Pointer to name (in user addr space).
            DWORD dwThreadID; // Thread ID (-1=caller thread).
            DWORD dwFlags; // Reserved for future use, must be zero.
        } THREADNAME_INFO;
#pragma pack(pop)

        THREADNAME_INFO info;
        info.dwType = 0x1000;
        info.szName = name;        
        info.dwThreadID = GetThreadId(thread);
        info.dwFlags = 0;

#pragma warning(push)
#pragma warning(disable: 6320 6322)
        __try {
            RaiseException(MS_VC_EXCEPTION, 0, sizeof(info) / sizeof(ULONG_PTR), (ULONG_PTR*)&info);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
        }
#pragma warning(pop)

    }
#endif
}

ThreadId ThreadStart(ThreadRoutine routine, void *param, const char* name)
{
#if defined(TUNDRA_UNIX)
    pthread_t thread;
    if (0 != pthread_create(&thread, nullptr, routine, param))
        CroakErrno("pthread_create() failed");
    return (ThreadId)thread;
#else
    uintptr_t handle = _beginthreadex(NULL, 0, routine, param, 0, NULL);
    if (!handle)
        CroakErrno("_beginthreadex() failed");


    ThreadSetName(handle, name);

    return handle;
#endif
}

void ThreadJoin(ThreadId thread_id)
{
#if defined(TUNDRA_UNIX)
    void *result;
    if (0 != pthread_join((pthread_t)thread_id, &result))
        CroakErrno("pthread_join() failed");
    return;
#else
    while (WAIT_OBJECT_0 != WaitForSingleObject((HANDLE)thread_id, INFINITE))
    {
        // nop
    }
    CloseHandle((HANDLE)thread_id);
#endif
}


