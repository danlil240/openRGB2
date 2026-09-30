/*---------------------------------------------------------*\
|| PresetListModel.h                                        ||
||                                                          ||
||   QAbstractListModel over PresetRegistry::List() — the ||
||   merged file-over-default device-type listing (Qt —   ||
||   the UI's projection, NOT part of the Qt-free core).  ||
||   Feeds the DeviceLibrary panel: search/category/fav   ||
||   sorting stays pure-QML on top of rowAt(), exactly    ||
||   like the source-of-record model this is adapted      ||
||   from.                                                ||
||                                                          ||
||   Two extra rows sources beyond the local registry:    ||
||   - engineCatalog — ids the CONNECTED engine reports   ||
||     via WORKSPACE_COMMAND catalog {kind:"devices"}     ||
||     that the local registry lacks (honest "the engine  ||
||     knows this type" rows; zero bounds mark them as    ||
||     add-at-origin-only candidates).                    ||
||   - favorites — read live from the bound favorites     ||
||     vector (the shared draft's meta.ui.favorites).     ||
||                                                          ||
||   Update paths:                                        ||
||   - Reload()           — registry content changed.     ||
||   - SetEngineCatalog() — catalog reply landed.         ||
||   - RefreshFavorites() — favorites changed: a          ||
||     dataChanged on FavoriteRole alone.                 ||
||                                                          ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
|\*---------------------------------------------------------*/

#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QString>
#include <QVariant>
#include <QVariantMap>
#include <QVariantList>
#include <QVector3D>

#include "presets/PresetRegistry.h"

#include <string>
#include <vector>

namespace openrgb2
{
namespace ui
{

class PresetListModel : public QAbstractListModel
{
    Q_OBJECT
public:
    enum Role
    {
        TypeIdRole = Qt::UserRole + 1,
        NameRole,
        CategoryRole,
        FromFileRole,
        ZoneCountRole,
        LedTotalRole,
        BoundsRole,        /* QVector3D size in metres   */
        FavoriteRole,      /* read live from the draft   */
        EngineOnlyRole,    /* true: known to the engine,
                              absent from the local
                              registry                    */
    };
    Q_ENUM(Role)

    explicit PresetListModel(QObject* parent = nullptr);

    /* `favorites` points at the draft's meta.ui.favorites vector —
       a const pointer into WorkspaceEditSession::Working() (stable
       for the session's life; contents re-read per query). */
    void Bind(const studio::PresetRegistry* reg,
              const std::vector<std::string>* favorites);

    int      rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index,
                  int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    /* Registry content changed (reload): re-snapshot List(). */
    void Reload();
    /* Engine catalog reply — ids only rows the local registry
       doesn't already carry are merged in (full reset). */
    void SetEngineCatalog(const QVariantList& entries);
    /* Only ui.favorites changed — re-badge every row. */
    void RefreshFavorites();

    /* Listing facts for a type id — the controller reads floor_y
       here to drop new instances onto the desk surface. */
    const studio::PresetRegistry::PresetInfo* InfoFor(
        const QString& id) const;
    bool Knows(const QString& id) const;

    /* QML helpers — the library panel's search/category/favorite
       pipeline consumes plain QVariantMaps so the same code path
       runs against a plain-JS stub in tests. */
    Q_INVOKABLE int count() const { return (int)rows_.size() + (int)extra_.size(); }
    Q_INVOKABLE QVariantMap rowAt(int row) const;

private:
    bool IsFavorite(const std::string& id) const;
    QVariantMap RowMap(const QString& id, const QString& name,
                       const QString& category, bool from_file,
                       int zone_count, int led_total,
                       const QVector3D& bounds, bool engine_only) const;

    const studio::PresetRegistry*         reg_ = nullptr;
    const std::vector<std::string>*       favorites_ = nullptr;
    std::vector<studio::PresetRegistry::PresetInfo> rows_;
    /* Engine-catalog-only rows (id/name/category/counts). */
    QVariantList                          extra_;
};

} /* namespace ui */
} /* namespace openrgb2 */
