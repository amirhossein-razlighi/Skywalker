#include "skywalker/audio/AudioSystem.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <set>

#include "MiniaudioConfig.h"
#include "skywalker/core/Log.h"
#include "skywalker/core/Strings.h"
#include "skywalker/scene/Scene.h"

namespace sky::audio {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

const char* toString(AudioMode m) {
    switch (m) {
        case AudioMode::Auto: return "auto";
        case AudioMode::Null: return "null";
        case AudioMode::Off: return "off";
    }
    return "auto";
}

AudioMode resolveAudioMode(AudioMode configured) {
    if (configured != AudioMode::Auto) return configured;
    if (const char* env = std::getenv("SKYWALKER_AUDIO")) {
        std::string v = str::lower(env);
        if (v == "off" || v == "0" || v == "none" || v == "disabled") return AudioMode::Off;
        if (v == "null" || v == "silent" || v == "headless") return AudioMode::Null;
    }
    return AudioMode::Auto;
}

Attenuation parseAttenuation(const std::string& name) {
    std::string n = str::lower(name);
    if (n == "linear") return Attenuation::Linear;
    if (n == "exponential") return Attenuation::Exponential;
    if (n == "none") return Attenuation::None;
    return Attenuation::Inverse;
}

float distanceGain(Attenuation model, float distance, float minDistance, float maxDistance, float rolloff) {
    if (model == Attenuation::None || minDistance >= maxDistance) return 1.f;
    const float d = std::clamp(distance, minDistance, maxDistance);
    switch (model) {
        case Attenuation::Inverse: return minDistance / (minDistance + rolloff * (d - minDistance));
        case Attenuation::Linear: return 1.f - rolloff * (d - minDistance) / (maxDistance - minDistance);
        case Attenuation::Exponential: return std::pow(d / minDistance, -rolloff);
        case Attenuation::None: break;
    }
    return 1.f;
}

const std::vector<std::string>& busNames() {
    static const std::vector<std::string> names{"master", "music", "sfx", "ambience", "voice", "ui"};
    return names;
}

namespace {
int busIndex(const std::string& bus) {
    const auto& names = busNames();
    auto it = std::find(names.begin(), names.end(), bus);
    return it == names.end() ? -1 : static_cast<int>(it - names.begin());
}
}  // namespace

MixSettings::MixSettings() {
    for (const auto& b : busNames()) buses[b] = BusSettings{};
}

Json MixSettings::toJson() const {
    Json b = Json::object();
    for (const auto& name : busNames()) {
        auto it = buses.find(name);
        BusSettings s = it == buses.end() ? BusSettings{} : it->second;
        b[name] = Json::object({{"volume", s.volume}, {"mute", s.mute}});
    }
    return Json::object({{"buses", b}, {"musicFade", musicFade}});
}

Result<MixSettings> MixSettings::fromJson(const Json& j) {
    if (!j.isObject()) return Error::make("invalid_arguments", "the mix must be an object {buses: {...}, musicFade}");
    MixSettings m;
    for (const auto& [key, value] : j.members()) {
        if (key == "musicFade") {
            m.musicFade = std::clamp(value.asFloat(1.5f), 0.f, 30.f);
        } else if (key == "buses") {
            if (!value.isObject()) return Error::make("invalid_arguments", "`buses` must be an object of bus -> {volume, mute}");
            for (const auto& [bus, settings] : value.members()) {
                if (busIndex(bus) < 0) {
                    std::string guess = str::closest(bus, busNames(), 3);
                    return Error::make("not_found", "unknown audio bus '" + bus + "'",
                                       guess.empty() ? "buses: master, music, sfx, ambience, voice, ui" : "did you mean '" + guess + "'?");
                }
                BusSettings s = m.buses[bus];
                if (settings.isNumber()) {
                    s.volume = settings.asFloat();
                } else if (settings.isObject()) {
                    for (const auto& [field, v] : settings.members()) {
                        if (field == "volume") s.volume = v.asFloat(1.f);
                        else if (field == "mute") s.mute = v.asBool();
                        else return Error::make("invalid_arguments", "unknown bus field '" + field + "'", "use volume and mute");
                    }
                } else {
                    return Error::make("invalid_arguments", "bus '" + bus + "' must be a number or {volume, mute}");
                }
                if (s.volume < 0.f || s.volume > 4.f) {
                    return Error::make("invalid_arguments", "bus '" + bus + "' volume must be between 0 and 4");
                }
                m.buses[bus] = s;
            }
        } else {
            return Error::make("invalid_arguments", "unknown mix field '" + key + "'", "use buses and musicFade");
        }
    }
    return m;
}

Json VoiceInfo::toJson() const {
    Json j = Json::object({{"kind", kind}, {"clip", clip}, {"bus", bus}, {"playing", playing}});
    if (entity) j["entity"] = entity;
    if (spatial) {
        j["distance"] = std::round(distance * 100.f) / 100.f;
        j["distanceGain"] = std::round(distanceGain * 1000.f) / 1000.f;
    }
    return j;
}

// ---------------------------------------------------------------------------
// Implementation
// ---------------------------------------------------------------------------

namespace {

struct SoundDeleter {
    void operator()(ma_sound* s) const {
        ma_sound_uninit(s);
        delete s;
    }
};
using SoundPtr = std::unique_ptr<ma_sound, SoundDeleter>;

constexpr size_t kMaxOneShots = 48;

}  // namespace

struct AudioSystem::Impl {
    enum class Kind { Entity, OneShot, Music, Preview };
    struct Voice {
        SoundPtr sound;
        Kind kind = Kind::OneShot;
        EntityId entity = 0;
        std::string clip, bus;
        bool spatial = false;
        bool loop = false;
        bool pausedByUs = false;
        double releaseAt = -1;  // audio clock time when a fading-out voice can be freed
        std::optional<Vec3> lastPos;
        Vec3 position;
        float volume = 1.f;
        // Spatial parameters for introspection
        Attenuation model = Attenuation::Inverse;
        float minDistance = 1, maxDistance = 50, rolloff = 1;
    };

