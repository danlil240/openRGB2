/*---------------------------------------------------------*\
||| WinHotkeys.cpp — see WinHotkeys.h (R-2).                  ||
|||                                                           ||
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#include "WinHotkeys.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <future>
#include <memory>
#include <string>

namespace openrgb2
{
namespace platform
{

namespace
{

/* WM_APP + 0x48 ('H'): lParam is a heap-allocated
   shared_ptr<ApplyRequest>*; the thread deletes it. */
constexpr UINT kApplyMsg = WM_APP + 0x48;

/* How long Apply() waits for the hotkey thread. */
constexpr auto kApplyTimeout = std::chrono::seconds(2);

struct ApplyRequest
{
    std::vector<HotkeyRequest>             set;
    std::promise<std::vector<HotkeyResult>> done;
};

std::vector<HotkeyResult> AllUnavailable(
    const std::vector<HotkeyRequest>& set, const std::string& why)
{
    std::vector<HotkeyResult> out;
    for(const HotkeyRequest& r : set)
    {
        out.push_back({ r.id, HotkeyState::Unavailable, why });
    }
    return out;
}

} /* anonymous namespace */

WinHotkeys::WinHotkeys(Sink sink)
    : sink_(std::move(sink))
{
}

WinHotkeys::~WinHotkeys()
{
    Stop();
}

__int64 __stdcall WinHotkeys::WndProc(void* h, unsigned int msg,
                                      unsigned __int64 w, __int64 l)
{
    const HWND hwnd = (HWND)h;
    if(msg == WM_NCCREATE)
    {
        const CREATESTRUCTW* cs = (const CREATESTRUCTW*)l;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          (LONG_PTR)cs->lpCreateParams);
        return DefWindowProcW(hwnd, msg, (WPARAM)w, (LPARAM)l);
    }
    WinHotkeys* self =
        (WinHotkeys*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if(self != nullptr)
    {
        if(msg == WM_HOTKEY)
        {
            if(self->sink_)
            {
                self->sink_(int(w));
            }
            return 0;
        }
        if(msg == kApplyMsg)
        {
            auto* holder = (std::shared_ptr<ApplyRequest>*)l;
            std::shared_ptr<ApplyRequest> req = *holder;
            delete holder;

            for(int id : self->registered_)
            {
                UnregisterHotKey(hwnd, id);
            }
            self->registered_.clear();

            std::vector<HotkeyResult> out;
            for(const HotkeyRequest& r : req->set)
            {
                HotkeyResult res;
                res.id = r.id;
                if(RegisterHotKey(hwnd, r.id,
                                  UINT(r.mods) | MOD_NOREPEAT,
                                  UINT(r.vk)))
                {
                    res.state = HotkeyState::Ok;
                    self->registered_.push_back(r.id);
                }
                else
                {
                    const DWORD e = GetLastError();
                    if(e == ERROR_HOTKEY_ALREADY_REGISTERED)
                    {
                        res.state  = HotkeyState::InUse;
                        res.detail = "in use by another app or "
                                     "reserved by Windows";
                    }
                    else
                    {
                        res.state  = HotkeyState::Invalid;
                        res.detail = "RegisterHotKey failed (error " +
                                     std::to_string(e) + ")";
                    }
                }
                out.push_back(std::move(res));
            }
            req->done.set_value(std::move(out));
            return 0;
        }
        if(msg == WM_DESTROY)
        {
            PostQuitMessage(0);
        }
    }
    return DefWindowProcW(hwnd, msg, (WPARAM)w, (LPARAM)l);
}

bool WinHotkeys::StartThread()
{
    if(running_.load())
    {
        return true;
    }
    if(thread_.joinable())
    {
        thread_.join();
    }
    init_done_ = false;
    hwnd_.store(nullptr);
    thread_ = std::thread(&WinHotkeys::ThreadMain, this);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while(!init_done_.load() &&
          std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if(hwnd_.load() == nullptr)
    {
        if(thread_.joinable() && init_done_.load())
        {
            thread_.join();
        }
        return false;
    }
    return true;
}

std::vector<HotkeyResult>
WinHotkeys::Apply(const std::vector<HotkeyRequest>& set)
{
    std::lock_guard<std::mutex> g(apply_mutex_);
    if(!running_.load())
    {
        if(set.empty())
        {
            return {};   /* nothing bound: no thread at all */
        }
        if(!StartThread())
        {
            return AllUnavailable(set,
                "the hotkey window could not be created");
        }
    }
    auto req = std::make_shared<ApplyRequest>();
    req->set = set;
    std::future<std::vector<HotkeyResult>> fut = req->done.get_future();
    auto* holder = new std::shared_ptr<ApplyRequest>(req);
    if(!PostMessageW((HWND)hwnd_.load(), kApplyMsg, 0, (LPARAM)holder))
    {
        delete holder;
        return AllUnavailable(set, "the hotkey thread is not running");
    }
    if(fut.wait_for(kApplyTimeout) != std::future_status::ready)
    {
        return AllUnavailable(set, "the hotkey thread did not answer");
    }
    return fut.get();
}

void WinHotkeys::Stop()
{
    std::lock_guard<std::mutex> g(apply_mutex_);
    if(thread_.joinable() && !init_done_.load())
    {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while(!init_done_.load() &&
              std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
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

void WinHotkeys::ThreadMain()
{
    tid_     = GetCurrentThreadId();
    running_ = true;

    WNDCLASSW wc     = {};
    wc.lpfnWndProc   = (WNDPROC)&WndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"OpenRGB2HotkeySink";
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
    init_done_ = true;

    MSG msg;
    while(GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    const HWND hwnd = (HWND)hwnd_.load();
    /* Answer any request still queued behind the quit (Apply and
       Stop share apply_mutex_, so this is belt and braces): the
       holder is freed and its waiter gets a result. */
    MSG pending;
    while(PeekMessageW(&pending, hwnd, kApplyMsg, kApplyMsg, PM_REMOVE))
    {
        auto* holder = (std::shared_ptr<ApplyRequest>*)pending.lParam;
        (*holder)->done.set_value(
            AllUnavailable((*holder)->set, "the hotkey thread stopped"));
        delete holder;
    }
    for(int id : registered_)
    {
        UnregisterHotKey(hwnd, id);
    }
    registered_.clear();
    DestroyWindow(hwnd);
    hwnd_.store(nullptr);
    running_ = false;
    tid_     = 0;
}

} /* namespace platform */
} /* namespace openrgb2 */
