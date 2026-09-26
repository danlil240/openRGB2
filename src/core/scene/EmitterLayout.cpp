/*---------------------------------------------------------*\
|| EmitterLayout.cpp                                         |
||                                                           |
||   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include "EmitterLayout.h"

#include <cmath>

namespace studio
{
namespace layout
{

std::vector<Emitter> Ring(unsigned int n, float radius,
                          float start_angle_deg, bool reversed,
                          const std::string& group, int addr_base,
                          float face_y)
{
    constexpr float DEG = 3.14159265358979323846f / 180.0f;
    std::vector<Emitter> out;
    out.reserve(n);

    for(unsigned int i = 0; i < n; i++)
    {
        /* address walks the physical LED order; direction and start
           angle only change where each address sits in space.       */
        const float step = reversed ? -(float)i : (float)i;
        const float ang  = (start_angle_deg + step * (360.0f / (float)n)) * DEG;

        Emitter e;
        e.local_pos = { radius * std::cos(ang), face_y, -radius * std::sin(ang) };
        e.group     = group;
        e.address   = (int)addr_base + (int)i;
        out.push_back(e);
    }
    return out;
}

std::vector<Emitter> Strip(unsigned int n, float spacing,
                           const Vec3& origin,
                           const std::string& group, int addr_base)
{
    std::vector<Emitter> out;
    out.reserve(n);
    for(unsigned int i = 0; i < n; i++)
    {
        Emitter e;
        e.local_pos = { origin.x + (float)i * spacing, origin.y, origin.z };
        e.group     = group;
        e.address   = (int)addr_base + (int)i;
        out.push_back(e);
    }
    return out;
}

std::vector<Emitter> KeyboardMatrix(unsigned int rows, unsigned int cols,
                                    const std::vector<unsigned int>& map,
                                    unsigned int empty,
                                    float pitch_x, float pitch_z,
                                    const Vec3& origin,
                                    const std::string& group)
{
    std::vector<Emitter> out;
    for(unsigned int r = 0; r < rows; r++)
    {
        for(unsigned int c = 0; c < cols; c++)
        {
            const size_t idx = (size_t)r * cols + c;
            if(idx >= map.size())
            {
                /* Short map — the file validator requires >=
                   rows*cols, but a hardware/programmatic caller
                   must never read out of bounds. */
                return out;
            }
            const unsigned int led = map[idx];
            if(led == empty)
            {
                continue;
            }
            Emitter e;
            e.local_pos = { origin.x + (float)c * pitch_x,
                            origin.y,
                            origin.z - (float)r * pitch_z };
            e.group     = group;
            e.address   = (int)led;
            out.push_back(e);
        }
    }
    return out;
}

} /* namespace layout */
} /* namespace studio */
