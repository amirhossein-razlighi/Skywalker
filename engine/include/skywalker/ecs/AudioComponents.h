#pragma once
// Audio components. Included at the end of Components.h; implemented in audio/AudioComponents.cpp.

#include <string>
#include <vector>

#include "skywalker/ecs/Reflection.h"

namespace sky {

/// A sound emitter: plays an audio clip (wav/mp3/flac) from the entity's position. It starts
/// when play begins (`playOnStart`) or when Wander calls play(entity); nothing is heard
/// while the scene is being edited.
struct AudioSource {
    std::string clip;               // project-relative path, e.g. "audio/campfire_loop.wav"
    float volume = 1.f;             // linear gain
    float pitch = 1.f;              // playback speed multiplier (also changes pitch)
    bool loop = false;
    bool playOnStart = true;        // start automatically when the game starts
    bool spatial = true;            // 3D positioned (distance falloff + panning); false = plain stereo
    float minDistance = 1.f;        // full volume inside this distance (m)
    float maxDistance = 50.f;       // attenuation stops growing beyond this distance (m)
    std::string rolloff = "inverse";  // how volume falls with distance: inverse | linear | exponential | none
    float rolloffFactor = 1.f;      // steepness of the falloff
    std::string bus = "sfx";        // mixer bus: master | music | sfx | ambience | voice | ui
    float doppler = 0.f;            // pitch shift from relative motion (0 = off, 1 = realistic)

    static const TypeInfo& type();
};

/// The "ears". Optional: without one, sound is heard from the active scene camera. Add it
/// to a different entity (the player) to hear from there.
struct AudioListener {
    bool enabled = true;
    float volume = 1.f;  // listener gain

    static const TypeInfo& type();
};

}  // namespace sky
