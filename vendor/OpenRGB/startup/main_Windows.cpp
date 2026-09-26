/*---------------------------------------------------------*\
| main_Windows.cpp                                          |
|                                                           |
|   Entry point for the OpenRGB application on Windows      |
|   Differentiate between service and application startup   |
|   Housekeeping for Windows service management             |
|                                                           |
|   This file is part of the OpenRGB project                |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <stdexcept>
#include <vector>
#include <windows.h>
#include <tlhelp32.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <thread>

#include <QApplication>

#include "cli.h"
#include "startup.h"
#include "WindowsLaunchPolicy.h"
#include "WindowsLaunchLock.h"
#include "LogManager.h"
#include "NetworkServer.h"
#include "DetectionManager.h"
#include "ResourceManager.h"
#include "StringUtils.h"

using namespace std::chrono_literals;

static int common_main(int argc, char* argv[]);

static void WINAPI ServiceMain(DWORD dwArgc, LPTSTR *lpszArgv);
static void ReportServiceStatus(DWORD dwCurrentState, DWORD dwWin32ExitCode, DWORD dwWaitHint);

static char                  service_name[]             = "OpenRGB";
static SERVICE_TABLE_ENTRY   service_dispatch_table[]   = { { service_name, ServiceMain }, { NULL, NULL } };
static DWORD                 service_checkpoint         = 1;
static SERVICE_STATUS_HANDLE service_status_handle;
static SERVICE_STATUS        service_status;

static bool                  started_as_service;
static volatile bool         service_stop_requested;
static std::mutex            service_stop_mutex;
static std::condition_variable service_stop_cv;
static bool                  have_console;
static WindowsLaunchLock     launch_lock;

void ReleaseWindowsLaunchLock()
{
    launch_lock.Release();
}

static std::mutex            service_status_mutex;

/*---------------------------------------------------------*\
| Detection progress goes from 0 to 100 twice, passing an   |
| estimate based on that confuses the service manager       |
\*---------------------------------------------------------*/
static int                   detection_pass;
static unsigned int          lastpercent                = 101;

/*---------------------------------------------------------*\
| ServiceResourceManagerCallback                            |
|                                                           |
|   Report detection progress when running as a service     |
\*---------------------------------------------------------*/
static void ServiceResourceManagerCallback(void *, unsigned int update_reason)
{
    switch(update_reason)
    {
        case RESOURCEMANAGER_UPDATE_REASON_DETECTION_PROGRESS_CHANGED:
            {
                unsigned int percent = ResourceManager::get()->GetDetectionPercent();
                unsigned int estimate;

                percent = std::clamp(percent, 0u, 100u);

                if(lastpercent > percent)
                {
                    detection_pass += 1;
                }

                lastpercent = percent;

                switch(detection_pass)
                {
                    case 0:
                        percent = 0;
                        break;
                    case 1:
                        percent = percent * 4 / 5;
                        break;
                    case 2:
                        percent = percent / 5 + 80;
                        break;
                    default:
                        percent = 100;
                        break;
                }

                estimate = (100 - percent) / 5 + 10;

                ReportServiceStatus(SERVICE_START_PENDING, NO_ERROR, estimate * 1000);
            }
            break;
    }
}