    Config cfg;
    AudioMode mode = AudioMode::Auto;
    std::string note;
    bool engineTried = false, engineOk = false, deviceOk = false;
    ma_engine engine{};
    ma_sound_group groups[6]{};
    int groupsReady = 0;  // number of initialized groups (index 1..)

    MixSettings mix;
    std::map<std::string, float> volumeOverride;

    std::vector<std::unique_ptr<Voice>> voices;
    std::set<EntityId> started;
    uint64_t lastRevision = ~0ull;
    int framesSinceScan = 0;
    EntityId listenerEntity = 0;
    double clock = 0;
    Phase lastPhase = Phase::Editing;
    bool paused = false;
    ListenerPose listener;
    Vec3 lastListenerPos;
    bool hasLastListener = false;
    float peak = 0.f;
    std::vector<float> scratch;

    explicit Impl(Config c) : cfg(std::move(c)), mode(resolveAudioMode(cfg.mode)) {}

    ~Impl() {
        voices.clear();
        if (engineOk) {
            for (int i = 1; i < groupsReady; ++i) ma_sound_group_uninit(&groups[i]);
            ma_engine_uninit(&engine);
        }
    }

    // --- engine lifetime ---------------------------------------------------------------------
    bool ensureEngine() {
        if (mode == AudioMode::Off) return false;
        if (engineTried) return engineOk;
        engineTried = true;
        ma_engine_config config = ma_engine_config_init();
        config.listenerCount = 1;
        config.defaultVolumeSmoothTimeInPCMFrames = 240;  // ~5 ms: no zipper noise when volumes change
        bool ok = false;
        if (mode == AudioMode::Auto) {
            ma_result r = ma_engine_init(&config, &engine);
            if (r == MA_SUCCESS) {
                ok = deviceOk = true;
            } else {
                note = std::string("no audio output device available (") + ma_result_description(r) + "); audio is silent";
                log::warn("audio", note);
            }
        }
        if (!ok) {
            ma_engine_config nd = ma_engine_config_init();
            nd.listenerCount = 1;
            nd.noDevice = MA_TRUE;
            nd.channels = 2;
            nd.sampleRate = 48000;
            nd.defaultVolumeSmoothTimeInPCMFrames = 240;
            ma_result r = ma_engine_init(&nd, &engine);
            if (r != MA_SUCCESS) {
                note = std::string("audio engine failed to start: ") + ma_result_description(r);
                log::warn("audio", note);
                return false;
            }
            ok = true;
            deviceOk = false;
        }
        engineOk = true;
        groupsReady = 1;  // master is the engine itself
        for (int i = 1; i < 6; ++i) {
            if (ma_sound_group_init(&engine, 0, nullptr, &groups[i]) != MA_SUCCESS) break;
            groupsReady = i + 1;
        }
        applyMix();
        return true;
    }

