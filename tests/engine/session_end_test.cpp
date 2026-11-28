// SPDX-License-Identifier: GPL-2.0-or-later
//
// OS shutdown / logoff / console-close notice (audit E-10 —
// src/platform/windows/WinSessionEnd.h). The engine used to die on
// these with no teardown: its only window was HWND_MESSAGE (no
// WM_ENDSESSION), and the CRT mapped CTRL_CLOSE to SIGBREAK, whose
// handler returns at once and lets Windows end the process.
//
// Covers:
//   - WM_QUERYENDSESSION is answered TRUE (never vetoes)
//   - WM_ENDSESSION(TRUE) runs the handler synchronously — the
//     message returns only after teardown; WM_ENDSESSION(FALSE)
//     (shutdown cancelled) does nothing
//   - CTRL_CLOSE / CTRL_LOGOFF / CTRL_SHUTDOWN run the handler and
//     report handled; CTRL_C / CTRL_BREAK fall through to the CRT
//   - after Stop() nothing is routed to the dead instance
//
// The messages are delivered straight to the hidden window and the
// console handler is called directly: no real logoff, no console.

#include "WinSessionEnd.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using openrgb2::platform::WinSessionEnd;

static int checks   = 0;
static int failures = 0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; \
    std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while(0)

int main()
{
    std::mutex               m;
    std::vector<std::string> calls;
    WinSessionEnd            watcher;
    std::string              err;
    const bool started = watcher.Start(
        [&](const std::string& why)
        {
            /* Stands in for "RequestStop + wait for Stop()". */
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            std::lock_guard<std::mutex> g(m);
            calls.push_back(why);
        }, err);
    CHECK(started);
    if(!started)
    {
        std::printf("start failed: %s\n", err.c_str());
        std::printf("%d checks, %d failures\n", checks, failures);
        return 1;
    }
    const HWND hwnd = (HWND)watcher.Window();
    CHECK(hwnd != nullptr);
    /* Top-level, so the session broadcast reaches it. */
    CHECK(GetParent(hwnd) == nullptr);
    CHECK(!IsWindowVisible(hwnd));

    CHECK(SendMessageW(hwnd, WM_QUERYENDSESSION, 0,
                       ENDSESSION_LOGOFF) != 0);

    /* Shutdown cancelled by another app: no teardown. */
    SendMessageW(hwnd, WM_ENDSESSION, FALSE, ENDSESSION_LOGOFF);
    {
        std::lock_guard<std::mutex> g(m);
        CHECK(calls.empty());
    }

    /* The session ends: the handler has finished by the time
       the message returns. */
    const auto t0 = std::chrono::steady_clock::now();
    SendMessageW(hwnd, WM_ENDSESSION, TRUE, 0);
    const auto took = std::chrono::steady_clock::now() - t0;
    CHECK(took >= std::chrono::milliseconds(50));
    {
        std::lock_guard<std::mutex> g(m);
        CHECK(calls.size() == 1);
        CHECK(!calls.empty() && calls.back() == "session end");
    }

    /* Console events. */
    CHECK(WinSessionEnd::OnConsoleCtrl(CTRL_CLOSE_EVENT) != 0);
    CHECK(WinSessionEnd::OnConsoleCtrl(CTRL_LOGOFF_EVENT) != 0);
    CHECK(WinSessionEnd::OnConsoleCtrl(CTRL_SHUTDOWN_EVENT) != 0);
    CHECK(WinSessionEnd::OnConsoleCtrl(CTRL_C_EVENT) == 0);
    CHECK(WinSessionEnd::OnConsoleCtrl(CTRL_BREAK_EVENT) == 0);
    {
        std::lock_guard<std::mutex> g(m);
        CHECK(calls.size() == 4);
        CHECK(calls.size() == 4 && calls[1] == "console close" &&
              calls[2] == "console logoff" &&
              calls[3] == "console shutdown");
    }

    watcher.Stop();
    CHECK(watcher.Window() == nullptr);
    CHECK(WinSessionEnd::OnConsoleCtrl(CTRL_CLOSE_EVENT) == 0);
    {
        std::lock_guard<std::mutex> g(m);
        CHECK(calls.size() == 4);
    }

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
