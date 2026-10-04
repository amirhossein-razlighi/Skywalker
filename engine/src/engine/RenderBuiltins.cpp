// Wander builtins for rendering: render layer masks by name (docs/RENDERING.md "Render layers").

#include <filesystem>
#include <mutex>
#include <unordered_map>

#include "skywalker/engine/Engine.h"
#include "skywalker/game/GameSettings.h"
#include "skywalker/render/RenderLayers.h"
#include "skywalker/wander/Builtins.h"

namespace sky {

namespace {

using namespace wander;

/// The project's layer names (game.json), re-read only when the file changes.
render::LayerNames projectLayerNames(const std::string& projectDir) {
    struct Cached {
        std::filesystem::file_time_type stamp{};
        render::LayerNames names;
    };
    static std::mutex mutex;
    static std::unordered_map<std::string, Cached> cache;
    std::error_code ec;
    const auto stamp = std::filesystem::last_write_time(std::filesystem::path(projectDir) / "game.json", ec);
    std::lock_guard<std::mutex> lock(mutex);
    auto it = cache.find(projectDir);
    if (it != cache.end() && it->second.stamp == stamp) return it->second.names;
    Cached c;
    c.stamp = stamp;
    if (auto settings = game::GameSettings::load(projectDir)) c.names = settings->renderLayers;
    return (cache[projectDir] = std::move(c)).names;
}

}  // namespace

void registerRenderLayerBuiltins(BuiltinRegistry& reg) {
    BuiltinDef mask;
    mask.name = "layer_mask";
    mask.params = {{"layers", kTString | kTNumber | kTList}};
    mask.variadic = true;
    mask.returns = kTNumber;
    mask.category = "render";
    mask.doc = "Render layer bitmask from layer names (game.json render.layers), layer numbers as strings (\"3\"), \"all\" / "
               "\"none\", or a list of names / layer numbers; several arguments are combined. Set it on mesh.layers, "
               "camera.cullMask or light.cullMask, e.g. hide the player's own body from the first-person camera.";
    mask.example = "find(\"Eyes\").camera.cullMask = layer_mask(\"world\", \"fx\")";
    mask.owner = "engine";
    mask.fn = [](CallContext& c) -> Value {
        render::LayerNames names;
        if (Engine* engine = c.service<Engine>()) names = projectLayerNames(engine->config().projectDir);
        uint32_t bits = 0;
        for (int i = 0; i < c.argc(); ++i) {
            auto m = render::parseLayerMask(toJson(c.arg(i)), names);
            if (!m) c.fail("layer_mask(): " + m.error().message + (m.error().hint.empty() ? "" : " (" + m.error().hint + ")"));
            bits |= *m;
        }
        return Value::number(static_cast<double>(bits));
    };
    reg.add(std::move(mask));
}

}  // namespace sky
