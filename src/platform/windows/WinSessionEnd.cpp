/*---------------------------------------------------------*\
||| WinSessionEnd.cpp                                         ||
|||                                                           ||
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
|\*---------------------------------------------------------*/

#include "WinSessionEnd.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>

#include <chrono>

namespace openrgb2
{
namespace platform
{

namespace
{
/* The console handler is process-wide; it reaches the one live
   instance through this pointer (set in Start, cleared in Stop
   after the handler is unregistered). */
std::atomic<WinSessionEnd*> g_instance{ nullptr };
}

int __stdcall WinSessionEnd::OnConsoleCtrl(unsigned long type)
{
    const char* why = nullptr;
    switch(type)
    {
    case CTRL_CLOSE_EVENT:    why = "console close";    break;
    case CTRL_LOGOFF_EVENT:   why = "console logoff";   break;
    case CTRL_SHUTDOWN_EVENT: why = "console shutdown"; break;
    default:
        /* CTRL_C / CTRL_BREAK: not ours — the CRT's SIGINT /
           SIGBREAK handlers (registered earlier, called after
           this one) still request the stop. */
        return FALSE;
    }
    WinSessionEnd* self = g_instance.load();
    if(self == nullptr || !self->handler_)
    {
        return FALSE;
    }
    /* Blocks until teardown is done: Windows terminates the
       process as soon as this returns. */
    ++self->in_flight_;
    self->handler_(why);
    --self->in_flight_;
    return TRUE;
}

__int64 __stdcall WinSessionEnd::WndProc(void* h, unsigned int msg,
                                         unsigned __int64 w, __int64 l)
{
    const HWND hwnd = (HWND)h;
    if(msg == WM_NCCREATE)
    {
        const CREATESTRUCTW* cs = (const CREATESTRUCTW*)l;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          (LONG_PTR)cs->lpCreateParams);
    }
    else if(msg == WM_QUERYENDSESSION)
    {
        return TRUE;        /* never veto the user's shutdown */
    }
    else if(msg == WM_ENDSESSION)
    {
        WinSessionEnd* self = (WinSessionEnd*)
            GetWindowLongPtrW(hwnd, GWLP_USERDATA);
        if(w != 0 && self != nullptr && self->handler_)
        {
            /* The session really ends: the process may be
               terminated once this returns, so teardown runs
               inside the handler. */
            self->handler_("session end");
        }
        return 0;
    }
    else if(msg == WM_DESTROY)
    {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, (WPARAM)w, (LPARAM)l);
}

bool WinSessionEnd::Start(Handler handler, std::string& err)
{
    if(thread_.joinable() || ctrl_registered_)
    {
        err = "already started";
        return false;
    }
    handler_ = std::move(handler);
    g_instance.store(this);
    /* Registered last-in, called first: CLOSE/LOGOFF/SHUTDOWN
       are taken here before the CRT would map them to
       SIGBREAK and let the process die on return. */
    ctrl_registered_ =
        SetConsoleCtrlHandler((PHANDLER_ROUTINE)&OnConsoleCtrl,
                              TRUE) != FALSE;

    init_done_ = false;
    hwnd_.store(nullptr);
    thread_ = std::thread(&WinSessionEnd::ThreadMain, this);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while(!init_done_.load() &&
          std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if(hwnd_.load() == nullptr)
    {
        if(init_done_.load() && thread_.joinable())
        {
            thread_.join();
        }
        err = "session-end window could not be created";
        return false;
    }
    return true;
}

void WinSessionEnd::Stop()
{
    if(ctrl_registered_)
    {
        SetConsoleCtrlHandler((PHANDLER_ROUTINE)&OnConsoleCtrl,
                              FALSE);
        ctrl_registered_ = false;
    }
    if(thread_.joinable())
    {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while(!init_done_.load() &&
              std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        const DWORD tid = tid_.load();
        if(tid != 0)
        {
            PostThreadMessageW(tid, WM_QUIT, 0, 0);
        }
        thread_.join();
    }
    WinSessionEnd* expected = this;
    g_instance.compare_exchange_strong(expected, nullptr);
    /* A console handler already inside handler_ (it read the
       instance before the unregister) finishes before this
       object may go away. Bounded: the caller ends teardown
       first, so the handler is only waking up. */
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while(in_flight_.load() > 0 &&
          std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

WinSessionEnd::~WinSessionEnd()
{
    Stop();
}

void WinSessionEnd::ThreadMain()
{
    tid_ = GetCurrentThreadId();

    WNDCLASSW wc      = {};
    wc.lpfnWndProc    = (WNDPROC)&WndProc;
    wc.hInstance      = GetModuleHandleW(nullptr);
    wc.lpszClassName  = L"OpenRGB2SessionEnd";
    RegisterClassW(&wc);   /* ERROR_CLASS_ALREADY_EXISTS is fine */

    /* Top-level (no parent, not HWND_MESSAGE) so the session
       broadcast reaches it; never shown. */
    hwnd_.store(CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName,
                                L"OpenRGB2 engine", WS_POPUP,
                                0, 0, 0, 0, nullptr, nullptr,
                                wc.hInstance, this));
    if(hwnd_.load() == nullptr)
    {
        tid_       = 0;
        init_done_ = true;
        return;
    }
    init_done_ = true;

    MSG msg;
    while(GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    DestroyWindow((HWND)hwnd_.load());
    hwnd_.store(nullptr);
    tid_ = 0;
}

} /* namespace platform */
} /* namespace openrgb2 */
