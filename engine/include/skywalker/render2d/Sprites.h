#pragma once
// 2D world rendering front end: sprites, flipbook animation, tilemaps, world text, 2D lights,
// parallax and the 2D camera. Everything here is CPU-side and backend independent: it turns the
// scene into Frame2D instance lists (see Render2D.h) that Metal and the CPU rasterizer draw.

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "skywalker/scene/Process.h"
#include "skywalker/render/Renderer.h"
#include "skywalker/render2d/Atlas.h"
#include "skywalker/render2d/ImageIO.h"
#include "skywalker/render2d/Tilemap.h"
#include "skywalker/text/Font.h"

namespace sky::render2d {

/// A resolved frame: which pixels of which image a sprite, tile or UI image shows.
struct FrameRef {
    std::string path;            // absolute image path
    int texW = 0, texH = 0;      // image size
    float x = 0, y = 0, w = 0, h = 0;  // drawn rect in the image (pixels)
    float sourceW = 0, sourceH = 0;    // untrimmed frame size
    float offsetX = 0, offsetY = 0;    // drawn rect inside the untrimmed frame (from its top-left)
};

struct Tileset {
    std::string image;  // absolute path
    int tileSize = 16, spacing = 0, margin = 0;
    int columns = 0, rows = 0, count = 0;
    int texW = 0, texH = 0;
    std::vector<uint32_t> solid;
    Json terrains = Json::object();
    std::map<std::string, uint32_t> names;
    /// Pixel rect of tile `id` (1-based).
    void rect(uint32_t id, int& x, int& y) const;
};

/// A parsed animation clip.
struct Clip {
    std::vector<int> frames;  // frame indices into the clip's sheet/atlas
    float fps = 10.f;
    bool loop = true;
    std::map<int, std::string> events;  // clip frame position -> event name
    std::string texture;     // optional sheet override
    int columns = 0, rows = 0;
};

/// Caches of files 2D rendering needs (images, atlases, tilesets, fonts, decoded tilemaps).
class Assets2D {
public:
    explicit Assets2D(std::string projectDir);

    std::string resolve(const std::string& path) const;  // project-relative -> absolute
    const std::string& projectDir() const { return projectDir_; }
    void setProjectDir(std::string dir);
    ImageCache& images() { return images_; }
    text::FontLibrary& fonts() { return fonts_; }

    Result<std::shared_ptr<const Atlas>> atlas(const std::string& path);
    /// Resolves texture + frame selector ("" / name / index) + grid + region into a frame.
    Status frame(const std::string& texture, const std::string& frame, int columns, int rows, Vec4 region, FrameRef& out);
    Status frameAt(const std::string& texture, int index, int columns, int rows, FrameRef& out);
    /// Number of frames of a texture (atlas frames or grid cells).
    int frameCount(const std::string& texture, int columns, int rows);
    Result<Tileset> tileset(const std::string& path, int tileSize);
    /// Decoded layers of a tilemap component (cached while its data is unchanged).
    Result<std::shared_ptr<const tiles::Grid>> grid(uint64_t entity, const Tilemap& map);
    /// Parsed clip `name` of an animator (cached while the clips are unchanged).
    Result<Clip> clip(const SpriteAnimator& anim, const std::string& name, const std::string& texture, int columns, int rows);

    void invalidate(const std::string& absolutePath);

private:
    std::string projectDir_;
    ImageCache images_;
    text::FontLibrary fonts_;
    struct CachedAtlas {
        int64_t mtime = -2;
        Result<std::shared_ptr<const Atlas>> atlas = Error::make("not_loaded", "");
    };
    std::unordered_map<std::string, CachedAtlas> atlases_;
    struct CachedTileset {
        int64_t mtime = -2;
        int tileSize = 0;
        Result<Tileset> tileset = Error::make("not_loaded", "");
    };
    std::unordered_map<std::string, CachedTileset> tilesets_;
    struct CachedGrid {
        Json layers;
        int width = 0, height = 0;
        Result<std::shared_ptr<const tiles::Grid>> grid = Error::make("not_loaded", "");
    };
    std::unordered_map<uint64_t, CachedGrid> grids_;
    std::unordered_map<std::string, Result<Clip>> clips_;
};

struct Gather2DOptions {
    float time = 0.f;  // effects clock (flicker)
    std::vector<EntityId> selection;
    float pixelSnap = 0.f;  // world units per texel to snap unrotated quads to (0 = off)
};

/// Fills frame.render2d (sprites, tile layers, world text, 2D lights, halos, screen boxes).
void gather2D(const Scene& scene, Assets2D& assets, FrameData& frame, const Gather2DOptions& opts);

/// Advances sprite animations by dt; `emit(entity, event)` receives frame events and "finished" (delivered to Wander as anim:<name>).
/// With a process gate (game pause, time scale) sprites whose entity does not run hold their frame.
void tickAnimators(Scene& scene, Assets2D& assets, float dt, const std::function<void(EntityId, const std::string&)>& emit,
                   const ProcessGate* gate = nullptr);
/// Starts `clip` on the entity's animator (restarts it when `restart` or when it differs).
Status playAnimation(Scene& scene, Assets2D& assets, EntityId entity, const std::string& clip, bool restart);
/// The sheet frame an animator shows now (false if it has no valid clip).
bool animatedFrame(const Scene& scene, Assets2D& assets, EntityId entity, std::string& texture, int& columns, int& rows, int& frame);

/// The camera entity a frame would use (the preferred one, else the first active primary camera).
EntityId activeCamera(const Scene& scene, EntityId preferred = kNoEntity);
/// Applies the camera entity's camera2d settings to a view (orthographic size, pixel snapping,
/// bounds). Returns the world units per texel for sprite snapping (0 = none).
float applyCamera2D(const Scene& scene, EntityId cameraEntity, ViewCamera& view, int width, int height);
/// Moves cameras with camera2d.follow toward their targets (deterministic; call per fixed tick).
void tickCameras(Scene& scene, float dt, const ProcessGate* gate = nullptr);

/// sRGB (authoring) -> linear (rendering).
Vec4 toLinear(Vec4 srgb);

}  // namespace sky::render2d
