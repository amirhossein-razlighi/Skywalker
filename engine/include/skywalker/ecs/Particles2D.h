#pragma once
// particles2d: pixel-art particles on the 2D plane (rain streaks, snowflakes, falling leaves and
// petals, fireflies, chimney puffs, puddle ripples). Unlike the 3D `particles` component they are
// drawn as sprites (nearest sampling, texel snapping, sorting layers) and simulated on the CPU on
// fixed ticks with a seeded generator, so they replay identically and reset when play stops.
// Simulation: render2d/Particles2D.cpp; drawing: render2d::gather2D; tools: Particles2DTools.cpp.

#include <cstdint>
#include <string>
#include <vector>

#include "skywalker/ecs/Reflection.h"
#include "skywalker/math/Math.h"

namespace sky {

/// One live 2D particle (runtime state, never serialized).
struct Particle2D {
    Vec2 pos;          // world (or wrap-box local) position
    Vec2 vel;
    float age = 0.f;
    float life = 1.f;
    float phase = 0.f;   // sway / pulse phase (radians)
    float shade = 1.f;   // brightness multiplier (colorJitter)
    int frame = 0;       // frame index into the selected frames (random mode)
};

struct Particles2D {
    bool emitting = true;
    std::string texture;              // grid sheet (png) or atlas; empty = solid rectangles of pixelSize texels
    int columns = 1;
    int rows = 1;
    std::string frames;               // sheet frames to use ("0-3,6"; empty = all)
    std::string animate = "random";   // random (each particle keeps one frame) | life (frames over its life) | loop (at fps)
    float fps = 8.f;
    float pixelsPerUnit = 16.f;
    Vec2 pixelSize{1.f, 1.f};         // untextured particles: size in texels
    float rate = 10.f;                // particles per second
    int burst = 0;                    // emitted at once when play starts
    int maxParticles = 300;
    float lifetime = 3.f;             // seconds
    float lifetimeJitter = 0.3f;      // fraction
    Vec2 area{8.f, 8.f};              // emission box (world units) centered on the entity
    bool wrap = false;                // weather: the box tiles endlessly, so particles fill any view the camera shows
    Vec2 velocity{0.f, -1.f};         // world units per second
    Vec2 velocityJitter{0.2f, 0.2f};  // +- per axis
    Vec2 gravity{0.f, 0.f};
    float drag = 0.f;                 // velocity damping per second
    float sway = 0.f;                 // side-to-side drift (world units/s): leaves, snow, petals
    float swayFrequency = 1.f;        // Hz
    float flutter = 0.f;              // random-walk wander (world units/s): fireflies, butterflies
    Vec4 color{1.f, 1.f, 1.f, 1.f};
    float colorJitter = 0.f;          // per-particle brightness variation (0..1)
    Vec4 emissive{0.f, 0.f, 0.f, 1.f};  // glow color, alpha = strength
    float pulse = 0.f;                // 0..1 per-particle twinkle of opacity and glow (fireflies)
    float pulseFrequency = 1.f;       // Hz
    float fadeIn = 0.1f;              // fraction of the life spent fading in
    float fadeOut = 0.25f;            // fraction of the life spent fading out
    std::string sortingLayer = "foreground";
    int order = 0;
    bool lit = false;                 // lit by 2D lights (night rain) — otherwise full bright
    bool prewarm = true;              // start as if it had been running for a while
    int seed = 0;

    // Runtime state (owned by the simulation, not serialized; cleared when play stops).
    std::vector<Particle2D> particles_;
    float carry_ = 0.f;
    uint64_t rng_ = 0;
    bool started_ = false;
    int pendingBurst_ = 0;            // burst(entity, n) from Wander: spawned on the next tick

    static const TypeInfo& type();
};

}  // namespace sky