/*---------------------------------------------------------*\
| ReportServiceStatus                                       |
|                                                           |
|   As of writing this (24H2) there are 7 possible service  |
|   states:                                                 |
|       SERVICE_START_PENDING, SERVICE_RUNNING,             |
|       SERVICE_STOP_PENDING, SERVICE_STOPPED,              |
|       SERVICE_PAUSE_PENDING, SERVICE_PAUSED,              |
|       SERVICE_CONTINUE_PENDING                            |
|                                                           |
|   We don't accept pause requests, so only start_pending   |
|   through stopped are possible.                           |
|                                                           |
|   Control requests can arrive asynchronously so we take   |
|   some precautions here not to report an out of order     |
|   status.                                                 |
\*---------------------------------------------------------*/
static void ReportServiceStatus(DWORD dwCurrentState, DWORD dwWin32ExitCode, DWORD dwWaitHint)
{
    static DWORD last_state = SERVICE_START_PENDING;

    LOG_TRACE("ReportServiceStatus(%lu, %lu, %lu), PID %lu Thread %lu", dwCurrentState, dwWin32ExitCode, dwWaitHint, GetCurrentProcessId(), GetCurrentThreadId());

    service_status_mutex.lock();

    switch(last_state)
    {
        case SERVICE_RUNNING:
            /*---------------------------------------------*\
            | Don't go back to SERVICE_START_PENDING        |
            \*---------------------------------------------*/
            if(dwCurrentState == SERVICE_START_PENDING)
            {
                service_status_mutex.unlock();
                return;
            }
            break;

        case SERVICE_STOP_PENDING:
            /*---------------------------------------------*\
            | Don't go back to starting or running          |
            \*---------------------------------------------*/
            if((dwCurrentState != SERVICE_STOP_PENDING) && (dwCurrentState != SERVICE_STOPPED))
            {
                service_status_mutex.unlock();
                return;
            }
            break;

        case SERVICE_STOPPED:
            /*---------------------------------------------*\
            | Don't call SetServiceStatus anymore after     |
            | SERVICE_STOPPED has been reported             |
            \*---------------------------------------------*/
            service_status_mutex.unlock();
            return;
    }

    switch(dwCurrentState)
    {
        case SERVICE_START_PENDING:
        case SERVICE_STOP_PENDING:
            service_status.dwControlsAccepted = 0;
            service_status.dwCheckPoint = service_checkpoint;
            service_checkpoint += 1;
            break;

        case SERVICE_RUNNING:
        case SERVICE_STOPPED:
            service_status.dwCheckPoint = 0;
            service_status.dwControlsAccepted =
                // SERVICE_ACCEPT_POWEREVENT   |
                SERVICE_ACCEPT_STOP         |
                SERVICE_ACCEPT_PRESHUTDOWN;
            break;

        default:
            /*---------------------------------------------*\
            | Shouldn't happen                              |
            \*---------------------------------------------*/
            service_status_mutex.unlock();
            return;
    }

    last_state                      = dwCurrentState;
    service_status.dwCurrentState   = dwCurrentState;
    service_status.dwWin32ExitCode  = dwWin32ExitCode;
    service_status.dwWaitHint       = dwWaitHint;

    SetServiceStatus(service_status_handle, &service_status);

    service_status_mutex.unlock();

    return;
}

/*---------------------------------------------------------*\
| ServiceControlHandler                                     |
|                                                           |
|   Handler function to register with                       |
|   RegisterServiceControlHandlerEx, to handle service      |
|   control operations (stop/shutdown).                     |
\*---------------------------------------------------------*/
static DWORD WINAPI ServiceControlHandler(DWORD dwControl, DWORD dwEventType, LPVOID lpEventData, LPVOID lpContext)
{
    DWORD retval = NO_ERROR;

    LOG_TRACE("ServiceControlHandler(%lu, %lu, %p, %p) called, PID %lu Thread %lu", dwControl, dwEventType, lpEventData, lpContext, GetCurrentProcessId(), GetCurrentThreadId());

    switch(dwControl)
    {
        /*-------------------------------------------------*\
        | TODO either this or                               |
        | RegisterSuspendResumeNotification somewhere       |
        \*-------------------------------------------------*/
        // case SERVICE_CONTROL_POWEREVENT:
        case SERVICE_CONTROL_INTERROGATE:
            /*---------------------------------------------*\
            | Should do nothing but return NO_ERROR         |
            \*---------------------------------------------*/
            break;

        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_PRESHUTDOWN:
            ReportServiceStatus(SERVICE_STOP_PENDING, NO_ERROR, 10000);
            service_stop_requested = true;
            service_stop_cv.notify_one();
            break;

        default:
            retval = ERROR_CALL_NOT_IMPLEMENTED;
            break;
    }

    return retval;
}

