#include <doctest/doctest.h>

#include "skywalker/scene/Scene.h"

using namespace sky;

TEST_CASE("scene: stable ids, names, find") {
    Scene s;
    EntityId a = s.create("Player");
    EntityId b = s.create("Crate");
    CHECK(a != b);
    CHECK(s.find("Player") == a);
    CHECK(s.find("player") == a);  // case-insensitive fallback
    CHECK(s.find("#2") == b);
    CHECK(s.find("2") == b);
    CHECK(s.find("Nope") == kNoEntity);
    s.destroy(a);
    EntityId c = s.create("New");
    CHECK(c != a);  // ids are never reused
}

TEST_CASE("scene: hierarchy, world matrices, cycle prevention, cascading destroy") {
    Scene s;
    EntityId root = s.create("Root");
    EntityId child = s.create("Child", root);
    EntityId grandchild = s.create("Grandchild", child);
    s.get<Transform>(root)->position = {10, 0, 0};
    s.get<Transform>(child)->position = {0, 5, 0};
    CHECK(s.worldMatrix(grandchild).translation() == Vec3{10, 5, 0});
    CHECK_FALSE(s.setParent(root, grandchild).ok());
    CHECK(s.destroy(root) == 3);
    CHECK(s.size() == 0);
}

TEST_CASE("scene: component patches, unknown component hint, removal") {
    Scene s;
    EntityId e = s.create("Lamp");
    CHECK(s.patchComponent(e, "light", Json::parse(R"({"kind":"spot","intensity":4})").value()).ok());
    REQUIRE(s.get<Light>(e));
    CHECK(s.get<Light>(e)->kind == "spot");
    Status bad = s.patchComponent(e, "lihgt", Json::object());
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.error().hint.find("light") != std::string::npos);
    // invalid patch on a missing component must not add it
    Status badAdd = s.patchComponent(e, "camera", Json::parse(R"({"fov":"wide"})").value());
    CHECK_FALSE(badAdd.ok());
    CHECK(s.get<Camera>(e) == nullptr);
    CHECK(s.patchComponent(e, "light", Json()).ok());
    CHECK(s.get<Light>(e) == nullptr);
    CHECK_FALSE(s.patchComponent(e, "transform", Json()).ok());
}

TEST_CASE("scene: save/load round trip is lossless") {
    Scene s;
    s.name = "Test";
    EntityId g = s.create("Ground");
    (void)s.applyEntityJson(g, Json::parse(R"({
        "tags": ["static"],
        "vars": {"friction": 0.8},
        "components": {"mesh": {"mesh": "plane", "color": "#336633"}, "transform": {"scale": [20, 1, 20]}},
        "behaviors": [{"name": "Idle", "intent": "do nothing", "source": "on tick\nend", "enabled": true}]
    })").value());
    EntityId c = s.create("Child", g);
    (void)c;
    (void)s.patchEnvironment(Json::parse(R"({"sunElevation": 12})").value());

    Json saved = s.toJson();
    Scene loaded;
    REQUIRE(loaded.loadJson(saved).ok());
    CHECK(loaded.toJson() == saved);
    CHECK(loaded.environment().sunElevation == doctest::Approx(12));
    CHECK(loaded.record(c)->parent == g);
}

TEST_CASE("scene: applyEntityJson rejects unknown keys with a hint") {
    Scene s;
    EntityId e = s.create("E");
    Status st = s.applyEntityJson(e, Json::parse(R"({"nmae": "x"})").value());
    REQUIRE_FALSE(st.ok());
    CHECK(st.error().hint.find("name") != std::string::npos);
    // top-level component shorthand is accepted
    CHECK(s.applyEntityJson(e, Json::parse(R"({"mesh": {"mesh": "sphere"}})").value()).ok());
    CHECK(s.get<MeshRenderer>(e)->mesh == "sphere");
}
