/*---------------------------------------------------------*\
|| TransformCommands.cpp                                     ||
||                                                           ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#include "TransformCommands.h"

#include <cmath>

namespace studio
{

Quat QuatMul(const Quat& a, const Quat& b)
{
    Quat r;
    r.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
    r.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
    r.y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
    r.z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
    return r;
}

Quat QuatConjugate(const Quat& q)
{
    return { q.w, -q.x, -q.y, -q.z };
}

Quat AxisAngleQuat(const Vec3& axis, float degrees)
{
    const float n = std::sqrt(axis.x * axis.x + axis.y * axis.y
                              + axis.z * axis.z);
    if(n <= 0.0f)
    {
        return {};
    }
    constexpr float DEG = 3.14159265358979323846f / 180.0f;
    const float h = degrees * DEG * 0.5f;
    const float s = std::sin(h) / n;
    Quat q;
    q.w = std::cos(h);
    q.x = axis.x * s;
    q.y = axis.y * s;
    q.z = axis.z * s;
    return q;
}

Mat4 QuatMatrix(const Quat& q)
{
    /* Same quat->matrix as LocalMatrix with unit scale. */
    const float x2 = q.x + q.x, y2 = q.y + q.y, z2 = q.z + q.z;
    const float xx = q.x * x2, xy = q.x * y2,  xz = q.x * z2;
    const float yy = q.y * y2, yz = q.y * z2,  zz = q.z * z2;
    const float wx = q.w * x2, wy = q.w * y2,  wz = q.w * z2;

    Mat4 m = Mat4Identity();
    m.m[0] = 1.0f - (yy + zz);
    m.m[1] = xy + wz;
    m.m[2] = xz - wy;
    m.m[4] = xy - wz;
    m.m[5] = 1.0f - (xx + zz);
    m.m[6] = yz + wx;
    m.m[8]  = xz + wy;
    m.m[9]  = yz - wx;
    m.m[10] = 1.0f - (xx + yy);
    return m;
}

Mat4 RigidInverse(const Mat4& m)
{
    /* R^-1 = R^T (orthonormal); t' = -R^T t. Column-major. */
    Mat4 r = Mat4Identity();
    r.m[0] = m.m[0];  r.m[4] = m.m[1];  r.m[8]  = m.m[2];
    r.m[1] = m.m[4];  r.m[5] = m.m[5];  r.m[9]  = m.m[6];
    r.m[2] = m.m[8];  r.m[6] = m.m[9];  r.m[10] = m.m[10];
    r.m[12] = -(r.m[0] * m.m[12] + r.m[4] * m.m[13] + r.m[8]  * m.m[14]);
    r.m[13] = -(r.m[1] * m.m[12] + r.m[5] * m.m[13] + r.m[9]  * m.m[14]);
    r.m[14] = -(r.m[2] * m.m[12] + r.m[6] * m.m[13] + r.m[10] * m.m[14]);
    return r;
}

Vec3 RigidInversePoint(const Mat4& m, const Vec3& p)
{
    /* local = R^T * (p - t): dot(d, column i). */
    const float dx = p.x - m.m[12];
    const float dy = p.y - m.m[13];
    const float dz = p.z - m.m[14];
    return {
        dx * m.m[0] + dy * m.m[1] + dz * m.m[2],
        dx * m.m[4] + dy * m.m[5] + dz * m.m[6],
        dx * m.m[8] + dy * m.m[9] + dz * m.m[10],
    };
}

Vec3 Mat4Translation(const Mat4& m)
{
    return { m.m[12], m.m[13], m.m[14] };
}

Vec3 EulerDegFromMat4(const Mat4& r)
{
    constexpr float RAD = 180.0f / 3.14159265358979323846f;
    /* For R = Rz*Ry*Rx (the stored convention):
         R[2][0] = -sin(y)                      -> m[2]
         R[2][1] = cos(y) sin(x), R[2][2] = cos(y) cos(x) -> m[6], m[10]
         R[1][0] = sin(z) cos(y), R[0][0] = cos(z) cos(y) -> m[1], m[0] */
    float sy = -r.m[2];
    if(sy >  1.0f) { sy =  1.0f; }
    if(sy < -1.0f) { sy = -1.0f; }
    Vec3 out;
    out.y = std::asin(sy) * RAD;
    if(std::fabs(r.m[2]) < 0.9999f)
    {
        out.x = std::atan2(r.m[6], r.m[10]) * RAD;
        out.z = std::atan2(r.m[1], r.m[0])  * RAD;
    }
    else
    {
        /* Gimbal lock: fold z into x (z = 0 convention). */
        out.x = -std::atan2(r.m[9], r.m[5]) * RAD;
        out.z = 0.0f;
    }
    return out;
}

bool EditorEdit::Empty() const
{
    return devices.Empty() && settings.Empty() && bindings.Empty()
        && object_colors.Empty() && emitter_colors.Empty()
        && !has_effect;
}

bool EditorEdit::TransformsOnly() const
{
    if(!settings.Empty() || !bindings.Empty()
       || !object_colors.Empty() || !emitter_colors.Empty()
       || has_effect)
    {
        return false;
    }
    if(devices.before.size() != devices.after.size())
    {
        return false;
    }
    for(const auto& kv : devices.after)
    {
        const auto it = devices.before.find(kv.first);
        if(it == devices.before.end())
        {
            return false;
        }
        if(it->second.type != kv.second.type
           || it->second.parent != kv.second.parent)
        {
            return false;
        }
    }
    return true;
}

std::set<std::string> EditorEdit::TransformIds() const
{
    std::set<std::string> ids;
    for(const auto& kv : devices.after)
    {
        ids.insert(kv.first);
    }
    for(const auto& kv : devices.before)
    {
        ids.insert(kv.first);
    }
    return ids;
}

void ApplyEditorEdit(StudioDocument& ws, const EditorEdit& e)
{
    ApplySection(ws.devices,         e.devices);
    ApplySection(ws.device_settings, e.settings);
    ApplySection(ws.bindings,        e.bindings);
    ApplySection(ws.object_colors,   e.object_colors);
    ApplySection(ws.emitter_colors,  e.emitter_colors);
    if(e.has_effect)
    {
        ws.effect.preset = e.effect_after.preset;
        ws.effect.seed   = e.effect_after.seed;
        ws.effect.layers = e.effect_after.layers;
    }
}

void RevertEditorEdit(StudioDocument& ws, const EditorEdit& e)
{
    RevertSection(ws.devices,         e.devices);
    RevertSection(ws.device_settings, e.settings);
    RevertSection(ws.bindings,        e.bindings);
    RevertSection(ws.object_colors,   e.object_colors);
    RevertSection(ws.emitter_colors,  e.emitter_colors);
    if(e.has_effect)
    {
        ws.effect.preset = e.effect_before.preset;
        ws.effect.seed   = e.effect_before.seed;
        ws.effect.layers = e.effect_before.layers;
    }
}

} /* namespace studio */
