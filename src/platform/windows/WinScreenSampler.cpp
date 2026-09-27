/*---------------------------------------------------------*\
||| WinScreenSampler.cpp                                      ||
|||                                                           ||
|||   Qt-free port of the preserved plugin's               ||
|||   inputs/ScreenSampler.cpp (Windows GDI section) onto  ||
|||   the engine's IReactiveSource seam (Task 4.6). The    ||
|||   plugin's QScreen/QTimer path is replaced by          ||
|||   EnumDisplayMonitors + a capture thread — the        ||
|||   engine has no Qt.                                    ||
|||                                                           ||
|||   Each grab StretchBlts the selected display straight  ||
|||   into a 12x6 32-bit DIB — the display driver          ||
|||   downsamples, so only ~72 pixels cross to user space  ||
|||   (~2-4 ms per grab, cheap at ~25 Hz beside a 60 Hz    ||
|||   tick). Smoothing + luminance cap feed the bus's      ||
|||   screenfield grid. A failed grab leaves the grid      ||
|||   empty — the primitive contributes nothing and the    ||
|||   scene falls back to its base layers.                 ||
|||                                                           ||
|||   Privacy/bounds (plan section 6): capture stays in    ||
|||   the engine process; only the 12x6 averaged grid      ||
|||   reaches scene evaluation, ~25 Hz max, one display.   ||
|||   capture_paused skips grabs entirely — no GPU reads   ||
|||   while displays sleep.                                ||
|||                                                           ||
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#include "WinReactiveInputs.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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

constexpr int kCols = 12;
constexpr int kRows = 6;

/* One enumerated display rect in physical pixels — the
   StretchBlt source region. */
struct DisplayRect
{
    std::string name;
    RECT        rect{};
};

BOOL CALLBACK MonitorEnumProc(HMONITOR mon, HDC, LPRECT,
                              LPARAM data)
{
    auto* out = reinterpret_cast<std::vector<DisplayRect>*>(data);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if(GetMonitorInfoW(mon, &mi))
    {
        DisplayRect d;
        d.rect = mi.rcMonitor;
        /* Friendly-ish label: the GDI device name is all the
           bounded engine surface needs ("\\\\.\\DISPLAY1"). */
        char name[CCHDEVICENAME + 1] = {};
        WideCharToMultiByte(CP_UTF8, 0, mi.szDevice, -1,
                            name, sizeof(name) - 1,
                            nullptr, nullptr);
        d.name = name;
        out->push_back(std::move(d));
    }
    return TRUE;
}

std::vector<DisplayRect> DisplayRects()
{
    std::vector<DisplayRect> out;
    EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc,
                        reinterpret_cast<LPARAM>(&out));
    return out;
}

/* Grab the display rect straight into a cols x rows 32-bit
   DIB via StretchBlt — same approach as the plugin's
   GrabGdi, minus QScreen. */
bool GrabGdi(const DisplayRect& disp, int cols, int rows,
             unsigned int* px)
{
    const int sw = (int)(disp.rect.right  - disp.rect.left);
    const int sh = (int)(disp.rect.bottom - disp.rect.top);
    if(sw <= 0 || sh <= 0)
        return false;

    bool ok = false;
    HDC hdc_screen = GetDC(nullptr);  /* virtual screen: all
                                          monitors, physical
                                          pixels */
    if(hdc_screen == nullptr)
        return false;
    HDC hdc_mem = CreateCompatibleDC(hdc_screen);
    if(hdc_mem != nullptr)
    {
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth       = cols;
        bmi.bmiHeader.biHeight      = -rows;  /* top-down */
        bmi.bmiHeader.biPlanes      = 1;
        bmi.bmiHeader.biBitCount    = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP bmp = CreateDIBSection(hdc_mem, &bmi,
                                       DIB_RGB_COLORS,
                                       &bits, nullptr, 0);
        if(bmp != nullptr && bits != nullptr)
        {
            HGDIOBJ prev = SelectObject(hdc_mem, bmp);
            SetStretchBltMode(hdc_mem, HALFTONE);
            SetBrushOrgEx(hdc_mem, 0, 0, nullptr);
            ok = StretchBlt(hdc_mem, 0, 0, cols, rows,
                            hdc_screen,
                            (int)disp.rect.left,
                            (int)disp.rect.top,
                            sw, sh, SRCCOPY) != FALSE;
            if(ok)
                std::memcpy(px, bits,
                            (size_t)cols * rows *
                            sizeof(unsigned int));
            SelectObject(hdc_mem, prev);
            DeleteObject(bmp);
        }
        DeleteDC(hdc_mem);
    }
    ReleaseDC(nullptr, hdc_screen);
    return ok;
}

