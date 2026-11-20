// T6.1: the page image array grows by copying the old layers on the GPU. [gpu]: skips itself without a Vulkan device.
#include <doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "gpu/device.h"
#include "light/page_allocator.h"
#include "light/page_images.h"

using namespace native;

namespace {

std::vector<uint16_t> pattern(int atlas, int layer) {
    std::vector<uint16_t> v(static_cast<size_t>(atlas) * atlas * 4);
    for (size_t i = 0; i < v.size(); ++i) v[i] = static_cast<uint16_t>(layer * 4099 + i * 31 + (i >> 7));
    return v;
}

} // namespace

TEST_CASE("page images: layers keep their content when the array grows by 8" * doctest::test_suite("gpu")) {
    gpu::DeviceOptions o;
    o.validation = true;
    std::string err;
    auto device = gpu::Device::create(o, &err);
    if (!device) {
        MESSAGE("no Vulkan device (" << err << "): GPU test skipped");
        return;
    }
    const int atlas = 64;
    light::PageAllocator alloc(atlas);
    light::PageImages images(*device, atlas);
    CHECK_FALSE(images.ok());
    for (int i = 0; i < 3; ++i) REQUIRE(alloc.alloc(atlas, atlas));
    CHECK(alloc.takeGrown());
    REQUIRE(images.ensureLayers(alloc.capacity()));
    REQUIRE(images.ok());
    CHECK(images.layers() == 8);
    const uint64_t gen = images.generation();
    for (int l = 0; l < 3; ++l) REQUIRE(images.writeLayer(l, pattern(atlas, l)));

    // pages 4..9 need the next 8 layers
    for (int i = 0; i < 7; ++i) REQUIRE(alloc.alloc(atlas, atlas));
    CHECK(alloc.pageCount() == 10);
    CHECK(alloc.takeGrown());
    CHECK_FALSE(images.ensureLayers(8));   // already there: nothing happens
    REQUIRE(images.ensureLayers(alloc.capacity()));
    CHECK(images.layers() == 16);
    CHECK(images.generation() == gen + 1);
    for (int l = 0; l < 3; ++l) {
        std::vector<uint16_t> back;
        REQUIRE(images.readLayer(l, back));
        CHECK(back == pattern(atlas, l));
    }
    // a new layer is writable and the old ones are not disturbed
    REQUIRE(images.writeLayer(12, pattern(atlas, 12)));
    std::vector<uint16_t> back;
    REQUIRE(images.readLayer(12, back));
    CHECK(back == pattern(atlas, 12));
    REQUIRE(images.readLayer(1, back));
    CHECK(back == pattern(atlas, 1));
    CHECK(images.bytes() >= static_cast<uint64_t>(atlas) * atlas * 8 * 16);
    CHECK(device->validationErrors() == 0);
}
