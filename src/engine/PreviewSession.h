/*---------------------------------------------------------*\
|| PreviewSession.h                                         ||
||                                                          ||
||   Bounded draft-preview session (studio-import Task 7, ||
||   plan section 3.4): evaluates an UNSAVED workspace    ||
||   candidate through the exact machinery output uses —  ||
||   the shared PrepareStudioScene prepared form, the     ||
||   deterministic studio::EffectEngine evaluator, the    ||
||   PositionStudioInput key lookup, and the DeviceMapper ||
||   brightness scale — and publishes honest per-object   ||
||   color snapshots with completeness metadata.          ||
||                                                          ||
||   The boundary is structural, not conventional:        ||
||   - no IFrameSink, no SDK pointer, no transport        ||
||     reference — nothing here can put a frame on a wire ||
||   - no reactive-input ownership: callers pass a        ||
||     Snapshot(); a preview NEVER starts a source        ||
||   - no scene-clock ownership: Sample takes the scene   ||
||     time, so the caller (the IPC publish tick)         ||
||     decides cadence — the 30 Hz cap lives there        ||
||   - authored data is never mutated: the prepared doc   ||
||     and runtime layers are the same immutable copies   ||
||     SceneWorker::SubmitPreparedScene consumes          ||
||                                                          ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#pragma once

#include "EngineTypes.h"
#include "StudioScenePreparation.h"

#include "config/WorkspaceCandidate.h"
#include "effects/EffectEngine.h"
#include "scene/SceneTypes.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace openrgb2
{

/*---------------------------------------------------------*\
|| PreviewObjectColors — one object's preview slice.      ||
||   indices empty => colors are the dense 0..emitter-1   ||
||   array; a reduced frame lists the sampled indices so  ||
||   a sparse result is never mistaken for a complete one.||
\*---------------------------------------------------------*/
struct PreviewObjectColors
{
    std::string           object_id;
    std::string           parent_id;           /* placement owner */
    std::string           geometry;            /* body mesh hint  */
    std::string           kind;                /* "device"/"linked"/"decor" */
    studio::Vec3          position;            /* local transform */
    studio::Vec3          rotation_deg;
    studio::Vec3          size_m;              /* ResolvedBodySize */
    bool                  bound    = false;    /* resolved binding  */
    bool                  verified = false;    /* output-eligible   */
    size_t                emitter_total = 0;   /* full size   */
    std::vector<uint32_t> indices;             /* sparse map  */
    std::vector<RgbColor> colors;              /* evaluated   */
};

struct PreviewFrame
{
    bool     valid          = false;
    uint64_t sequence       = 0;   /* per-session, monotonic  */
    uint64_t draft_revision = 0;   /* echoes SetCandidate     */
    double   scene_time_s   = 0.0; /* t handed to Evaluate    */
    std::vector<PreviewObjectColors> objects;
    size_t   object_total   = 0;
    size_t   emitter_total  = 0;   /* full scene emitter size */
    size_t   emitter_sent   = 0;   /* colors actually carried */
    bool     reduced        = false;
};

class PreviewSession
{
public:
    struct Options
    {
        /* Total emitters above which the frame flips to
           reduced detail (geometry + strided color sample,
           honestly labelled — plan 3.4: "show geometry plus
           a clearly labelled reduced preview, never an
           apparently complete truncated frame"). */
        size_t display_budget_leds = 4096;
    };

    struct Deps
    {
        /* Read-only live topology, consumed once per candidate
           application — bindings resolve by persisted identity
           (PrepareStudioScene), never a controller index. */
        std::function<std::vector<DeviceSnapshot>()> snapshots;
    };

    explicit PreviewSession(Deps deps, Options options = Options{});

    PreviewSession(const PreviewSession&)            = delete;
    PreviewSession& operator=(const PreviewSession&) = delete;

    /* Latest-wins draft replacement: the newest candidate
       replaces any not-yet-applied one — a burst of edits
       prepares only the last. The candidate is immutable;
       nothing here mutates authored data. Thread-safe. */
    bool SetCandidate(std::shared_ptr<const WorkspaceCandidate> cand,
                      uint64_t draft_revision,
                      std::string& err);
    void SetVisible(bool visible);
    void Stop();
    bool Active() const;
    bool Visible() const;

    /* Evaluate one preview frame at scene time `t` (seconds —
       the caller supplies the same monotonic timeline the
       scene clock uses; the session owns NO clock). `input`
       is a transient snapshot — nullptr or an empty state
       contributes nothing, matching the worker's disabled-
       input behavior.
       Returns false while hidden, stopped, never loaded, or
       while the pending candidate fails to prepare (Error()
       reports why; the session stays active and a later
       candidate clears it). Single-caller — the owner loop's
       publish tick. */
    bool Sample(double scene_time_s,
                const studio::InputState* input,
                PreviewFrame& out);

    /* State reads for status events (thread-safe). */
    uint64_t    DraftRevision() const;
    uint64_t    Sequence() const;
    std::string Error() const;

private:
    Deps    deps_;
    Options options_;

    mutable std::mutex    mutex_;
    /* Depth-1 pending slot — replaced wholesale by the next
       SetCandidate, applied at the next Sample. */
    std::shared_ptr<const WorkspaceCandidate> pending_cand_;
    uint64_t              pending_revision_   = 0;
    bool                  has_pending_        = false;
    bool                  visible_            = true;
    bool                  stopped_            = false;
    bool                  active_             = false;

    /* Applied state — Sample-only, prepared lazily so a stale
       pending candidate is never expanded. */
    PreparedStudioScene   prepared_;
    uint64_t              applied_revision_   = 0;
    std::string           error_;
    uint64_t              sequence_           = 0;

    studio::EffectEngine  engine_;
};

} /* namespace openrgb2 */
