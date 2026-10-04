#pragma once
// sprite_trail: afterimages of a moving sprite (dashes, lunges, fast slashes, a boss swooping).
// While the entity moves faster than `minSpeed`, the trail records a snapshot (world transform,
// animation frame, flip) every `interval` seconds on fixed ticks; render2d::gather2D draws the last
// `count` snapshots behind the sprite as tinted, fading copies. Snapshots expire after
// count * interval seconds, so a stopped character's trail dissolves. Deterministic (fixed ticks)
// and cleared when play stops. Simulation: render2d/SpriteTrail.cpp.

#include <string>
#include <vector>

#include "skywalker/ecs/Reflection.h"
#include "skywalker/math/Math.h"

namespace sky {

/// One recorded pose of a trailing sprite (runtime only).
struct TrailSnapshot {
    Mat4 world;              // the entity's world matrix when recorded
    std::string texture;     // the sheet the animator showed ("" = the sprite's own texture/frame)
    int columns = 1, rows = 1;
    int frame = -1;          // animator frame (-1 = the sprite's static frame)
    bool flipX = false;
    float age = 0.f;         // seconds since recorded
};

struct SpriteTrail {
    bool emitting = true;            // record new afterimages (old ones still fade out)
    int count = 6;                   // afterimages drawn at most
    float interval = 0.035f;         // seconds between snapshots
    float minSpeed = 2.f;            // world units per second the entity must move for new snapshots (0 = always)
    Vec4 color{0.55f, 0.75f, 1.f, 1.f};  // tint of the afterimages (sRGB)
    float opacity = 0.55f;           // opacity of the newest afterimage; older ones fade to 0
    float emissive = 0.6f;           // glow strength of the afterimages (tinted by color)
    bool additive = false;           // blend afterimages additively (energy) instead of as translucent copies

    // Runtime state (not serialized; cleared when play stops).
    std::vector<TrailSnapshot> snapshots_;  // newest last
    float timer_ = 0.f;
    Vec3 lastPos_{0.f, 0.f, 0.f};
    bool started_ = false;

    static const TypeInfo& type();
};

}  // namespace sky
