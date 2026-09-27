/*---------------------------------------------------------*\
||| LayoutEditModel.h                                         ||
|||                                                           ||
|||   Qt-free edit model for the Layout view (Task 4.4,   ||
|||   plan section 5): a read-model over the APPLIED       ||
|||   SceneDocument (GET_SCENE / scene-event document)     ||
|||   joined with engine topology snapshots, plus the op   ||
|||   builders that emit LAYOUT_COMMAND bodies             ||
|||   (docs/ipc-protocol.md §5) and the bounded undo/redo  ||
|||   history.                                             ||
|||                                                           ||
|||   Unlike the Lighting view's pending-save working      ||
|||   copy, layout ops apply immediately — each builder    ||
|||   returns the complete command body; the controller    ||
|||   sends it, and the engine's scene event echoes the    ||
|||   new applied document back into this model. Undo is   ||
|||   whole-document snapshot reversion through SET_SCENE  ||
|||   (the honest inverse for ops like set_mirror that     ||
|||   destroy emitter/binding data the revert verb cannot  ||
|||   express).                                            ||
|||                                                           ||
|||   Validation mirrored at this boundary: emitter        ||
|||   addresses are range-checked against the known zone   ||
|||   LED bound BEFORE a body is produced — the engine     ||
|||   validates again on apply (calibration report, plan   ||
|||   section 5: reject out-of-range at every boundary).   ||
|||   Nothing here touches hardware or the wire.           ||
|||                                                           ||
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace openrgb2
{
namespace ui
{

/*---------------------------------------------------------*\
||| LayoutItem — one renderable scene object for the      ||
||| arrangement canvas. `state` is the display-hint join  ||
||| of the object's binding against live snapshots        ||
||| ("resolved" | "missing" | "ambiguous" | "unbound" |   ||
||| "linked" | "") — the engine's resolver is the         ||
||| authority; this is only for painting badges.          ||
||| Footprint: authored size_m x/z when positive, else    ||
||| the emitter local-position bounding box, else a       ||
||| default body — provenance stays with the document,    ||
||| never invented dimensions.                            ||
\*---------------------------------------------------------*/
struct LayoutItem
{
    std::string id;
    std::string label;
    std::string kind;        /* "device"|"linked"|"decor"|"group" */
    double      x = 0, y = 0, z = 0;     /* position, meters     */
    double      rx = 0, ry = 0, rz = 0;  /* euler XYZ, degrees   */
    double      w = 0, d = 0;            /* footprint x/z, meters */
    std::string binding;
    std::string mirror_of;
    std::string geometry;
    int         emitter_count   = 0;
    int         addressed_count = 0;     /* emitters with addr>=0 */
    bool        verified = false;
    bool        visible  = true;
    std::string state;                   /* join result, above   */
    bool        connected = false;
    /* From the object's binding (document data). */
    std::string controller_name;
    std::string zone_name;
    uint32_t    zone_leds = 0;
};

/* One LAYOUT_COMMAND issue — `body` is the COMPLETE request
   body including "verb". */
struct LayoutOp
{
    std::string    label;      /* undo text */
    nlohmann::json body;
};

/*---------------------------------------------------------*\
||| LayoutEditModel                                        ||
\*---------------------------------------------------------*/
class LayoutEditModel
{
public:
    /* Bound on each history stack — one entry is a document
       snapshot (~KBs); 32 edits of headroom without
       unbounded growth (plan section 6: bound everything). */
    static constexpr size_t kHistoryBound = 32;

    /* The applied document the engine last reported
       (bridge `scene` store's `document` — null clears).
       History stacks are kept: entries hold their own
       snapshots, and the engine re-validates any revert. */
    void LoadApplied(const nlohmann::json& doc_or_null);
    /* The topology snapshots the bridge holds (raw JSON
       entries from SNAPSHOTS/topology events). */
    void SetSnapshots(const std::vector<nlohmann::json>& snaps);

