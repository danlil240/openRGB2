// SPDX-License-Identifier: GPL-2.0-or-later
//
// PreviewSession (studio-import Task 7, plan section 3.4) —
// see the header for the boundary contract. This file is
// Qt-free like the rest of src/engine.

#include "PreviewSession.h"

namespace openrgb2
{

namespace
{

/* The DeviceMapper brightness scale (DeviceMapper::ScaleColor)
   — preview colors are the bytes output WOULD have carried,
   scaled identically so the draft view and the delivered view
   answer the same math. */
RgbColor PreviewScale(studio::SceneColor c, float brightness)
{
    const RgbColor in = FromSceneColor(c);
    const float b = !(brightness > 0.0f) ? 0.0f
                  : (brightness > 1.0f ? 1.0f : brightness);
    RgbColor out;
    out.r = uint8_t((unsigned)(in.r * b));
    out.g = uint8_t((unsigned)(in.g * b));
    out.b = uint8_t((unsigned)(in.b * b));
    return out;
}

/* The object whose emitters a preview object displays: itself,
   or — for a Linked copy, which the resolver strips of its own
   emitters — its output owner. */
const studio::SceneObject& EmitterSource(
    const studio::SceneDocument& doc, const studio::SceneObject& o)
{
    if(o.kind == studio::ObjectKind::Linked)
    {
        const studio::SceneObject* owner =
            studio::OutputOwner(doc, o.id);
        if(owner != nullptr)
        {
            return *owner;
        }
    }
    return o;
}

/* True for objects the draft view paints: anything carrying
   emitters, directly or through its owner. Linked copies follow
   their owner's frame exactly like the renderer does;
   unverified/unbound Device objects still show — the draft
   preview is where a user checks an UNSAVED layout, so objects
   the output path skips (never verified, binding unresolved)
   must appear here. */
bool Previewable(const studio::SceneDocument& doc,
                 const studio::SceneObject& o)
{
    return o.kind != studio::ObjectKind::Group &&
           !EmitterSource(doc, o).emitters.empty();
}

} /* anonymous namespace */

PreviewSession::PreviewSession(Deps deps, Options options)
    : deps_(std::move(deps)), options_(options)
{
}

bool PreviewSession::SetCandidate(
    std::shared_ptr<const WorkspaceCandidate> cand,
    uint64_t draft_revision, std::string& err)
{
    if(!cand || !cand->resolved)
    {
        err = "preview candidate carries no resolved scene";
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if(stopped_)
    {
        err = "preview session is stopped";
        return false;
    }
    /* Depth-1 latest-wins slot — the replaced draft is
       dropped, never queued (plan 3.4). */
    pending_cand_     = std::move(cand);
    pending_revision_ = draft_revision;
    has_pending_      = true;
    active_           = true;
    return true;
}

void PreviewSession::SetVisible(bool visible)
{
    std::lock_guard<std::mutex> lock(mutex_);
    visible_ = visible;
}

void PreviewSession::Stop()
{
    std::lock_guard<std::mutex> lock(mutex_);
    stopped_      = true;
    active_       = false;
    pending_cand_.reset();
    has_pending_  = false;
    applied_.reset();
    error_.clear();
}

bool PreviewSession::Active() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return active_;
}

bool PreviewSession::Visible() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return visible_;
}

uint64_t PreviewSession::DraftRevision() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return has_pending_ ? pending_revision_ : applied_revision_;
}

uint64_t PreviewSession::Sequence() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return sequence_;
}

std::string PreviewSession::Error() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
}

uint64_t PreviewSession::DraftCacheBuilds() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return cache_builds_;
}

std::shared_ptr<const PreviewSession::AppliedDraft>
PreviewSession::BuildDraft(PreparedStudioScene prepared)
{
    auto d = std::make_shared<AppliedDraft>();
    d->prepared = std::move(prepared);
    const studio::SceneDocument& doc = *d->prepared.doc;
    d->world = studio::ResolveWorldMatrices(doc);
    /* Same object order, Previewable filter, EmitterSource
       redirect and EmitterColor fallback the per-sample loop
       used — only hoisted to the apply. */
    for(const studio::SceneObject& o : doc.objects)
    {
        if(!Previewable(doc, o))
        {
            continue;
        }
        AppliedDraft::Entry e;
        e.obj = &o;
        e.src = &EmitterSource(doc, o);
        switch(o.kind)
        {
        case studio::ObjectKind::Device: e.kind = "device"; break;
        case studio::ObjectKind::Linked: e.kind = "linked"; break;
        case studio::ObjectKind::Decor:  e.kind = "decor";  break;
        default:                         e.kind = "group";  break;
        }
        const size_t n = e.src->emitters.size();
        e.painted.reserve(n);
        for(size_t i = 0; i < n; ++i)
        {
            e.painted.push_back(
                studio::EmitterColor(doc, o.id, (int)i));
        }
        d->emitter_total += n;
        d->entries.push_back(std::move(e));
    }
    return d;
}

