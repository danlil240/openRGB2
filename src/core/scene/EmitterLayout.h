/*---------------------------------------------------------*\
|| EmitterLayout.h                                           |
||                                                           |
||   Parametric emitter-position generators. All produce    |
||   object-local coordinates in meters, Y up.              |
||                                                           |
||   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#pragma once

#include "SceneTypes.h"

namespace studio
{
namespace layout
{

/*---------------------------------------------------------*\
|| Ring — n emitters on a circle in the local XZ plane     |
|| (face-on to +/-Y). Angle 0 = +X axis, counterclockwise  |
|| seen from +Y looking down. `reversed` walks indices the |
|| other way; `start_angle_deg` rotates where LED 0 sits.  |
|| Both are the LED-order calibration knobs.               |
\*---------------------------------------------------------*/
std::vector<Emitter> Ring(unsigned int n, float radius,
                          float start_angle_deg, bool reversed,
                          const std::string& group, int addr_base = 0,
                          float face_y = 0.0f);

/*---------------------------------------------------------*\
|| Strip — n emitters spaced along +X from `origin`.       |
\*---------------------------------------------------------*/
std::vector<Emitter> Strip(unsigned int n, float spacing,
                           const Vec3& origin,
                           const std::string& group, int addr_base = 0);

/*---------------------------------------------------------*\
|| KeyboardMatrix — emitters from a zone matrix map:       |
|| map[row*cols+col] gives the LED index (or skip when     |
|| equal to `empty`). Origin is the top-left key center,   |
|| keys march +X across and -Z down (desk plane). The map  |
|| is passed by vector so a short map can never be read    |
|| out of bounds — cells past the map are treated empty.   |
\*---------------------------------------------------------*/
std::vector<Emitter> KeyboardMatrix(unsigned int rows, unsigned int cols,
                                    const std::vector<unsigned int>& map,
                                    unsigned int empty,
                                    float pitch_x, float pitch_z,
                                    const Vec3& origin,
                                    const std::string& group);

} /* namespace layout */
} /* namespace studio */
