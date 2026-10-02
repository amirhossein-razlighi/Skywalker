#include <doctest/doctest.h>

#include <string>

#include "skywalker/ecs/Components.h"
#include "skywalker/ecs/Registry.h"

using namespace sky;

namespace {
struct Health {
    int hp = 100;
};
struct Tag {
    std::string label;
};
}  // namespace

TEST_CASE("ecs: create, destroy and stale handles") {
    ecs::Registry reg;
    auto a = reg.create();
    auto b = reg.create();
    CHECK(reg.valid(a));
    CHECK(reg.alive() == 2);
    reg.destroy(a);
    CHECK_FALSE(reg.valid(a));
    auto c = reg.create();  // reuses a's slot with a new generation
    CHECK(c.index == a.index);
    CHECK(c.generation != a.generation);
    CHECK_FALSE(reg.valid(a));
    CHECK(reg.valid(b));
    CHECK(reg.get<Health>(a) == nullptr);
}

TEST_CASE("ecs: components add/get/remove with swap-and-pop") {
    ecs::Registry reg;
    std::vector<ecs::Entity> es;
    for (int i = 0; i < 10; ++i) {
        es.push_back(reg.create());
        reg.emplace<Health>(es.back(), Health{i});
    }
    reg.remove<Health>(es[3]);
    CHECK_FALSE(reg.has<Health>(es[3]));
    CHECK(reg.get<Health>(es[9])->hp == 9);  // moved into slot 3, still found
    CHECK(reg.count<Health>() == 9);
    reg.destroy(es[5]);
    CHECK(reg.count<Health>() == 8);
}

TEST_CASE("ecs: multi-component each") {
    ecs::Registry reg;
    for (int i = 0; i < 6; ++i) {
        auto e = reg.create();
        reg.emplace<Health>(e, Health{i});
        if (i % 2 == 0) reg.emplace<Tag>(e, Tag{"even"});
    }
    int sum = 0, visits = 0;
    reg.each<Tag, Health>([&](ecs::Entity, Tag& t, Health& h) {
        CHECK(t.label == "even");
        sum += h.hp;
        ++visits;
    });
    CHECK(visits == 3);
    CHECK(sum == 0 + 2 + 4);
}

TEST_CASE("reflection: json roundtrip, validation and hints") {
    MeshRenderer m;
    Json j = reflect::toJson(&m, MeshRenderer::type());
    CHECK(j["mesh"].asString() == "cube");

    Status ok = reflect::applyJson(&m, MeshRenderer::type(), Json::parse(R"({"color":"#ff0000","roughness":5})").value());
    CHECK(ok.ok());
    CHECK(m.color.x == doctest::Approx(1));
    CHECK(m.roughness == doctest::Approx(1));  // clamped to max

    Status typo = reflect::applyJson(&m, MeshRenderer::type(), Json::parse(R"({"colour":"#00ff00"})").value());
    REQUIRE_FALSE(typo.ok());
    CHECK(typo.error().code == "unknown_field");
    CHECK(typo.error().hint.find("color") != std::string::npos);

    Light l;
    Status badEnum = reflect::applyJson(&l, Light::type(), Json::parse(R"({"kind":"spott"})").value());
    REQUIRE_FALSE(badEnum.ok());
    CHECK(badEnum.error().hint.find("spot") != std::string::npos);

    // A failed patch must not partially apply.
    Transform t;
    Status bad = reflect::applyJson(&t, Transform::type(), Json::parse(R"({"position":[1,2,3],"scale":"big"})").value());
    CHECK_FALSE(bad.ok());
    CHECK(t.position == Vec3{0, 0, 0});
}

TEST_CASE("reflection: schema advertises enums and ranges") {
    Json s = reflect::schema(Light::type());
    CHECK(s["properties"]["kind"]["enum"].size() == 3);
    CHECK(s["properties"]["spotAngle"]["maximum"].asNumber() == doctest::Approx(89));
}
