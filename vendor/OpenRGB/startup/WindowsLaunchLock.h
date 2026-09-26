#pragma once
#include <windows.h>

// Kept until the server is initialized and any GUI window has been created.
class WindowsLaunchLock
{
public:
    DWORD Acquire(const char* name = "Global\\OpenRGB.Startup")
    {
        handle = CreateMutexA(NULL, FALSE, name);
        if(!handle)
            return WAIT_FAILED;
        const DWORD result = WaitForSingleObject(handle, 0);
        owned = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
        if(!owned)
            Release();
        return result;
    }

    void Release()
    {
        if(handle)
        {
            if(owned)
                ReleaseMutex(handle);
            CloseHandle(handle);
            handle = NULL;
            owned = false;
        }
    }

    ~WindowsLaunchLock() { Release(); }
    WindowsLaunchLock() = default;
    WindowsLaunchLock(const WindowsLaunchLock&) = delete;
    WindowsLaunchLock& operator=(const WindowsLaunchLock&) = delete;

private:
    HANDLE handle = NULL;
    bool owned = false;
};
