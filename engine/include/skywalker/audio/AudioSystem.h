#pragma once
// Runtime audio: mixing, spatialization, buses and music, built on miniaudio.
//
// The AudioSystem owned by the Engine plays `audio` components while the game runs, one-shots
// and music from Wander, and clip previews while editing. It is designed so that audio can
// never break the engine:
//   * Auto mode opens the output device lazily (on the first sound) and silently falls back
//     to a "null" engine when there is no device (CI, servers, headless agents);
//   * Null mode never opens a device but still mixes in simulated time, so playback state,
//     levels and spatialization are observable and testable;
//   * Off mode disables audio entirely.
// `SKYWALKER_AUDIO=off|null|auto` overrides the configured mode.

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "skywalker/audio/Wav.h"
#include "skywalker/core/Json.h"
#include "skywalker/core/Result.h"
#include "skywalker/math/Math.h"

namespace sky {
class Scene;
using EntityId = uint64_t;
}  // namespace sky

namespace sky::audio {

enum class AudioMode { Auto, Null, Off };
const char* toString(AudioMode m);
/// Applies the SKYWALKER_AUDIO environment override to a configured mode.
AudioMode resolveAudioMode(AudioMode configured);

/// What the game is doing: audio plays only while Playing. Paused = the editor's pause (everything
/// holds); GamePaused = the game's own pause (pause_game): every bus but `ui` holds, menu sounds play.
enum class Phase { Editing, Playing, Paused, GamePaused };

// --- Distance attenuation (mirrors miniaudio's spatializer) ------------------------------------------------

enum class Attenuation { None, Inverse, Linear, Exponential };
Attenuation parseAttenuation(const std::string& name);
/// Gain 0..1 at `distance` for the given model. Matches what the mixer applies.
float distanceGain(Attenuation model, float distance, float minDistance, float maxDistance, float rolloff);

// --- Buses -----------------------------------------------------------------------------------------------

const std::vector<std::string>& busNames();  // master, music, sfx, ambience, voice, ui

struct BusSettings {
    float volume = 1.f;
    bool mute = false;
};

/// The project's mixer settings (stored in `audio.json`).
struct MixSettings {
    std::map<std::string, BusSettings> buses;  // every bus in busNames()
    float musicFade = 1.5f;                    // default seconds for music crossfades

    MixSettings();
    Json toJson() const;
    static Result<MixSettings> fromJson(const Json& j);
};

struct ListenerPose {
    Vec3 position{0, 0, 0};
    Vec3 forward{0, 0, -1};
    Vec3 up{0, 1, 0};
    float volume = 1.f;
};

struct VoiceInfo {
    std::string kind;  // entity | oneshot | music | preview
    uint64_t entity = 0;
    std::string clip;
    std::string bus;
    bool playing = false;
    bool spatial = false;
    float volume = 0;
    float distance = 0;       // to the listener (spatial voices)
    float distanceGain = 1;   // attenuation currently applied
    Json toJson() const;
};

class AudioSystem {
public:
    struct Config {
        AudioMode mode = AudioMode::Auto;
        /// Maps project-relative paths ("audio/hit.wav") to files on disk.
        std::function<std::string(const std::string&)> resolvePath;
    };

    explicit AudioSystem(Config config);
    ~AudioSystem();
    AudioSystem(const AudioSystem&) = delete;
    AudioSystem& operator=(const AudioSystem&) = delete;

    AudioMode mode() const;
    /// A real output device is open (false in null mode, before the first sound, or on failure).
    bool deviceActive() const;
    /// Why audio is not going to a device, when it is not ("" when it is or nothing was played yet).
    const std::string& statusNote() const;

    // --- Mixer ---------------------------------------------------------------------------------------
    const MixSettings& mix() const;
    /// Replaces the project mix and applies it.
    void setMix(const MixSettings& mix);
    /// Live override (Wander `set_volume`); the project mix returns on stop.
    void setBusVolume(const std::string& bus, float volume);
    bool busMuted(const std::string& bus) const;
    float busVolume(const std::string& bus) const;  // effective (configured or overridden)

    // --- Per frame ----------------------------------------------------------------------------------------------
    /// Advances audio by `dt` seconds. `camera` is the fallback listener (the active scene camera).
    void update(Scene& scene, Phase phase, double dt, const std::optional<ListenerPose>& camera);
    /// Stops every voice (play stopped) and restores the project mix.
    void stopAll();

    // --- Playback ----------------------------------------------------------------------------------------------
    /// Starts (or restarts) the AudioSource of `entity`. Returns an error message, "" on success.
    std::string playEntity(Scene& scene, EntityId entity);
    void stopEntity(EntityId entity);
    /// Plays a clip once; `position` makes it a 3D sound at that point. Error message, "" on success.
    std::string playOneShot(const std::string& clip, float volume, const std::string& bus, std::optional<Vec3> position);
    /// Crossfades to a new looping music track (empty path fades the music out).
    std::string playMusic(const std::string& clip, float fadeSeconds);
    /// Edit-mode audition of a clip (no scene needed).
    std::string preview(const std::string& clip, float volume, const std::string& bus);
    void stopPreview();
    /// Drops cached decoded data for a changed file.
    void invalidate(const std::string& clip);

    // --- Introspection -------------------------------------------------------------------------------------------
    size_t voiceCount() const;
    std::vector<VoiceInfo> voices() const;
    bool entityPlaying(EntityId entity) const;
    /// Peak (linear) of the audio mixed since the last call; only measured in null mode.
    float takePeak();
    /// Null mode only: mixes `seconds` of audio and returns it (stereo, engine sample rate).
    Result<Pcm> mixdown(double seconds);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sky::audio
