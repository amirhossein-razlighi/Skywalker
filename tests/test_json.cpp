#include <doctest/doctest.h>

#include "skywalker/core/Json.h"
#include "skywalker/core/Strings.h"

using sky::Json;

TEST_CASE("json: parse and dump round trip") {
    const char* text = R"({"a":1,"b":[true,false,null],"c":{"d":"x\ny","e":-2.5e3},"f":"é😀"})";
    auto parsed = Json::parse(text);
    REQUIRE(parsed.ok());
    const Json& j = parsed.value();
    CHECK(j["a"].asInt() == 1);
    CHECK(j["b"].size() == 3);
    CHECK(j["b"][2].isNull());
    CHECK(j["c"]["d"].asString() == "x\ny");
    CHECK(j["c"]["e"].asNumber() == doctest::Approx(-2500));
    CHECK(j["f"].asString() == "\xC3\xA9\xF0\x9F\x98\x80");
    auto again = Json::parse(j.dump());
    REQUIRE(again.ok());
    CHECK(again.value() == j);
}

TEST_CASE("json: object order is preserved (deterministic output)") {
    Json j = Json::object();
    j["zeta"] = 1;
    j["alpha"] = 2;
    CHECK(j.dump() == R"({"zeta":1,"alpha":2})");
}

TEST_CASE("json: integers print without decimals, floats round-trip") {
    CHECK(Json(42).dump() == "42");
    CHECK(Json(uint64_t{9007199254740991ULL}).dump() == "9007199254740991");
    auto r = Json::parse(Json(0.1).dump());
    REQUIRE(r.ok());
    CHECK(r.value().asNumber() == 0.1);
}

TEST_CASE("json: errors report line and column") {
    auto r = Json::parse("{\n  \"a\": tru\n}");
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == "parse_error");
    CHECK(r.error().message.find("line 2") != std::string::npos);
    CHECK_FALSE(Json::parse("[1,2,]").ok());
    CHECK_FALSE(Json::parse("\"abc").ok());
    CHECK_FALSE(Json::parse("{} extra").ok());
}

TEST_CASE("json: nesting depth is bounded") {
    std::string deep(1000, '[');
    deep += std::string(1000, ']');
    CHECK_FALSE(Json::parse(deep).ok());
}

TEST_CASE("json: merge patch (RFC 7386)") {
    Json target = Json::parse(R"({"a":1,"b":{"c":2,"d":3}})").value();
    target.mergePatch(Json::parse(R"({"a":null,"b":{"c":9},"e":true})").value());
    CHECK_FALSE(target.contains("a"));
    CHECK(target["b"]["c"].asInt() == 9);
    CHECK(target["b"]["d"].asInt() == 3);
    CHECK(target["e"].asBool());
}

TEST_CASE("strings: glob, edit distance, closest, base64") {
    using namespace sky::str;
    CHECK(globMatch("crate*", "Crate_01"));
    CHECK(globMatch("*tree?", "PineTree1"));
    CHECK_FALSE(globMatch("rock", "rocks"));
    CHECK(editDistance("kitten", "sitting") == 3);
    CHECK(closest("postion", {"position", "rotation", "scale"}) == "position");
    CHECK(closest("zzzzzz", {"position"}).empty());
    CHECK(base64Encode("Man", 3) == "TWFu");
    CHECK(base64Encode("Ma", 2) == "TWE=");
    CHECK(base64Encode("M", 1) == "TQ==");
}
