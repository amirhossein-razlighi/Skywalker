#include "skywalker/world/WorldRuntime.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>

#include "skywalker/core/Log.h"
#include "skywalker/ecs/Reflection.h"
#include "skywalker/render/Impostor.h"

namespace sky::world {

namespace fs = std::filesystem;

namespace {

int64_t mtimeOf(const std::string& path) {
    std::error_code ec;
    auto t = fs::last_write_time(path, ec);
    if (ec) return -1;
    return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
}

uint64_t fnv(const std::string& s, uint64_t h = 1469598103934665603ull) {
    for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
    return h;
}

/// Size and modification time of a file ("" when missing): impostor cache keys change when a
/// source mesh or texture is edited.
std::string fileStamp(const std::string& path) {
    std::error_code ec;
    auto size = fs::file_size(path, ec);
    if (ec) return {};
    return "|" + std::to_string(size) + "@" + std::to_string(mtimeOf(path));
}

/// Everything about a part that changes how it looks (project-relative paths).
std::string describePart(const InstancePart& p) {
    const Surface& s = p.surface;
    char buf[512];
    std::snprintf(buf, sizeof(buf), "|c%.4f,%.4f,%.4f,%.4f|e%.4f,%.4f,%.4f|m%.3f,%.3f,%.3f,%.3f,%.3f|t%.4f,%.4f,%d,%d|a%.3f,%.3f,%d,%d",
                  s.color.x, s.color.y, s.color.z, s.color.w, s.emissive.x, s.emissive.y, s.emissive.z, s.metallic, s.roughness,
                  s.normalStrength, s.subsurface, s.occlusionStrength, s.tiling.x, s.tiling.y, s.triplanar ? 1 : 0,
                  static_cast<int>(s.shading), s.alphaCutoff, s.clearcoat, s.textureAlphaOnly ? 1 : 0, s.doubleSided ? 1 : 0);
    std::string out = "{" + p.mesh + "|" + s.texture + "|" + s.normalMap + "|" + s.ormMap + "|" + s.emissiveMap + buf + "|L";
    for (float v : p.local.m) {
        char f[24];
        std::snprintf(f, sizeof(f), "%.4f,", v);
        out += f;
    }
    return out + "}";
}

bool isGrassLike(const std::string& mesh) {
    return mesh == "grass" || mesh == "grass_tall" || mesh == "fern" || mesh == "flowers" || mesh == "shell";
}

}  // namespace

EntityId WorldRuntime::terrainFor(const Scene& scene, EntityId e) const {
    for (EntityId cur = e; cur != kNoEntity; cur = scene.record(cur) ? scene.record(cur)->parent : kNoEntity) {
        if (scene.get<Terrain>(cur)) return cur;
    }
    return kNoEntity;
}

std::string WorldRuntime::sourceKey(const Scene& scene, EntityId e) const {
    const Terrain* t = scene.get<Terrain>(e);
    if (!t) return {};
    if (!t->data.empty()) {
        std::string abs = hooks_.resolvePath ? hooks_.resolvePath(t->data) : t->data;
        int64_t m = mtimeOf(abs);
        if (m >= 0) return "file:" + abs + "@" + std::to_string(m);
    }
    return "gen:" + std::to_string(t->resolution) + ":" + std::to_string(t->size) + ":" + t->generator.dump() + ":" +
           t->layers.dump() + ":" + t->edits.dump();
}

std::shared_ptr<TerrainData> WorldRuntime::terrain(const Scene& scene, EntityId e) {
    const Terrain* t = scene.get<Terrain>(e);
    if (!t) return nullptr;
    std::string key = sourceKey(scene, e);
    TerrainEntry& entry = terrains_[e];
    if (entry.data && entry.key == key) return entry.data;
    std::shared_ptr<TerrainData> data;
    if (key.rfind("file:", 0) == 0) {
        auto loaded = TerrainData::load(hooks_.resolvePath ? hooks_.resolvePath(t->data) : t->data);
        if (loaded) {
            data = std::make_shared<TerrainData>(std::move(loaded.value()));
        } else {
            log::warn("terrain", loaded.error().message);
        }
    }
    if (!data) {
        int res = std::clamp(t->resolution, 17, 4097);
        data = std::make_shared<TerrainData>(res, t->size);
        TerrainGenParams p = genParamsFromJson(t->generator);
        if (Status st = resolveHeightmap(p, hooks_.resolvePath); !st) log::warn("terrain", st.error().message);
        generate(*data, p);
        autoPaint(*data, t->layers, p.seed);
        applyEdits(*data, t->edits, t->layers, p.seed);
        // The data file is a cache of the (deterministic) generator: rebuild a missing one, with
        // its physics heightmap, so projects can leave large terrain binaries out of git.
        if (!t->data.empty() && hooks_.resolvePath) {
            std::string abs = hooks_.resolvePath(t->data);
            std::error_code ec;
            if (!fs::exists(abs, ec)) {
                fs::create_directories(fs::path(abs).parent_path(), ec);
                float lo = 0, hi = 0;
                if (data->save(abs) && data->saveHeightmap16(fs::path(abs).replace_extension(".r16").string(), lo, hi)) {
                    key = sourceKey(scene, e);
                } else {
                    log::warn("terrain", "could not write the terrain cache " + abs);
                }
            }
        }
    }
    entry.data = data;
    entry.key = key;
    return data;
}

void WorldRuntime::adopt(EntityId e, std::shared_ptr<TerrainData> data, const std::string& key) {
    terrains_[e] = TerrainEntry{std::move(data), key};
}

void WorldRuntime::forget(EntityId e) {
    terrains_.erase(e);
    foliage_.erase(e);
}

bool WorldRuntime::terrainHeight(const Scene& scene, float x, float z, float& y, Vec3* normal, EntityId* which) {
    bool found = false;
    for (EntityId e : scene.entities()) {
        if (!scene.get<Terrain>(e) || !scene.isActive(e)) continue;
        auto data = terrain(scene, e);
        if (!data) continue;
        Vec3 o = scene.worldMatrix(e).translation();
        float h = 0;
        if (!data->heightAt(x - o.x, z - o.z, h)) continue;
        if (!found || o.y + h > y) {
            y = o.y + h;
            if (normal) *normal = data->normalAt(x - o.x, z - o.z);
            if (which) *which = e;
            found = true;
        }
    }
    return found;
}

std::optional<WorldRuntime::Hit> WorldRuntime::raycast(const Scene& scene, const Ray& ray, float maxDist) {
    std::optional<Hit> best;
    for (EntityId e : scene.entities()) {
        if (!scene.get<Terrain>(e) || !scene.isActive(e)) continue;
        auto data = terrain(scene, e);
        if (!data) continue;
        Vec3 o = scene.worldMatrix(e).translation();
        float t = data->raycast(ray.origin - o, ray.dir, best ? best->distance : maxDist);
        if (t < 0) continue;
        Vec3 p = ray.origin + ray.dir * t;
        best = Hit{e, p, data->normalAt(p.x - o.x, p.z - o.z), t};
    }
    return best;
}

void WorldRuntime::gather(const Scene& scene, const ViewCamera& view, FrameData& frame, bool realtime) {
    stats_ = {};
    std::vector<EntityId> liveTerrains, liveFoliage;
    for (EntityId e : scene.entities()) {
        const Terrain* t = scene.get<Terrain>(e);
        if (!t || !scene.isActive(e)) continue;
        liveTerrains.push_back(e);
        auto data = terrain(scene, e);
        if (!data) continue;
        TerrainItem item;
        item.entity = e;
        item.origin = scene.worldMatrix(e).translation();
        item.data = data;
        item.waterLevel = t->waterLevel;
        item.wetBand = t->wetBand;
        item.macroVariation = t->macroVariation;
        item.detail = t->detail;
        item.castShadows = t->castShadows;
        if (!t->overlay.empty() && t->overlayOpacity > 0.f) {
            item.overlay = hooks_.resolvePath ? hooks_.resolvePath(t->overlay) : t->overlay;
            item.overlayOpacity = std::clamp(t->overlayOpacity, 0.f, 1.f);
            item.overlayBlend = t->overlayBlend == "multiply" ? 1 : t->overlayBlend == "glow" ? 2 : 0;
        }
        size_t count = std::min<size_t>(t->layers.isArray() ? t->layers.size() : 0, TerrainData::kMaxLayers);
        for (size_t i = 0; i < count; ++i) {
            const Json& l = t->layers[i];
            TerrainItem::Layer layer;
            if (l.contains("material") && hooks_.material) {
                if (const Surface* s = hooks_.material(l.get("material").asString())) layer.surface = *s;
            }
            Surface& s = layer.surface;
            if (l.contains("color")) reflect::jsonToColor(l.get("color"), s.color);
            if (l.contains("roughness")) s.roughness = l.get("roughness").asFloat();
            if (l.contains("metallic")) s.metallic = l.get("metallic").asFloat();
            if (l.contains("normalStrength")) s.normalStrength = l.get("normalStrength").asFloat();
            auto path = [&](const char* k, std::string& out) {
                if (l.contains(k) && !l.get(k).asString().empty()) out = l.get(k).asString();
                if (!out.empty() && hooks_.resolvePath) out = hooks_.resolvePath(out);
            };
            path("texture", s.texture);
            path("normalMap", s.normalMap);
            path("ormMap", s.ormMap);
            // `tiling` is meters per texture repeat (agent-friendly); the shader wants repeats per meter.
            float meters = l.get("tiling").asFloat(4.f);
            layer.tiling = 1.f / std::max(meters, 0.05f);
            layer.triplanar = l.get("triplanar").asBool(false);
            item.layers.push_back(layer);
        }
        if (item.layers.empty()) {
            TerrainItem::Layer base;
            base.surface.color = {0.45f, 0.42f, 0.36f, 1.f};
            base.surface.roughness = 0.9f;
            item.layers.push_back(base);
        }
        frame.terrains.push_back(std::move(item));
        ++stats_.terrains;
    }

    int budget = realtime ? 24 : 1 << 20;
    for (EntityId e : scene.entities()) {
        const Foliage* fo = scene.get<Foliage>(e);
        if (!fo || !fo->visible || !scene.isActive(e)) continue;
        liveFoliage.push_back(e);
        FoliageEntry& entry = foliage_[e];
        // The surface: a terrain (self/parent) or the scene's meshes under `area`. The terrain's
        // layer names let `terrainLayer` name a layer instead of giving its index.
        EntityId te = fo->surface == "terrain" ? terrainFor(scene, e) : kNoEntity;
        const Terrain* terrainComp = te != kNoEntity ? scene.get<Terrain>(te) : nullptr;
        std::vector<std::string> terrainNames = terrainComp ? terrainLayerNames(terrainComp->layers) : std::vector<std::string>{};
        std::string layersKey = fo->layers.dump();
        for (const auto& n : terrainNames) layersKey += "|" + n;
        if (entry.layersKey != layersKey) {
            entry.layers = foliageLayersFromJson(fo->layers, terrainNames);
            entry.layersKey = layersKey;
        }
        SurfaceFn surface;
        Vec2 areaMin, areaMax;
        uint64_t sig = fnv(layersKey) ^ (static_cast<uint64_t>(fo->seed) * 0x9E3779B97F4A7C15ull) ^
                       fnv(std::to_string(fo->density)) ^ fnv(fo->surface);
        if (te != kNoEntity) {
            auto data = terrain(scene, te);
            if (!data) continue;
            Vec3 o = scene.worldMatrix(te).translation();
            float half = data->size() * 0.5f;
            areaMin = {o.x - half, o.z - half};
            areaMax = {o.x + half, o.z + half};
            sig ^= data->version() * 0xC2B2AE3D27D4EB4Full ^ fnv(std::to_string(o.x) + "," + std::to_string(o.y) + "," + std::to_string(o.z));
            surface = [data, o](float x, float z, SurfaceSample& out) {
                float h = 0;
                float lx = x - o.x, lz = z - o.z;
                if (!data->heightAt(lx, lz, h)) return false;
                out.y = o.y + h;
                out.normal = data->normalAt(lx, lz);
                int n = data->resolution();
                int ix = std::clamp(static_cast<int>(std::lround((lx / data->size() + 0.5f) * (n - 1))), 0, n - 1);
                int iz = std::clamp(static_cast<int>(std::lround((lz / data->size() + 0.5f) * (n - 1))), 0, n - 1);
                const uint8_t* w = data->weightsAt(ix, iz);
                for (int i = 0; i < 8; ++i) out.layerWeights[i] = w[i] / 255.f;
                return true;
            };
        } else {
            if (!hooks_.sceneSurface) continue;
            Mat4 m = scene.worldMatrix(e);
            Vec3 c = m.translation();
            Vec3 a = fo->area;
            areaMin = {c.x - a.x * 0.5f, c.z - a.z * 0.5f};
            areaMax = {c.x + a.x * 0.5f, c.z + a.z * 0.5f};
            float top = c.y + a.y * 0.5f, bottom = c.y - a.y * 0.5f;
            sig ^= fnv(std::to_string(c.x) + "," + std::to_string(c.y) + "," + std::to_string(c.z) + "," + std::to_string(a.x) +
                       "," + std::to_string(a.y) + "," + std::to_string(a.z));
            auto fn = hooks_.sceneSurface;
            surface = [fn, top, bottom](float x, float z, SurfaceSample& out) {
                Vec3 n;
                if (!fn(x, z, top, bottom, out.y, n)) return false;
                out.normal = n;
                for (float& w : out.layerWeights) w = 1.f;
                return true;
            };
        }
        if (entry.cache.signature != sig) {
            entry.cache.clear();
            entry.cache.signature = sig;
        }
        entry.models.resize(entry.layers.size());
        for (size_t li = 0; li < entry.layers.size(); ++li) {
            FoliageLayer layer = entry.layers[li];
            layer.density *= fo->density;
            const LayerModel* model = layerModel(entry, li, layer);
            if (!model) continue;  // streaming in: chunk bounds need the real mesh size
            auto chunks = entry.cache.visibleChunks(layer, static_cast<int>(li), static_cast<uint32_t>(fo->seed), view.eye, areaMin,
                                                    areaMax, surface, model->meshHeight, budget);
            if (chunks.empty()) continue;
            // Far instances switch to the impostor where its texels match the screen's pixels.
            int impostor = -1;
            float impostorDistance = 0.f;
            if (model->impostorWorthy) {
                impostor::TransitionParams tp;
                tp.modelRadius = length(model->bounds.extents()) * (layer.scaleMin + layer.scaleMax) * 0.5f;
                tp.atlasResolution = model->impostor.resolution;
                tp.frames = model->impostor.frames;
                tp.screenHeight = frame.height > 0 ? frame.height : 1080;
                tp.fovDeg = view.orthographic ? 55.f : view.fovDeg;
                tp.quality = frame.quality;
                tp.overrideDistance = layer.impostorDistance;
                tp.cullDistance = layer.cullDistance;
                impostorDistance = impostor::transitionDistance(tp);
                if (impostorDistance > 0.f) {
                    impostor = static_cast<int>(frame.impostors.size());
                    frame.impostors.push_back(model->impostor);
                    ++stats_.impostorLayers;
                }
            }
            for (const auto& c : chunks) {
                InstanceBatch b;
                b.entity = e;
                b.id = c.id ^ (static_cast<uint64_t>(e) << 40);
                b.parts = model->parts;
                b.instances = c.instances;
                b.bounds = c.bounds;
                b.castShadows = layer.castShadows;
                b.wind = layer.wind;
                b.cullDistance = layer.cullDistance;
                b.meshHeight = model->meshHeight;
                b.maxScale = layer.scaleMax;
                b.modelBounds = model->bounds;
                b.impostor = impostor;
                b.impostorDistance = impostorDistance;
                stats_.foliageInstances += c.instances->size();
                frame.instances.push_back(std::move(b));
            }
        }
        stats_.foliageChunks += entry.cache.chunkCount();
    }
    std::erase_if(terrains_, [&](const auto& kv) { return std::find(liveTerrains.begin(), liveTerrains.end(), kv.first) == liveTerrains.end(); });
    std::erase_if(foliage_, [&](const auto& kv) { return std::find(liveFoliage.begin(), liveFoliage.end(), kv.first) == liveFoliage.end(); });
}

const WorldRuntime::LayerModel* WorldRuntime::layerModel(FoliageEntry& entry, size_t li, const FoliageLayer& layer) {
    // The drawable parts: one mesh, or every part of a prefab sharing the instances.
    std::vector<Hooks::Part> parts;
    if (!layer.prefab.empty() && hooks_.prefabParts) parts = hooks_.prefabParts(layer.prefab);
    if (parts.empty()) parts.push_back({layer.mesh, layer.material, Mat4{}});
    if (hooks_.meshReady &&
        std::any_of(parts.begin(), parts.end(), [&](const Hooks::Part& p) { return !hooks_.meshReady(p.mesh); })) {
        return nullptr;
    }
    // Surfaces as the project names them (relative paths): they key the impostor cache.
    std::vector<InstancePart> out;
    std::string source;
    for (const auto& part : parts) {
        InstancePart ip;
        ip.mesh = part.mesh;
        ip.local = part.local;
        Surface& surf = ip.surface;
        std::string material = !part.material.empty() ? part.material : layer.material;
        if (!material.empty() && hooks_.material) {
            if (const Surface* sm = hooks_.material(material)) surf = *sm;
        } else {
            surf.color = layer.color;
            surf.roughness = layer.roughness;
            surf.subsurface = layer.subsurface;
            surf.texture = layer.texture;
            surf.normalMap = layer.normalMap;
            surf.ormMap = layer.ormMap;
            surf.triplanar = layer.triplanar;
            surf.tiling = {layer.tiling, layer.tiling};
        }
        // Leaves and blades let light through.
        if (layer.subsurface > 0.f && surf.subsurface <= 0.f && surf.alphaCutoff > 0.f) surf.subsurface = layer.subsurface;
        surf.doubleSided = surf.doubleSided || isGrassLike(part.mesh);
        source += describePart(ip);
        out.push_back(std::move(ip));
    }
    char settings[160];
    std::snprintf(settings, sizeof(settings), "|imp:%d,%d,%d,%d|scale:%.3f,%.3f", layer.impostors ? 1 : 0,
                  layer.impostorDistance < 0.f ? 1 : 0, layer.impostorResolution, layer.impostorFrames, layer.scaleMin,
                  layer.scaleMax);
    LayerModel& m = entry.models[li];
    if (m.parts && m.signature == source + settings) return &m;
    m = LayerModel{};
    m.signature = source + settings;
    // Bounds of the whole model (all parts) and its height above the base (wind bending).
    m.meshHeight = hooks_.meshBounds ? 0.02f : 1.f;
    m.bounds = Aabb{Vec3(1e30f), Vec3(-1e30f)};
    size_t triangles = 0;
    std::string stamp;
    for (const auto& p : out) {
        if (hooks_.meshBounds) {
            if (auto bb = hooks_.meshBounds(p.mesh)) {
                Aabb wb = bb->transformed(p.local);
                m.meshHeight = std::max(m.meshHeight, wb.max.y - std::min(wb.min.y, 0.f));
                m.bounds.min = vmin(m.bounds.min, wb.min);
                m.bounds.max = vmax(m.bounds.max, wb.max);
            }
        }
        triangles += hooks_.meshTriangles ? hooks_.meshTriangles(p.mesh) : 0;
        if (p.mesh.rfind("asset:", 0) == 0 && hooks_.resolvePath) {
            std::string file = p.mesh.substr(6);
            file = file.substr(0, file.find('#'));
            stamp += fileStamp(hooks_.resolvePath(file));
        }
    }
    if (m.bounds.min.x > m.bounds.max.x) m.bounds = Aabb{Vec3(-0.5f, 0.f, -0.5f), Vec3(0.5f, 1.f, 0.5f)};
    // Absolute texture paths for the renderer (and content stamps for the cache key).
    for (auto& p : out) {
        if (!hooks_.resolvePath) break;
        for (std::string* path : {&p.surface.texture, &p.surface.normalMap, &p.surface.ormMap, &p.surface.emissiveMap}) {
            if (path->empty()) continue;
            *path = hooks_.resolvePath(*path);
            stamp += fileStamp(*path);
        }
    }
    m.parts = std::make_shared<const std::vector<InstancePart>>(std::move(out));
    // Impostors pay off for real meshes (photoscans, imported trees); a few dozen triangles of a
    // procedural blade are cheaper than any impostor.
    m.impostorWorthy = layer.impostors && layer.impostorDistance >= 0.f && (!hooks_.meshTriangles || triangles >= 300);
    if (m.impostorWorthy) {
        ImpostorModel& im = m.impostor;
        im.label = layer.name + " (" + (!layer.prefab.empty() ? layer.prefab : layer.mesh) + ")";
        im.parts = m.parts;
        im.source = source;
        im.stamp = stamp;
        im.bounds = m.bounds;
        im.frames = std::clamp(layer.impostorFrames, impostor::kMinFrames, impostor::kMaxFrames);
        im.resolution = layer.impostorResolution > 0
                            ? layer.impostorResolution
                            : impostor::autoResolution(length(m.bounds.extents()) * 2.f * layer.scaleMax);
        im.hemi = true;
        im.key = impostor::cacheKey(im);
        if (hooks_.resolvePath) im.cachePath = hooks_.resolvePath(".skywalker/cache/impostors/" + im.key + ".skyimp");
    }
    return &m;
}

std::vector<WorldRuntime::LayerImpostor> WorldRuntime::impostorModels(const Scene& scene, EntityId e, int layer) {
    std::vector<LayerImpostor> out;
    const Foliage* fo = scene.get<Foliage>(e);
    if (!fo) return out;
    FoliageEntry& entry = foliage_[e];
    // Same key and names as update(), so the two never re-parse each other's layers.
    EntityId te = fo->surface == "terrain" ? terrainFor(scene, e) : kNoEntity;
    const Terrain* terrainComp = te != kNoEntity ? scene.get<Terrain>(te) : nullptr;
    std::vector<std::string> terrainNames = terrainComp ? terrainLayerNames(terrainComp->layers) : std::vector<std::string>{};
    std::string layersKey = fo->layers.dump();
    for (const auto& n : terrainNames) layersKey += "|" + n;
    if (entry.layersKey != layersKey) {
        entry.layers = foliageLayersFromJson(fo->layers, terrainNames);
        entry.layersKey = layersKey;
    }
    entry.models.resize(entry.layers.size());
    for (size_t li = 0; li < entry.layers.size(); ++li) {
        if (layer >= 0 && static_cast<int>(li) != layer) continue;
        const FoliageLayer& l = entry.layers[li];
        const LayerModel* m = layerModel(entry, li, l);
        if (!m || !m->impostorWorthy) continue;
        LayerImpostor info;
        info.layer = static_cast<int>(li);
        info.name = l.name;
        info.model = m->impostor;
        info.cullDistance = l.cullDistance;
        impostor::TransitionParams tp;
        tp.modelRadius = length(m->bounds.extents()) * (l.scaleMin + l.scaleMax) * 0.5f;
        tp.atlasResolution = m->impostor.resolution;
        tp.frames = m->impostor.frames;
        tp.overrideDistance = l.impostorDistance;
        tp.cullDistance = l.cullDistance;
        info.transitionDistance = impostor::transitionDistance(tp);
        out.push_back(std::move(info));
    }
    return out;
}

Json defaultTerrainLayers(const std::string& preset) {
    auto layer = [](const char* name, const char* texgen, const char* color, float tiling, float rough) {
        return Json::object({{"name", name}, {"texgen", texgen}, {"color", color}, {"tiling", tiling}, {"roughness", rough}});
    };
    auto rules = [](Json l, float hMin, float hMax, float sMin, float sMax, float noise = 0.4f, float sharp = 0.5f) {
        if (hMin > -1e8f) l["heightMin"] = hMin;
        if (hMax < 1e8f) l["heightMax"] = hMax;
        l["slopeMin"] = sMin;
        l["slopeMax"] = sMax;
        l["noise"] = noise;
        l["sharpness"] = sharp;
        return l;
    };
    constexpr float kAny = 1e9f;
    if (preset == "alpine" || preset == "mountain_valley") {
        Json rock = rules(layer("rock", "rock", "#8a8580", 6, 0.85f), -kAny, kAny, 32, 90, 0.5f, 0.55f);
        rock["triplanar"] = true;
        return Json::array({layer("grass", "grass", "#ffffff", 3, 0.9f),
                            rules(layer("scree", "dirt", "#9b9288", 3, 0.95f), -kAny, kAny, 24, 40, 0.6f, 0.3f), rock,
                            rules(layer("snow", "noise", "#f4f6fa", 5, 0.5f), preset == "alpine" ? 260.f : 190.f, kAny, 0, 42, 0.7f, 0.6f)});
    }
    if (preset == "canyon") {
        Json rock = rules(layer("cliff", "rock", "#c27a52", 7, 0.85f), -kAny, kAny, 28, 90, 0.4f, 0.6f);
        rock["triplanar"] = true;
        return Json::array({layer("dust", "sand", "#c99a6c", 3, 0.95f), rules(layer("gravel", "dirt", "#a8745a", 2, 0.9f), -kAny, kAny, 12, 30, 0.6f, 0.3f), rock});
    }
    if (preset == "desert_dunes") {
        return Json::array({layer("sand", "sand", "#e8c896", 2, 0.92f), rules(layer("crust", "dirt", "#c9a77c", 3, 0.95f), -kAny, kAny, 24, 90, 0.6f, 0.3f)});
    }
    if (preset == "rolling_hills" || preset == "flat") {
        Json rock = rules(layer("rock", "rock", "#8d8a84", 5, 0.85f), -kAny, kAny, 30, 90, 0.5f, 0.5f);
        rock["triplanar"] = true;
        return Json::array({layer("grass", "grass", "#ffffff", 3, 0.9f), rules(layer("soil", "dirt", "#ffffff", 2.5f, 0.95f), -kAny, kAny, 20, 34, 0.7f, 0.3f), rock});
    }
    // island_beach / tropical_coast: seabed, sand, grass inland, soil on banks, rock cliffs.
    Json rock = rules(layer("rock", "rock", "#8f8a80", 6, 0.8f), -kAny, kAny, 34, 90, 0.5f, 0.55f);
    rock["triplanar"] = true;
    return Json::array({layer("sand", "sand", "#f2e2c2", 2.5f, 0.95f),
                        rules(layer("seabed", "sand", "#b5a688", 3, 0.9f), -kAny, -1.5f, 0, 90, 0.6f, 0.4f),
                        rules(layer("grass", "grass", "#ffffff", 3, 0.9f), 3.5f, kAny, 0, 26, 0.8f, 0.35f),
                        rules(layer("soil", "dirt", "#ffffff", 2.5f, 0.95f), 2.5f, kAny, 22, 36, 0.7f, 0.3f), rock});
}

}  // namespace sky::world
