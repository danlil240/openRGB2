/*---------------------------------------------------------*\
|| SceneTypes.cpp                                            |
||                                                           |
||   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include "SceneTypes.h"
#include "SceneGraph.h"

namespace studio
{

/* T * Rxyz * S — implemented through LocalMatrix so the quaternion
   path (RotationQuat) is the single rotation convention for the
   whole plugin. */
Vec3 TransformPoint(const Transform& t, const Vec3& p)
{
    return TransformPoint(LocalMatrix(t), p);
}

const SceneObject* FindObject(const SceneDocument& doc, const std::string& id)
{
    for(const SceneObject& obj : doc.objects)
    {
        if(obj.id == id)
        {
            return &obj;
        }
    }
    return nullptr;
}

SceneObject* FindObject(SceneDocument& doc, const std::string& id)
{
    for(SceneObject& obj : doc.objects)
    {
        if(obj.id == id)
        {
            return &obj;
        }
    }
    return nullptr;
}

/* Canonical body dimensions in meters for every geometry the
   renderer knows (ui/StudioScene.qml's bodySpec). These double as
   the per-axis fallback when a size_m component is <= 0, so a
   decor body authored without size_m still renders instead of
   collapsing to an invisible zero-volume mesh. */
Vec3 CanonicalBodySize(const std::string& geometry)
{
    if(geometry == "desk")          return { 1.4f,   0.04f,  0.75f  };
    if(geometry == "case_shell")    return { 0.21f,  0.47f,  0.46f  };
    if(geometry == "monitor")       return { 0.62f,  0.36f,  0.02f  };
    if(geometry == "mouse_body")    return { 0.066f, 0.04f,  0.117f };
    if(geometry == "gpu_body")      return { 0.30f,  0.05f,  0.13f  };
    if(geometry == "keyboard_body") return { 0.45f,  0.03f,  0.145f };
    if(geometry == "fan_body")      return { 0.125f, 0.028f, 0.125f };
    if(geometry == "ram_body")      return { 0.135f, 0.045f, 0.008f };
    if(geometry == "pump_body")     return { 0.055f, 0.045f, 0.055f };
    return { 0.0f, 0.0f, 0.0f };
}

Vec3 ResolvedBodySize(const SceneObject& o)
{
    const Vec3 c = CanonicalBodySize(o.geometry);
    return {
        o.size_m.x > 0.0f ? o.size_m.x : c.x,
        o.size_m.y > 0.0f ? o.size_m.y : c.y,
        o.size_m.z > 0.0f ? o.size_m.z : c.z,
    };
}

const SceneObject* OutputOwner(const SceneDocument& doc, const std::string& id)
{
    const SceneObject* obj = FindObject(doc, id);
    if(obj != nullptr && obj->kind == ObjectKind::Linked && !obj->mirror_of.empty())
    {
        obj = FindObject(doc, obj->mirror_of);
    }
    return obj;
}

SceneColor EmitterColor(const SceneDocument& doc, const std::string& object_id,
                        int emitter_index)
{
    const SceneObject* owner = OutputOwner(doc, object_id);
    const std::string& owner_id = owner ? owner->id : object_id;

    auto ov = doc.emitter_colors.find(owner_id);
    if(ov != doc.emitter_colors.end())
    {
        auto it = ov->second.find(emitter_index);
        if(it != ov->second.end())
        {
            return it->second;
        }
    }
    auto base = doc.object_colors.find(owner_id);
    return (base != doc.object_colors.end()) ? base->second : 0;
}

} /* namespace studio */
