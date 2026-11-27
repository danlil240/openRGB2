/*---------------------------------------------------------*\
| RGBController_GigabyteRGBFusion2BlackwellGPU.h            |
|                                                           |
|   RGBController for Gigabyte RGB Fusion 2 Blackwell GPU   |
|                                                           |
|   This file is part of the OpenRGB project                |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include "RGBController.h"
#include "GigabyteRGBFusion2BlackwellGPUController.h"

class RGBController_RGBFusion2BlackwellGPU : public RGBController
{
public:
    typedef std::chrono::steady_clock::time_point   TimePoint;
    typedef std::function<TimePoint()>              ClockFn;

    RGBController_RGBFusion2BlackwellGPU(RGBFusion2BlackwellGPUController* controller_ptr, uint8_t led_layout);
    ~RGBController_RGBFusion2BlackwellGPU();

    void        SetupZones();

    void        DeviceUpdateLEDs();
    void        DeviceUpdateZoneLEDs(int zone);
    void        DeviceUpdateSingleLED(int led);

    void        DeviceUpdateMode();
    void        DeviceSaveMode();

    /*-----------------------------------------------------*\
    | Settle step: one smoothing sweep toward the current   |
    | targets, run by the settle thread while the filter    |
    | has not converged and no frame arrived for one settle |
    | interval. Returns true while still unsettled. Public  |
    | so offline tests can drive it deterministically.      |
    \*-----------------------------------------------------*/
    bool        SettleStep();

    /*-----------------------------------------------------*\
    | Test hook: replace the clock and stop the background  |
    | settle thread (the test then calls SettleStep).       |
    \*-----------------------------------------------------*/
    void        SetClockForTest(ClockFn clock);

private:
    RGBFusion2BlackwellGPUController* controller;
    uint8_t     gpu_layout;

    /*-----------------------------------------------------*\
    | Temporal smoothing state (firmware chase-the-target   |
    | flicker workaround): last smoothed channel value per  |
    | hardware zone and LED slot, and when each zone last   |
    | stepped (the filter is time-based). Guarded by        |
    | sweep_mutex: DeviceUpdateLEDs runs on the device call |
    | thread, the network thread (zone/single-LED updates)  |
    | and the settle thread. See Sweep.                     |
    \*-----------------------------------------------------*/
    std::mutex                          sweep_mutex;
    double                              smoothed[RGB_FUSION_2_BLACKWELL_GPU_NUMBER_OF_ZONES][RGB_FUSION_2_BLACKWELL_AORUS_MASTER_FAN_LEDS][3] = {};
    bool                                has_smoothed[RGB_FUSION_2_BLACKWELL_GPU_NUMBER_OF_ZONES] = {};
    TimePoint                           last_step[RGB_FUSION_2_BLACKWELL_GPU_NUMBER_OF_ZONES] = {};
    TimePoint                           last_sweep{};
    TimePoint                           last_throttle_sweep{};
    bool                                force_write = false;
    bool                                unsettled   = false;
    bool                                throttle_dropped = false;   /* diagnostic throttle dropped a frame */
    ClockFn                             clock_fn;

    /*-----------------------------------------------------*\
    | Settle thread: keeps sweeping after frames stop until |
    | every channel is within 0.5 of its target.            |
    \*-----------------------------------------------------*/
    std::mutex                          settle_mutex;
    std::condition_variable             settle_cv;
    bool                                settle_wake = false;
    bool                                settle_stop = false;
    std::thread                         settle_thread;

    void        Sweep(bool throttle);
    void        SweepLocked(bool throttle);         /* caller holds sweep_mutex */
    bool        SettleSweepLocked(bool require_idle); /* caller holds sweep_mutex */
    void        SettleThreadFunction();
    void        StopSettleThread();
};
