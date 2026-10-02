#include <doctest/doctest.h>

#include "skywalker/scene/History.h"

using namespace sky;

TEST_CASE("history: undo/redo of create, modify and delete") {
    Scene s;
    History h(s);
    Json empty = s.toJson();

    h.begin("user", "Create");
    EntityId e = s.create("Box");
    (void)s.patchComponent(e, "mesh", Json::object({{"mesh", "cube"}}));
    REQUIRE(h.commit());
    Json created = s.toJson();

    h.begin("agent:Nimbus", "Move");
    (void)s.patchComponent(e, "transform", Json::parse(R"({"position":[1,2,3]})").value());
    REQUIRE(h.commit());
    Json moved = s.toJson();
    CHECK(h.lastCommitted()->actor == "agent:Nimbus");

    h.begin("user", "Delete");
    s.destroy(e);
    REQUIRE(h.commit());

    REQUIRE(h.undo());
    CHECK(s.toJson() == moved);
    REQUIRE(h.undo());
    CHECK(s.toJson() == created);
    REQUIRE(h.undo());
    CHECK(s.toJson() == empty);
    CHECK_FALSE(h.undo());

    REQUIRE(h.redo());
    CHECK(s.toJson() == created);
    REQUIRE(h.redo());
    REQUIRE(h.redo());
    CHECK_FALSE(s.exists(e));
}

TEST_CASE("history: deleting a hierarchy restores parents and order") {
    Scene s;
    History h(s);
    EntityId a = s.create("A");
    EntityId p = s.create("Parent");
    EntityId c1 = s.create("C1", p);
    EntityId c2 = s.create("C2", c1);
    EntityId z = s.create("Z");
    (void)a;
    (void)z;
    Json before = s.toJson();

    h.begin("user", "Delete parent");
    s.destroy(p);
    REQUIRE(h.commit());
    CHECK(s.size() == 2);
    REQUIRE(h.undo());
    CHECK(s.toJson() == before);
    CHECK(s.record(c2)->parent == c1);
}

TEST_CASE("history: rollback leaves the scene untouched and records nothing") {
    Scene s;
    History h(s);
    EntityId e = s.create("E");
    Json before = s.toJson();
    h.begin("agent", "Batch");
    (void)s.rename(e, "Renamed");
    s.create("Extra");
    (void)s.patchEnvironment(Json::parse(R"({"ambient": 2})").value());
    h.rollback();
    CHECK(s.toJson() == before);
    CHECK_FALSE(h.canUndo());
}

TEST_CASE("history: no-op transactions are dropped; new edits clear redo") {
    Scene s;
    History h(s);
    EntityId e = s.create("E");
    h.begin("user", "Noop");
    (void)s.rename(e, "E");
    CHECK_FALSE(h.commit());

    h.begin("user", "Rename");
    (void)s.rename(e, "F");
    REQUIRE(h.commit());
    REQUIRE(h.undo());
    CHECK(h.canRedo());
    h.begin("user", "Rename2");
    (void)s.rename(e, "G");
    REQUIRE(h.commit());
    CHECK_FALSE(h.canRedo());
}

TEST_CASE("history: environment changes are undoable") {
    Scene s;
    History h(s);
    h.begin("user", "Sunset");
    (void)s.patchEnvironment(Json::parse(R"({"sunElevation": 5})").value());
    REQUIRE(h.commit());
    REQUIRE(h.undo());
    CHECK(s.environment().sunElevation == doctest::Approx(Environment{}.sunElevation));
}