    float effectiveVolume(const std::string& bus) const {
        auto ov = volumeOverride.find(bus);
        auto cfgIt = mix.buses.find(bus);
        BusSettings s = cfgIt == mix.buses.end() ? BusSettings{} : cfgIt->second;
        float v = ov != volumeOverride.end() ? ov->second : s.volume;
        return s.mute ? 0.f : v;
    }

    void applyMix() {
        if (!engineOk) return;
        ma_engine_set_volume(&engine, effectiveVolume("master") * listener.volume);
        for (int i = 1; i < groupsReady; ++i) ma_sound_group_set_volume(&groups[i], effectiveVolume(busNames()[static_cast<size_t>(i)]));
    }

    // --- voices ---------------------------------------------------------------------------------
    Voice* findEntityVoice(EntityId e) {
        for (auto& v : voices) {
            if (v->kind == Kind::Entity && v->entity == e) return v.get();
        }
        return nullptr;
    }

    void removeVoice(Voice* target) {
        voices.erase(std::remove_if(voices.begin(), voices.end(), [&](const auto& v) { return v.get() == target; }), voices.end());
    }

    std::string resolve(const std::string& clip) const {
        return cfg.resolvePath ? cfg.resolvePath(clip) : clip;
    }

    /// Creates a (stopped) voice for a clip. Returns an error message on failure.
    std::string createVoice(const std::string& clip, const std::string& bus, Kind kind, EntityId entity, Voice*& out) {
        out = nullptr;
        if (mode == AudioMode::Off) return {};
        if (clip.empty()) return "no audio clip set";
        if (!ensureEngine()) return note;
        std::string path = resolve(clip);
        std::error_code ec;
        if (!fs::is_regular_file(path, ec)) {
            return "audio file not found: " + clip + " (generate one with the audio_generate tool, or list audio with asset_list type=audio)";
        }
        int bi = busIndex(bus);
        if (bi < 0) bi = busIndex("sfx");
        ma_uint32 flags = 0;
        auto size = fs::file_size(path, ec);
        flags |= (kind == Kind::Music || (!ec && size > 4u * 1024u * 1024u)) ? MA_SOUND_FLAG_STREAM : MA_SOUND_FLAG_DECODE;
        SoundPtr sound(new ma_sound{});
        ma_sound_group* group = bi == 0 || bi >= groupsReady ? nullptr : &groups[bi];
        ma_result r = ma_sound_init_from_file(&engine, path.c_str(), flags, group, nullptr, sound.get());
        if (r != MA_SUCCESS) {
            delete sound.release();  // never initialized: free without ma_sound_uninit
            return "cannot play " + clip + ": " + ma_result_description(r) + " (supported: wav, mp3, flac)";
        }
        auto voice = std::make_unique<Voice>();
        voice->sound = std::move(sound);
        voice->kind = kind;
        voice->entity = entity;
        voice->clip = clip;
        voice->bus = busNames()[static_cast<size_t>(bi)];
        out = voice.get();
        voices.push_back(std::move(voice));
        return {};
    }

    void setSpatial(Voice& v, bool spatial, Attenuation model, float minD, float maxD, float rolloff, float doppler) {
        v.spatial = spatial;
        v.model = model;
        v.minDistance = minD;
        v.maxDistance = maxD;
        v.rolloff = rolloff;
        ma_sound_set_spatialization_enabled(v.sound.get(), spatial ? MA_TRUE : MA_FALSE);
        if (!spatial) return;
        ma_attenuation_model m = ma_attenuation_model_inverse;
        if (model == Attenuation::Linear) m = ma_attenuation_model_linear;
        else if (model == Attenuation::Exponential) m = ma_attenuation_model_exponential;
        else if (model == Attenuation::None) m = ma_attenuation_model_none;
        ma_sound_set_attenuation_model(v.sound.get(), m);
        ma_sound_set_rolloff(v.sound.get(), rolloff);
        ma_sound_set_min_distance(v.sound.get(), minD);
        ma_sound_set_max_distance(v.sound.get(), std::max(maxD, minD + 0.001f));
        ma_sound_set_doppler_factor(v.sound.get(), doppler);
    }

