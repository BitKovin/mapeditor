// scene/model_store + scene/model_entity: loading models by path, their bounds in the entity boxes (T11.9b).
#include <doctest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>

#include "base/jobs.h"
#include "model_builder.h"
#include "scene/model_entity.h"
#include "scene/model_store.h"
#include "scene/point_box.h"

using namespace native::scene;
using modeltest::Builder;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

struct TempAssets {
    fs::path root;
    TempAssets() {
        root = fs::temp_directory_path() / ("model_store_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(root / "models");
    }
    ~TempAssets() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    void write(const std::string& rel, const std::string& text) const { std::ofstream(root / rel, std::ios::binary) << text; }
    void write(const std::string& rel, const std::vector<uint8_t>& bytes) const { std::ofstream(root / rel, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())); }
};

// A glb of one triangle (0 0 0, 2 0 0, 0 4 0).
std::vector<uint8_t> triangleGlb() {
    Builder b;
    const int pos = b.floats({0, 0, 0, 2, 0, 0, 0, 4, 0}, "VEC3");
    const json g = b.document({{"meshes", json::array({json{{"primitives", json::array({json{{"attributes", {{"POSITION", pos}}}}})}}})}, {"nodes", json::array({json{{"mesh", 0}}})}});
    return b.glb(g);
}

const char* kObj = "v 0 0 0\nv 1 0 0\nv 0 3 0\nv 0 0 5\nusemtl m\nf 1 2 3\nf 1 3 4\n";

bool near(double a, double b) { return std::fabs(a - b) < 1e-5; }

core::Entity modelEntity(const std::string& path) {
    core::Entity e;
    e.classname = "misc_model";
    e.keyvalues["model_path"] = path;
    return e;
}

} // namespace

TEST_CASE("model store: a .glb and an .obj load, with their bounds") {
    TempAssets a;
    a.write("models/tri.glb", triangleGlb());
    a.write("models/box.obj", std::string(kObj));
    ModelStore store;
    store.setSource(a.root, nullptr, {});
    const ModelStore::Slot glb = store.get("models/tri.glb");
    REQUIRE(glb.status == ModelStore::Status::Ready);
    CHECK(glb.model->data.primitives.size() == 1);
    CHECK(near(store.bounds("models/tri.glb")->max[1], 4));
    const ModelStore::Slot obj = store.get("models/box.obj");
    REQUIRE(obj.status == ModelStore::Status::Ready);
    CHECK(near(store.bounds("models/box.obj")->max[2], 5));
    CHECK(store.revision() > 0);
}

TEST_CASE("model store: missing, unsupported and outside files are errors, not crashes") {
    TempAssets a;
    a.write("models/readme.txt", std::string("hello"));
    a.write("models/bad.glb", std::string("not a glb at all, just text that is long enough"));
    ModelStore store;
    store.setSource(a.root, nullptr, {});
    CHECK(store.get("models/none.glb").status == ModelStore::Status::Error);
    const auto txt = store.get("models/readme.txt");
    CHECK(txt.status == ModelStore::Status::Error);
    CHECK(txt.error.find("unsupported") != std::string::npos);
    CHECK(store.get("models/bad.glb").error.find("not a binary glTF") != std::string::npos);
    CHECK(store.get("../outside.glb").status == ModelStore::Status::Error);
    CHECK_FALSE(store.bounds("models/bad.glb").has_value());
}

TEST_CASE("model store: loading on the pool tells the listener, and a changed source forgets the old models") {
    TempAssets a;
    a.write("models/tri.glb", triangleGlb());
    native::base::JobPool pool(2);
    std::atomic<int> changes{0};
    ModelStore store;
    store.setSource(a.root, &pool, [&] { ++changes; });
    const auto first = store.get("models/tri.glb");
    CHECK(first.status == ModelStore::Status::Loading);   // or already Ready on a fast pool: it must not be an error
    pool.waitIdle();
    CHECK(store.get("models/tri.glb").status == ModelStore::Status::Ready);
    CHECK(changes.load() == 1);
    store.clear();
    CHECK_FALSE(store.bounds("models/tri.glb").has_value());
    CHECK(changes.load() == 2);
    pool.waitIdle();
}

TEST_CASE("model entity: the path keys, the scale, the defaults") {
    core::Entity e;
    e.classname = "misc_model";
    CHECK(modelPathOf(e).empty());
    e.keyvalues["model"] = " \\models\\a\\b.glb ";
    CHECK(modelPathOf(e) == "models/a/b.glb");
    e.keyvalues["model_path"] = "/models/c.obj";
    CHECK(modelPathOf(e) == "models/c.obj");   // model_path wins; the leading slash goes
    e.keyvalues["model_animation"] = " Walk ";
    CHECK(modelAnimationOf(e) == "Walk");
    CHECK(near(native::scene::modelScaleOf(e)[0], 1));
    e.keyvalues["model_scale"] = "2";
    e.keyvalues["modelscale_vec"] = "1 3 1";
    const V3 s = native::scene::modelScaleOf(e);
    CHECK(near(s[0], 2));
    CHECK(near(s[1], 6));
    CHECK(near(s[2], 2));
    core::Brush brush;
    core::Entity brushEntity = e;
    brushEntity.brushes.push_back(brush);
    CHECK(modelPathOf(brushEntity).empty());   // a brush entity shows no model

    core::Entity door;
    door.classname = "env_door";
    CHECK(modelPathOf(door).empty());
    setKeyDefaultProvider([](const std::string& cls, const std::string& key) { return cls == "env_door" && key == "model_path" ? std::string("models/tools/door.obj") : std::string(); });
    CHECK(modelPathOf(door) == "models/tools/door.obj");
    setKeyDefaultProvider({});
    CHECK(modelPathOf(door).empty());
}

TEST_CASE("model entity: the model's box replaces the class box once loaded") {
    TempAssets a;
    a.write("models/tri.glb", triangleGlb());
    native::base::JobPool pool(1);
    ModelStore store;
    store.setSource(a.root, &pool, {});
    store.install();
    const BoxOf classBox = [](const std::string&) { return Box{{-0.25, -0.25, -0.25}, {0.25, 0.25, 0.25}}; };
    core::Entity e = modelEntity("models/tri.glb");
    e.origin = {10, 0, 0};
    e.keyvalues["model_scale"] = "2";
    // not loaded yet (the first look starts the load): the class box
    const OrientedBox before = pointBox(e, classBox);
    CHECK(near(before.max[0], 0.25));
    pool.waitIdle();
    const OrientedBox after = pointBox(e, classBox);
    CHECK(near(after.min[0], 0));
    CHECK(near(after.max[0], 4));    // 2 m wide, scaled by 2
    CHECK(near(after.max[1], 8));
    CHECK(near(after.origin[0], 10));
    CHECK(entityModelBox(e).has_value());
    // a decal and an entity without a model keep their own boxes
    core::Entity plain;
    plain.classname = "info_player_start";
    CHECK(near(pointBox(plain, classBox).max[0], 0.25));
    store.uninstall();
    CHECK_FALSE(entityModelBox(e).has_value());
    CHECK(near(pointBox(e, classBox).max[0], 0.25));
    pool.waitIdle();
}
