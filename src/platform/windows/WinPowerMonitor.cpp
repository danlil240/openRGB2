/*---------------------------------------------------------*\
||| WinPowerMonitor.cpp                                       ||
|||                                                           ||
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
|\*---------------------------------------------------------*/

#include "WinPowerMonitor.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00   /* Win10 — SuspendResume notification */
#endif
#include <windows.h>
/* PBT_* power-broadcast constants and POWERBROADCAST_SETTING
   come from winuser.h (via windows.h) — no WDK header needed. */

/* wdm.h power-setting GUIDs, redeclared so user-mode builds need
   no WDK include path (same as the reference monitor). */
static const GUID kConsoleDisplayState =
    { 0x6FE69556, 0x704A, 0x47A0,
      { 0x8F, 0x24, 0xC2, 0x8D, 0x93, 0x6F, 0xDA, 0x47 } };
static const GUID kSessionDisplayStatus =
    { 0x2B84C20E, 0xAD23, 0x4DDF,
      { 0x93, 0xDB, 0x05, 0xFF, 0xBD, 0x7E, 0xFC, 0xA5 } };

namespace openrgb2
{
namespace platform
{

__int64 __stdcall WinPowerMonitor::PowerWndProc(void* h,
                                              unsigned int msg,
                                              unsigned __int64 w,
                                              __int64 l)
{
    const HWND hwnd = (HWND)h;
    if(msg == WM_NCCREATE)
    {
        /* Recover the monitor handed over via lpCreateParams so
           the proc can find it on every later message. */
        const CREATESTRUCTW* cs = (const CREATESTRUCTW*)l;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          (LONG_PTR)cs->lpCreateParams);
    }
    else
    {
        WinPowerMonitor* self = (WinPowerMonitor*)
            GetWindowLongPtrW(hwnd, GWLP_USERDATA);
        if(self != nullptr)
        {
            if(msg == WM_POWERBROADCAST)
            {
                if(w == (unsigned __int64)PBT_POWERSETTINGCHANGE)
                {
                    self->HandlePowerSetting((const void*)l);
                }
                else if(w == (unsigned __int64)PBT_APMSUSPEND)
                {
                    self->Emit(Event::Suspend);
                }
                else if(w == (unsigned __int64)PBT_APMRESUMEAUTOMATIC ||
                        w == (unsigned __int64)PBT_APMRESUMESUSPEND
#ifdef PBT_APMRESUMECRITICAL
                        ||
                        w == (unsigned __int64)PBT_APMRESUMECRITICAL
#endif
                        )
                {
                    self->Emit(Event::Resume);
                }
            }
            else if(msg == WM_DESTROY)
            {
                PostQuitMessage(0);
            }
        }
    }
    return DefWindowProcW(hwnd, msg, (WPARAM)w, (LPARAM)l);
}

void WinPowerMonitor::HandlePowerSetting(const void* setting)
{
    const POWERBROADCAST_SETTING* s =
        (const POWERBROADCAST_SETTING*)setting;
    if(s->DataLength < sizeof(DWORD))
    {
        return;
    }
    const DWORD v = *(const DWORD*)s->Data;
    if(IsEqualGUID(s->PowerSetting, kConsoleDisplayState))
    {
        console_state_ = (int)v;
    }
    else if(IsEqualGUID(s->PowerSetting, kSessionDisplayStatus))
    {
        session_state_ = (int)v;
    }
    else
    {
        return;
    }
    /* Asleep only when EVERY reported state says off: a GUID that
       never delivered counts as neither on nor off, and dimmed (2)
       still counts as awake (same rule as the reference). */
    const bool any_on  = console_state_ > 0 || session_state_ > 0;
    const bool any_off = console_state_ == 0 || session_state_ == 0;
    const bool on      = any_on || !any_off;
    if(on != display_on_.load())
    {
        display_on_.store(on);
        Emit(on ? Event::DisplayOn : Event::DisplayOff);
    }
}

void WinPowerMonitor::Emit(Event e)
{
    Sink sink;
    {
        /* The sink is fixed at Start() before the thread spawns —
           a plain copy is safe (Stop() joins the thread before the
           object dies). */
        sink = sink_;
    }
    if(sink)
    {
        sink(e);
    }
}

bool WinPowerMonitor::Start(Sink sink, std::string& err)
{
    if(running_.load())
    {
        return false;
    }
    sink_ = std::move(sink);
    thread_ = std::thread(&WinPowerMonitor::ThreadMain, this);

    /* Wait until the window attempt finished so Start()'s
       return value honestly means "registered". */
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    while(!init_done_.load() && Clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if(hwnd_.load() == nullptr)
    {
        if(thread_.joinable())
        {
            thread_.join();
        }
        err = "power monitor window could not be created";
        return false;
    }
    if(reg_console_ == nullptr && reg_session_ == nullptr &&
       reg_suspend_ == nullptr)
    {
        err = "no power notifications could be registered";
        /* Not fatal — the monitor still watches what it could
           register (none), Registered() reports false. */
    }
    return true;
}

void WinPowerMonitor::Stop()
{
    /* The WM_QUIT release needs the thread's message queue,
       which exists only once the window was created — wait
       (bounded) for the init attempt before posting, else the
       message can be lost and the join would hang. */
    if(thread_.joinable() && !init_done_.load())
    {
        const auto deadline =
            Clock::now() + std::chrono::seconds(5);
        while(!init_done_.load() && Clock::now() < deadline)
        {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(5));
        }
    }
    const DWORD tid = tid_.load();
    if(tid != 0)
    {
        PostThreadMessageW(tid, WM_QUIT, 0, 0);
    }
    if(thread_.joinable())
    {
        thread_.join();
    }
}

WinPowerMonitor::~WinPowerMonitor()
{
    Stop();
}

bool WinPowerMonitor::Running() const
{
    return running_.load();
}

bool WinPowerMonitor::Registered() const
{
    return hwnd_.load() != nullptr &&
           (reg_console_ != nullptr || reg_session_ != nullptr ||
            reg_suspend_ != nullptr);
}

bool WinPowerMonitor::DisplayOn() const
{
    return display_on_.load();
}

void WinPowerMonitor::ThreadMain()
{
    tid_     = GetCurrentThreadId();
    running_ = true;

    WNDCLASSW wc      = {};
    wc.lpfnWndProc    = (WNDPROC)&PowerWndProc;
    wc.hInstance      = GetModuleHandleW(nullptr);
    wc.lpszClassName  = L"OpenRGB2PowerSink";
    RegisterClassW(&wc);   /* ERROR_CLASS_ALREADY_EXISTS is fine */

    hwnd_.store(CreateWindowExW(0, wc.lpszClassName, L"", 0,
                                0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                wc.hInstance, this));
    if(hwnd_.load() == nullptr)
    {
        running_   = false;
        tid_       = 0;
        init_done_ = true;
        return;
    }
    const HWND hwnd = (HWND)hwnd_.load();

    /* Display power states (console + session — the session GUID
       covers the WTS/session-scoped display status). */
    reg_console_ = RegisterPowerSettingNotification(
        hwnd, &kConsoleDisplayState, DEVICE_NOTIFY_WINDOW_HANDLE);
    reg_session_ = RegisterPowerSettingNotification(
        hwnd, &kSessionDisplayStatus, DEVICE_NOTIFY_WINDOW_HANDLE);
    /* System suspend/resume — needs its own register call; a
       plain broadcast never reaches a message-only window. */
    reg_suspend_ = RegisterSuspendResumeNotification(
        hwnd, DEVICE_NOTIFY_WINDOW_HANDLE);
    init_done_ = true;   /* Start() may now judge registrations */

    MSG msg;
    while(GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if(reg_console_ != nullptr)
    {
        UnregisterPowerSettingNotification(
            (HPOWERNOTIFY)reg_console_);
        reg_console_ = nullptr;
    }
    if(reg_session_ != nullptr)
    {
        UnregisterPowerSettingNotification(
            (HPOWERNOTIFY)reg_session_);
        reg_session_ = nullptr;
    }
    if(reg_suspend_ != nullptr)
    {
        UnregisterSuspendResumeNotification(
            (HPOWERNOTIFY)reg_suspend_);
        reg_suspend_ = nullptr;
    }
    DestroyWindow((HWND)hwnd_.load());
    hwnd_.store(nullptr);
    running_ = false;
    tid_     = 0;
}

} /* namespace platform */
} /* namespace openrgb2 */