class WinScreenSampler final : public IReactiveSource
{
public:
    ~WinScreenSampler() override { Stop(); }

    bool Start(studio::InputBus* b) override
    {
        /* lifecycle_mu_ closes the check-then-spawn race:
           two concurrent Starts can never both pass the
           running check, and a Stop can never slip between
           the flag set and the thread_ assignment. */
        std::lock_guard<std::mutex> lock(lifecycle_mu_);
        if(running_.load())
            return false;
        /* A self-terminated capture thread leaves thread_
           joinable — reap it or the move-assign below is
           std::terminate. A still-running thread can never
           reach here (running_==true already returned), so
           joinable+!running is always a zombie. */
        if(thread_.joinable())
            thread_.join();
        /* Synchronous pre-flight: run the same bounds check
           + one real grab before promising "started" — a
           gone display or a GDI read that can't work at all
           refuses Start honestly (Unavailable + stays
           disabled). Conditions arising mid-capture (the
           display later disconnecting) still surface
           asynchronously via Status(). */
        std::string err;
        if(!Probe(err))
        {
            SetStatus(err);
            return false;
        }
        bus_       = b;
        running_   = true;
        cells_.assign(kCols * kRows, studio::ColorF{});
        thread_ = std::thread(&WinScreenSampler::ThreadMain,
                              this);
        return true;
    }

    void Stop() override
    {
        std::lock_guard<std::mutex> lock(lifecycle_mu_);
        running_ = false;
        if(thread_.joinable())
            thread_.join();
        if(bus_ != nullptr)
            bus_->ClearScreenGrid();
        bus_ = nullptr;
        SetStatus("screen: off");
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

    void SetScreenIndex(int index) override
    {
        screen_index_.store(index);
    }

private:
    /* Cheap pre-flight under lifecycle_mu_: the display-index
       bounds check + one real 12x6 grab (~2-4 ms, result
       discarded). Skipped while capture_paused — displays
       are sleeping so there is nothing to sample, and the
       grab itself must not touch the GPU then; capture
       state surfaces via status once the gate opens. */
    bool Probe(std::string& err)
    {
        if(paused_.load())
            return true;
        const int idx = screen_index_.load();
        const std::vector<DisplayRect> displays =
            DisplayRects();
        if(idx < 0 || idx >= (int)displays.size())
        {
            err = "screen: display " + std::to_string(idx) +
                  " gone";
            return false;
        }
        unsigned int px[kCols * kRows];
        if(!GrabGdi(displays[idx], kCols, kRows, px))
        {
            err = "screen: capture unavailable";
            return false;
        }
        return true;
    }

    void SetStatus(const std::string& s)
    {
        std::lock_guard<std::mutex> lock(status_mu_);
        status_ = s;
    }

    void ThreadMain()
    {
        while(running_.load())
        {
            if(paused_.load())
            {
                /* display-off/suspend: NO grabs — the
                   expensive, privacy-sensitive read never
                   runs while displays sleep */
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(50));
                continue;
            }
            Grab();
            std::this_thread::sleep_for(
                std::chrono::milliseconds(40)); /* ~25 Hz */
        }
    }

