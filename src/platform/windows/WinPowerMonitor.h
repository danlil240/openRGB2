/*---------------------------------------------------------*\
||| WinPowerMonitor.h                                         ||
|||                                                           ||
|||   Qt-free Windows display/system power monitor for the ||
|||   engine (Task 3.7 — adapts the preserved plugin's      ||
|||   DisplayPowerMonitor approach,                         ||
|||   openrgb-unified/plugins/DesktopLightingStudio/       ||
|||   output/DisplayPowerMonitor.*).                        ||
|||                                                           ||
|||   A console engine has no window/message pump, so the   ||
|||   monitor owns a dedicated thread hosting a private     ||
|||   message-only window. Three registrations hang off it: ||
|||   - RegisterPowerSettingNotification for               ||
|||     GUID_CONSOLE_DISPLAY_STATE and                      ||
|||     GUID_SESSION_DISPLAY_STATUS — display-off travels   ||
|||     as PBT_POWERSETTINGCHANGE, delivered only to HWNDs  ||
|||     passed to the register call                         ||
|||   - RegisterSuspendResumeNotification — PBT_APMSUSPEND /||
|||     PBT_APMRESUME* need the explicit register; plain    ||
|||     broadcast would miss a message-only window          ||
|||                                                           ||
|||   Events funnel to one sink callback — production wires ||
|||   PowerLifecycle::PostEvent. The sink is invoked on the ||
|||   monitor thread and must be cheap (PostEvent only      ||
|||   queues); blocking restore I/O never runs here.        ||
|||                                                           ||
|||   Display-off semantics preserved from the reference:   ||
|||   the display counts as off only when EVERY reported    ||
|||   state says off — a lit second monitor (session on,    ||
|||   console off) keeps output running; dimmed counts as   ||
|||   awake; a GUID that never reported counts as neither.  ||
|||                                                           ||
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
|\*---------------------------------------------------------*/

#pragma once

#include "PowerLifecycle.h"   /* PowerLifecycle::Event */

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace openrgb2
{
namespace platform
{

class WinPowerMonitor
{
public:
    using Event = PowerLifecycle::Event;
    using Sink  = std::function<void(Event)>;

    WinPowerMonitor() = default;
    ~WinPowerMonitor();                          /* Stop() */

    WinPowerMonitor(const WinPowerMonitor&)            = delete;
    WinPowerMonitor& operator=(const WinPowerMonitor&) = delete;

    /* Starts the monitor thread + message-only window and
       registers the notifications. False + err when the window
       could not be created — the engine then runs unmonitored
       (log the failure; output simply never power-pauses). */
    bool Start(Sink sink, std::string& err);
    void Stop();
    bool Running()    const;
    bool Registered() const;   /* >= 1 notification registered  */
    bool DisplayOn()  const;

private:
    void ThreadMain();
    static __int64 __stdcall PowerWndProc(void* hwnd,
                                          unsigned int msg,
                                          unsigned __int64 w,
                                          __int64 l);
    void HandlePowerSetting(const void* setting);
    void Emit(Event e);

    Sink                  sink_;
    std::thread           thread_;
    std::atomic<bool>     running_    { false };
    std::atomic<bool>     init_done_  { false }; /* window attempt finished */
    std::atomic<bool>     display_on_ { true };

    /* Per-GUID last reported state: -1 = never reported,
       0 = off, 1 = on, 2 = dimmed. Monitor-thread owned. */
    int                   console_state_ = -1;
    int                   session_state_ = -1;

    /* Monitor-thread owned (valid only while Running()); hwnd_
       is atomic only so Start()/Stop() can poll for creation
       before the thread is joined. */
    std::atomic<void*>    hwnd_       { nullptr }; /* HWND         */
    void*                 reg_console_ = nullptr; /* HPOWERNOTIFY */
    void*                 reg_session_ = nullptr;
    void*                 reg_suspend_ = nullptr;
    std::atomic<unsigned long> tid_   { 0 };      /* for WM_QUIT */
};

} /* namespace platform */
} /* namespace openrgb2 */
