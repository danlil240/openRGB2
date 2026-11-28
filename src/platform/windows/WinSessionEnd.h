/*---------------------------------------------------------*\
||| WinSessionEnd.h                                           ||
|||                                                           ||
|||   OS shutdown / logoff / console-close notice for the  ||
|||   engine process (audit E-10). Without it Windows ends ||
|||   the engine with no teardown: the power monitor's     ||
|||   HWND_MESSAGE window never gets WM_ENDSESSION, and the ||
|||   CRT maps CTRL_CLOSE to SIGBREAK, whose handler only   ||
|||   sets the stop flag and returns — the process is      ||
|||   terminated the moment a console handler returns.     ||
|||                                                           ||
|||   Two sources, one handler:                             ||
|||   - a hidden top-level window on its own thread         ||
|||     (WM_ENDSESSION reaches top-level windows only; a    ||
|||     console process that loads user32 gets logoff and   ||
|||     shutdown only this way)                             ||
|||   - SetConsoleCtrlHandler for CTRL_CLOSE / CTRL_LOGOFF /||
|||     CTRL_SHUTDOWN (CTRL_C / CTRL_BREAK fall through to  ||
|||     the CRT signal handlers as before)                  ||
|||                                                           ||
|||   The handler runs synchronously and should return only ||
|||   once teardown is done (or its own bound expired):    ||
|||   Windows may end the process as soon as it returns.   ||
|||                                                           ||
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
|\*---------------------------------------------------------*/

#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace openrgb2
{
namespace platform
{

class WinSessionEnd
{
public:
    /* `why` names the source ("session end", "console close",
       "console logoff", "console shutdown"). */
    using Handler = std::function<void(const std::string& why)>;

    WinSessionEnd() = default;
    ~WinSessionEnd();                            /* Stop() */

    WinSessionEnd(const WinSessionEnd&)            = delete;
    WinSessionEnd& operator=(const WinSessionEnd&) = delete;

    /* One instance per process (the console handler is a
       process-wide registration). False + err when the window
       could not be created; the console handler is still
       registered then. */
    bool Start(Handler handler, std::string& err);
    void Stop();

    /* The hidden window (HWND), for tests that deliver
       WM_QUERYENDSESSION / WM_ENDSESSION. */
    void* Window() const { return hwnd_.load(); }

    /* The registered console control handler, exposed so tests
       can deliver CTRL_CLOSE_EVENT etc. without a console.
       Returns nonzero when the event was handled. */
    static int __stdcall OnConsoleCtrl(unsigned long type);

private:
    void ThreadMain();
    static __int64 __stdcall WndProc(void* hwnd, unsigned int msg,
                                     unsigned __int64 w, __int64 l);

    Handler                    handler_;
    std::thread                thread_;
    std::atomic<void*>         hwnd_      { nullptr };
    std::atomic<unsigned long> tid_       { 0 };
    std::atomic<bool>          init_done_ { false };
    /* Console-handler calls still inside handler_: Stop()
       waits for them so the object never dies under one. */
    std::atomic<int>           in_flight_ { 0 };
    bool                       ctrl_registered_ = false;
};

} /* namespace platform */
} /* namespace openrgb2 */
