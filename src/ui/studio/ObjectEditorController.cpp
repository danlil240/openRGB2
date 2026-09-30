/*---------------------------------------------------------*\
|| ObjectEditorController.cpp                               ||
||                                                          ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
|\*---------------------------------------------------------*/

#include "ObjectEditorController.h"

#include "../JsonVariant.h"

#include "editor/TransformCommands.h"
#include "presets/DevicePreset.h"    /* IsPresetId              */
#include "scene/SceneGraph.h"
#include "scene/SceneJson.h"
#include "scene/SceneResolver.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace openrgb2
{
namespace ui
{

namespace
{

/* Mirrors LayoutEditModel's display join: identity match by
   name/serial/location against live snapshots — a display hint
   only; the engine resolves authoritatively. */
bool NameMatches(const QString& wanted, const QString& actual)
{
    if(wanted.isEmpty())
    {
        return true;
    }
    const QString w = wanted.toLower();
    const QString a = actual.toLower();
    return a == w || a.contains(w);
}

QString HexColor(const studio::SceneColor c)
{
    return QString::fromStdString(studio::SceneColorHex(c));
}

} /* namespace */

ObjectEditorController::ObjectEditorController(EngineBridge* bridge,
                                               WorkspaceController* ws,
                                               QObject* parent)
    : QObject(parent)
    , bridge_(bridge)
    , ws_(ws)
{
    /* Preset dirs — the engine's own probe order plus the test env
       override (DLS_PRESET_DIR, same convention as tests/core). */
    std::vector<std::string> candidates;
    if(const char* env = std::getenv("DLS_PRESET_DIR"))
    {
        if(*env != '\0')
        {
            candidates.push_back(env);
        }
    }
    const QString app = QCoreApplication::applicationDirPath();
    candidates.push_back((app + "/presets/devices").toStdString());
    candidates.push_back("presets/devices");
    candidates.push_back("src/core/presets/devices");
    for(const std::string& c : candidates)
    {
        std::vector<std::string> errors;
        types_.LoadDirectory(c, &errors);
        if(types_.FileCount() > 0)
        {
            break;   /* first dir that yields files wins */
        }
    }
    preset_model_.Bind(&types_, nullptr);
    preset_model_.Reload();

    /* The object model re-reads the session's Working() draft and
       the controller-owned resolved snapshot — bound lazily per
       query so a session attaching later needs no rewire. The
       BoundFn joins live snapshots for the tree's status chips. */
    object_model_.Bind(nullptr, &resolved_,
        [this](const std::string& binding_id) -> QString
        {
            return BoundStatusFor(binding_id);
        });

    if(ws_ != nullptr)
    {
        /* The favorites vector lives INSIDE the session's working
           document — rebind the pointer each draft change so the
           model never holds a stale address... the session object
           owns Working() by value so the pointer is in fact stable;
           we still re-bind for clarity at every ready transition. */
        preset_model_.Bind(&types_,
            ws_->Session().HasWorking()
                ? &ws_->Session().Working().meta.ui.favorites
                : nullptr);

        connect(ws_, &WorkspaceController::draftEdited,
                this, &ObjectEditorController::OnWorkspaceDraftEdited);
        connect(ws_, &WorkspaceController::draftChanged,
                this, &ObjectEditorController::OnWorkspaceDraftChanged);
        connect(ws_, &WorkspaceController::historyChanged,
                this, [this] { emit undoChanged(); });
        connect(ws_, &WorkspaceController::availabilityChanged,
                this, &ObjectEditorController::OnAvailabilityChanged);
    }
    if(bridge_ != nullptr)
    {
        connect(bridge_, &EngineBridge::replyReceived,
                this, &ObjectEditorController::OnBridgeReply);
        connect(bridge_, &EngineBridge::sceneChanged,
                this, [this]
        {
            /* The applied scene moved — if the rows mirror it
               (unresolvable draft), rebuild; snapshot changes also
               re-derive bound chips. */
            if(resolved_src_ == ResolvedSrc::Applied)
            {
                RebuildResolved();
                object_model_.ResetFrom();
            }
            else
            {
                object_model_.RefreshAll();
            }
            emit sceneChanged();
        });
        connect(bridge_, &EngineBridge::snapshotsChanged,
                this, [this]
        {
            object_model_.RefreshAll();   /* bound chips re-join */
        });
        connect(bridge_, &EngineBridge::connectionStateChanged,
                this, [this]
        {
            if(bridge_->connected())
            {
                RequestCatalog();
            }
        });
        if(bridge_->connected())
        {
            RequestCatalog();
        }
    }
    RefreshAll();
}

/*---------------------------------------------------------*\
|| Session access + resolve                                 ||
\*---------------------------------------------------------*/

WorkspaceEditSession* ObjectEditorController::Session() const
{
    return ws_ != nullptr ? &ws_->Session() : nullptr;
}

studio::EditorController* ObjectEditorController::Editor() const
{
    WorkspaceEditSession* s = Session();
    return (s != nullptr && s->HasWorking()) ? &s->Editor() : nullptr;
}

const studio::StudioDocument* ObjectEditorController::WsDoc() const
{
    WorkspaceEditSession* s = Session();
    return (s != nullptr && s->HasWorking()) ? &s->Working()
                                             : nullptr;
}

bool ObjectEditorController::ready() const
{
    return WsDoc() != nullptr;
}

QVariantList ObjectEditorController::selectedInstances() const
{
    QVariantList out;
    studio::EditorController* e = Editor();
    if(e == nullptr)
    {
        return out;
    }
    for(const std::string& id : e->Selection())
    {
        out.push_back(QString::fromStdString(id));
    }
    return out;
}

QString ObjectEditorController::selectedId() const
{
    studio::EditorController* e = Editor();
    return e != nullptr
        ? QString::fromStdString(e->PrimarySelection()) : QString();
}

bool ObjectEditorController::canUndo() const
{
    return ws_ != nullptr && ws_->canUndo();
}

bool ObjectEditorController::canRedo() const
{
    return ws_ != nullptr && ws_->canRedo();
}

bool ObjectEditorController::gestureActive() const
{
    studio::EditorController* e = Editor();
    return e != nullptr && e->GestureActive();
}

const studio::SceneObject*
ObjectEditorController::ResolvedObject(const std::string& id) const
{
    if(resolved_src_ == ResolvedSrc::None)
    {
        return nullptr;
    }
    return studio::FindObject(resolved_, id);
}

QString ObjectEditorController::BoundStatusFor(
    const std::string& binding_id) const
{
    /* Same join the layout controller performs — display only. */
    if(bridge_ == nullptr || binding_id.empty())
    {
        return QStringLiteral("unresolved");
    }
    const studio::StudioDocument* w = WsDoc();
    if(w == nullptr)
    {
        return QStringLiteral("unresolved");
    }
    const auto bit = w->bindings.find(binding_id);
    if(bit == w->bindings.end())
    {
        return QStringLiteral("unresolved");
    }
    const studio::DeviceBinding& b = bit->second;
    QVariantList matches;
    for(const QVariant& sv : bridge_->snapshots())
    {
        const QVariantMap snap = sv.toMap();
        const QVariantMap key  = snap.value(QStringLiteral("key")).toMap();
        const QString name = snap.value(QStringLiteral("name")).toString();
        if(!b.serial.empty())
        {
            if(key.value(QStringLiteral("serial")).toString() !=
                   QString::fromStdString(b.serial)
               || !NameMatches(QString::fromStdString(b.controller_name),
                               name))
            {
                continue;
            }
            matches.push_back(snap);
            continue;
        }
        if(!NameMatches(QString::fromStdString(b.controller_name),
                        name))
        {
            continue;
        }
        matches.push_back(snap);
    }
    if(matches.size() > 1 && !b.location.empty())
    {
        QVariantList narrowed;
        for(const QVariant& mv : matches)
        {
            if(mv.toMap().value(QStringLiteral("key")).toMap()
                   .value(QStringLiteral("location")).toString()
               == QString::fromStdString(b.location))
            {
                narrowed.push_back(mv);
            }
        }
        if(!narrowed.isEmpty())
        {
            matches = narrowed;
        }
    }
    if(matches.isEmpty())
    {
        return QStringLiteral("unresolved");
    }
    if(matches.size() > 1)
    {
        return QStringLiteral("ambiguous");
    }
    return QStringLiteral("ok");
}

std::string ObjectEditorController::PaintOwnerOf(
    const std::string& object_id) const
{
    /* Linked objects never own output — the paint lands on the
       OWNER's key so both copies read the same color. */
    if(resolved_src_ != ResolvedSrc::None)
    {
        const studio::SceneObject* owner =
            studio::OutputOwner(resolved_, object_id);
        if(owner != nullptr)
        {
            return owner->id;
        }
    }
    return object_id;
}

std::set<std::string> ObjectEditorController::PaintTargets(
    const std::string& id) const
{
    std::set<std::string> out;
    out.insert(PaintOwnerOf(id));
    if(resolved_src_ == ResolvedSrc::None)
    {
        return out;
    }
    /* An instance/group id paints the device under it: every
       resolved object carrying emitters below the prefix (and
       the root itself) gets the color — one undo record writes
       all keys. */
    const std::string prefix = id + "/";
    for(const studio::SceneObject& o : resolved_.objects)
    {
        if(o.emitters.empty())
        {
            continue;
        }
        if(o.id != id
           && o.id.compare(0, prefix.size(), prefix) != 0)
        {
            continue;
        }
        const studio::SceneObject* owner =
            studio::OutputOwner(resolved_, o.id);
        out.insert(owner != nullptr ? owner->id : o.id);
    }
    return out;
}

bool ObjectEditorController::ChainLocked(const std::string& iid) const
{
    const studio::StudioDocument* w = WsDoc();
    if(w == nullptr)
    {
        return false;
    }
    std::set<std::string> seen;
    std::string cur = iid;
    while(!cur.empty() && seen.insert(cur).second)
    {
        const auto sit = w->device_settings.find(cur);
        if(sit != w->device_settings.end() && sit->second.locked)
        {
            return true;
        }
        const auto dit = w->devices.find(cur);
        if(dit == w->devices.end())
        {
            break;
        }
        cur = dit->second.parent;
    }
    return false;
}

void ObjectEditorController::SetStatus(const QString& s)
{
    if(status_text_ == s)
    {
        return;
    }
    status_text_ = s;
    emit statusChanged();
    emit statusMessage(s);
}

void ObjectEditorController::setStatusMessage(const QString& text)
{
    SetStatus(text);
}

/*---------------------------------------------------------*\
|| Resolve + model refresh                                  ||
\*---------------------------------------------------------*/

void ObjectEditorController::RebuildResolved()
{
    const studio::StudioDocument* w = WsDoc();
    if(w == nullptr)
    {
        resolved_ = studio::SceneDocument();
        resolved_src_ = ResolvedSrc::None;
        return;
    }
    std::vector<std::string> errors;
    if(studio::ResolveScene(*w, types_, resolved_, &errors))
    {
        resolved_src_ = ResolvedSrc::Draft;
        return;
    }
    /* The draft doesn't resolve (e.g. a type the local registry
       lacks): fall back to the engine's APPLIED resolved document
       for entity enrichment — the draft still drives the rows. */
    resolved_src_ = ResolvedSrc::None;
    if(bridge_ != nullptr)
    {
        const nlohmann::json doc = VariantToJson(
            bridge_->scene().value(QStringLiteral("document")));
        if(doc.is_object())
        {
            studio::SceneDocument parsed;
            if(studio::FromJson(doc, parsed))
            {
                resolved_ = std::move(parsed);
                resolved_src_ = ResolvedSrc::Applied;
            }
        }
    }
}

void ObjectEditorController::RefreshAll()
{
    RebuildResolved();
    object_model_.Bind(WsDoc(), &resolved_,
        [this](const std::string& binding_id) -> QString
        {
            return BoundStatusFor(binding_id);
        });
    preset_model_.Bind(&types_,
        (ws_ != nullptr && ws_->Session().HasWorking())
            ? &ws_->Session().Working().meta.ui.favorites
            : nullptr);
    object_model_.ResetFrom();
    emit sceneChanged();
}

void ObjectEditorController::PreviewAdopt(
    const std::set<std::string>& ids)
{
    /* Gesture preview — re-resolve (cheap for desk-size scenes)
       and dataChanged the touched rows only; no history, no
       dirty. A failed resolve during preview keeps the last
       resolved snapshot rather than blanking the view. */
    const studio::StudioDocument* w = WsDoc();
    if(w != nullptr)
    {
        studio::SceneDocument r;
        if(studio::ResolveScene(*w, types_, r, nullptr))
        {
            resolved_ = std::move(r);
            resolved_src_ = ResolvedSrc::Draft;
        }
    }
    object_model_.UpdateTransforms(ids);
    emit sceneChanged();
}

void ObjectEditorController::CommitEdit(studio::EditorEdit e)
{
    WorkspaceEditSession* s = Session();
    if(s == nullptr || e.Empty())
    {
        return;
    }
    const bool structural = !e.TransformsOnly();
    const std::set<std::string> touched = e.TransformIds();
    if(!s->CommitEdit(e))
    {
        return;
    }
    ws_->NoteDraftEdited();
    /* Resolve AFTER the record exists — a broken draft keeps the
       applied-scene fallback rather than rolling the edit back;
       the draft is the user's to fix. */
    RebuildResolved();
    if(structural)
    {
        object_model_.ResetFrom();
    }
    else
    {
        object_model_.UpdateTransforms(touched);
    }
    emit sceneChanged();
    emit undoChanged();
}

/*---------------------------------------------------------*\
|| Selection                                                ||
\*---------------------------------------------------------*/

void ObjectEditorController::select(const QString& objectOrInstanceId)
{
    studio::EditorController* e = Editor();
    if(e == nullptr)
    {
        return;
    }
    if(objectOrInstanceId.isEmpty())
    {
        e->ClearSelection();
    }
    else
    {
        /* A resolved object id maps to its owning root instance —
           SetSelection takes object-or-instance ids directly. */
        e->SetSelection({ objectOrInstanceId.toStdString() });
    }
    emit selectionChanged();
}

void ObjectEditorController::selectInstance(const QString& instanceId,
                                            bool additive)
{
    studio::EditorController* e = Editor();
    if(e == nullptr)
    {
        return;
    }
    e->Select(instanceId.toStdString(), additive);
    emit selectionChanged();
}

void ObjectEditorController::clearEditorSelection()
{
    studio::EditorController* e = Editor();
    if(e == nullptr)
    {
        return;
    }
    e->ClearSelection();
    emit selectionChanged();
}

/*---------------------------------------------------------*\
|| Transform gestures                                       ||
\*---------------------------------------------------------*/

void ObjectEditorController::beginTransformGesture()
{
    studio::EditorController* e = Editor();
    if(e == nullptr)
    {
        return;
    }
    if(e->BeginTransform())
    {
        emit gestureActiveChanged();
    }
}

void ObjectEditorController::updateTransformGesture(
    double dx, double dy, double dz, int plane, bool snap)
{
    studio::EditorController* e = Editor();
    if(e == nullptr || !e->GestureActive())
    {
        return;
    }
    /* Reject out-of-range plane values instead of silently
       treating them as Free. */
    if(plane < (int)studio::EditPlane::Free
       || plane > (int)studio::EditPlane::SideYZ)
    {
        return;
    }
    e->PreviewTranslate({ (float)dx, (float)dy, (float)dz },
                        (studio::EditPlane)plane, snap);
    PreviewAdopt(e->GestureIds());
}

void ObjectEditorController::updateRotateGesture(double degrees,
                                                 bool snap)
{
    updateRotateGestureAxis(0.0, 1.0, 0.0, degrees, snap);
}

void ObjectEditorController::updateRotateGestureAxis(
    double ax, double ay, double az, double degrees, bool snap)
{
    studio::EditorController* e = Editor();
    if(e == nullptr || !e->GestureActive())
    {
        return;
    }
    e->PreviewRotate({ (float)ax, (float)ay, (float)az },
                     (float)degrees, snap);
    PreviewAdopt(e->GestureIds());
}

void ObjectEditorController::commitTransformGesture()
{
    studio::EditorController* e = Editor();
    if(e == nullptr || !e->GestureActive())
    {
        return;
    }
    std::optional<studio::EditorEdit> edit = e->Commit();
    emit gestureActiveChanged();
    if(edit.has_value())
    {
        CommitEdit(std::move(*edit));
    }
}

void ObjectEditorController::cancelTransformGesture()
{
    studio::EditorController* e = Editor();
    if(e == nullptr || !e->GestureActive())
    {
        return;
    }
    const std::set<std::string> ids = e->GestureIds();
    e->Cancel();
    emit gestureActiveChanged();
    PreviewAdopt(ids);
}

/*---------------------------------------------------------*\
|| Discrete ops — each = one undo record                    ||
\*---------------------------------------------------------*/

bool ObjectEditorController::setInstancePosition(
    const QString& id, double x, double y, double z)
{
    studio::EditorController* e = Editor();
    if(e == nullptr)
    {
        return false;
    }
    /* Locked ancestor = frozen subtree: the core only checks the
       instance's own flag, so the adapter refuses the chain. */
    if(ChainLocked(id.toStdString()))
    {
        SetStatus(QStringLiteral("move refused: %1 is locked")
                      .arg(id));
        return false;
    }
    std::optional<studio::EditorEdit> edit =
        e->SetPosition(id.toStdString(),
                       { (float)x, (float)y, (float)z });
    if(!edit.has_value())
    {
        return false;
    }
    CommitEdit(std::move(*edit));
    return true;
}

bool ObjectEditorController::setInstancePositionWorld(
    const QString& id, double x, double y, double z)
{
    studio::EditorController* e = Editor();
    if(e == nullptr)
    {
        return false;
    }
    if(ChainLocked(id.toStdString()))
    {
        SetStatus(QStringLiteral("move refused: %1 is locked")
                      .arg(id));
        return false;
    }
    std::optional<studio::EditorEdit> edit =
        e->SetPositionWorld(id.toStdString(),
                            { (float)x, (float)y, (float)z });
    if(!edit.has_value())
    {
        return false;
    }
    CommitEdit(std::move(*edit));
    return true;
}

bool ObjectEditorController::setInstanceRotation(
    const QString& id, double rx, double ry, double rz)
{
    studio::EditorController* e = Editor();
    if(e == nullptr)
    {
        return false;
    }
    if(ChainLocked(id.toStdString()))
    {
        SetStatus(QStringLiteral("rotate refused: %1 is locked")
                      .arg(id));
        return false;
    }
    std::optional<studio::EditorEdit> edit =
        e->SetRotation(id.toStdString(),
                       { (float)rx, (float)ry, (float)rz });
    if(!edit.has_value())
    {
        return false;
    }
    CommitEdit(std::move(*edit));
    return true;
}

bool ObjectEditorController::renameInstance(const QString& id,
                                            const QString& newId)
{
    studio::EditorController* e = Editor();
    if(e == nullptr)
    {
        return false;
    }
    std::optional<studio::EditorEdit> edit =
        e->Rename(id.toStdString(), newId.toStdString());
    if(!edit.has_value())
    {
        const std::string& why = e->LastError();
        SetStatus(why.empty()
            ? QStringLiteral("rename rejected — check the new id")
            : QString::fromStdString(why));
        return false;
    }
    CommitEdit(std::move(*edit));
    emit selectionChanged();
    return true;
}

void ObjectEditorController::setInstanceVisible(const QString& id,
                                                bool on)
{
    studio::EditorController* e = Editor();
    if(e == nullptr)
    {
        return;
    }
    std::optional<studio::EditorEdit> edit =
        e->SetVisible(id.toStdString(), on);
    if(edit.has_value())
    {
        CommitEdit(std::move(*edit));
    }
    else if(!e->LastError().empty())
    {
        SetStatus(QString::fromStdString(e->LastError()));
    }
}

void ObjectEditorController::setInstanceLocked(const QString& id,
                                               bool on)
{
    studio::EditorController* e = Editor();
    if(e == nullptr)
    {
        return;
    }
    std::optional<studio::EditorEdit> edit =
        e->SetLocked(id.toStdString(), on);
    if(edit.has_value())
    {
        CommitEdit(std::move(*edit));
    }
    else if(!e->LastError().empty())
    {
        SetStatus(QString::fromStdString(e->LastError()));
    }
}

/*---------------------------------------------------------*\
|| Hierarchy                                                ||
\*---------------------------------------------------------*/

void ObjectEditorController::groupSelected()
{
    studio::EditorController* e = Editor();
    if(e == nullptr)
    {
        return;
    }
    std::optional<studio::EditorEdit> edit = e->Group();
    if(edit.has_value())
    {
        CommitEdit(std::move(*edit));
        emit selectionChanged();
    }
    else if(!e->LastError().empty())
    {
        SetStatus(QString::fromStdString(e->LastError()));
    }
}

void ObjectEditorController::ungroupSelected()
{
    studio::EditorController* e = Editor();
    if(e == nullptr)
    {
        return;
    }
    std::optional<studio::EditorEdit> edit = e->Ungroup();
    if(edit.has_value())
    {
        CommitEdit(std::move(*edit));
        emit selectionChanged();
    }
    else if(!e->LastError().empty())
    {
        SetStatus(QString::fromStdString(e->LastError()));
    }
}

void ObjectEditorController::deleteSelected()
{
    studio::EditorController* e = Editor();
    if(e == nullptr)
    {
        return;
    }
    std::optional<studio::EditorEdit> edit = e->DeleteSelected();
    if(edit.has_value())
    {
        CommitEdit(std::move(*edit));
        emit selectionChanged();
    }
    else if(!e->LastError().empty())
    {
        SetStatus(QString::fromStdString(e->LastError()));
    }
}

QStringList ObjectEditorController::deletePreview() const
{
    QStringList out;
    studio::EditorController* e = Editor();
    if(e == nullptr)
    {
        return out;
    }
    /* Same kill set DeleteSelected computes — the tree lists the
       children before commit. */
    for(const std::string& id : e->DeleteCascade(e->Selection()))
    {
        out << QString::fromStdString(id);
    }
    return out;
}

void ObjectEditorController::duplicateMirrored()
{
    studio::EditorController* e = Editor();
    if(e == nullptr)
    {
        return;
    }
    std::optional<studio::EditorEdit> edit = e->DuplicateMirrored();
    if(edit.has_value())
    {
        CommitEdit(std::move(*edit));
        emit selectionChanged();
    }
    else if(!e->LastError().empty())
    {
        SetStatus(QString::fromStdString(e->LastError()));
    }
}

void ObjectEditorController::alignSelected(int axis, int mode)
{
    studio::EditorController* e = Editor();
    if(e == nullptr)
    {
        return;
    }
    std::optional<studio::EditorEdit> edit = e->Align(axis, mode);
    if(edit.has_value())
    {
        CommitEdit(std::move(*edit));
    }
    else if(!e->LastError().empty())
    {
        SetStatus(QString::fromStdString(e->LastError()));
    }
}

void ObjectEditorController::distributeSelected(int axis)
{
    studio::EditorController* e = Editor();
    if(e == nullptr)
    {
        return;
    }
    std::optional<studio::EditorEdit> edit = e->Distribute(axis);
    if(edit.has_value())
    {
        CommitEdit(std::move(*edit));
    }
    else if(!e->LastError().empty())
    {
        SetStatus(QString::fromStdString(e->LastError()));
    }
}

/*---------------------------------------------------------*\
|| Device library                                           ||
\*---------------------------------------------------------*/

void ObjectEditorController::addDeviceInstance(const QString& typeId,
                                               double x, double y,
                                               double z)
{
    const std::string tid = typeId.toStdString();
    studio::EditorController* e = Editor();
    if(e == nullptr)
    {
        SetStatus(QStringLiteral("add refused — no draft"));
        return;
    }
    if(!studio::IsPresetId(tid))
    {
        SetStatus(QStringLiteral("add refused: '%1' is not a valid type id")
                      .arg(typeId));
        return;
    }
    const studio::DevicePreset* p = types_.Find(tid);
    if(p == nullptr)
    {
        SetStatus(QStringLiteral("add refused: unknown type '%1'")
                      .arg(typeId));
        return;
    }
    /* Sit ON the surface, never inside it: the caller's y is the
       desk height the drop measured — lift the origin by the
       type's local footprint floor (PresetInfo floor_y). */
    float floor_y = 0.0f;
    if(const studio::PresetRegistry::PresetInfo* info =
           preset_model_.InfoFor(typeId))
    {
        floor_y = info->floor_y;
    }
    std::optional<studio::EditorEdit> edit =
        e->AddInstance(tid, { (float)x, (float)y - floor_y, (float)z });
    if(!edit.has_value())
    {
        SetStatus(e->LastError().empty()
            ? QStringLiteral("add refused — finish the drag first")
            : QString::fromStdString(e->LastError()));
        return;
    }
    CommitEdit(std::move(*edit));
    emit selectionChanged();
    SetStatus(QStringLiteral("added %1 (%2)")
                  .arg(selectedId(), QString::fromStdString(p->name)));
}

void ObjectEditorController::setTypeFavorite(const QString& typeId,
                                             bool fav)
{
    const std::string tid = typeId.toStdString();
    WorkspaceEditSession* s = Session();
    if(s == nullptr || !s->HasWorking())
    {
        return;
    }
    if(!studio::IsPresetId(tid) || !preset_model_.Knows(typeId))
    {
        SetStatus(QStringLiteral("favorite refused: unknown type '%1'")
                      .arg(typeId));
        return;
    }
    /* UI preference like camera state — dirty/autosave, never
       undo. Erase-then-append keeps first-favorited-first order
       and dedupes stale repeats in one pass. CommitDocEdit would
       leave an undo record, so this takes the same non-undo
       channel recovery staging uses: restage the draft in place —
       baseline, history and revisions are untouched. */
    studio::StudioDocument draft = s->Working();
    std::vector<std::string>& favs = draft.meta.ui.favorites;
    favs.erase(std::remove(favs.begin(), favs.end(), tid),
               favs.end());
    if(fav)
    {
        favs.push_back(tid);
    }
    s->LoadDraft(draft);
    ws_->NoteDraftEdited();
    preset_model_.RefreshFavorites();
    emit presetLibraryChanged();
    SetStatus(fav ? QStringLiteral("%1 starred").arg(typeId)
                  : QStringLiteral("%1 unstarred").arg(typeId));
}

void ObjectEditorController::refreshLibrary()
{
    preset_model_.Reload();
    emit presetLibraryChanged();
    RequestCatalog();
}

void ObjectEditorController::RequestCatalog()
{
    if(bridge_ == nullptr || !bridge_->connected()
       || pending_catalog_ != 0)
    {
        return;
    }
    pending_catalog_ = bridge_->workspaceCommand(
        QStringLiteral("catalog"),
        QVariantMap({ { QStringLiteral("kind"),
                        QStringLiteral("devices") },
                      { QStringLiteral("limit"), 512 } }));
}

void ObjectEditorController::OnBridgeReply(
    int requestId, const QString& typeName, const QVariantMap& body)
{
    if(requestId == 0 || requestId != pending_catalog_)
    {
        return;
    }
    pending_catalog_ = 0;
    if(typeName != QLatin1String("OK")
       || body.value(QStringLiteral("kind")).toString()
              != QLatin1String("devices"))
    {
        return;   /* Unavailable/ERROR — local rows still stand */
    }
    preset_model_.SetEngineCatalog(
        body.value(QStringLiteral("entries")).toList());
    emit presetLibraryChanged();
}

/*---------------------------------------------------------*\
|| Row reads                                                ||
\*---------------------------------------------------------*/

QVariantMap ObjectEditorController::instanceState(
    const QString& id) const
{
    QVariantMap m;
    const studio::StudioDocument* w = WsDoc();
    if(w == nullptr)
    {
        return m;
    }
    const std::string iid =
        studio::EditorController::InstanceOf(id.toStdString());
    const auto it = w->devices.find(iid);
    if(it == w->devices.end())
    {
        return m;
    }
    const studio::DeviceInstance& d = it->second;
    m[QStringLiteral("id")]   = QString::fromStdString(iid);
    m[QStringLiteral("type")] = QString::fromStdString(d.type);
    m[QStringLiteral("x")]  = d.position.x;
    m[QStringLiteral("y")]  = d.position.y;
    m[QStringLiteral("z")]  = d.position.z;
    m[QStringLiteral("rx")] = d.rotation_deg.x;
    m[QStringLiteral("ry")] = d.rotation_deg.y;
    m[QStringLiteral("rz")] = d.rotation_deg.z;
    const auto sit = w->device_settings.find(iid);
    m[QStringLiteral("visible")] =
        (sit == w->device_settings.end()) || sit->second.visible;
    m[QStringLiteral("locked")] =
        (sit != w->device_settings.end()) && sit->second.locked;
    return m;
}

QVariantList ObjectEditorController::emittersOf(
    const QString& objectId) const
{
    QVariantList out;
    if(resolved_src_ == ResolvedSrc::None)
    {
        return out;
    }
    const std::string id = objectId.toStdString();
    const studio::SceneObject* obj =
        studio::FindObject(resolved_, id);
    if(obj == nullptr)
    {
        return out;
    }
    const studio::SceneObject* owner =
        studio::OutputOwner(resolved_, id);
    if(owner == nullptr)
    {
        owner = obj;
    }
    for(size_t i = 0; i < owner->emitters.size(); ++i)
    {
        const studio::Emitter& e = owner->emitters[i];
        QVariantMap m;
        m[QStringLiteral("x")] = e.local_pos.x;
        m[QStringLiteral("y")] = e.local_pos.y;
        m[QStringLiteral("z")] = e.local_pos.z;
        m[QStringLiteral("c")] = HexColor(
            studio::EmitterColor(resolved_, owner->id, (int)i));
        m[QStringLiteral("i")] = (int)i;
        out.push_back(m);
    }
    return out;
}

QVariantList ObjectEditorController::emitterColorsOf(
    const QString& objectId) const
{
    QVariantList out;
    if(resolved_src_ == ResolvedSrc::None)
    {
        return out;
    }
    const std::string id = objectId.toStdString();
    const studio::SceneObject* obj =
        studio::FindObject(resolved_, id);
    if(obj == nullptr)
    {
        return out;
    }
    const studio::SceneObject* owner =
        studio::OutputOwner(resolved_, id);
    if(owner == nullptr)
    {
        owner = obj;
    }
    out.reserve((qsizetype)owner->emitters.size());
    for(size_t i = 0; i < owner->emitters.size(); ++i)
    {
        out.push_back(HexColor(
            studio::EmitterColor(resolved_, owner->id, (int)i)));
    }
    return out;
}

/*---------------------------------------------------------*\
|| Painting — into the compact color maps, one record each  ||
\*---------------------------------------------------------*/

void ObjectEditorController::setSelectedColor(const QColor& color)
{
    studio::EditorController* e = Editor();
    if(e == nullptr)
    {
        return;
    }
    const std::string sel = e->PrimarySelection();
    if(sel.empty())
    {
        SetStatus(QStringLiteral("select a light device first"));
        return;
    }
    paintObject(QString::fromStdString(sel), color);
}

void ObjectEditorController::paintObject(const QString& objectId,
                                         const QColor& color)
{
    WorkspaceEditSession* s = Session();
    if(s == nullptr || !s->HasWorking())
    {
        return;
    }
    /* The 2D canvas clicks land on INSTANCE ids — expand to the
       emitter-bearing resolved objects underneath (each mapped
       through its output owner) so the paint actually reaches
       the device's emitters, not just the group-node key. One
       CommitDocEdit writes every key -> one undo record. */
    const std::set<std::string> keys =
        PaintTargets(objectId.toStdString());
    const studio::SceneColor nc = studio::MakeSceneColor(
        (unsigned)color.red(), (unsigned)color.green(),
        (unsigned)color.blue());
    /* Whole-document record — paint lives in EditorEdit's
       coverage but the session owns mutation, so CommitDocEdit's
       snapshot record is the one-record-per-gesture path here. */
    if(!s->CommitDocEdit("paint",
           [keys, nc](studio::StudioDocument& d) -> bool
           {
               bool changed = false;
               for(const std::string& key : keys)
               {
                   const auto it = d.object_colors.find(key);
                   if(it != d.object_colors.end()
                      && it->second == nc)
                   {
                       continue;   /* already that color */
                   }
                   d.object_colors[key] = nc;
                   changed = true;
               }
               return changed;   /* all no-op -> no record */
           }))
    {
        return;
    }
    ws_->NoteDraftEdited();
    RebuildResolved();
    /* Refresh the touched entity rows AND their instance rows —
       the tree's per-row color chips read the instance key. */
    std::set<std::string> touched = keys;
    for(const std::string& k : keys)
    {
        touched.insert(k.substr(0, k.find('/')));
    }
    object_model_.UpdateTransforms(touched);
    emit emittersChanged(objectId);
    emit sceneChanged();
    emit undoChanged();
}

void ObjectEditorController::paintEmitter(const QString& objectId,
                                          int index,
                                          const QColor& color)
{
    WorkspaceEditSession* s = Session();
    if(s == nullptr || !s->HasWorking() || index < 0)
    {
        return;
    }
    const std::string key = PaintOwnerOf(objectId.toStdString());
    const studio::SceneColor nc = studio::MakeSceneColor(
        (unsigned)color.red(), (unsigned)color.green(),
        (unsigned)color.blue());
    if(!s->CommitDocEdit("paint emitter",
           [key, index, nc](studio::StudioDocument& d) -> bool
           {
               auto& m = d.emitter_colors[key];
               const auto it = m.find(index);
               if(it != m.end() && it->second == nc)
               {
                   return false;
               }
               m[index] = nc;
               return true;
           }))
    {
        return;
    }
    ws_->NoteDraftEdited();
    RebuildResolved();
    /* Same refresh paintObject takes — emitter-color displays
       (tree/canvas chips, the calibration list) re-read on
       dataChanged, not just the signal. */
    object_model_.UpdateTransforms(
        { key, key.substr(0, key.find('/')) });
    emit emittersChanged(objectId);
    emit sceneChanged();
    emit undoChanged();
}

/*---------------------------------------------------------*\
|| Undo/redo — the shared session's history                 ||
\*---------------------------------------------------------*/

void ObjectEditorController::undo()
{
    WorkspaceEditSession* s = Session();
    if(s == nullptr || gestureActive())
    {
        return;
    }
    if(s->Undo())
    {
        ws_->NoteDraftEdited();
        RefreshAll();
        emit selectionChanged();
        emit undoChanged();
    }
}

void ObjectEditorController::redo()
{
    WorkspaceEditSession* s = Session();
    if(s == nullptr || gestureActive())
    {
        return;
    }
    if(s->Redo())
    {
        ws_->NoteDraftEdited();
        RefreshAll();
        emit selectionChanged();
        emit undoChanged();
    }
}

/*---------------------------------------------------------*\
|| Panel state                                              ||
\*---------------------------------------------------------*/

void ObjectEditorController::setTool(int t)
{
    if(t < 0 || t > 2 || tool_ == t)
    {
        return;
    }
    tool_ = t;
    emit toolChanged();
}

void ObjectEditorController::setEditPlane(int p)
{
    if(p < (int)studio::EditPlane::Free
       || p > (int)studio::EditPlane::SideYZ || edit_plane_ == p)
    {
        return;
    }
    edit_plane_ = p;
    emit editPlaneChanged();
}

void ObjectEditorController::setSnapEnabled(bool on)
{
    if(snap_ == on)
    {
        return;
    }
    snap_ = on;
    emit snapChanged();
}

void ObjectEditorController::setPaintColor(const QColor& c)
{
    if(paint_color_ == c || !c.isValid())
    {
        return;
    }
    paint_color_ = c;
    emit paintColorChanged();
}

double ObjectEditorController::snapMeters(double meters) const
{
    /* 10 mm grid — the same step the gesture path snaps to. */
    return snap_ ? std::round(meters * 100.0) / 100.0 : meters;
}

/*---------------------------------------------------------*\
|| Session feeds                                            ||
\*---------------------------------------------------------*/

void ObjectEditorController::OnWorkspaceDraftEdited()
{
    /* Undo/redo/discard or another adapter's commit — the draft
       moved; re-resolve + rebuild rows + re-sync selection
       mirror. */
    RefreshAll();
    emit selectionChanged();
}

void ObjectEditorController::OnWorkspaceDraftChanged()
{
    /* Draft loaded/cleared — full rebuild + ready transition. */
    RefreshAll();
    preset_model_.RefreshFavorites();
    emit presetLibraryChanged();
    emit selectionChanged();
    emit readyChanged();
}

void ObjectEditorController::OnAvailabilityChanged()
{
    if(ws_ != nullptr && ws_->available())
    {
        RequestCatalog();
    }
}

/*---------------------------------------------------------*\
|| Diagnostics/test hooks                                   ||
\*---------------------------------------------------------*/

std::vector<std::string> ObjectEditorController::LoadPresetDirectory(
    const std::string& dir)
{
    std::vector<std::string> errors;
    types_.LoadDirectory(dir, &errors);
    preset_model_.Reload();
    emit presetLibraryChanged();
    RefreshAll();   /* new types may make the draft resolvable */
    return errors;
}

void ObjectEditorController::RegisterPresetForSession(
    const studio::DevicePreset& p)
{
    /* The engine landed this type — the draft resolves it
       immediately instead of waiting on a catalog pull. */
    types_.Add(p, nullptr);
    preset_model_.Reload();
    emit presetLibraryChanged();
    RefreshAll();
}

void ObjectEditorController::UnregisterPresetForSession(
    const std::string& id)
{
    if(!types_.RemoveFile(id))
    {
        return;      /* nothing on the file layer to drop */
    }
    preset_model_.Reload();
    emit presetLibraryChanged();
    RefreshAll();
}

} /* namespace ui */
} /* namespace openrgb2 */