    void place(Voice& v, Vec3 pos, double dt) {
        v.position = pos;
        ma_sound_set_position(v.sound.get(), pos.x, pos.y, pos.z);
        Vec3 vel{};
        if (v.lastPos && dt > 1e-6) {
            vel = (pos - *v.lastPos) / static_cast<float>(dt);
            float speed = length(vel);
            if (speed > 150.f) vel = vel * (150.f / speed);  // teleports must not produce absurd doppler
        }
        ma_sound_set_velocity(v.sound.get(), vel.x, vel.y, vel.z);
        v.lastPos = pos;
    }

    void syncEntityVoice(Voice& v, const AudioSource& src, Vec3 pos, double dt) {
        if (v.volume != src.volume) {
            v.volume = src.volume;
            ma_sound_set_volume(v.sound.get(), src.volume);
        }
        ma_sound_set_pitch(v.sound.get(), std::clamp(src.pitch, 0.1f, 4.f));
        if (v.loop != src.loop) {
            v.loop = src.loop;
            ma_sound_set_looping(v.sound.get(), src.loop ? MA_TRUE : MA_FALSE);
        }
        if (v.bus != src.bus && busIndex(src.bus) > 0 && busIndex(src.bus) < groupsReady) {
            ma_node_attach_output_bus(&v.sound->engineNode, 0, &groups[busIndex(src.bus)], 0);
            v.bus = src.bus;
        }
        setSpatial(v, src.spatial, parseAttenuation(src.rolloff), src.minDistance, src.maxDistance, src.rolloffFactor, src.doppler);
        if (src.spatial) place(v, pos, dt);
    }

    std::string startEntity(Scene& scene, EntityId e) {
        const AudioSource* src = scene.get<AudioSource>(e);
        if (!src) return "entity has no `audio` component (add one with component_set, or use play_sound(\"path\"))";
        if (src->clip.empty()) return "the audio component has no clip set";
        if (Voice* old = findEntityVoice(e)) removeVoice(old);
        Voice* v = nullptr;
        if (std::string err = createVoice(src->clip, src->bus, Kind::Entity, e, v); !err.empty()) return err;
        if (!v) return {};  // audio off
        v->volume = -1.f;  // force first sync
        v->loop = !src->loop;
        syncEntityVoice(*v, *src, scene.worldMatrix(e).translation(), 0.0);
        ma_sound_start(v->sound.get());
        return {};
    }

    // --- per-frame scanning -----------------------------------------------------------------------
    void scan(Scene& scene) {
        listenerEntity = 0;
        for (EntityId id : scene.entities()) {
            if (!listenerEntity) {
                if (const AudioListener* l = scene.get<AudioListener>(id)) {
                    if (l->enabled && scene.isActive(id)) listenerEntity = id;
                }
            }
            const AudioSource* s = scene.get<AudioSource>(id);
            if (!s || !scene.isActive(id)) continue;
            if (started.insert(id).second && s->playOnStart && !s->clip.empty()) {
                if (std::string err = startEntity(scene, id); !err.empty()) {
                    log::warn("audio", "entity " + std::to_string(id) + ": " + err);
                }
            }
        }
    }

    void updateListener(Scene& scene, const std::optional<ListenerPose>& camera) {
        ListenerPose pose;
        bool have = false;
        if (listenerEntity && scene.exists(listenerEntity)) {
            const AudioListener* l = scene.get<AudioListener>(listenerEntity);
            if (l && l->enabled) {
                Mat4 m = scene.worldMatrix(listenerEntity);
                pose.position = m.translation();
                pose.forward = normalize(m.transformDir({0, 0, -1}));
                pose.up = normalize(m.transformDir({0, 1, 0}));
                pose.volume = l->volume;
                have = true;
            }
        }
        if (!have && camera) {
            pose = *camera;
            have = true;
        }
        listener = pose;
        if (!engineOk) return;
        ma_engine_listener_set_position(&engine, 0, pose.position.x, pose.position.y, pose.position.z);
        ma_engine_listener_set_direction(&engine, 0, pose.forward.x, pose.forward.y, pose.forward.z);
        ma_engine_listener_set_world_up(&engine, 0, pose.up.x, pose.up.y, pose.up.z);
        applyMix();
    }

    void setListenerVelocity(double dt) {
        if (!engineOk) return;
        Vec3 vel{};
        if (hasLastListener && dt > 1e-6) {
            vel = (listener.position - lastListenerPos) / static_cast<float>(dt);
            float speed = length(vel);
            if (speed > 150.f) vel = vel * (150.f / speed);
        }
        lastListenerPos = listener.position;
        hasLastListener = true;
        ma_engine_listener_set_velocity(&engine, 0, vel.x, vel.y, vel.z);
    }

