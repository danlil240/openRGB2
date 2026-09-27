/*---------------------------------------------------------*\
||| ReactiveInputs.h                                          ||
|||                                                           ||
|||   The engine-side reactive-input manager (Task 4.6 —   ||
|||   plan section 5, Lighting view reactive controls).    ||
|||   Owns the InputBus the scene worker snapshots each    ||
|||   tick and one capture slot per reactive input id      ||
|||   ("audio", "screen", "key").                          ||
|||                                                           ||
|||   Capture sources are IReactiveSource producers        ||
|||   injected per id — production wires the Win32         ||
|||   sources (src/platform/windows/: WASAPI loopback,     ||
|||   GDI screen sampler, WH_KEYBOARD_LL hook); tests      ||
|||   inject fakes and never touch real desktop capture.   ||
|||   A slot with no source reports `available=false` —    ||
|||   enabling it is an honest Unavailable, never a        ||
|||   fabricated toggle.                                   ||
|||                                                           ||
|||   Bounds and gates (plan section 6):                   ||
|||   - disabled inputs capture NOTHING — the source is    ||
|||     stopped, so there is no hidden CPU/device cost     ||
|||   - capture_paused (PowerLifecycle's display-off /     ||
|||     suspend / post-wake gate — the Task 3.7 atomic     ||
|||     finally has its reader): paused sources skip       ||
|||     their capture work AND Snapshot() returns an       ||
|||     empty state, so events stop reaching scenes        ||
|||   - InputBus queues are already bounded (96 events,    ||
|||     12x6 screen grid); capture rates are bounded by    ||
|||     the sources themselves (audio ~5 ms energy         ||
|||     chunks, screen ~25 Hz, keys on-demand)             ||
|||   - privacy: capture stays inside the engine process — ||
|||     only energies, a 12x6 color grid, and VK codes     ||
|||     ever cross a boundary (no audio samples, no        ||
|||     framebuffer contents, no typed text)               ||
|||                                                           ||
|||   Qt-free. Thread-safe: enable/disable arrives on IPC  ||
|||   session threads, pause on the owner loop, Snapshot   ||
|||   on the scene worker.                                 ||
|||                                                           ||
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#pragma once

#include "inputs/InputBus.h"

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace openrgb2
{

/*---------------------------------------------------------*\
||| IReactiveSource — one capture producer. Implementations||
||| live in src/platform/windows/ (production) and in the  ||
||| engine test suite (fakes). Start/Stop own the capture  ||
||| thread; a stopped source must leave NO capture         ||
||| machinery running. SetPaused is the display-off gate:  ||
||| the source skips its capture work while paused (the    ||
||| screen sampler in particular must not read the GPU     ||
||| while displays sleep) but may keep its thread alive    ||
||| for a fast resume.                                     ||
\*---------------------------------------------------------*/
class IReactiveSource
{
public:
    virtual ~IReactiveSource() = default;

    /* Attach to the bus and start capturing. Real sources
       run a cheap synchronous PRE-FLIGHT first (endpoint
       enumeration, hook install probe, first grab — see the
       Windows implementations) so a genuinely unavailable
       source refuses here instead of reporting OK and
       failing on the capture thread later. False means the
       source could not start — Status() carries the honest
       reason ("no default output device", "unsupported
       platform", ...). Conditions only detectable mid-
       capture (device removed, display gone) still surface
       asynchronously through Status(). A source that
       self-terminated (running=false with the thread still
       joinable) must be reaped here BEFORE respawning —
       move-assigning onto a joinable std::thread is
       std::terminate. */
    virtual bool        Start(studio::InputBus* bus) = 0;
    /* Stop capturing and release resources (joins the
       capture thread). Safe when not running — and must
       still join a thread that already self-terminated, so
       no joinable thread is ever left for the destructor. */
    virtual void        Stop()                       = 0;
    virtual bool        Running() const              = 0;
    /* Last state/error for honest UI reporting. */
    virtual std::string Status() const               = 0;

    /* Display-off/suspend gate — skip capture work while
       paused. Default no-op for sources that need none. */
    virtual void        SetPaused(bool /*paused*/)   {}
    /* Audio sensitivity 0.25..2.0 (UI percent 25..200). */
    virtual void        SetSensitivity(float /*s*/)  {}
    /* Screen source: which display to sample. */
    virtual void        SetScreenIndex(int /*index*/){}
};

/*---------------------------------------------------------*\
||| ReactiveDisplay — one enumerated display for the       ||
||| screen-source selection combo.                         ||
\*---------------------------------------------------------*/
struct ReactiveDisplay
{
    int         index  = 0;
    std::string name;
    int         width  = 0;
    int         height = 0;
};

/*---------------------------------------------------------*\
||| Per-input status as reported over IPC (honest — a      ||
||| source that failed to start keeps enabled=false and    ||
||| carries its own status string).                        ||
\*---------------------------------------------------------*/
struct ReactiveInputStatus
{
    std::string id;
    bool        available    = false; /* a source exists     */
    bool        enabled      = false; /* user selection      */
    bool        running      = false; /* actually capturing  */
    bool        paused       = false; /* capture_paused gate */
    float       sensitivity  = 1.0f;
    int         screen_index = 0;
    std::string status;               /* source's own report */
};

