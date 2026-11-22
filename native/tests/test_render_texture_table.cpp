// render/texture_table: what a texture's sidecar (<texture>.png.json) says about its transparency reaches the GPU meta, also after the sidecar changed. [gpu] suite.
#include <doctest.h>

#include <QImage>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

#include "base/jobs.h"
#include "gpu/device.h"
#include "gpu/upload_ring.h"
#include "render/texture_table.h"

using namespace native;
namespace fs = std::filesystem;

namespace {

struct Game {
    fs::path root;
    Game() {
        root = fs::temp_directory_path() / ("tex_table_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(root / "textures" / "common");
    }
    ~Game() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    void png(const std::string& name, bool withAlpha) const {
        QImage img(8, 8, QImage::Format_RGBA8888);
        img.fill(withAlpha ? QColor(200, 30, 30, 128) : QColor(200, 30, 30, 255));
        REQUIRE(img.save(QString::fromStdString((root / "textures" / (name + ".png")).string())));
    }
    void sidecar(const std::string& name, const std::string& text) const { std::ofstream(root / "textures" / (name + ".png.json"), std::ios::binary) << text; }
};

std::unique_ptr<gpu::Device> makeDevice() {
    gpu::DeviceOptions o;
    std::string err;
    auto d = gpu::Device::create(o, &err);
    if (!d) MESSAGE("no Vulkan device (" << err << "): GPU test skipped");
    return d;
}

// One frame's worth of upload(): a command buffer and the ring, submitted and waited for.
struct Uploader {
    gpu::Device& dev;
    gpu::UploadRing ring;
    VkCommandPool pool = VK_NULL_HANDLE;
    explicit Uploader(gpu::Device& d) : dev(d), ring(d, [] {
                                            gpu::UploadRingOptions o;
                                            o.bytesPerFrame = 4u << 20;
                                            return o;
                                        }()) {
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.queueFamilyIndex = d.graphicsFamily();
        vkCreateCommandPool(d.device(), &pi, nullptr, &pool);
    }
    ~Uploader() {
        vkDeviceWaitIdle(dev.device());
        vkDestroyCommandPool(dev.device(), pool, nullptr);
    }
    uint64_t value = 0;
    // Runs upload() until nothing is waiting.
    void pump(render::TextureTable& table) {
        for (int i = 0; i < 4; ++i) {
            VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            ai.commandPool = pool;
            ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            ai.commandBufferCount = 1;
            VkCommandBuffer cmd;
            vkAllocateCommandBuffers(dev.device(), &ai, &cmd);
            VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            REQUIRE(ring.beginFrame(value));
            vkBeginCommandBuffer(cmd, &bi);
            table.upload(cmd, ring);
            vkEndCommandBuffer(cmd);
            VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            si.commandBufferCount = 1;
            si.pCommandBuffers = &cmd;
            vkQueueSubmit(dev.graphicsQueue(), 1, &si, VK_NULL_HANDLE);
            vkQueueWaitIdle(dev.graphicsQueue());
            ring.endFrame(++value);
            vkFreeCommandBuffers(dev.device(), pool, 1, &cmd);
        }
    }
};

void waitLoaded(render::TextureTable& t, Uploader& up) {
    for (int i = 0; i < 400 && (t.pending() != 0 || t.loaded() + t.failed() < t.assigned()); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        up.pump(t);
    }
    up.pump(t);
}

} // namespace

TEST_CASE("texture table: an opaque image whose sidecar says translucent blends (trigger textures)" * doctest::test_suite("gpu")) {
    auto dev = makeDevice();
    if (!dev) return;
    Game g;
    g.png("common/trigger", false);
    g.sidecar("common/trigger", R"({"transparency": {"mode": "translucent", "opacity": 0.5, "tint": 1}})");
    g.png("common/plain", false);
    base::JobPool pool(2);
    render::TextureTable table(*dev);
    Uploader up(*dev);
    table.setSource(g.root, &pool, [] {});
    const uint32_t trigger = table.indexOf("common/trigger"), plain = table.indexOf("common/plain");
    REQUIRE(trigger != 0);
    waitLoaded(table, up);
    CHECK(table.loaded() == 2);
    CHECK(table.translucent(trigger));
    CHECK(table.alphaSurface(trigger));
    CHECK_FALSE(table.translucent(plain));
}

TEST_CASE("texture table: a sidecar written after the texture was loaded is read again by reload" * doctest::test_suite("gpu")) {
    auto dev = makeDevice();
    if (!dev) return;
    Game g;
    g.png("common/wall", false);
    base::JobPool pool(2);
    render::TextureTable table(*dev);
    Uploader up(*dev);
    table.setSource(g.root, &pool, [] {});
    const uint32_t wall = table.indexOf("common/wall");
    waitLoaded(table, up);
    REQUIRE(table.loaded() == 1);
    CHECK_FALSE(table.translucent(wall));
    g.sidecar("common/wall", R"({"transparency": {"mode": "translucent", "opacity": 0.25}})");
    table.reload("common/wall");
    waitLoaded(table, up);
    CHECK(table.translucent(wall));
    CHECK(table.opacity(wall) == doctest::Approx(0.25f));
    // and back
    g.sidecar("common/wall", R"({"transparency": {"mode": "opaque"}})");
    table.reload("common/wall");
    waitLoaded(table, up);
    CHECK_FALSE(table.translucent(wall));
    // a name that was never asked for is ignored
    table.reload("common/never");
    CHECK(table.assigned() == 2);   // the placeholder and the wall
}

TEST_CASE("texture table: refreshChanged reads again what changed on disk and leaves the rest" * doctest::test_suite("gpu")) {
    auto dev = makeDevice();
    if (!dev) return;
    Game g;
    g.png("common/a", false);
    g.png("common/b", false);
    base::JobPool pool(2);
    render::TextureTable table(*dev);
    Uploader up(*dev);
    table.setSource(g.root, &pool, [] {});
    const uint32_t a = table.indexOf("common/a"), b = table.indexOf("common/b");
    waitLoaded(table, up);
    REQUIRE(table.loaded() == 2);
    table.refreshChanged();   // nothing changed: nothing is read again
    CHECK(table.pending() == 0);
    g.sidecar("common/a", R"({"transparency": {"mode": "translucent", "opacity": 0.4}})");
    table.refreshChanged();
    waitLoaded(table, up);
    CHECK(table.translucent(a));
    CHECK_FALSE(table.translucent(b));
    CHECK(table.loaded() == 2);
    // edited again: a later change is noticed too
    g.sidecar("common/a", R"({"transparency": {"mode": "opaque"}})");
    table.refreshChanged();
    waitLoaded(table, up);
    CHECK_FALSE(table.translucent(a));
}