/*---------------------------------------------------------*\
| LogEvent                                                  |
|                                                           |
|   Log an event to the service log when running as a       |
|   service.  Only used for startup and shutdown events.    |
\*---------------------------------------------------------*/
static void LogEvent(const char *fmt, ...)
{
    va_list args1;
    va_list args2;

    /*-----------------------------------------------------*\
    | Handle variable arguments                             |
    \*-----------------------------------------------------*/
    va_start(args1, fmt);
    va_copy(args2, args1);

    /*-----------------------------------------------------*\
    | Determine string length and allocate buffer           |
    \*-----------------------------------------------------*/
    int     len = vsnprintf(NULL, 0, fmt, args1);
    char *  buf = (char *)malloc(len + 1);

    /*-----------------------------------------------------*\
    | Fill the string buffer                                |
    \*-----------------------------------------------------*/
    va_end(args1);
    vsnprintf(buf, len + 1, fmt, args2);
    va_end(args2);

    /*-----------------------------------------------------*\
    | If we somehow ended up here with a working console    |
    | then just print it                                    |
    \*-----------------------------------------------------*/
    if(have_console)
    {
        printf("%s\n", buf);
        free(buf);
        return;
    }

    /*-----------------------------------------------------*\
    | Otherwise, register event source with our service     |
    | name and report event to the event log                |
    \*-----------------------------------------------------*/
    HANDLE event_source = RegisterEventSource(NULL, service_name);

    if(event_source)
    {
        LPCSTR strings[2] = { service_name, buf };
        ReportEvent(
            event_source,
            EVENTLOG_ERROR_TYPE,
            0,
            1, // error #
            NULL,
            2,
            0,
            strings,
            NULL);
        DeregisterEventSource(event_source);
    }
    else
    {
        /*-------------------------------------------------*\
        | No console, no logmanager, no eventlog, no more   |
        | ideas. Sometimes it's OK to give up.              |
        \*-------------------------------------------------*/
    }

    /*-----------------------------------------------------*\
    | Free the buffer                                       |
    \*-----------------------------------------------------*/
    free(buf);
}

/*---------------------------------------------------------*\
| LogError                                                  |
|                                                           |
|   Log an error event when running as a service            |
\*---------------------------------------------------------*/
static void LogError(const char *fn, DWORD lasterror)
{
    std::string message = std::system_category().message(lasterror);
    LogEvent("%s failed with error code %lu \"%s\"", fn, lasterror, message.c_str());
}

/*---------------------------------------------------------*\
| ServiceMain                                               |
|                                                           |
|   Figures out whether we are started as a service, calls  |
|   common_main one way or another.                         |
\*---------------------------------------------------------*/
static void WINAPI ServiceMain(DWORD dwArgc, LPTSTR *lpszArgv)
{
    /*-----------------------------------------------------*\
    | Initialize service status                             |
    \*-----------------------------------------------------*/
    service_status.dwServiceType                = SERVICE_WIN32_OWN_PROCESS;
    service_status.dwServiceSpecificExitCode    = 0;

    started_as_service                          = true;
    service_status_handle                       = RegisterServiceCtrlHandlerEx(service_name, ServiceControlHandler, NULL);

    /*-----------------------------------------------------*\
    | Exit if service status handle is invalid              |
    \*-----------------------------------------------------*/
    if(service_status_handle == 0)
    {
        LogError("RegisterServiceCtrlHandlerEx", GetLastError());
        return;
    }

    /*-----------------------------------------------------*\
    | Report service status start pending                   |
    \*-----------------------------------------------------*/
    ReportServiceStatus(SERVICE_START_PENDING, NO_ERROR, 30000);

    /*-----------------------------------------------------*\
    | Perform common main processing                        |
    \*-----------------------------------------------------*/
    int exitval = common_main(dwArgc, lpszArgv);

    /*-----------------------------------------------------*\
    | Log if exit was unsuccessful                          |
    \*-----------------------------------------------------*/
    if(exitval != EXIT_SUCCESS)
    {
        LogEvent("OpenRGB finishing with exit code %d", exitval);
    }

    /*-----------------------------------------------------*\
    | Report service status stopped                         |
    \*-----------------------------------------------------*/
    ReportServiceStatus(SERVICE_STOPPED, NO_ERROR, 0);
}

