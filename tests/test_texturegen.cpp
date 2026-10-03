#include <doctest/doctest.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>

#include "skywalker/render/TextureGen.h"

using namespace sky;

namespace {

texgen::Params small(const std::string& kind, int size = 64) {
    texgen::Params p = texgen::defaults(kind);
    p.size = size;
    return p;
}

/// Mean absolute RGB difference between two columns (or rows) of an image.
double columnDiff(const Image& im, int xa, int xb) {
    double sum = 0;
    for (int y = 0; y < im.height; ++y)
        for (int c = 0; c < 3; ++c) sum += std::abs(int(im.at(xa, y)[c]) - int(im.at(xb, y)[c]));
    return sum / (im.height * 3.0);
}
double rowDiff(const Image& im, int ya, int yb) {
    double sum = 0;
    for (int x = 0; x < im.width; ++x)
        for (int c = 0; c < 3; ++c) sum += std::abs(int(im.at(x, ya)[c]) - int(im.at(x, yb)[c]));
    return sum / (im.width * 3.0);
}

/// Seam difference (last -> first) compared with the neighbor steps inside the tile. Smooth kinds
/// must have a seam no larger than a typical step; hard-edged kinds (bricks, tiles, ...) may have a
/// genuine edge exactly at the seam, so there it only has to look like one of the interior steps.
void expectSeamless(const Image& im, const std::string& what, bool hardEdged) {
    double meanX = 0, meanY = 0, maxX = 0, maxY = 0;
    for (int i = 0; i + 1 < im.width; ++i) {
        double d = columnDiff(im, i, i + 1);
        meanX += d;
        maxX = std::max(maxX, d);
    }
    for (int i = 0; i + 1 < im.height; ++i) {
        double d = rowDiff(im, i, i + 1);
        meanY += d;
        maxY = std::max(maxY, d);
    }
    meanX /= (im.width - 1);
    meanY /= (im.height - 1);
    double seamX = columnDiff(im, im.width - 1, 0), seamY = rowDiff(im, im.height - 1, 0);
    INFO(what << " seamX=" << seamX << " meanX=" << meanX << " maxX=" << maxX << " seamY=" << seamY << " meanY=" << meanY << " maxY=" << maxY);
    double limX = meanX * 1.5 + 3.0, limY = meanY * 1.5 + 3.0;
    if (hardEdged) {
        limX = std::max(limX, maxX * 1.1);
        limY = std::max(limY, maxY * 1.1);
    }
    CHECK(seamX <= limX);
    CHECK(seamY <= limY);
}

}  // namespace

TEST_CASE("texgen: every kind generates with matching sizes") {
    CHECK(texgen::kinds().size() >= 19);
    for (const auto& kind : texgen::kinds()) {
        INFO(kind);
        auto r = texgen::generate(small(kind));
        REQUIRE(r.ok());
        for (const Image* im : {&r->albedo, &r->normal, &r->orm}) {
            CHECK(im->width == 64);
            CHECK(im->height == 64);
            CHECK(im->pixels.size() == 64u * 64u * 4u);
        }
        CHECK(texgen::defaults(kind).kind == kind);
    }
}

TEST_CASE("texgen: albedo and normals tile seamlessly") {
    for (const auto& kind : texgen::kinds()) {
        if (kind == "curtain") continue;  // fades out vertically by design (auroras, light shafts)
        auto r = texgen::generate(small(kind, 128));
        REQUIRE(r.ok());
        const bool hard = kind == "bricks" || kind == "tiles" || kind == "planks" || kind == "cobblestone" ||
                          kind == "checker" || kind == "stripes" || kind == "hexagons" || kind == "scales" || kind == "fabric";
        expectSeamless(r->albedo, kind + " albedo", hard);
        expectSeamless(r->normal, kind + " normal", hard);
    }
}

TEST_CASE("texgen: deterministic per seed") {
    for (const auto& kind : texgen::kinds()) {
        INFO(kind);
        texgen::Params p = small(kind);
        p.seed = 7;
        auto a = texgen::generate(p);
        auto b = texgen::generate(p);
        REQUIRE(a.ok());
        REQUIRE(b.ok());
        CHECK(a->albedo.pixels == b->albedo.pixels);
        CHECK(a->normal.pixels == b->normal.pixels);
        CHECK(a->orm.pixels == b->orm.pixels);
        p.seed = 8;
        auto c = texgen::generate(p);
        REQUIRE(c.ok());
        if (kind != "checker" && kind != "stripes") {  // purely geometric kinds only change by a faint mottle
            CHECK(a->albedo.pixels != c->albedo.pixels);
        }
    }
}

