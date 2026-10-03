#include <type_traits>

#include "skywalker/ecs/Components.h"

namespace sky {

static_assert(std::is_standard_layout_v<AudioSource>);
static_assert(std::is_standard_layout_v<AudioListener>);

const TypeInfo& AudioSource::type() {
    static const TypeInfo info{
        "audio",
        "Plays a sound clip from this entity: music, ambience, sound effects. Spatial by default (louder when near, "
        "panned left/right). Plays only while the game runs. Generate clips with the audio_generate tool, then set "
        "`clip`. One-shots are easier from Wander: play_sound(\"audio/hit.wav\").",
        {
            SKY_FIELD(AudioSource, clip, String, "Audio file in the project (wav, mp3 or flac), e.g. audio/wind_loop.wav"),
            SKY_FIELD_RANGE(AudioSource, volume, Float, "Linear gain (1 = unchanged)", 0.f, 4.f),
            SKY_FIELD_RANGE(AudioSource, pitch, Float, "Playback speed multiplier; also shifts pitch", 0.1f, 4.f),
            SKY_FIELD(AudioSource, loop, Bool, "Repeat the clip forever (ambience, music, engines)"),
            SKY_FIELD(AudioSource, playOnStart, Bool, "Start automatically when the game starts"),
            SKY_FIELD(AudioSource, spatial, Bool, "3D sound: distance attenuation and panning; false = plain stereo (music, UI)"),
            SKY_FIELD_RANGE(AudioSource, minDistance, Float, "Full volume within this distance (m)", 0.01f, 10000.f),
            SKY_FIELD_RANGE(AudioSource, maxDistance, Float, "Distance where attenuation stops (inverse/exponential) or reaches silence (linear)", 0.1f, 100000.f),
            SKY_FIELD_ENUM(AudioSource, rolloff, "Distance falloff curve (inverse is physically natural; linear fades to silence at maxDistance)",
                           "inverse", "linear", "exponential", "none"),
            SKY_FIELD_RANGE(AudioSource, rolloffFactor, Float, "Falloff steepness (1 = natural)", 0.f, 20.f),
            SKY_FIELD_ENUM(AudioSource, bus, "Mixer bus whose volume/mute applies (see the audio_mix tool)", "master", "music",
                           "sfx", "ambience", "voice", "ui"),
            SKY_FIELD_RANGE(AudioSource, doppler, Float, "Doppler effect strength for moving emitters (0 = off, 1 = realistic)", 0.f, 5.f),
        }};
    return info;
}

const TypeInfo& AudioListener::type() {
    static const TypeInfo info{
        "listener",
        "The ears of the game. Optional: by default sound is heard from the active scene camera. Put this on another "
        "entity (the player character) to hear from there.",
        {
            SKY_FIELD(AudioListener, enabled, Bool, "Use this entity as the listener"),
            SKY_FIELD_RANGE(AudioListener, volume, Float, "Overall gain heard by this listener", 0.f, 4.f),
        }};
    return info;
}

}  // namespace sky
