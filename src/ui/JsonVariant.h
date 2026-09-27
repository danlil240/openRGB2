/*---------------------------------------------------------*\
||| JsonVariant.h                                             ||
|||                                                           ||
|||   nlohmann::json <-> QVariant bridge for the QML side.  ||
|||   IPC bodies cross the wire as JSON; the EngineBridge   ||
|||   exposes them to QML as QVariantMap/List.              ||
|||                                                           ||
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
|\*---------------------------------------------------------*/

#pragma once

#include <nlohmann/json.hpp>

#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

#include <QString>

namespace openrgb2
{
namespace ui
{

inline QVariant JsonToVariant(const nlohmann::json& j)
{
    if(j.is_discarded() || j.is_null())
    {
        return QVariant();
    }
    if(j.is_object())
    {
        QVariantMap m;
        for(auto it = j.begin(); it != j.end(); ++it)
        {
            m.insert(QString::fromStdString(it.key()),
                     JsonToVariant(it.value()));
        }
        return m;
    }
    if(j.is_array())
    {
        QVariantList l;
        l.reserve((qsizetype)j.size());
        for(const nlohmann::json& e : j)
        {
            l.push_back(JsonToVariant(e));
        }
        return l;
    }
    if(j.is_boolean())
    {
        return j.get<bool>();
    }
    if(j.is_number_integer())
    {
        return QVariant::fromValue<qlonglong>(
            j.get<long long>());
    }
    if(j.is_number_unsigned())
    {
        return QVariant::fromValue<qulonglong>(
            j.get<unsigned long long>());
    }
    if(j.is_number_float())
    {
        return j.get<double>();
    }
    if(j.is_string())
    {
        return QString::fromStdString(j.get<std::string>());
    }
    return QVariant();
}

inline nlohmann::json VariantToJson(const QVariant& v)
{
    if(!v.isValid() || v.isNull())
    {
        return nullptr;
    }
    switch(v.typeId())
    {
    case QMetaType::QVariantMap:
    case QMetaType::QVariantHash:
    {
        nlohmann::json o = nlohmann::json::object();
        const QVariantMap m = v.toMap();
        for(auto it = m.begin(); it != m.end(); ++it)
        {
            o[it.key().toStdString()] = VariantToJson(it.value());
        }
        return o;
    }
    case QMetaType::QVariantList:
    case QMetaType::QStringList:
    {
        nlohmann::json a = nlohmann::json::array();
        const QVariantList l = v.toList();
        for(const QVariant& e : l)
        {
            a.push_back(VariantToJson(e));
        }
        return a;
    }
    case QMetaType::Bool:
        return v.toBool();
    case QMetaType::Int:
    case QMetaType::LongLong:
        return v.toLongLong();
    case QMetaType::UInt:
    case QMetaType::ULongLong:
        return v.toULongLong();
    case QMetaType::Float:
    case QMetaType::Double:
        return v.toDouble();
    default:
        if(v.canConvert<QString>())
        {
            return v.toString().toStdString();
        }
        return nullptr;
    }
}

} /* namespace ui */
} /* namespace openrgb2 */