TEST_CASE("texgen: invalid parameters are rejected") {
    texgen::Params p = small("bricks");
    p.size = 100;
    CHECK_FALSE(texgen::generate(p).ok());
    p.size = 16;
    CHECK_FALSE(texgen::generate(p).ok());
    p.size = 4096;
    CHECK_FALSE(texgen::generate(p).ok());
    p = small("bricks");
    p.kind = "nonsense";
    auto r = texgen::generate(p);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == "invalid_argument");
    p = small("bricks");
    p.scale = 0.f;
    CHECK_FALSE(texgen::generate(p).ok());
}

TEST_CASE("texgen: checker normal map is flat and ORM carries material values") {
    texgen::Params p = small("checker", 128);
    p.roughness = 0.4f;
    p.metallic = 0.25f;
    auto r = texgen::generate(p);
    REQUIRE(r.ok());
    double sx = 0, sy = 0, sz = 0;
    const Image& n = r->normal;
    for (int y = 0; y < n.height; ++y)
        for (int x = 0; x < n.width; ++x) {
            sx += n.at(x, y)[0];
            sy += n.at(x, y)[1];
            sz += n.at(x, y)[2];
        }
    double count = n.width * n.height;
    CHECK(sx / count == doctest::Approx(128.0).epsilon(0.02));
    CHECK(sy / count == doctest::Approx(128.0).epsilon(0.02));
    CHECK(sz / count == doctest::Approx(255.0).epsilon(0.02));
    const uint8_t* orm = r->orm.at(10, 10);
    CHECK(orm[1] == doctest::Approx(0.4 * 255).epsilon(0.1));
    CHECK(orm[2] == doctest::Approx(0.25 * 255).epsilon(0.02));
}

TEST_CASE("texgen: bricks have relief and rust varies metallic") {
    auto bricks = texgen::generate(small("bricks", 128));
    REQUIRE(bricks.ok());
    int nonFlat = 0;
    for (int y = 0; y < 128; ++y)
        for (int x = 0; x < 128; ++x)
            if (std::abs(int(bricks->normal.at(x, y)[0]) - 128) > 10) ++nonFlat;
    CHECK(nonFlat > 200);

    auto rust = texgen::generate(small("rust", 128));
    REQUIRE(rust.ok());
    int lo = 255, hi = 0;
    for (int y = 0; y < 128; ++y)
        for (int x = 0; x < 128; ++x) {
            lo = std::min<int>(lo, rust->orm.at(x, y)[2]);
            hi = std::max<int>(hi, rust->orm.at(x, y)[2]);
        }
    CHECK(hi - lo > 100);
}

// Developer aid: SKY_TEXGEN_OUT=/some/dir writes albedo + normal PNGs of every kind at 512^2.
TEST_CASE("texgen: optional PNG sample dump" * doctest::skip(std::getenv("SKY_TEXGEN_OUT") == nullptr)) {
    const char* dir = std::getenv("SKY_TEXGEN_OUT");
    if (!dir) return;
    std::filesystem::create_directories(dir);
    for (const auto& kind : texgen::kinds()) {
        texgen::Params p = texgen::defaults(kind);
        p.size = 512;
        auto r = texgen::generate(p);
        REQUIRE(r.ok());
        CHECK(writePng(r->albedo, std::string(dir) + "/" + kind + "_albedo.png").ok());
        CHECK(writePng(r->normal, std::string(dir) + "/" + kind + "_normal.png").ok());
    }
}

TEST_CASE("texgen: soft kinds fade to transparent") {
    for (const char* kind : {"glow", "curtain"}) {
        INFO(kind);
        auto r = texgen::generate(small(kind, 64));
        REQUIRE(r.ok());
        const Image& a = r->albedo;
        CHECK(a.at(0, 0)[3] < 20);  // corner (glow) / top (curtain) is nearly transparent
        int maxAlpha = 0;
        for (int y = 0; y < a.height; ++y)
            for (int x = 0; x < a.width; ++x) maxAlpha = std::max(maxAlpha, static_cast<int>(a.at(x, y)[3]));
        CHECK(maxAlpha > 150);
    }
    auto glow = texgen::generate(small("glow", 64));
    CHECK(glow->albedo.at(32, 32)[3] > 200);  // bright core
}
