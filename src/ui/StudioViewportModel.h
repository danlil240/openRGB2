/*---------------------------------------------------------*\
|| StudioViewportModel.h                                    ||
||                                                          ||
||   QML-facing draft preview (studio-import Task 7, plan ||
||   section 3.4): the UNSAVED workspace draft evaluated  ||
||   engine-side and pushed back over the session-bound   ||
||   "preview" event feed — chunked below the IPC cap,    ||
||   reassembled strictly by sequence, honestly labelled. ||
||                                                          ||
||   Boundary (plan 3.4):                                 ||
||   - this model speaks WORKSPACE_COMMAND + EVENT only   ||
||     through EngineBridge — no hardware, no SDK, no     ||
||     second writer                                      ||
||   - draft colors are DRAFT colors: "frame_status" is   ||
||     the transport-accepted feed and stays a separate   ||
||     store; nothing here claims physical delivery       ||
||   - only COMPLETE matching sequences are applied — a   ||
||     stale or partial frame is dropped and counted      ||
||   - a reduced frame surfaces its reduced flag in the   ||
||     label, never an apparently-complete truncation     ||
||                                                          ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#pragma once

#include "EngineBridge.h"
#include "WorkspaceController.h"

#include <QObject>
#include <QString>
#include <QHash>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include <cstdint>
#include <map>

namespace openrgb2
{
namespace ui
{

class StudioViewportModel : public QObject
{
    Q_OBJECT

    /* Client-side intent — false ends the preview honestly. */
    Q_PROPERTY(bool wantPreview READ wantPreview
                   WRITE setWantPreview NOTIFY wantPreviewChanged)
    /* Surface visibility -> the engine pauses (hidden) or
       resumes the feed. */
    Q_PROPERTY(bool visible READ visible
                   WRITE setVisible NOTIFY visibleChanged)

    /* Engine-side state, all honest: */
    Q_PROPERTY(bool previewActive READ previewActive
                   NOTIFY previewChanged)
    Q_PROPERTY(bool paused READ paused NOTIFY previewChanged)
    Q_PROPERTY(QString pausedReason READ pausedReason
                   NOTIFY previewChanged)
    /* e.g. "Draft preview — unsaved changes, not applied to
       lights" / "... (reduced detail)". NEVER uses the
       "preview" wording of the transport-accepted strip. */
    Q_PROPERTY(QString label READ label NOTIFY previewChanged)
    Q_PROPERTY(bool reduced READ reduced NOTIFY previewChanged)
    Q_PROPERTY(quint64 sequence READ sequence NOTIFY previewChanged)
    Q_PROPERTY(quint64 draftRevision READ draftRevision
                   NOTIFY previewChanged)
    Q_PROPERTY(double sceneTime READ sceneTime
                   NOTIFY previewChanged)
    Q_PROPERTY(int objectTotal READ objectTotal
                   NOTIFY previewChanged)
    Q_PROPERTY(int emitterTotal READ emitterTotal
                   NOTIFY previewChanged)
    Q_PROPERTY(int emitterSent READ emitterSent
                   NOTIFY previewChanged)
    /* [{id,kind,x,y,z,rx,ry,rz,w,h,d,bound,verified,
        emitter_total,colors:["#RRGGBB"],indices?}] — the last
       COMPLETE sequence, dropped whole when stale/partial. */
    Q_PROPERTY(QVariantList objects READ objects
                   NOTIFY previewChanged)
    /* [{id,kind,bound,emitter_total}] of the last complete frame,
       NO colors, notified only when that set changes (audit Q-5).
       The stable model QML lists use: a frame with the same
       objects repaints through colorsFor() and never rebuilds the
       delegates nor copies every object's colors into JS. */
    Q_PROPERTY(QVariantList objectRows READ objectRows
                   NOTIFY objectRowsChanged)
    Q_PROPERTY(int droppedFrames READ droppedFrames
                   NOTIFY previewChanged)
    Q_PROPERTY(QString lastError READ lastError
                   NOTIFY lastErrorChanged)