/*---------------------------------------------------------*\
| main                                                      |
|                                                           |
|   Entry point, checks if started as a service and then    |
|   calls the common main processing                        |
\*---------------------------------------------------------*/
/*---------------------------------------------------------*\
| Single-instance guard helpers                             |
|                                                           |
| FindOtherOpenRGBInstances enumerates processes named      |
| OpenRGB.exe other than this one.                          |
| TerminateOtherInstances closes them, waiting up to 10s    |
| per process. Fails when an instance is elevated and this  |
| one is not.                                               |
\*---------------------------------------------------------*/
static bool FindOtherOpenRGBInstances(std::vector<DWORD>& pids)
{
    DWORD  current_pid = GetCurrentProcessId();
    HANDLE snapshot    = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);

    if(snapshot == INVALID_HANDLE_VALUE)
    {
        throw std::runtime_error("Cannot inspect running OpenRGB processes.");
    }

    PROCESSENTRY32 entry;
    entry.dwSize = sizeof(entry);

    if(Process32First(snapshot, &entry))
    {
        do
        {
            if(_stricmp(entry.szExeFile, "OpenRGB.exe") == 0
            && entry.th32ProcessID != current_pid)
            {
                pids.push_back(entry.th32ProcessID);
            }
        }
        while(Process32Next(snapshot, &entry));
    }

    CloseHandle(snapshot);

    return (pids.size() > 0);
}

static bool TerminateOtherInstances(const std::vector<DWORD>& pids)
{
    bool all_terminated = true;

    for(DWORD pid: pids)
    {
        HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);

        if(process == NULL)
        {
            all_terminated = false;
            continue;
        }

        if(!TerminateProcess(process, 0))
        {
            all_terminated = false;
        }
        else
        {
            if(WaitForSingleObject(process, 10000) != WAIT_OBJECT_0)
            {
                all_terminated = false;
            }
        }

        CloseHandle(process);
    }

    return all_terminated;
}

/*---------------------------------------------------------*\
|| IsSDKServerRunning                                        |
||                                                           |
||   Try to connect to 127.0.0.1:6742 to check whether the   |
||   running instance is already acting as an SDK server.    |
||   Used by the single-instance guard to decide whether a   |
||   second launch should become a GUI client instead of     |
||   prompting to kill the existing instance.                |
\*---------------------------------------------------------*/
static bool IsSDKServerRunning()
{
    WSADATA wsa;
    if(WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        return false;
    }

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if(s == INVALID_SOCKET)
    {
        WSACleanup();
        return false;
    }

    DWORD timeout = 500;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(6742);
    InetPtonA(AF_INET, "127.0.0.1", &addr.sin_addr);

    bool running = (connect(s, (sockaddr*)&addr, sizeof(addr)) == 0);

    closesocket(s);
    WSACleanup();

    return running;
}

/*---------------------------------------------------------*\
|| HasServerFlag                                             |
||                                                           |
||   Check argv for --server to distinguish a server launch  |
||   from a GUI client launch.                               |
\*---------------------------------------------------------*/
static bool HasServerFlag(int argc, char* argv[])
{
    for(int i = 1; i < argc; i++)
    {
        if(_stricmp(argv[i], "--server") == 0
        || _stricmp(argv[i], "--server-port") == 0
        || _stricmp(argv[i], "--server-host") == 0)
        {
            return true;
        }
    }
    return false;
}

