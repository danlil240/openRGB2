/*---------------------------------------------------------*\
|| SceneGraph.h                                              ||
||                                                           ||
||   One transform convention for renderer and effects.    ||
||   Qt-free: the same math runs under plain cl and is     ||
||   what the bridge hands to the QML node tree.           ||
||                                                           ||
||   - parent_id gives an object its local placement       ||
||     frame; a child's world transform is                 ||
||     parentWorld * localTransform (T * R * S).           ||
||   - Stored rotation_deg (XYZ degrees, Rz*Ry*Rx) is      ||
||     converted ONCE into a normalized quaternion that    ||
||     both the core matrices and the QML Node `rotation`  ||
||     property consume — no second Euler order exists.    ||
||   - mirror_of is output ownership, not placement.       ||
||                                                           ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#pragma once

#include "SceneTypes.h"

#include <map>
#include <string>
#include <vector>

namespace studio
{

/*---------------------------------------------------------*\
|| Quat — normalized rotation quaternion, scalar part w.  ||
|| Construct ONLY via RotationQuat so every consumer sees ||
|| the same conversion of the stored XYZ degrees.         ||
\*---------------------------------------------------------*/
struct Quat
{
    float w = 1.0f;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

/* Stored rotation_deg -> quaternion under the core's
   Rz*Ry*Rx convention. This is THE Euler conversion —
   SceneBridge publishes it to QML (Qt.quaternion(w,x,y,z))
   and LocalMatrix builds its rotation from it. */
Quat RotationQuat(const Vec3& rotation_deg);

Vec3 RotateVec(const Quat& q, const Vec3& v);

/*---------------------------------------------------------*\
|| Mat4 — 4x4 affine transform, column-major              ||
|| (m[col*4 + row], translation in m[12..14]), matching   ||
|| the QMatrix4x4/QQuick3D memory layout.                 ||
\*---------------------------------------------------------*/
struct Mat4
{
    float m[16];
};

Mat4 Mat4Identity();
Mat4 Mat4Mul(const Mat4& a, const Mat4& b);   /* a * b — b applies first */
Vec3 TransformPoint(const Mat4& m, const Vec3& p);

/* local = T * R(rotation_deg) * S */
Mat4 LocalMatrix(const Transform& t);

/* object id -> world matrix = parentWorld * localTransform,
   resolved over parent_id chains. Robust on invalid input:
   a dangling or cyclic parent_id is treated as "no parent"
   (validation is what rejects bad documents). */
std::map<std::string, Mat4> ResolveWorldMatrices(const SceneDocument& doc);

/* doc.objects reordered so every object's parent precedes it.
   The bridge model needs this: the QML delegate looks its
   parent's node up in a map populated in model order. */
std::vector<const SceneObject*> TopologicalOrder(const SceneDocument& doc);

/*---------------------------------------------------------*\
|| ValidateSceneGraph — whole-document structural checks: ||
|| unique ids, finite transforms, positive dimensionless  ||
|| scale, resolvable parent_id with no cycles, resolvable ||
|| mirror_of with no chains/cycles (a Linked object may   ||
|| only mirror a non-Linked owner). `errors` may be null. ||
\*---------------------------------------------------------*/
bool ValidateSceneGraph(const SceneDocument& doc,
                        std::vector<std::string>* errors);

} /* namespace studio */
