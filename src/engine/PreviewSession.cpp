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

/* True for objects the draft view paints: anything carrying
   emitters. Linked copies follow their owner's frame exactly
   like the renderer does; unverified/unbound Device objects
   still show — the draft preview is where a user checks an
   UNSAVED layout, so objects the output path skips (never
   verified, binding unresolved) must appear here. */
bool Previewable(const studio::SceneObject& o)
{
    return !o.emitters.empty() &&
           o.kind != studio::ObjectKind::Group;
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
    prepared_     = PreparedStudioScene{};
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
        {
            std::lock_guard<std::mutex> lock(mutex_);
            prepared_         = std::move(prepared);
            applied_revision_ = to_apply_rev;
            error_.clear();
        }
        engine_.SetLayers(prepared_.layers);
    }

    /* Snapshot the applied state under the lock — a concurrent
       Stop() clears prepared_, so the evaluation must hold the
       shared document + key lookup itself, never a reference
       into member storage. */
    std::shared_ptr<const studio::SceneDocument> doc_sp;
    std::map<int, studio::Vec3>                  key_positions;
    uint64_t rev;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(!active_ || !prepared_.doc)
        {
            return false;
        }
        rev           = applied_revision_;
        doc_sp        = prepared_.doc;
        key_positions = prepared_.key_positions;
    }
    const studio::SceneDocument& doc = *doc_sp;

    /* Transient key events position through THIS draft's key
       lookup — the prepared copy stays local to the sample,
       exactly like SceneWorker::Step. Disabled/absent inputs
       contribute an empty state: preview NEVER starts a
       source, it consumes the snapshot it is handed. */
    const studio::InputState* in = input;
    studio::InputState        positioned;
    if(in != nullptr && !in->events.empty() &&
       !key_positions.empty())
    {
        PreparedStudioScene view;
        view.doc           = doc_sp;
        view.key_positions = key_positions;
        positioned = PositionStudioInput(*in, view);
        in = &positioned;
    }

    studio::FrameColors eval;
    engine_.Evaluate(doc, scene_time_s, eval, in);

    PreviewFrame f;
    f.valid          = true;
    f.scene_time_s   = scene_time_s;
    f.draft_revision = rev;

    /* Size pass first: emitter_total decides dense-vs-reduced
       for the WHOLE frame, so a reduced frame's stride is
       uniform and honest across objects. */
    size_t emitter_total = 0;
    for(const studio::SceneObject& o : doc.objects)
    {
        if(Previewable(o))
        {
            emitter_total += o.emitters.size();
        }
    }
    const bool reduced =
        emitter_total > options_.display_budget_leds;
    const size_t stride =
        reduced && emitter_total > 0
            ? (emitter_total + options_.display_budget_leds - 1)
                  / options_.display_budget_leds
            : 1;

    f.reduced        = reduced;
    f.emitter_total  = emitter_total;

    for(const studio::SceneObject& o : doc.objects)
    {
        if(!Previewable(o))
        {
            continue;
        }
        PreviewObjectColors pc;
        pc.object_id     = o.id;
        pc.parent_id     = o.parent_id;
        pc.geometry      = o.geometry;
        pc.position      = o.transform.position;
        pc.rotation_deg  = o.transform.rotation_deg;
        pc.size_m        = studio::ResolvedBodySize(o);
        pc.bound         = !o.binding.empty();
        pc.verified      = o.verified;
        pc.emitter_total = o.emitters.size();
        switch(o.kind)
        {
        case studio::ObjectKind::Device: pc.kind = "device"; break;
        case studio::ObjectKind::Linked: pc.kind = "linked"; break;
        case studio::ObjectKind::Decor:  pc.kind = "decor";  break;
        default:                         pc.kind = "group";  break;
        }

        /* The evaluated frame is keyed by the OUTPUT owner —
           a linked copy displays its owner's evaluation. */
        const studio::SceneObject* owner =
            studio::OutputOwner(doc, o.id);
        const std::string owner_id = owner ? owner->id : o.id;
        const auto fit = eval.find(owner_id);

        const size_t n = o.emitters.size();
        if(reduced)
        {
            for(size_t e = 0; e < n; e += stride)
            {
                studio::SceneColor c =
                    studio::EmitterColor(doc, o.id, (int)e);
                if(fit != eval.end() &&
                   e < fit->second.size())
                {
                    c = fit->second[e];
                }
                pc.indices.push_back((uint32_t)e);
                pc.colors.push_back(
                    PreviewScale(c, doc.brightness));
            }
        }
        else
        {
            pc.colors.reserve(n);
            for(size_t e = 0; e < n; ++e)
            {
                studio::SceneColor c =
                    studio::EmitterColor(doc, o.id, (int)e);
                if(fit != eval.end() &&
                   e < fit->second.size())
                {
                    c = fit->second[e];
                }
                pc.colors.push_back(
                    PreviewScale(c, doc.brightness));
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
