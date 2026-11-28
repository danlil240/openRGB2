/*---------------------------------------------------------*\
||| WinHotkeys.h                                              ||
|||                                                           ||
|||   R-2: system-wide hotkeys through RegisterHotKey — the ||
|||   IHotkeyRegistrar the engine's Automation drives.      ||
|||                                                           ||
|||   Why RegisterHotKey and not the WH_KEYBOARD_LL hook    ||
|||   (WinKeyHook.cpp): the LL hook only listens, is tied   ||
|||   to the reactive "key" toggle, and Windows silently    ||
|||   unhooks a hook that exceeds its timeout. A registered ||
|||   hotkey is delivered as WM_HOTKEY to its owner thread  ||
|||   in any foreground app (games included); it consumes   ||
|||   only that one chord, never other keys. MOD_NOREPEAT   ||
|||   keeps a held chord from auto-repeating.                ||
|||                                                           ||
|||   A dedicated thread owns a message-only window (the    ||
|||   WinPowerMonitor pattern). RegisterHotKey binds to the ||
|||   calling thread, so Apply() posts the new set to that  ||
|||   thread (WM_APP) and waits — bounded — for the per-     ||
|||   binding results: ok, in_use (ERROR_HOTKEY_ALREADY_    ||
|||   REGISTERED — another app or Windows holds it), or     ||
|||   invalid (any other refusal, with the error code).     ||
|||                                                           ||
|||   The thread starts lazily on the first non-empty set:  ||
|||   an engine with nothing bound never creates it.        ||
|||   WM_HOTKEY calls the sink with the id; the sink only   ||
|||   queues (Automation::PostHotkey).                      ||
|||                                                           ||
|||   Platform code: not loaded by the offline suites       ||
|||   (manual test only).                                   ||
|||                                                           ||
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#pragma once

#include "Automation.h"   /* IHotkeyRegistrar */

#include <atomic>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace openrgb2
{
namespace platform
{

class WinHotkeys final : public IHotkeyRegistrar
{
public:
    using Sink = std::function<void(int id)>;

    explicit WinHotkeys(Sink sink);
    ~WinHotkeys() override;                    /* Stop() */

    WinHotkeys(const WinHotkeys&)            = delete;
    WinHotkeys& operator=(const WinHotkeys&) = delete;

    std::vector<HotkeyResult>
         Apply(const std::vector<HotkeyRequest>& set) override;

    /* Unregisters everything and joins the thread
       (idempotent). */
    void Stop();
    bool Running() const { return running_.load(); }

private:
    bool StartThread();
    void ThreadMain();
    static __int64 __stdcall WndProc(void* hwnd, unsigned int msg,
                                     unsigned __int64 w, __int64 l);

    Sink                  sink_;
    std::mutex            apply_mutex_;
    std::thread           thread_;
    std::atomic<bool>     running_   { false };
    std::atomic<bool>     init_done_ { false };
    std::atomic<void*>    hwnd_      { nullptr };
    std::atomic<unsigned long> tid_  { 0 };
    std::vector<int>      registered_;   /* thread-owned */
};

} /* namespace platform */
} /* namespace openrgb2 */