bool PreviewSession::Sample(double scene_time_s,
                            const studio::InputState* input,
                            PreviewFrame& out)
{
    out = PreviewFrame{};

    /* Apply the pending candidate — the prepare runs HERE, on
       the publish tick, not on the request thread: a draft
       replaced while hidden is never expanded (latest-wins is
       structural, not just queued-order). */
    std::shared_ptr<const WorkspaceCandidate> to_apply;
    uint64_t to_apply_rev = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(stopped_ || !visible_)
        {
            return false;
        }
        if(has_pending_)
        {
            to_apply      = pending_cand_;
            to_apply_rev  = pending_revision_;
            pending_cand_.reset();
            has_pending_  = false;
        }
    }
    if(to_apply)
    {
        PreparedStudioScene prepared;
        std::string         err;
        const std::vector<DeviceSnapshot> snaps =
            deps_.snapshots ? deps_.snapshots()
                            : std::vector<DeviceSnapshot>{};
        if(!PrepareStudioScene(*to_apply, snaps, prepared, err))
        {
            std::lock_guard<std::mutex> lock(mutex_);
            error_ = err;
            return false;
        }
        /* The per-draft cache is built here, once per applied
           candidate, outside the lock (audit E-8). */
        std::vector<studio::EffectLayer> layers = prepared.layers;
        std::shared_ptr<const AppliedDraft> built =
            BuildDraft(std::move(prepared));
        {
            std::lock_guard<std::mutex> lock(mutex_);
            applied_          = std::move(built);
            applied_revision_ = to_apply_rev;
            ++cache_builds_;
            error_.clear();
        }
        engine_.SetLayers(std::move(layers));
    }

    /* Snapshot the applied state under the lock — a concurrent
       Stop() drops applied_, so the evaluation holds its own
       reference to the immutable draft (document, key lookup,
       matrices, per-object plan) — never a reference into
       member storage, and no per-sample copy of the key map. */
    std::shared_ptr<const AppliedDraft> draft;
    uint64_t rev;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(!active_ || !applied_ || !applied_->prepared.doc)
        {
            return false;
        }
        rev   = applied_revision_;
        draft = applied_;
    }
    const studio::SceneDocument& doc = *draft->prepared.doc;

    /* Transient key events position through THIS draft's key
       lookup — the positioned copy stays local to the sample,
       exactly like SceneWorker::Step. Disabled/absent inputs
       contribute an empty state: preview NEVER starts a
       source, it consumes the snapshot it is handed. */
    const studio::InputState* in = input;
    studio::InputState        positioned;
    if(in != nullptr && !in->events.empty() &&
       !draft->prepared.key_positions.empty())
    {
        positioned = PositionStudioInput(*in, draft->prepared);
        in = &positioned;
    }

    studio::FrameColors eval;
    engine_.Evaluate(doc, draft->world, scene_time_s, eval, in);

    PreviewFrame f;
    f.valid          = true;
    f.scene_time_s   = scene_time_s;
    f.draft_revision = rev;

    /* emitter_total decides dense-vs-reduced for the WHOLE
       frame (cached per draft), so a reduced frame's stride
       is uniform and honest across objects. */
    const size_t emitter_total = draft->emitter_total;
    const bool reduced =
        emitter_total > options_.display_budget_leds;
    const size_t stride =
        reduced && emitter_total > 0
            ? (emitter_total + options_.display_budget_leds - 1)
                  / options_.display_budget_leds
            : 1;

    f.reduced        = reduced;
    f.emitter_total  = emitter_total;

    f.objects.reserve(draft->entries.size());
    for(const AppliedDraft::Entry& ent : draft->entries)
    {
        const studio::SceneObject& o   = *ent.obj;
        const studio::SceneObject& src = *ent.src;
        PreviewObjectColors pc;
        pc.object_id     = o.id;
        pc.parent_id     = o.parent_id;
        pc.geometry      = o.geometry;
        pc.position      = o.transform.position;
        pc.rotation_deg  = o.transform.rotation_deg;
        pc.size_m        = studio::ResolvedBodySize(o);
        pc.bound         = !o.binding.empty();
        pc.verified      = o.verified;
        pc.emitter_total = src.emitters.size();
        pc.kind          = ent.kind;

        /* The evaluated frame is keyed by the OUTPUT owner —
           a linked copy displays its owner's evaluation; an
           emitter no layer touched shows its painted color. */
        const auto fit = eval.find(src.id);
        const std::vector<studio::SceneColor>* evald =
            fit != eval.end() ? &fit->second : nullptr;
        auto color_at = [&](size_t e)
        {
            const studio::SceneColor c =
                (evald != nullptr && e < evald->size())
                    ? (*evald)[e]
                    : ent.painted[e];
            return PreviewScale(c, doc.brightness);
        };

        const size_t n = src.emitters.size();
        if(reduced)
        {
            pc.indices.reserve(n / stride + 1);
            pc.colors.reserve(n / stride + 1);
            for(size_t e = 0; e < n; e += stride)
            {
                pc.indices.push_back((uint32_t)e);
                pc.colors.push_back(color_at(e));
            }
        }
        else
        {
            pc.colors.reserve(n);
            for(size_t e = 0; e < n; ++e)
            {
                pc.colors.push_back(color_at(e));
            }
        }
        f.emitter_sent += pc.colors.size();
        f.objects.push_back(std::move(pc));
    }
    f.object_total = f.objects.size();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        f.sequence = ++sequence_;
    }
    out = std::move(f);
    return true;
}

} /* namespace openrgb2 */