public:
    StudioViewportModel(EngineBridge* bridge,
                        WorkspaceController* workspace,
                        QObject* parent = nullptr);

    /* State */
    bool        wantPreview()    const { return want_preview_; }
    bool        visible()        const { return visible_; }
    bool        previewActive()  const { return active_; }
    bool        paused()         const { return paused_; }
    QString     pausedReason()   const { return paused_reason_; }
    QString     label()          const;
    bool        reduced()        const { return reduced_; }
    quint64     sequence()       const { return sequence_; }
    quint64     draftRevision()  const { return draft_rev_; }
    double      sceneTime()      const { return scene_time_; }
    int         objectTotal()    const { return object_total_; }
    int         emitterTotal()   const { return emitter_total_; }
    int         emitterSent()    const { return emitter_sent_; }
    QVariantList objects()       const { return objects_; }
    QVariantList objectRows()    const { return object_rows_; }
    int         droppedFrames()  const { return dropped_frames_; }
    QString     lastError()      const { return last_error_; }

    void        setWantPreview(bool want);
    void        setVisible(bool visible);
    /* Re-push the current draft immediately (settings page
       "preview now"). */
    Q_INVOKABLE void refreshNow();

    /* The last complete frame's colors for one object id, dense
       per emitter (a reduced frame's sparse `indices` expand with
       unmapped emitters "#000000"); undefined when the preview is
       inactive or has no record for the id. O(1) via an id index —
       the 3D viewport asks once per object per frame, so it must
       not copy/scan the whole `objects` list each time. */
    Q_INVOKABLE QVariant colorsFor(const QString& objectId,
                                   int emitterCount) const;

    /* Test seam — the debounce interval for draft pushes. */
    void setDebounceMsForTesting(int ms);

signals:
    void previewChanged();
    void objectRowsChanged();
    void wantPreviewChanged();
    void visibleChanged();
    void lastErrorChanged();

private:
    void RecomputeWanted();
    void Begin();
    void PushUpdate();
    void End();
    void SendVisibility();
    void OnPreviewEvent(const QVariantMap& data);
    void OnReply(int request_id, const QString& type_name,
                 const QVariantMap& body);
    void ClearFrame();
    void SyncObjectRows();
    void SetError(const QString& msg);

    EngineBridge*        bridge_;
    WorkspaceController* workspace_;

    bool     want_preview_ = true;
    bool     visible_      = true;
    bool     active_       = false;
    bool     paused_       = false;
    QString  paused_reason_;
    bool     reduced_      = false;
    quint64  sequence_       = 0;
    quint64  draft_rev_      = 0;
    double   scene_time_     = 0.0;
    int      object_total_   = 0;
    int      emitter_total_  = 0;
    int      emitter_sent_   = 0;
    QVariantList objects_;
    QHash<QString, int> object_index_;   /* id -> objects_ row */
    QVariantList object_rows_;           /* see objectRows */
    int      dropped_frames_ = 0;
    QString  last_error_;

    /* Latest-wins client side: a burst of edits pushes ONE
       debounced candidate; a refusal latches until the next edit. */
    quint64  push_rev_       = 0;
    int      pending_req_    = 0;
    bool     refused_        = false;
    bool     begin_sent_     = false;
    int      debounce_ms_    = 120;
    /* Trailing-edge debounce for draft pushes (one timer,
       restarted per draft signal). */
    QTimer   push_timer_;
    /* The visibility the engine last received (begin or
       visibility update) — reconciled when the begin reply lands. */
    bool     sent_visible_   = true;

    /* Chunk reassembly — keyed by sequence; a newer sequence
       orphans the partial (counted as dropped). */
    struct Partial
    {
        int                       chunks = 0;
        int                       seen   = 0;
        QVariantMap               envelope;
        std::map<int, bool>       got;
        std::map<int, QVariantList> chunk_objs;
    };
    std::map<quint64, Partial> partials_;
};

} /* namespace ui */
} /* namespace openrgb2 */
