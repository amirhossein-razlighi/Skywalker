#include "skywalker/assets/Prefab.h"

#include <fstream>
#include <sstream>

namespace sky {

Json prefabFromEntity(const Scene& scene, EntityId root) { return prefab::documentFromScene(scene, root, true); }

Result<EntityId> instantiatePrefab(Scene& scene, const std::shared_ptr<const PrefabTemplate>& tmpl,
                                   const PrefabPlacement& p, bool linked) {
    auto root = prefab::instantiate(scene, tmpl, p.parent, p.name);
    if (!root) return root;
    if (Transform* t = scene.get<Transform>(*root)) {
        if (p.hasPosition) t->position = p.position;
        if (p.hasYaw) t->rotation.y = p.yaw;
        t->scale = t->scale * p.scale;
    }
    if (!linked || tmpl->source.empty()) prefab::unpack(scene, *root);
    return root;
}

Result<EntityId> instantiatePrefab(Scene& scene, const Json& prefab, const PrefabPlacement& p) {
    auto t = buildPrefabTemplate(prefab);
    if (!t) return t.error();
    return instantiatePrefab(scene, *t, p, false);
}

Result<Json> loadPrefab(const std::string& path) {
    std::ifstream f(path);
    if (!f) return Error::make("io_error", "cannot read prefab " + path);
    std::stringstream ss;
    ss << f.rdbuf();
    return Json::parse(ss.str());
}

Status savePrefab(const std::string& path, const Json& prefab) {
    std::ofstream f(path);
    if (!f) return Error::make("io_error", "cannot write prefab " + path);
    f << prefab.dump(2) << "\n";
    return {};
}

}  // namespace sky
