/*---------------------------------------------------------*\
|| SceneGraph.cpp                                            ||
||                                                           ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#include "SceneGraph.h"

#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace studio
{

static bool Finite(const Vec3& v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

Quat RotationQuat(const Vec3& rotation_deg)
{
    constexpr float DEG = 3.14159265358979323846f / 180.0f;
    const float hx = rotation_deg.x * DEG * 0.5f;
    const float hy = rotation_deg.y * DEG * 0.5f;
    const float hz = rotation_deg.z * DEG * 0.5f;
    const float cx = std::cos(hx), sx = std::sin(hx);
    const float cy = std::cos(hy), sy = std::sin(hy);
    const float cz = std::cos(hz), sz = std::sin(hz);

    /* q = qz * qy * qx — Hamilton product, so the composed rotation
       matrix is Rz*Ry*Rx (column-vector convention). */
    Quat q;
    q.x = sx * cy * cz - cx * sy * sz;
    q.y = cx * sy * cz + sx * cy * sz;
    q.z = cx * cy * sz - sx * sy * cz;
    q.w = cx * cy * cz + sx * sy * sz;

    const float n = std::sqrt(q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w);
    if(n > 0.0f)
    {
        q.x /= n; q.y /= n; q.z /= n; q.w /= n;
    }
    return q;
}

Vec3 RotateVec(const Quat& q, const Vec3& v)
{
    /* v' = q * v * q^-1 expanded (q normalized). */
    const float tx = 2.0f * (q.y * v.z - q.z * v.y);
    const float ty = 2.0f * (q.z * v.x - q.x * v.z);
    const float tz = 2.0f * (q.x * v.y - q.y * v.x);
    return {
        v.x + q.w * tx + q.y * tz - q.z * ty,
        v.y + q.w * ty + q.z * tx - q.x * tz,
        v.z + q.w * tz + q.x * ty - q.y * tx,
    };
}

Mat4 Mat4Identity()
{
    Mat4 m = {};
    m.m[0] = m.m[5] = m.m[10] = m.m[15] = 1.0f;
    return m;
}

Mat4 Mat4Mul(const Mat4& a, const Mat4& b)
{
    Mat4 r;
    for(int c = 0; c < 4; c++)
    {
        for(int row = 0; row < 4; row++)
        {
            r.m[c * 4 + row] =
                  a.m[0 * 4 + row] * b.m[c * 4 + 0]
                + a.m[1 * 4 + row] * b.m[c * 4 + 1]
                + a.m[2 * 4 + row] * b.m[c * 4 + 2]
                + a.m[3 * 4 + row] * b.m[c * 4 + 3];
        }
    }
    return r;
}

Vec3 TransformPoint(const Mat4& m, const Vec3& p)
{
    return {
        m.m[0] * p.x + m.m[4] * p.y + m.m[8]  * p.z + m.m[12],
        m.m[1] * p.x + m.m[5] * p.y + m.m[9]  * p.z + m.m[13],
        m.m[2] * p.x + m.m[6] * p.y + m.m[10] * p.z + m.m[14],
    };
}

Mat4 LocalMatrix(const Transform& t)
{
    const Quat q = RotationQuat(t.rotation_deg);

    /* R = quat-to-matrix (unit quaternion assumed — RotationQuat
       normalizes). Column-major. */
    const float x2 = q.x + q.x, y2 = q.y + q.y, z2 = q.z + q.z;
    const float xx = q.x * x2, xy = q.x * y2,  xz = q.x * z2;
    const float yy = q.y * y2, yz = q.y * z2,  zz = q.z * z2;
    const float wx = q.w * x2, wy = q.w * y2,  wz = q.w * z2;

    Mat4 m = Mat4Identity();
    m.m[0] = (1.0f - (yy + zz)) * t.scale.x;
    m.m[1] = (xy + wz)          * t.scale.x;
    m.m[2] = (xz - wy)          * t.scale.x;

    m.m[4] = (xy - wz)          * t.scale.y;
    m.m[5] = (1.0f - (xx + zz)) * t.scale.y;
    m.m[6] = (yz + wx)          * t.scale.y;

    m.m[8]  = (xz + wy)         * t.scale.z;
    m.m[9]  = (yz - wx)         * t.scale.z;
    m.m[10] = (1.0f - (xx + yy)) * t.scale.z;

    m.m[12] = t.position.x;
    m.m[13] = t.position.y;
    m.m[14] = t.position.z;
    return m;
}

std::map<std::string, Mat4> ResolveWorldMatrices(const SceneDocument& doc)
{
    std::unordered_map<std::string, const SceneObject*> by_id;
    for(const SceneObject& o : doc.objects)
    {
        by_id[o.id] = &o;
    }

    std::map<std::string, Mat4> world;
    std::unordered_set<std::string> visiting;

    /* DFS up the parent chain; memoize into `world`. A node already
       in `visiting` means a cycle (invalid doc) — the cycle member's
       parent link is cut so the resolution always terminates. */
    for(const SceneObject& root : doc.objects)
    {
        const SceneObject* cur = &root;
        std::vector<const SceneObject*> chain;
        while(cur != nullptr && world.find(cur->id) == world.end())
        {
            if(!visiting.insert(cur->id).second)
            {
                break;  /* cycle — treat `cur` as a root */
            }
            chain.push_back(cur);
            const SceneObject* parent = nullptr;
            if(!cur->parent_id.empty())
            {
                auto it = by_id.find(cur->parent_id);
                if(it != by_id.end())
                {
                    parent = it->second;
                }
            }
            cur = parent;
        }
        /* `cur` is either nullptr, a resolved node, or the cut point
           of a cycle; `chain` holds unresolved nodes root-first. */
        Mat4 pw = (cur != nullptr && world.find(cur->id) != world.end())
                ? world[cur->id]
                : Mat4Identity();
        for(auto it = chain.rbegin(); it != chain.rend(); ++it)
        {
            pw = Mat4Mul(pw, LocalMatrix((*it)->transform));
            world[(*it)->id] = pw;
        }
        for(const SceneObject* o : chain)
        {
            visiting.erase(o->id);
        }
    }
    return world;
}

