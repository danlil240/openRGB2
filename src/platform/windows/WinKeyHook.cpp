/*---------------------------------------------------------*\
||| WinKeyHook.cpp                                            ||
|||                                                           ||
|||   Qt-free port of the preserved plugin's               ||
|||   inputs/KeyHook.cpp (Windows WH_KEYBOARD_LL section)  ||
|||   onto the engine's IReactiveSource seam (Task 4.6).   ||
|||   A dedicated thread hosts the low-level keyboard      ||
|||   hook's message pump; first key edges push bounded    ||
|||   "key" events with the virtual-key code. The hook is  ||
|||   listen-only — events are never consumed, and no      ||
|||   typed text/history is retained (privacy).            ||
|||                                                           ||
|||   capture_paused: edges are swallowed (the down[]      ||
|||   repeat-suppression still tracks, so a key held       ||
|||   through the pause can't refire on resume).           ||
|||                                                           ||
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#include "WinReactiveInputs.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace openrgb2
{
namespace platform
{

namespace
{

/* Hook state — touched only on the hook thread (the LL
   callback runs inside that thread's message pump). One
   hook exists per process. */
struct HookState
{
    studio::InputBus* bus    = nullptr;
    std::atomic<bool>* paused = nullptr;
    bool              down[256] = {};  /* auto-repeat suppression */
};

HookState g_hook;

LRESULT CALLBACK LowLevelKeyProc(int code, WPARAM w, LPARAM l)
{
    if(code == HC_ACTION)
    {
        const KBDLLHOOKSTRUCT* kb =
            reinterpret_cast<const KBDLLHOOKSTRUCT*>(l);
        const int vk = (int)kb->vkCode;
        if(vk >= 0 && vk <= 255)
        {
            if(w == WM_KEYDOWN || w == WM_SYSKEYDOWN)
            {
                /* Only the first edge of a held key spawns a
                   ripple — auto-repeat repeats keydown. */
                if(!g_hook.down[vk])
                {
                    g_hook.down[vk] = true;
                    const bool paused =
                        g_hook.paused != nullptr &&
                        g_hook.paused->load();
                    if(!paused && g_hook.bus != nullptr)
                        g_hook.bus->PushEvent("key", 1.0f, vk);
                }
            }
            else if(w == WM_KEYUP || w == WM_SYSKEYUP)
            {
                g_hook.down[vk] = false;
            }
        }
    }
    return CallNextHookEx(nullptr, code, w, l);
}

class WinKeyHook final : public IReactiveSource
{
public:
    WinKeyHook()  { g_hook.paused = &paused_; }
    ~WinKeyHook() override { Stop(); g_hook.paused = nullptr; }

    bool Start(studio::InputBus* b) override
    {
        if(running_.load())
            return false;
        bus_     = b;
        running_ = true;
        thread_ = std::thread(&WinKeyHook::ThreadMain, this);
        return true;
    }

    void Stop() override
    {
        if(!running_.load())
            return;
        running_ = false;
        /* The hook thread may not have published its id yet —
           give it a moment before posting, otherwise the quit
           message is lost and join() would hang on a live
           message loop. */
        unsigned long id = 0;
        for(int i = 0;
            i < 200 && (id = tid_.load()) == 0 &&
            thread_.joinable();
            i++)
        {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(5));
        }
        if(id != 0)
            PostThreadMessage(id, WM_QUIT, 0, 0);
        if(thread_.joinable())
            thread_.join();
        tid_ = 0;
        bus_ = nullptr;
        SetStatus("keys: off");
    }

    bool Running() const override { return running_.load(); }

    std::string Status() const override
    {
        std::lock_guard<std::mutex> lock(status_mu_);
        return status_;
    }

    void SetPaused(bool paused) override
    {
        paused_.store(paused);
    }

private:
    void SetStatus(const std::string& s)
    {
        std::lock_guard<std::mutex> lock(status_mu_);
        status_ = s;
    }

    void ThreadMain()
    {
        /* Force the message queue into existence before
           publishing the id, so Stop()'s WM_QUIT can never
           land before the queue exists and get dropped. */
        MSG msg;
        PeekMessage(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
        tid_ = GetCurrentThreadId();

        HHOOK hook = SetWindowsHookExW(WH_KEYBOARD_LL,
                                       LowLevelKeyProc,
                                       GetModuleHandleW(nullptr),
                                       0);
        if(hook == nullptr)
        {
            SetStatus("keys: hook install failed");
            running_ = false;
            return;
        }
        g_hook.bus = bus_;
        SetStatus("keys: listening");

        while(GetMessage(&msg, nullptr, 0, 0) > 0)
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        UnhookWindowsHookEx(hook);
        g_hook.bus = nullptr;
        g_hook     = HookState{};
        g_hook.paused = &paused_;
        SetStatus("keys: off");
        running_ = false;
    }

    studio::InputBus*  bus_ = nullptr;
    std::thread        thread_;
    std::atomic<bool>  running_ { false };
    std::atomic<bool>  paused_  { false };
    std::atomic<unsigned long> tid_ { 0 };
    mutable std::mutex status_mu_;
    std::string        status_  = "keys: off";
};

} /* namespace */

std::unique_ptr<IReactiveSource> MakeKeyHookSource()
{
    return std::make_unique<WinKeyHook>();
}

} /* namespace platform */
} /* namespace openrgb2 */

#else /* unsupported platform */

namespace openrgb2
{
namespace platform
{

namespace
{
class StubKeys final : public IReactiveSource
{
public:
    bool Start(studio::InputBus*) override
    {
        status_ = "keys: unsupported platform";
        return false;
    }
    void        Stop()    override               {}
    bool        Running() const override         { return false; }
    std::string Status() const override          { return status_; }
private:
    std::string status_ = "keys: unsupported platform";
};
} /* namespace */

std::unique_ptr<IReactiveSource> MakeKeyHookSource()
{
    return std::make_unique<StubKeys>();
}

} /* namespace platform */
} /* namespace openrgb2 */

#endif