class ReactiveInputs
{
public:
    struct Deps
    {
        /* Builds the capture source for an input id
           ("audio"|"screen"|"key"). Empty or returning
           nullptr -> the slot reports available=false. */
        std::function<std::unique_ptr<IReactiveSource>(
            const std::string& id)> source_factory;

        /* Display enumeration for the screen source (the
           status surface and the set_params bounds check).
           Empty -> no list; index is then only range-checked
           against >= 0. */
        std::function<std::vector<ReactiveDisplay>()>
            enumerate_displays;

        /* Scene-clock seconds — event timestamps share the
           worker's t base so event ages evaluate correctly.
           Empty -> the bus stamps 0 (events age out). */
        std::function<double()> now;

        /* Any externally visible state changed (enable,
           running, pause, params) — production emits the
           IPC "reactive" event and persists the selection.
           Called WITHOUT the manager mutex held. */
        std::function<void()> changed;
    };

    explicit ReactiveInputs(Deps deps);
    ~ReactiveInputs();                             /* Shutdown() */

    ReactiveInputs(const ReactiveInputs&)            = delete;
    ReactiveInputs& operator=(const ReactiveInputs&) = delete;

    /* The fixed input ids, in reported order. */
    static const std::vector<std::string>& InputIds();

    /* Sensitivity domain for the audio source (the old
       plugin's 25..200 percent scale). */
    static constexpr float kSensitivityMin = 0.25f;
    static constexpr float kSensitivityMax = 2.0f;

    /* Enable/disable one input. Enabling STARTS the capture
       source — a failure leaves the slot disabled and
       returns false with the source's own reason. Disabling
       stops the source so a disabled input captures
       nothing. Unknown id -> false + err.
       Transitions SERIALIZE per input: a command arriving
       while a Start/Stop for the same input is in flight
       waits for it to settle, then applies on the settled
       state — a disable issued during an in-flight enable
       stops the source right after it starts (last command
       wins, in arrival order). Two commands can therefore
       never interleave inside a source's thread lifecycle:
       exactly one capture thread exists per input. */
    bool SetEnabled(const std::string& id, bool enabled,
                    std::string& err);

    /* Per-input parameters. sensitivity applies to "audio"
       (clamped to the domain); screen_index applies to
       "screen" (bounds-checked against enumerated displays
       when the dep is wired). Wrong input -> false + err. */
    bool SetSensitivity(const std::string& id, float s,
                        std::string& err);
    bool SetScreenIndex(int index, std::string& err);

    /* The capture_paused gate (display off / suspend /
       post-wake restore): paused sources skip capture work
       and Snapshot() returns an empty state — events stop
       reaching scenes. */
    void SetPaused(bool paused);
    bool Paused() const;

    /* One tick's input for the scene worker — null-free:
       the returned state may be EMPTY (paused, or nothing
       captured yet) so reactive primitives contribute
       nothing and the scene falls back to its base layers.
       Events older than the bounded max age are dropped. */
    std::shared_ptr<const studio::InputState> Snapshot();

    std::vector<ReactiveInputStatus> Status() const;
    std::vector<ReactiveDisplay>     Displays() const;
    studio::InputBus*                Bus() { return &bus_; }

    /*---- persistence -------------------------------------*\
    || PersistedInput is what the engine profile records     ||
    || per input. Restore() replays the recorded selection:  ||
    || params apply first, then recorded-enabled inputs      ||
    || start — a source that fails to start reports in       ||
    || Status() rather than lying enabled.                   ||
    \*------------------------------------------------------*/
    struct PersistedInput
    {
        std::string id;
        bool        enabled      = false;
        float       sensitivity  = 1.0f;
        int         screen_index = 0;

        bool operator==(const PersistedInput& o) const
        {
            return id == o.id && enabled == o.enabled &&
                   sensitivity == o.sensitivity &&
                   screen_index == o.screen_index;
        }
        bool operator!=(const PersistedInput& o) const
        {
            return !(*this == o);
        }
    };
    std::vector<PersistedInput> PersistedState() const;
    void                        Restore(
        const std::vector<PersistedInput>& saved);

    /* Ordered-teardown hook: stop every running source
       (idempotent). */
    void Shutdown();

private:
    struct Slot
    {
        std::unique_ptr<IReactiveSource> source;
        bool        enabled      = false;
        /* A Start/Stop is in flight with mutex_ dropped —
           concurrent commands for THIS slot wait on
           transition_cv_ so source thread lifecycles never
           interleave (the SetEnabled TOCTOU fix). */
        bool        busy         = false;
        float       sensitivity  = 1.0f;
        int         screen_index = 0;
    };

    Slot*  Find(const std::string& id);
    void   Changed();

    Deps                    deps_;
    studio::InputBus        bus_;
    mutable std::mutex      mutex_;
    std::condition_variable transition_cv_;
    Slot                    audio_;
    Slot                    screen_;
    Slot                    key_;
    bool                    paused_ = false;
    bool                    suppress_changed_ = false;
};

} /* namespace openrgb2 */
