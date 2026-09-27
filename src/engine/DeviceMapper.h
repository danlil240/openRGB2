/*---------------------------------------------------------*\
|| DeviceMapper.h                                            |
||                                                           |
||   Device mapping and identity resolution (plan sections  |
||   2.1, 4, and 6): turns the scene worker's evaluated     |
||   frame (per-scene-object colors) into per-controller    |
||   RgbFrames for the delivery pipeline.                   |
||                                                           |
||   Identity:                                              |
||   - scene DeviceBindings are the PERSISTED identities    |
||     (name/vendor/serial/location + zone name — never a   |
||     controller-list index). SyncDevices re-resolves      |
||     every binding against the CURRENT snapshots on       |
||     each enumeration/reconnect, so reordered             |
||     controllers can never misroute colors.               |
||   - A binding whose device disappears stays bound to     |
||     its identity: it reports unresolved until the SAME   |
||     device returns — it is never reassigned to a         |
||     same-named unit with a different serial/location     |
||     (plan section 6, "preserve missing-device           |
||     profiles").                                          |
||                                                           |
||   Writes:                                                |
||   - ONE complete-controller RgbFrame per controller —    |
||     the wireless group's three fan-zone edits assemble   |
||     into a single 120-LED frame, never three uploads     |
||     (plan section 4).                                    |
||   - Unmapped LEDs keep their colors: the frame starts    |
||     from the last emitted frame for that controller +    |
||     generation, else the enumerated snapshot colors,     |
||     then mapped writes are overlaid. Unmapped zones      |
||     (motherboard LED_C / Chipset Accent, keyboard        |
||     Extras) are simply never addressed — they appear     |
||     in snapshots but receive no writes.                  |
||   - Mirrored fans are Linked scene objects sharing the   |
||     bound object's emitter group: they show identical    |
||     colors and never multiply the addressable LED        |
||     count (8/16/8 headers stay 8/16/8).                  |
||                                                           |
||   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#pragma once

#include "EngineTypes.h"
#include "SceneWorker.h" /* EvaluatedFrame */