    // --- null-device mixing -----------------------------------------------------------------------------
    void advance(double seconds) {
        if (!engineOk || deviceOk || seconds <= 0) return;
        const ma_uint32 rate = ma_engine_get_sample_rate(&engine);
        auto frames = static_cast<ma_uint64>(std::min(seconds, 5.0) * rate);
        constexpr ma_uint64 kBlock = 1024;
        scratch.resize(kBlock * 2);
        while (frames > 0) {
            ma_uint64 n = std::min(frames, kBlock), read = 0;
            ma_engine_read_pcm_frames(&engine, scratch.data(), n, &read);
            for (ma_uint64 i = 0; i < read * 2; ++i) peak = std::max(peak, std::fabs(scratch[i]));
            if (read == 0) break;
            frames -= read;
        }
    }

    void stopMatching(const std::function<bool(const Voice&)>& pred) {
        voices.erase(std::remove_if(voices.begin(), voices.end(), [&](const auto& v) { return pred(*v); }), voices.end());
    }

    void pauseAll(bool pause) {
        for (auto& v : voices) {
            if (v->kind == Kind::Preview || v->bus == "ui") continue;
            if (pause && ma_sound_is_playing(v->sound.get())) {
                ma_sound_stop(v->sound.get());
                v->pausedByUs = true;
            } else if (!pause && v->pausedByUs) {
                ma_sound_start(v->sound.get());
                v->pausedByUs = false;
            }
        }
    }
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

AudioSystem::AudioSystem(Config config) : impl_(std::make_unique<Impl>(std::move(config))) {}
AudioSystem::~AudioSystem() = default;

AudioMode AudioSystem::mode() const { return impl_->mode; }
bool AudioSystem::deviceActive() const { return impl_->deviceOk; }
const std::string& AudioSystem::statusNote() const { return impl_->note; }
const MixSettings& AudioSystem::mix() const { return impl_->mix; }

void AudioSystem::setMix(const MixSettings& mix) {
    impl_->mix = mix;
    impl_->volumeOverride.clear();
    impl_->applyMix();
}

void AudioSystem::setBusVolume(const std::string& bus, float volume) {
    if (busIndex(bus) < 0) return;
    impl_->volumeOverride[bus] = std::clamp(volume, 0.f, 4.f);
    impl_->applyMix();
}

bool AudioSystem::busMuted(const std::string& bus) const {
    auto it = impl_->mix.buses.find(bus);
    return it != impl_->mix.buses.end() && it->second.mute;
}

float AudioSystem::busVolume(const std::string& bus) const {
    auto ov = impl_->volumeOverride.find(bus);
    if (ov != impl_->volumeOverride.end()) return ov->second;
    auto it = impl_->mix.buses.find(bus);
    return it == impl_->mix.buses.end() ? 1.f : it->second.volume;
}

void AudioSystem::stopAll() {
    impl_->voices.clear();
    impl_->started.clear();
    impl_->volumeOverride.clear();
    impl_->lastRevision = ~0ull;
    impl_->listenerEntity = 0;
    impl_->paused = false;
    impl_->hasLastListener = false;
    impl_->applyMix();
}

void AudioSystem::update(Scene& scene, Phase phase, double dt, const std::optional<ListenerPose>& camera) {
    Impl& s = *impl_;
    s.clock += dt;
    const Phase previous = s.lastPhase;
    s.lastPhase = phase;

    if (phase == Phase::Editing) {
        if (previous != Phase::Editing || !s.voices.empty()) {
            // Only previews survive outside play.
            s.stopMatching([](const Impl::Voice& v) { return v.kind != Impl::Kind::Preview; });
            s.started.clear();
            s.paused = false;
        }
        s.stopMatching([](const Impl::Voice& v) { return v.kind == Impl::Kind::Preview && ma_sound_at_end(v.sound.get()); });
        s.advance(dt);
        return;
    }
    if (phase == Phase::Paused) {
        if (!s.paused) {
            s.pauseAll(true);
            s.paused = true;
        }
        return;
    }

    // Playing
    if (s.paused) {
        s.pauseAll(false);
        s.paused = false;
    }
    if (previous == Phase::Editing) {
        s.started.clear();
        s.lastRevision = ~0ull;
    }
    if (scene.revision() != s.lastRevision || ++s.framesSinceScan >= 60) {
        s.lastRevision = scene.revision();
        s.framesSinceScan = 0;
        s.scan(scene);
    }

    if (s.engineOk) {
        s.updateListener(scene, camera);
        s.setListenerVelocity(dt);
    }

    // Entity voices follow their components; finished and orphaned voices are released.
    std::vector<Impl::Voice*> dead;
    for (auto& v : s.voices) {
        if (v->releaseAt >= 0) {
            if (s.clock >= v->releaseAt) dead.push_back(v.get());
            continue;
        }
        if (v->kind == Impl::Kind::Entity) {
            const AudioSource* src = scene.exists(v->entity) ? scene.get<AudioSource>(v->entity) : nullptr;
            if (!src || !scene.isActive(v->entity) || src->clip != v->clip) {
                const bool clipChanged = src && scene.isActive(v->entity) && src->clip != v->clip;
                dead.push_back(v.get());
                if (clipChanged) s.started.erase(v->entity);  // restart with the new clip on the next scan
                if (clipChanged) s.lastRevision = ~0ull;
                continue;
            }
            s.syncEntityVoice(*v, *src, scene.worldMatrix(v->entity).translation(), dt);
        }
        if (!v->loop && v->kind != Impl::Kind::Music && ma_sound_at_end(v->sound.get())) dead.push_back(v.get());
        if (v->kind == Impl::Kind::Preview && ma_sound_at_end(v->sound.get())) dead.push_back(v.get());
    }
    for (Impl::Voice* d : dead) s.removeVoice(d);
    s.advance(dt);
    // Clips that ended during this advance are released now (one update can span many ticks).
    s.stopMatching([](const Impl::Voice& v) {
        return v.releaseAt < 0 && !v.loop && v.kind != Impl::Kind::Music && ma_sound_at_end(v.sound.get());
    });
}

std::string AudioSystem::playEntity(Scene& scene, EntityId entity) {
    if (impl_->mode == AudioMode::Off) return {};
    return impl_->startEntity(scene, entity);
}

void AudioSystem::stopEntity(EntityId entity) {
    if (auto* v = impl_->findEntityVoice(entity)) impl_->removeVoice(v);
}

std::string AudioSystem::playOneShot(const std::string& clip, float volume, const std::string& bus, std::optional<Vec3> position) {
    Impl& s = *impl_;
    if (s.mode == AudioMode::Off) return {};
    // Keep the voice count bounded: drop the oldest one-shot when too many are alive.
    size_t count = 0;
    for (auto& v : s.voices) count += v->kind == Impl::Kind::OneShot;
    if (count >= kMaxOneShots) {
        for (auto& v : s.voices) {
            if (v->kind == Impl::Kind::OneShot) {
                s.removeVoice(v.get());
                break;
            }
        }
    }
    Impl::Voice* v = nullptr;
    if (std::string err = s.createVoice(clip, bus, Impl::Kind::OneShot, 0, v); !err.empty()) return err;
    if (!v) return {};
    v->volume = std::clamp(volume, 0.f, 4.f);
    ma_sound_set_volume(v->sound.get(), v->volume);
    if (position) {
        s.setSpatial(*v, true, Attenuation::Inverse, 1.f, 50.f, 1.f, 0.f);
        s.place(*v, *position, 0.0);
    } else {
        s.setSpatial(*v, false, Attenuation::None, 1.f, 50.f, 1.f, 0.f);
    }
    ma_sound_start(v->sound.get());
    return {};
}

std::string AudioSystem::playMusic(const std::string& clip, float fadeSeconds) {
    Impl& s = *impl_;
    if (s.mode == AudioMode::Off) return {};
    const float fade = std::clamp(fadeSeconds < 0.f ? s.mix.musicFade : fadeSeconds, 0.f, 60.f);
    Impl::Voice* current = nullptr;
    for (auto& v : s.voices) {
        if (v->kind == Impl::Kind::Music && v->releaseAt < 0) current = v.get();
    }
    if (current && current->clip == clip) return {};  // already playing
    // Create the new track first so a bad path leaves the current music untouched.
    Impl::Voice* next = nullptr;
    if (!clip.empty()) {
        if (std::string err = s.createVoice(clip, "music", Impl::Kind::Music, 0, next); !err.empty()) return err;
    }
    if (current) {
        if (fade > 0.f && s.engineOk) {
            ma_sound_set_fade_in_milliseconds(current->sound.get(), -1.f, 0.f, static_cast<ma_uint64>(fade * 1000.f));
            current->releaseAt = s.clock + fade + 0.05;
        } else {
            s.removeVoice(current);
        }
    }
    if (next) {
        next->loop = true;
        next->volume = 1.f;
        ma_sound_set_looping(next->sound.get(), MA_TRUE);
        s.setSpatial(*next, false, Attenuation::None, 1.f, 50.f, 1.f, 0.f);
        if (fade > 0.f && current) ma_sound_set_fade_in_milliseconds(next->sound.get(), 0.f, 1.f, static_cast<ma_uint64>(fade * 1000.f));
        ma_sound_start(next->sound.get());
    }
    return {};
}

std::string AudioSystem::preview(const std::string& clip, float volume, const std::string& bus) {
    Impl& s = *impl_;
    if (s.mode == AudioMode::Off) return "audio is disabled (SKYWALKER_AUDIO=off)";
    stopPreview();
    Impl::Voice* v = nullptr;
    if (std::string err = s.createVoice(clip, bus, Impl::Kind::Preview, 0, v); !err.empty()) return err;
    if (!v) return {};
    v->volume = std::clamp(volume, 0.f, 4.f);
    ma_sound_set_volume(v->sound.get(), v->volume);
    s.setSpatial(*v, false, Attenuation::None, 1.f, 50.f, 1.f, 0.f);
    ma_sound_start(v->sound.get());
    return {};
}

void AudioSystem::stopPreview() {
    impl_->stopMatching([](const Impl::Voice& v) { return v.kind == Impl::Kind::Preview; });
}

void AudioSystem::invalidate(const std::string& clip) {
    Impl& s = *impl_;
    if (!s.engineOk) return;
    std::string path = s.resolve(clip);
    ma_resource_manager_unregister_file(ma_engine_get_resource_manager(&s.engine), path.c_str());
}

size_t AudioSystem::voiceCount() const { return impl_->voices.size(); }

bool AudioSystem::entityPlaying(EntityId entity) const {
    for (auto& v : impl_->voices) {
        if (v->kind == Impl::Kind::Entity && v->entity == entity) return ma_sound_is_playing(v->sound.get()) != 0;
    }
    return false;
}

std::vector<VoiceInfo> AudioSystem::voices() const {
    std::vector<VoiceInfo> out;
    for (auto& v : impl_->voices) {
        VoiceInfo info;
        switch (v->kind) {
            case Impl::Kind::Entity: info.kind = "entity"; break;
            case Impl::Kind::OneShot: info.kind = "oneshot"; break;
            case Impl::Kind::Music: info.kind = "music"; break;
            case Impl::Kind::Preview: info.kind = "preview"; break;
        }
        info.entity = v->entity;
        info.clip = v->clip;
        info.bus = v->bus;
        info.playing = ma_sound_is_playing(v->sound.get()) != 0;
        info.spatial = v->spatial;
        info.volume = v->volume;
        if (v->spatial) {
            info.distance = length(v->position - impl_->listener.position);
            info.distanceGain = distanceGain(v->model, info.distance, v->minDistance, v->maxDistance, v->rolloff);
        }
        out.push_back(std::move(info));
    }
    return out;
}

float AudioSystem::takePeak() {
    float p = impl_->peak;
    impl_->peak = 0.f;
    return p;
}

Result<Pcm> AudioSystem::mixdown(double seconds) {
    Impl& s = *impl_;
    if (s.mode == AudioMode::Off) return Error::make("audio_disabled", "audio is disabled");
    if (!s.ensureEngine()) return Error::make("audio_unavailable", s.note);
    if (s.deviceOk) return Error::make("audio_unavailable", "mixdown needs the null audio mode (SKYWALKER_AUDIO=null)");
    Pcm pcm;
    pcm.channels = 2;
    pcm.sampleRate = static_cast<int>(ma_engine_get_sample_rate(&s.engine));
    auto frames = static_cast<ma_uint64>(std::clamp(seconds, 0.0, 120.0) * pcm.sampleRate);
    pcm.samples.resize(frames * 2);
    ma_uint64 read = 0;
    ma_engine_read_pcm_frames(&s.engine, pcm.samples.data(), frames, &read);
    pcm.samples.resize(read * 2);
    return pcm;
}

}  // namespace sky::audio