    bool                  HasScene() const { return applied_.is_object(); }
    const nlohmann::json& Applied() const  { return applied_; }

    /*---- views ----------------------------------------------
       Items(): every object in document order — decor and
       group nodes included so the canvas can show the case/
       desk frame. */
    std::vector<LayoutItem> Items() const;
    /* Objects eligible as a mirror source: kind "device",
       excluding `exclude_id`. */
    std::vector<std::string> LinkSources(
        const std::string& exclude_id) const;
    /* The document's bindings as a json array (pickers). */
    nlohmann::json Bindings() const;
    /* One object's emitters as a json array
       [{index,address,group}] (calibration list). */
    nlohmann::json ObjectEmitters(const std::string& id) const;
    /* The LED bound calibration must respect for `id`:
       the binding's declared zone_leds, else the joined
       snapshot zone's enumerated count, else 0 (unknown —
       non-negative addresses are refused). */
    uint32_t BoundZoneLeds(const std::string& id) const;
    const nlohmann::json* FindObject(const std::string& id) const;

    /*---- op builders (false + err on invalid input; NO
           body is produced for refused input — the UI is
           the first rejection boundary) ------------------*/
    bool BuildSetPosition(const std::string& id,
                          double x, double y, double z,
                          LayoutOp& out, std::string& err) const;
    bool BuildSetOrientation(const std::string& id,
                             double rx, double ry, double rz,
                             LayoutOp& out, std::string& err) const;
    /* target "" = unlink. */
    bool BuildSetMirror(const std::string& id,
                        const std::string& target,
                        LayoutOp& out, std::string& err) const;
    /* Point `id` at an EXISTING binding id, or "" to unbind. */
    bool BuildSetBinding(const std::string& id,
                         const std::string& binding_id,
                         LayoutOp& out, std::string& err) const;
    /* Point `id` at a NEW binding the engine derives from
       `controller_key` ({device,serial,location,zone}). */
    bool BuildSetBindingFromController(
        const std::string& id, const std::string& new_binding_id,
        const nlohmann::json& controller_key,
        LayoutOp& out, std::string& err) const;
    /* address: -1 = render-only, else < the bound zone's LED
       count — out-of-range is refused HERE. */
    bool BuildCalibrateLed(const std::string& id,
                           int emitter_index, int address,
                           LayoutOp& out, std::string& err) const;

    /*---- undo/redo (whole-document snapshot reversion) ------
       PushUndo snapshots Applied() at issue time under
       `label` and clears the redo stack (a new edit is a new
       timeline). The controller calls DropUndo when the
       issue itself failed so refused ops leave no history.
       Peek/Pop/Push drive the symmetric undo<->redo entry
       movement on confirmed replies. */
    struct HistoryEntry
    {
        std::string    label;
        nlohmann::json doc;    /* the document to restore */
    };
    void PushUndo(const std::string& label);
    void DropUndo();                    /* issue failed       */
    void PushRedo(const std::string& label,
                  const nlohmann::json& doc);
    void PushUndoEntry(const std::string& label,
                       const nlohmann::json& doc);
    const HistoryEntry* PeekUndo() const;
    const HistoryEntry* PeekRedo() const;
    void PopUndo();
    void PopRedo();
    void ClearHistory();
    size_t UndoCount() const { return undo_.size(); }
    size_t RedoCount() const { return redo_.size(); }

private:
    const nlohmann::json* FindBinding(const std::string& id) const;
    /* Display-hint snapshot join (mirrors the controller's
       NameMatches convention — the engine resolves
       authoritatively). Returns matches count and fills
       `first` with the best match's connection/leds. */
    int MatchSnapshots(const nlohmann::json& binding,
                       const nlohmann::json** first) const;

    nlohmann::json              applied_ = nullptr;
    std::vector<nlohmann::json> snapshots_;
    std::deque<HistoryEntry>    undo_;
    std::deque<HistoryEntry>    redo_;
};

} /* namespace ui */
} /* namespace openrgb2 */