// Include hidden Qt windows: a GUI minimized to the tray still counts.
struct OpenRGBWindowSearch
{
    const std::vector<DWORD>& pids;
    bool found = false;
};

static BOOL CALLBACK FindOpenRGBWindow(HWND window, LPARAM param)
{
    auto& search = *reinterpret_cast<OpenRGBWindowSearch*>(param);
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if(std::find(search.pids.begin(), search.pids.end(), pid) != search.pids.end())
    {
        char class_name[256] = {};
        GetClassNameA(window, class_name, sizeof(class_name));
        if(strncmp(class_name, "Qt", 2) == 0 && GetWindowTextLengthA(window) > 0)
        {
            search.found = true;
            return FALSE;
        }
    }
    return TRUE;
}

static bool IsGuiLaunch(int argc, char* argv[])
{
    bool config_only = true;
    for(int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if(arg == "--gui" || arg == "--startminimized" || arg == "--i2c-tools")
            return true;
        if(arg == "--config" || arg == "--loglevel")
        {
            ++i;
            continue;
        }
        if(arg != "--localconfig" && arg != "--noautoconnect" && arg != "--nodetect"
        && arg != "--verbose" && arg != "-v" && arg != "--very-verbose" && arg != "-vv"
        && arg != "--yolo" && arg != "--print-source")
            config_only = false;
    }
    return config_only;
}