#include "scene/BindingResolver.h"
#include "scene/SceneTypes.h"

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace openrgb2
{

/*---------------------------------------------------------*\
|| IsWirelessGroupController — the Lian Li wireless service |
|| owns ONE full-group LED buffer; its controllers report a |
|| "Wireless:" location prefix (the service's own transport |
|| identity — vendor/OpenRGB RGBController_LianLiWireless   |
|| sets location = "Wireless: " + MAC). This is the         |
|| device-aware qualification for per-controller budgets:   |
|| the group's measured ceiling is ~33.5 admitted           |
|| uploads/s (docs/benchmarks/wireless-result.md).          |
\*---------------------------------------------------------*/
inline bool IsWirelessGroupController(const DeviceSnapshot& snap)
{
    return snap.key.location.rfind("Wireless:", 0) == 0;
}

/*---------------------------------------------------------*\
|| ZonePlacement — one resolved binding: which controller's |
|| complete-controller buffer, which zone, and where that   |
|| zone starts inside it. zone_index/start_idx are runtime  |
|| resolution results, never persisted.                     |
\*---------------------------------------------------------*/
struct ZonePlacement
{
    DeviceKey   controller;           /* controller-level key   */
    uint32_t    zone_index = 0;
    uint32_t    start_idx  = 0;       /* zone's first LED       */
    uint32_t    led_count  = 0;
    std::string reason;               /* resolver note          */
};

class DeviceMapper
{
public:
    DeviceMapper() = default;

    DeviceMapper(const DeviceMapper&)            = delete;
    DeviceMapper& operator=(const DeviceMapper&) = delete;

    /* The scene document supplies the persisted bindings
       (doc->bindings) and the object/emitter layout. Shared
       immutable like the worker's scene slot; nullptr clears. */
    void                                        SetScene(
        std::shared_ptr<const studio::SceneDocument> doc);
    std::shared_ptr<const studio::SceneDocument> Scene() const;

    /* Re-resolve every persisted binding against the current
       snapshots. Call on every enumeration/topology change —
       resolution results hold DeviceKeys + zone indexes only,
       never controller-list indices. */
    void SyncDevices(const std::vector<DeviceSnapshot>& snapshots);

    /* Assemble complete-controller frames from one evaluated
       scene frame: per controller a baseline of preserved
       colors (last emitted frame for this topology
       generation, else the enumerated snapshot colors) with
       mapped emitter writes overlaid in document order.
       Emits one RgbFrame per controller that received at
       least one mapped write — stamped with the current
       topology generation and a per-controller monotonic
       sequence; sample_time passes through. Controllers
       whose snapshot reports Disconnected are skipped
       (obsolete frames stop on disconnect, plan section 6).
       No scene / no bindings -> empty. */
    std::vector<RgbFrame> MapFrame(const EvaluatedFrame& eval);

    /*---- LED identification (Task 4.4, LAYOUT_COMMAND      --*\
    |  "identify"): a bounded per-controller overlay. While   |
    |  active, MapFrame emits a blink pattern for the target  |
    |  controller — every LED dark except the identified      |
    |  range, which alternates lit/dark on a 250 ms phase     |
    |  derived from each frame's sample_time (deterministic).  |
    |  The overlay produces a frame even when NO binding maps |
    |  to the controller (identification exists to FIND the    |
    |  hardware before it is bound) and even when no scene is  |
    |  loaded; it travels the normal delivery lane like any    |
    |  frame, so lane suppression/budgets stay authoritative.  |
    |  Expiry is checked against sample_time — a stalled       |
    |  worker simply ends identification on its next frame,    |
    |  emitting ONE restore frame on the pre-identify          |
    |  baseline so the blink can't freeze the LEDs on a lit    |
    |  phase when no scene frame follows. led_start is         |
    |  absolute in the complete-controller buffer; the caller  |
    |  resolves zone offsets.                                  |
    \*---------------------------------------------------------*/
    bool StartIdentify(const DeviceKey& controller,
                       uint32_t led_start, uint32_t led_count,
                       Clock::duration duration, TimePoint now,
                       std::string& err);
    bool CancelIdentify(const DeviceKey& controller);
    bool Identifying(const DeviceKey& controller,
                     TimePoint now) const;

    /* True while ANY identification overlay is still booked —
       including an expired-but-unpruned one, so the frame path
       stays awake for the prune+restore tick that ends it.
       EngineService wires this as the worker's idle-frame
       hook: the blink must reach hardware BEFORE a scene
       exists to bind it (docs/ipc-protocol.md §5). */
    bool IdentifyActive() const;

    /*---- Introspection (diagnostics/tests) ----------------*\
    | BindingState: the persisted binding's latest resolver   |
    | status (Resolved/Unresolved/Ambiguous — Ambiguous is    |
    | never silently picked). false = binding id unknown.     |
    | BoundZone: the resolved placement when Resolved.        |
    | BoundControllers: controller keys with >= 1 resolved    |
    | binding — the devices live output is qualified for.     |
    \*---------------------------------------------------------*/
    bool                    BindingState(const std::string& binding_id,
                                         studio::BindingStatus& out) const;
    bool                    BoundZone(const std::string& binding_id,
                                      ZonePlacement& out) const;
    std::vector<DeviceKey>  BoundControllers() const;

private:
    struct Resolution
    {
        studio::BindingStatus status = studio::BindingStatus::Unresolved;
        ZonePlacement         zone;
        std::string           reason;
    };

    /* Per-controller emitted state: the baseline for the next
       complete-controller frame and the sequence space. The
       generation tag invalidates it across topology changes —
       a new generation re-seeds from the enumerated colors. */
    struct Emitted
    {
        uint64_t             generation = 0;
        uint64_t             seq        = 0;
        std::vector<RgbColor> colors;
    };

    /* One active identification overlay. */
    struct Identify
    {
        uint32_t  led_start = 0;
        uint32_t  led_count = 0;
        TimePoint start;
        TimePoint until;
    };

    void ApplyIdentify(std::vector<RgbFrame>& out,
                       TimePoint sample_time);

    mutable std::mutex                              mutex_;
    std::shared_ptr<const studio::SceneDocument>    doc_;
    std::map<DeviceKey, DeviceSnapshot>             snapshots_;
    std::map<std::string, Resolution>               resolved_;
    std::map<DeviceKey, Emitted>                    emitted_;
    std::map<DeviceKey, Identify>                   identify_;
};

} /* namespace openrgb2 */
