#include <doctest/doctest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <zlib.h>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <thread>

#include "skywalker/assets/Fetch.h"
#include "skywalker/engine/Engine.h"
#include "skywalker/render/MeshData.h"

using namespace sky;
namespace fs = std::filesystem;

namespace {

std::vector<uint8_t> bytesOf(const std::string& s) { return {s.begin(), s.end()}; }

const float* vert(const MeshData& m, size_t i) { return &m.vertices[i * MeshData::kFloatsPerVertex]; }

struct TempDir {
    fs::path dir;
    explicit TempDir(const char* name) {
        dir = fs::temp_directory_path() / ("skywalker-import-" + std::string(name) + "-" + AssetDatabase::newGuid().substr(0, 8));
        fs::create_directories(dir);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    void write(const std::string& rel, const std::string& text) const {
        fs::create_directories((dir / rel).parent_path());
        std::ofstream(dir / rel, std::ios::binary) << text;
    }
};

/// Minimal zip writer (stored or raw-deflated entries) for tests.
std::vector<uint8_t> makeZip(const std::vector<std::pair<std::string, std::string>>& files, bool deflate) {
    std::vector<uint8_t> out, central;
    auto p16 = [](std::vector<uint8_t>& v, uint16_t x) { v.push_back(x & 0xFF); v.push_back(x >> 8); };
    auto p32 = [](std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back((x >> (8 * i)) & 0xFF); };
    for (const auto& [name, content] : files) {
        std::vector<uint8_t> data(content.begin(), content.end());
        if (deflate) {
            std::vector<uint8_t> comp(compressBound(static_cast<uLong>(data.size())) + 64);
            z_stream zs{};
            deflateInit2(&zs, 6, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
            zs.next_in = data.data();
            zs.avail_in = static_cast<uInt>(data.size());
            zs.next_out = comp.data();
            zs.avail_out = static_cast<uInt>(comp.size());
            ::deflate(&zs, Z_FINISH);
            comp.resize(zs.total_out);
            deflateEnd(&zs);
            data.swap(comp);
        }
        uint32_t offset = static_cast<uint32_t>(out.size());
        uint32_t crc = static_cast<uint32_t>(crc32(0, reinterpret_cast<const Bytef*>(content.data()), static_cast<uInt>(content.size())));
        p32(out, 0x04034b50); p16(out, 20); p16(out, 0); p16(out, deflate ? 8 : 0); p16(out, 0); p16(out, 0);
        p32(out, crc); p32(out, static_cast<uint32_t>(data.size())); p32(out, static_cast<uint32_t>(content.size()));
        p16(out, static_cast<uint16_t>(name.size())); p16(out, 0);
        out.insert(out.end(), name.begin(), name.end());
        out.insert(out.end(), data.begin(), data.end());
        p32(central, 0x02014b50); p16(central, 20); p16(central, 20); p16(central, 0); p16(central, deflate ? 8 : 0);
        p16(central, 0); p16(central, 0); p32(central, crc); p32(central, static_cast<uint32_t>(data.size()));
        p32(central, static_cast<uint32_t>(content.size())); p16(central, static_cast<uint16_t>(name.size()));
        p16(central, 0); p16(central, 0); p16(central, 0); p16(central, 0); p32(central, 0); p32(central, offset);
        central.insert(central.end(), name.begin(), name.end());
    }
    uint32_t cdOffset = static_cast<uint32_t>(out.size());
    out.insert(out.end(), central.begin(), central.end());
    p32(out, 0x06054b50); p16(out, 0); p16(out, 0); p16(out, static_cast<uint16_t>(files.size()));
    p16(out, static_cast<uint16_t>(files.size())); p32(out, static_cast<uint32_t>(central.size())); p32(out, cdOffset); p16(out, 0);
    return out;
}

/// A tiny HTTP/1.0 server on 127.0.0.1 serving fixed files, for download tests.
class TestServer {
public:
    explicit TestServer(std::map<std::string, std::string> files) : files_(std::move(files)) {
        fd_ = socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        socklen_t len = sizeof(addr);
        getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
        listen(fd_, 8);
        thread_ = std::thread([this] { run(); });
    }
    ~TestServer() {
        stop_ = true;
        shutdown(fd_, SHUT_RDWR);
        close(fd_);
        thread_.join();
    }
    std::string url(const std::string& path) const { return "http://127.0.0.1:" + std::to_string(port_) + path; }

private:
    void run() {
        while (!stop_) {
            int c = accept(fd_, nullptr, nullptr);
            if (c < 0) return;
            char buf[4096];
            ssize_t n = recv(c, buf, sizeof(buf) - 1, 0);
            std::string req(buf, n > 0 ? static_cast<size_t>(n) : 0);
            std::string path = req.substr(4, req.find(' ', 4) - 4);
            auto it = files_.find(path);
            std::string body = it == files_.end() ? "not found" : it->second;
            std::string head = std::string("HTTP/1.0 ") + (it == files_.end() ? "404 Not Found" : "200 OK") +
                               "\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
            std::string all = head + body;
            send(c, all.data(), all.size(), 0);
            close(c);
        }
    }
    std::map<std::string, std::string> files_;
    int fd_ = -1;
    int port_ = 0;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

}  // namespace

TEST_CASE("import: PLY ascii with vertex colors and quad faces") {
    std::string ply = "ply\nformat ascii 1.0\ncomment test\nelement vertex 4\nproperty float x\nproperty float y\nproperty float z\n"
                      "property uchar red\nproperty uchar green\nproperty uchar blue\nelement face 1\n"
                      "property list uchar int vertex_indices\nend_header\n"
                      "0 0 0 255 0 0\n1 0 0 255 0 0\n1 1 0 0 0 255\n0 1 0 0 0 255\n4 0 1 2 3\n";
    auto m = mesh::parsePly(bytesOf(ply), false);
    REQUIRE_MESSAGE(m, (m ? "" : m.error().message));
    CHECK(m->indices.size() == 6);  // quad -> 2 triangles
    CHECK(m->hasVertexColors);
    CHECK(vert(*m, 0)[8] == doctest::Approx(1));  // red (linear)
    CHECK(vert(*m, 0)[10] == doctest::Approx(0));
    CHECK(vert(*m, 2)[10] == doctest::Approx(1));
    CHECK(std::fabs(vert(*m, 0)[5]) == doctest::Approx(1));  // computed normal along z

    std::string cloud = "ply\nformat ascii 1.0\nelement vertex 1\nproperty float x\nproperty float y\nproperty float z\nend_header\n0 0 0\n";
    auto pc = mesh::parsePly(bytesOf(cloud), false);
    CHECK_FALSE(pc);
    CHECK_FALSE(mesh::parsePly(bytesOf("not a ply"), false));
}

TEST_CASE("import: PLY binary little endian") {
    std::string head = "ply\nformat binary_little_endian 1.0\nelement vertex 3\nproperty float x\nproperty float y\nproperty float z\n"
                       "element face 1\nproperty list uchar uint vertex_indices\nend_header\n";
    std::vector<uint8_t> b(head.begin(), head.end());
    float pos[9] = {0, 0, 0, 2, 0, 0, 0, 2, 0};
    auto* pp = reinterpret_cast<uint8_t*>(pos);
    b.insert(b.end(), pp, pp + sizeof(pos));
    b.push_back(3);
    uint32_t idx[3] = {0, 1, 2};
    auto* ip = reinterpret_cast<uint8_t*>(idx);
    b.insert(b.end(), ip, ip + sizeof(idx));
    auto m = mesh::parsePly(b, false);
    REQUIRE_MESSAGE(m, (m ? "" : m.error().message));
    CHECK(m->indices.size() == 3);
    CHECK(m->bounds.max.x == doctest::Approx(2));
    b.pop_back();  // truncated
    CHECK_FALSE(mesh::parsePly(b, false));
}

TEST_CASE("import: STL ascii and binary") {
    std::string ascii = "solid t\nfacet normal 0 0 1\nouter loop\nvertex 0 0 0\nvertex 1 0 0\nvertex 0 1 0\nendloop\nendfacet\nendsolid t\n";
    auto a = mesh::parseStl(bytesOf(ascii), false);
    REQUIRE(a);
    CHECK(a->indices.size() == 3);
    std::vector<uint8_t> bin(80, 0);
    uint32_t n = 2;
    bin.insert(bin.end(), reinterpret_cast<uint8_t*>(&n), reinterpret_cast<uint8_t*>(&n) + 4);
    for (int t = 0; t < 2; ++t) {
        float f[12] = {0, 0, 1, 0, 0, 0, 3, 0, 0, 0, 3, static_cast<float>(t)};
        bin.insert(bin.end(), reinterpret_cast<uint8_t*>(f), reinterpret_cast<uint8_t*>(f) + sizeof(f));
        bin.push_back(0);
        bin.push_back(0);
    }
    auto b = mesh::parseStl(bin, false);
    REQUIRE(b);
    CHECK(b->indices.size() == 6);
    CHECK(b->bounds.max.x == doctest::Approx(3));
}

TEST_CASE("import: OBJ with MTL material and vertex colors; Z-up conversion; persisted import options") {
    TempDir d("obj");
    d.write("models/crate.obj", "mtllib crate.mtl\nv 0 0 0 1 0 0\nv 1 0 0 0 1 0\nv 0 0 1 0 0 1\nf 1 2 3\n");
    d.write("models/crate.mtl", "newmtl wood\nKd 0.5 0.25 0.1\nNs 50\nmap_Kd crate.png\n");
    d.write("models/crate.png", "fake");
    auto r = mesh::loadObjWithMaterial((d.dir / "models/crate.obj").string(), false);
    REQUIRE(r);
    CHECK(r->mesh.hasVertexColors);
    CHECK(r->material.present);
    CHECK(r->material.color.x == doctest::Approx(0.5));
    CHECK(r->material.roughness < 0.3f);
    CHECK(r->material.texture == (d.dir / "models/crate.png").string());

    EngineConfig cfg;
    cfg.renderer = RendererBackend::Null;
    cfg.projectDir = d.dir.string();
    Engine e(cfg);
    (void)e.newScene("t", true);
    ToolResult t = e.callTool("asset_import", Json::parse(R"({"path":"models/crate.obj","create_entity":"Crate","z_up":true,"normalize":false})").value(), "agent:test");
    INFO(t.content.front().text);
    REQUIRE_FALSE(t.isError);
    CHECK(t.structured.get("material").asString() == "models/crate.mat.json");
    CHECK(t.structured.get("vertexColors").asBool());
    // Z-up: the z extent (1) became the y extent.
    CHECK(t.structured.get("size")[1].asFloat() == doctest::Approx(1));
    const MeshRenderer* mr = e.scene().get<MeshRenderer>(e.scene().find("Crate"));
    REQUIRE(mr);
    CHECK(mr->color.x == doctest::Approx(1));  // white so vertex colors show unchanged
    CHECK(e.assets().find("models/crate.obj")->importSettings.get("zUp").asBool());
}

TEST_CASE("import: zip extraction (stored + deflate) and traversal guard") {
    for (bool deflate : {false, true}) {
        auto z = makeZip({{"model/a.obj", "v 0 0 0\n"}, {"model/tex/b.png", std::string(5000, 'x')}}, deflate);
        auto entries = zip::extract(z);
        REQUIRE_MESSAGE(entries, (entries ? "" : entries.error().message));
        REQUIRE(entries->size() == 2);
        CHECK((*entries)[1].name == "model/tex/b.png");
        CHECK((*entries)[1].data.size() == 5000);
    }
    CHECK_FALSE(zip::extract(makeZip({{"../evil.txt", "x"}}, false)));
    CHECK_FALSE(zip::extract(bytesOf("PK not really a zip at all, just text")));
    CHECK_FALSE(zip::extract(makeZip({{"big.bin", std::string(4096, 'z')}}, true), 1000));  // zip bomb guard
}

TEST_CASE("import: asset_download fetches, credits, imports (multi-file glTF from a local server)") {
    // One-triangle glTF with an external buffer.
    std::string bin;
    float pos[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    bin.append(reinterpret_cast<char*>(pos), sizeof(pos));
    std::string gltf = R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],
        "meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}],
        "buffers":[{"uri":"tri.bin","byteLength":36}],"bufferViews":[{"buffer":0,"byteLength":36}],
        "accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"}]})";
    TestServer server({{"/models/tri.gltf", gltf}, {"/models/tri.bin", bin}, {"/pack.zip", ""}});
    TempDir d("download");
    EngineConfig cfg;
    cfg.renderer = RendererBackend::Null;
    cfg.projectDir = d.dir.string();
    Engine e(cfg);
    (void)e.newScene("t", true);

    auto call = [&](const std::string& args, bool ok) {
        ToolResult t = e.callTool("asset_download", Json::parse(args).value(), "agent:test");
        INFO(t.content.front().text);
        CHECK(t.isError == !ok);
        return t.structured;
    };
    call(R"({"url":")" + server.url("/models/tri.gltf") + R"(","license":"unknown"})", false);
    call(R"({"url":"file:///etc/passwd","license":"CC0-1.0"})", false);
    call(R"({"url":")" + server.url("/missing.glb") + R"(","license":"CC0-1.0"})", false);
    Json r = call(R"({"url":")" + server.url("/models/tri.gltf") +
                      R"(","license":"CC-BY-4.0","author":"Ada","source_page":"https://example.org/tri","create_entity":"Tri"})",
                  true);
    CHECK(fs::exists(d.dir / "downloads/tri/tri.gltf"));
    CHECK(fs::exists(d.dir / "downloads/tri/tri.bin"));  // dependency fetched
    CHECK(r.get("mesh").asString() == "asset:downloads/tri/tri.gltf");
    CHECK(e.scene().find("Tri") != kNoEntity);
    const AssetRecord* rec = e.assets().find("downloads/tri/tri.gltf");
    REQUIRE(rec);
    CHECK(rec->source.get("license").asString() == "CC-BY-4.0");
    CHECK(rec->source.get("author").asString() == "Ada");
    std::ifstream credits(d.dir / "CREDITS.md");
    std::stringstream ss;
    ss << credits.rdbuf();
    CHECK(ss.str().find("**tri** by Ada — CC-BY-4.0") != std::string::npos);
    // Open-world tools are flagged for MCP clients so they ask the human first.
    const ToolDef* def = e.tools().find("asset_download");
    REQUIRE(def);
    CHECK(def->openWorld);
}