    void Grab()
    {
        const int idx = screen_index_.load();
        const std::vector<DisplayRect> displays =
            DisplayRects();
        if(idx < 0 || idx >= (int)displays.size())
        {
            SetStatus("screen: display " + std::to_string(idx) +
                      " gone");
            if(bus_ != nullptr)
                bus_->ClearScreenGrid();
            return;
        }

        unsigned int px[kCols * kRows];
        if(!GrabGdi(displays[idx], kCols, kRows, px))
        {
            SetStatus("screen: capture unavailable");
            if(bus_ != nullptr)
                bus_->ClearScreenGrid();
            return;
        }

        std::lock_guard<std::mutex> lock(cells_mu_);
        if(cells_.size() != (size_t)(kCols * kRows))
            cells_.assign(kCols * kRows, studio::ColorF{});

        for(int i = 0; i < kCols * kRows; i++)
        {
            const unsigned int v = px[i];
            studio::ColorF c;
            c.r = ((v >> 16) & 0xFF) / 255.0f;
            c.g = ((v >>  8) & 0xFF) / 255.0f;
            c.b = ( v        & 0xFF) / 255.0f;
            c.a = 1.0f;

            /* Brightness limit: scale saturated cells down so
               the ambient layer never drives LEDs at full
               tilt. */
            const float lum =
                0.2126f * c.r + 0.7152f * c.g +
                0.0722f * c.b;
            if(lum > max_lum_)
            {
                const float k = max_lum_ / lum;
                c.r *= k; c.g *= k; c.b *= k;
            }

            studio::ColorF& cell = cells_[i];
            cell.r += (c.r - cell.r) * smoothing_;
            cell.g += (c.g - cell.g) * smoothing_;
            cell.b += (c.b - cell.b) * smoothing_;
        }

        SetStatus("screen: sampling " + displays[idx].name);
        if(bus_ != nullptr)
            bus_->SetScreenGrid(kCols, kRows, cells_);
    }

    studio::InputBus*      bus_ = nullptr;
    std::mutex             lifecycle_mu_; /* Start/Stop body */
    std::thread            thread_;
    std::atomic<bool>      running_      { false };
    std::atomic<bool>      paused_       { false };
    std::atomic<int>       screen_index_ { 0 };
    mutable std::mutex     status_mu_;
    std::string            status_       = "screen: off";
    std::mutex             cells_mu_;
    std::vector<studio::ColorF> cells_;
    float                  max_lum_   = 0.85f;
    float                  smoothing_ = 0.35f;
};

} /* namespace */

std::unique_ptr<IReactiveSource> MakeScreenSamplerSource()
{
    return std::make_unique<WinScreenSampler>();
}

std::vector<ReactiveDisplay> EnumerateDisplays()
{
    const std::vector<DisplayRect> rects = DisplayRects();
    std::vector<ReactiveDisplay> out;
    out.reserve(rects.size());
    for(size_t i = 0; i < rects.size(); i++)
    {
        ReactiveDisplay d;
        d.index  = (int)i;
        d.name   = rects[i].name;
        d.width  = (int)(rects[i].rect.right -
                         rects[i].rect.left);
        d.height = (int)(rects[i].rect.bottom -
                         rects[i].rect.top);
        out.push_back(std::move(d));
    }
    return out;
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
class StubScreen final : public IReactiveSource
{
public:
    bool Start(studio::InputBus*) override
    {
        status_ = "screen: unsupported platform";
        return false;
    }
    void        Stop()    override               {}
    bool        Running() const override         { return false; }
    std::string Status() const override          { return status_; }
private:
    std::string status_ = "screen: unsupported platform";
};
} /* namespace */

std::unique_ptr<IReactiveSource> MakeScreenSamplerSource()
{
    return std::make_unique<StubScreen>();
}

std::vector<ReactiveDisplay> EnumerateDisplays()
{
    return {};
}

} /* namespace platform */
} /* namespace openrgb2 */

#endif
