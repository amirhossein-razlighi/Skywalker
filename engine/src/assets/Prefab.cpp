#include "skywalker/assets/Prefab.h"

#include <fstream>
#include <sstream>

namespace sky {

namespace {

Json nodeFor(const Scene& s, EntityId id) {
    Json doc = s.entityToJson(id);
    doc.erase("id");
    doc.erase("parent");
    Json children = Json::array();
    for (EntityId c : s.children(id)) children.push(nodeFor(s, c));
    if (children.size()) doc["children"] = children;
    return doc;
}

Result<EntityId> build(Scene& s, const Json& node, EntityId parent, const std::string& nameOverride) {
    if (!node.isObject()) return Error::make("invalid_prefab", "prefab node must be an object");
    std::string name = nameOverride.empty() ? node.get("name").asString("Entity") : nameOverride;
    EntityId id = s.create(name, parent);
    Json body = node;
    body.erase("children");
    body.erase("name");
    if (Status st = s.applyEntityJson(id, body); !st) return st.error();
    for (const auto& child : node.get("children").elements()) {
        auto c = build(s, child, id, "");
        if (!c) return c.error();
    }
    return id;
}

}  // namespace

Json prefabFromEntity(const Scene& scene, EntityId root) {
    Json node = nodeFor(scene, root);
    // Store the root at the origin; keep its rotation and scale.
    if (Json* t = node["components"].find("transform")) (*t)["position"] = Json::array({0, 0, 0});
    return Json::object({{"format", "skywalker.prefab"}, {"version", 1}, {"name", node.get("name")}, {"root", node}});
}

Result<EntityId> instantiatePrefab(Scene& scene, const Json& prefab, const PrefabPlacement& p) {
    if (prefab.get("format").asString() != "skywalker.prefab" || !prefab.get("root").isObject()) {
        return Error::make("invalid_prefab", "not a skywalker prefab (missing \"format\": \"skywalker.prefab\")");
    }
    auto root = build(scene, prefab.get("root"), p.parent, p.name);
    if (!root) return root;
    if (Transform* t = scene.get<Transform>(*root)) {
        if (p.hasPosition) t->position = p.position;
        if (p.hasYaw) t->rotation.y = p.yaw;
        t->scale = t->scale * p.scale;
    }
    return root;
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