int main(int argc, char* argv[])
{
    started_as_service = false;

    /*-----------------------------------------------------*\
    | This will call ServiceMain() if we are started as a   |
    | service                                               |
    \*-----------------------------------------------------*/
    bool sscd = StartServiceCtrlDispatcher(service_dispatch_table);

    /*-----------------------------------------------------*\
    | ... returns a specific error code otherwise           |
    \*-----------------------------------------------------*/
    DWORD lasterror = GetLastError();

    if(sscd)
    {
        if(started_as_service)
        {
            /*---------------------------------------------*\
            | StartServiceCtrlDispatcher reported success,  |
            | ServiceMain was reached. Don't log anything,  |
            | it's the normal exit path when a service is   |
            | stopped.                                      |
            \*---------------------------------------------*/
            return EXIT_SUCCESS;
        }
        else
        {
            /*---------------------------------------------*\
            | Should not happen                             |
            \*---------------------------------------------*/
            LogEvent("StartServiceCtrlDispatcher returned true, but ServiceMain has not been reached.");
            return EXIT_FAILURE;
        }
    }

    /*-----------------------------------------------------*\
    | StartServiceCtrlDispatcher reported failure.          |
    | First, check that the error isn't the one returned    |
    | when it's called from an app                          |
    \*-----------------------------------------------------*/
    if(lasterror != ERROR_FAILED_SERVICE_CONTROLLER_CONNECT)
    {
        LogError("StartServiceCtrlDispatcher", lasterror);
        return EXIT_FAILURE;
    }

    /*-----------------------------------------------------*\
    | Now we are quite sure we are not started as a service |
    \*-----------------------------------------------------*/
    if(started_as_service)
    {
        /*-------------------------------------------------*\
        | ... so this should not happen                     |
        \*-------------------------------------------------*/
        LogEvent("StartServiceCtrlDispatcher returned false, but ServiceMain has been reached.");
        return EXIT_FAILURE;
    }

    /*-----------------------------------------------------*\
    | Code that needs to be run at startup when we are      |
    | started as an app and not as a service goes here.     |
    \*-----------------------------------------------------*/

    /*-----------------------------------------------------*\
    | Attach console output                                 |
    \*-----------------------------------------------------*/
    if(AttachConsole(ATTACH_PARENT_PROCESS))
    {
        /*-------------------------------------------------*\
        | We are running under some terminal context;       |
        | otherwise leave the GUI and CRT alone             |
        \*-------------------------------------------------*/
        freopen("CONIN$",  "r", stdin);
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
        have_console = true;
    }

    /*-----------------------------------------------------*\
    | Single-instance guard: if another OpenRGB.exe is     |
    | running, offer to close it or exit this copy. This    |
    | prevents two instances fighting over the same        |
    | devices and SDK port.                                 |
    \*-----------------------------------------------------*/
    // Hold through initialization and window creation, then release in startup().
    DWORD lock_result = launch_lock.Acquire();
    if(lock_result == WAIT_FAILED)
    {
        MessageBoxA(NULL, "Cannot acquire the OpenRGB startup lock. Try running as administrator.",
                    "OpenRGB", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
        return EXIT_FAILURE;
    }
    if(lock_result != WAIT_OBJECT_0 && lock_result != WAIT_ABANDONED)
    {
        return lock_result == WAIT_TIMEOUT ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    struct LaunchLockCleanup
    {
        ~LaunchLockCleanup() { ReleaseWindowsLaunchLock(); }
    } cleanup;

    std::vector<DWORD> other_pids;
    try
    {
        FindOtherOpenRGBInstances(other_pids);
    }
    catch(const std::exception& error)
    {
        MessageBoxA(NULL, error.what(), "OpenRGB", MB_OK | MB_ICONWARNING);
        return EXIT_FAILURE;
    }
    const bool gui_requested = IsGuiLaunch(argc, argv);
    const bool server_running = IsSDKServerRunning();
    OpenRGBWindowSearch window_search{other_pids};
    EnumWindows(FindOpenRGBWindow, reinterpret_cast<LPARAM>(&window_search));
    // Treat multiple older processes as a pair, even across desktop sessions.
    const bool gui_running = window_search.found || other_pids.size() > 1;
    const auto action = GetWindowsLaunchAction(!other_pids.empty(), server_running,
                                               gui_running, gui_requested);
    if(action == WindowsLaunchAction::Restart)
    {
        if(other_pids.empty())
        {
            MessageBoxA(NULL, "The SDK port is occupied by an unidentified process.\n"
                             "Close it before starting OpenRGB.",
                        "OpenRGB", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
            return EXIT_FAILURE;
        }
        const char* prompt = gui_requested
            ? "OpenRGB is already running. Start a fresh server and GUI?\n\n"
              "Yes - close the existing OpenRGB processes and start fresh\n"
              "No - keep the existing session"
            : "OpenRGB is already running. Restart it?\n\n"
              "Yes - close the existing OpenRGB processes and start fresh\n"
              "No - keep the existing session";
        if(MessageBoxA(NULL, prompt, "OpenRGB",
                       MB_YESNO | MB_DEFBUTTON2 | MB_ICONQUESTION | MB_SETFOREGROUND | MB_TOPMOST) != IDYES)
            return EXIT_SUCCESS;
        if(!TerminateOtherInstances(other_pids))
        {
            MessageBoxA(NULL, "The existing OpenRGB processes could not be closed.\n"
                             "Close them manually or run this copy as administrator.",
                        "OpenRGB", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
            return EXIT_FAILURE;
        }
        if(IsSDKServerRunning())
        {
            MessageBoxA(NULL, "The SDK port is still in use. OpenRGB will not start another server.",
                        "OpenRGB", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
            return EXIT_FAILURE;
        }
    }

    std::vector<std::string> args;
    args.emplace_back(argv[0]);
    for(int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if(gui_requested && (arg == "--client"
            || (action == WindowsLaunchAction::ConnectGui
                && (arg == "--server-port" || arg == "--server-host"))))
        {
            ++i;
            continue;
        }
        if(gui_requested && (arg == "--noautoconnect"
            || (action == WindowsLaunchAction::ConnectGui && arg == "--server")))
            continue;
        args.push_back(arg);
    }
    if(gui_requested)
    {
        args.emplace_back("--gui");
        args.emplace_back("--noautoconnect");
        if(action == WindowsLaunchAction::ConnectGui)
        {
            args.emplace_back("--nodetect");
            args.emplace_back("--client");
            args.emplace_back("127.0.0.1:6742");
        }
        else if(!HasServerFlag(argc, argv))
            args.emplace_back("--server");
    }
    std::vector<char*> launch_argv;
    for(auto& arg : args)
        launch_argv.push_back(&arg[0]);
    launch_argv.push_back(nullptr);
    return common_main(static_cast<int>(args.size()), launch_argv.data());
}

/*---------------------------------------------------------*\
| InitializeTimerResolutionThreadFunction (Win32)           |
|                                                           |
|   On Windows, the default timer resolution is 15.6ms.     |
|   For higher accuracy delays, the timer resolution should |
|   be set to a shorter interval.  The shortest interval    |
|   that can be set is 0.5ms.                               |
\*---------------------------------------------------------*/
void InitializeTimerResolutionThreadFunction()
{
    /*-----------------------------------------------------*\
    | NtSetTimerResolution function pointer type            |
    \*-----------------------------------------------------*/
    typedef unsigned int NTSTATUS;
    typedef NTSTATUS (*NTSETTIMERRESOLUTION)(ULONG DesiredResolution, BOOLEAN SetResolution, PULONG CurrentResolution);

    /*-----------------------------------------------------*\
    | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION = 4, |
    | isn't defined in Win10 headers                        |
    \*-----------------------------------------------------*/
    PROCESS_POWER_THROTTLING_STATE PowerThrottlingState { PROCESS_POWER_THROTTLING_CURRENT_VERSION, 4, 0 };

    ULONG                CurrentResolution;
    HMODULE              NtDllHandle;
    NTSETTIMERRESOLUTION NtSetTimerResolution;

    /*-----------------------------------------------------*\
    | Load ntdll.dll and get pointer to NtSetTimerResolution|
    \*-----------------------------------------------------*/
    NtDllHandle             = LoadLibrary("ntdll.dll");
    NtSetTimerResolution    = (NTSETTIMERRESOLUTION)GetProcAddress(NtDllHandle, "NtSetTimerResolution");

    /*-----------------------------------------------------*\
    | Windows 11 requires                                   |
    | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION      |
    \*-----------------------------------------------------*/
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &PowerThrottlingState, sizeof(PowerThrottlingState));

    /*-----------------------------------------------------*\
    | Call NtSetTimerResolution to set timer resolution to  |
    | 0.5ms every 500ms                                     |
    \*-----------------------------------------------------*/
    while(1)
    {
        NtSetTimerResolution(5000, TRUE, &CurrentResolution);

        std::this_thread::sleep_for(500ms);
    }
}

/*---------------------------------------------------------*\
| WaitWhileServerOnline                                     |
|                                                           |
|   Wait while NetworkServer is online and return only when |
|   it has shut down                                        |
\*---------------------------------------------------------*/
static void WaitWhileServerOnline(NetworkServer* srv)
{
    std::unique_lock<std::mutex> lock(service_stop_mutex);
    while(srv->GetOnline())
    {
        if(service_stop_requested)
        {
            srv->StopServer();
            break;
        }
        service_stop_cv.wait_for(lock, 1s);
    };
}

/*---------------------------------------------------------*\
| common_main                                               |
|                                                           |
|   Common entry functionality after determining whether we |
|   were started as a service or not.                       |
\*---------------------------------------------------------*/
static int common_main(int argc, char* argv[])
{
    unsigned int ret_flags;

    if(started_as_service)
    {
        /*-------------------------------------------------*\
        | Passing command line arguments to a service is    |
        | difficult, can cause all kinds of trouble and     |
        | doesn't have a way to warn about them             |
        \*-------------------------------------------------*/
        ret_flags = RET_FLAG_START_SERVER | RET_FLAG_NO_AUTO_CONNECT;

        /*-------------------------------------------------*\
        | Get the path to the executable and create a       |
        | directory called service_config there, use this   |
        | as the service's configuration directory          |
        \*-------------------------------------------------*/
        WCHAR exe_path_wchar[MAX_PATH];
        GetModuleFileNameW(NULL, exe_path_wchar, MAX_PATH);

        std::string exe_path_string = StringUtils::wstring_to_string(std::wstring(exe_path_wchar));

        filesystem::path exe_path(exe_path_string);
        filesystem::path config_path = exe_path.remove_filename() / filesystem::u8path("service_config");

        filesystem::create_directories(config_path);

        ResourceManager::get()->SetConfigurationDirectory(config_path);
    }
    else
    {
        /*-------------------------------------------------*\
        | Perform CLI pre-detection processing to get       |
        | return flags                                      |
        \*-------------------------------------------------*/
        ret_flags = cli_pre_detection(argc, argv);
    }

    /*-----------------------------------------------------*\
    | Start timer resolution correction thread              |
    \*-----------------------------------------------------*/
    std::thread * InitializeTimerResolutionThread;
    InitializeTimerResolutionThread = new std::thread(InitializeTimerResolutionThreadFunction);
    InitializeTimerResolutionThread->detach();

    /*-----------------------------------------------------*\
    | Initialize ResourceManager                            |
    \*-----------------------------------------------------*/
    ResourceManager::get()->Initialize(
        !(ret_flags & RET_FLAG_NO_AUTO_CONNECT),
        !(ret_flags & RET_FLAG_NO_DETECT),
        ret_flags & RET_FLAG_START_SERVER,
        ret_flags & RET_FLAG_CLI_POST_DETECTION,
        ret_flags & RET_FLAG_START_GUI);

    /*-----------------------------------------------------*\
    | If running as a service, register the service startup |
    | progress callback with ResourceManager to report      |
    | startup progress based on detection progress.         |
    \*-----------------------------------------------------*/
    if(started_as_service)
    {
        ResourceManager::get()->RegisterResourceManagerCallback(ServiceResourceManagerCallback, NULL);
    }

    /*-----------------------------------------------------*\
    | Perform application startup and run the application.  |
    | This call returns only when the GUI application is    |
    | closing or if not running the GUI.                    |
    \*-----------------------------------------------------*/
    int exitval = startup(argc, argv, ret_flags);

    /*-----------------------------------------------------*\
    | If running as a service, unregister the service       |
    | startup progress callback when shutting down.         |
    \*-----------------------------------------------------*/
    if(started_as_service)
    {
        ResourceManager::get()->UnregisterResourceManagerCallback(ServiceResourceManagerCallback, NULL);
    }

    /*-----------------------------------------------------*\
    | If started in headless server mode, wait until server |
    | shuts down before closing application.                |
    \*-----------------------------------------------------*/
    if((ret_flags & RET_FLAG_START_SERVER) && !(ret_flags & RET_FLAG_START_GUI))
    {
        NetworkServer* server = ResourceManager::get()->GetServer();
        if(server && server->GetOnline())
        {
            if(started_as_service)
            {
                ReportServiceStatus(SERVICE_RUNNING, NO_ERROR, 0);
            }
            WaitWhileServerOnline(server);
        }
    }

    /*-----------------------------------------------------*\
    | Call ServiceShutdown to allow operations before       |
    | controllers are closed and deleted. Only runs when    |
    | running as a background service (headless server).    |
    \*-----------------------------------------------------*/
    ResourceManager::get()->ServiceShutdown();

    /*-----------------------------------------------------*\
    | Clean up detected devices so destructors can run.     |
    \*-----------------------------------------------------*/
    DetectionManager::get()->Cleanup();

    LOG_TRACE("OpenRGB finishing with exit code %d", exitval);

    return exitval;
}