std::vector<const SceneObject*> TopologicalOrder(const SceneDocument& doc)
{
    std::unordered_map<std::string, const SceneObject*> by_id;
    for(const SceneObject& o : doc.objects)
    {
        by_id[o.id] = &o;
    }

    std::vector<const SceneObject*> out;
    out.reserve(doc.objects.size());
    std::unordered_set<std::string> emitted;
    std::unordered_set<std::string> visiting;

    for(const SceneObject& o : doc.objects)
    {
        std::vector<const SceneObject*> chain;
        const SceneObject* cur = &o;
        while(cur != nullptr && emitted.find(cur->id) == emitted.end())
        {
            if(!visiting.insert(cur->id).second)
            {
                break;  /* cycle — cut, emit as-is */
            }
            chain.push_back(cur);
            cur = nullptr;
            if(!chain.back()->parent_id.empty())
            {
                auto it = by_id.find(chain.back()->parent_id);
                if(it != by_id.end())
                {
                    cur = it->second;
                }
            }
        }
        for(auto it = chain.rbegin(); it != chain.rend(); ++it)
        {
            if(emitted.insert((*it)->id).second)
            {
                out.push_back(*it);
            }
        }
        for(const SceneObject* p : chain)
        {
            visiting.erase(p->id);
        }
    }
    return out;
}

bool ValidateSceneGraph(const SceneDocument& doc,
                        std::vector<std::string>* errors)
{
    std::vector<std::string> local;
    std::vector<std::string>& errs = errors ? *errors : local;

    std::unordered_map<std::string, const SceneObject*> by_id;
    for(const SceneObject& o : doc.objects)
    {
        if(o.id.empty())
        {
            errs.push_back("object with empty id");
            continue;
        }
        if(!by_id.emplace(o.id, &o).second)
        {
            errs.push_back("duplicate object id: " + o.id);
        }
    }

    for(const SceneObject& o : doc.objects)
    {
        const std::string id = o.id.empty() ? std::string("?") : o.id;

        if(!Finite(o.transform.position) || !Finite(o.transform.rotation_deg)
           || !Finite(o.transform.scale) || !Finite(o.size_m))
        {
            errs.push_back(id + ": non-finite transform or size_m");
        }
        if(o.transform.scale.x <= 0.0f || o.transform.scale.y <= 0.0f
           || o.transform.scale.z <= 0.0f)
        {
            errs.push_back(id + ": scale must be positive");
        }
        if(o.size_m.x < 0.0f || o.size_m.y < 0.0f || o.size_m.z < 0.0f)
        {
            errs.push_back(id + ": size_m must be >= 0");
        }

        /* Placement: parent_id must resolve and not cycle. */
        if(!o.parent_id.empty())
        {
            if(o.parent_id == o.id)
            {
                errs.push_back(id + ": object is its own parent");
            }
            else if(by_id.find(o.parent_id) == by_id.end())
            {
                errs.push_back(id + ": dangling parent_id '" + o.parent_id + "'");
            }
            else
            {
                std::unordered_set<std::string> seen;
                const SceneObject* cur = &o;
                while(cur != nullptr && seen.insert(cur->id).second)
                {
                    auto it = by_id.find(cur->parent_id);
                    cur = (cur->parent_id.empty() || it == by_id.end())
                        ? nullptr : it->second;
                }
                if(cur != nullptr)
                {
                    errs.push_back(id + ": parent_id cycle through '" + cur->id + "'");
                }
            }
        }

        /* Output ownership: mirror_of is only meaningful on Linked
           objects and must land on a non-Linked object — chains
           would break OutputOwner's single-hop semantics. */
        if(o.kind == ObjectKind::Linked)
        {
            if(o.mirror_of.empty())
            {
                errs.push_back(id + ": linked object without mirror_of");
            }
            else if(o.mirror_of == o.id)
            {
                errs.push_back(id + ": object mirrors itself");
            }
            else
            {
                auto it = by_id.find(o.mirror_of);
                if(it == by_id.end())
                {
                    errs.push_back(id + ": dangling mirror_of '" + o.mirror_of + "'");
                }
                else if(it->second->kind == ObjectKind::Linked)
                {
                    errs.push_back(id + ": mirror_of chains through linked '"
                                   + o.mirror_of + "'");
                }
            }
        }
        else if(!o.mirror_of.empty())
        {
            errs.push_back(id + ": mirror_of set on non-linked object");
        }
    }

    return errs.empty();
}

} /* namespace studio */
