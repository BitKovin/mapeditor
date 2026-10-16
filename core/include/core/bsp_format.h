#pragma once
#include <cstdint>

namespace core::rbsp {

// The rBSP file (REQUIREMENTS §25, byte layout in DESIGN §12): Raven's RBSP lump list with 8 light style slots, external EXR lightmaps and an
// explicit light grid header. Little endian, Quake units and axes. The engine reads the same structs (SdlWasmEngine/source/Engine/BSP).

inline constexpr char kIdent[4] = {'r', 'B', 'S', 'P'};
inline constexpr int32_t kVersion = 2;  // not Raven's 1: a reader that compares the ident without case still rejects the file
inline constexpr int kStyles = 8;       // light style slots per surface and per grid point
inline constexpr uint8_t kNoStyle = 255;
inline constexpr double kUnitsPerMeter = 32.0;

enum Lump {
    kEntities = 0, kShaders, kPlanes, kNodes, kLeafs, kLeafSurfaces, kLeafBrushes, kModels, kBrushes, kBrushSides, kDrawVerts, kDrawIndexes, kFogs,
    kSurfaces, kLightmaps, kLightGrid, kVisibility, kLightArray, kLumpCount
};

#pragma pack(push, 4)
struct Header {
    char ident[4];
    int32_t version;
    struct { int32_t offset, length; } lumps[kLumpCount];
};
struct Shader { char name[64]; int32_t surfaceFlags, contentFlags; };
struct Plane { float normal[3]; float dist; };
struct Node { int32_t plane; int32_t children[2]; int32_t mins[3], maxs[3]; };
struct Leaf {
    int32_t cluster, area;
    int32_t mins[3], maxs[3];
    int32_t firstLeafSurface, numLeafSurfaces, firstLeafBrush, numLeafBrushes;
};
struct Model { float mins[3], maxs[3]; int32_t firstSurface, numSurfaces, firstBrush, numBrushes; };
struct Brush { int32_t firstSide, numSides, shader; };
struct BrushSide { int32_t plane, shader; };
struct DrawVert {
    float xyz[3];
    float st[2];
    float lightmap[kStyles][2];
    float normal[3];
    uint8_t color[kStyles][4];
};
struct Surface {
    int32_t shader, fog, type;
    int32_t firstVert, numVerts, firstIndex, numIndexes;
    uint8_t lightmapStyles[kStyles], vertexStyles[kStyles];
    int32_t lightmapNum[kStyles];
    int32_t lightmapCorner[kStyles][2];
    int32_t lightmapSize[2];
    float lightmapOrigin[3];
    float lightmapVecs[2][3];
    float normal[3];
    int32_t patchSize[2];
};
struct GridPoint {
    uint8_t ambient[kStyles][3];
    uint8_t directed[kStyles][3];
    uint8_t styles[kStyles];
    uint8_t latLong[2];
};
// Start of the lightarray lump, then one uint16 palette index per grid point (x fastest, then y, then z; Quake axes).
struct LightGridHeader {
    float origin[3];    // Quake units: position of grid point (0, 0, 0)
    float cellSize[3];  // Quake units
    int32_t dims[3];
    int32_t reserved;
};
#pragma pack(pop)

static_assert(sizeof(Header) == 8 + 8 * kLumpCount);
static_assert(sizeof(Shader) == 72);
static_assert(sizeof(Plane) == 16);
static_assert(sizeof(Node) == 36);
static_assert(sizeof(Leaf) == 48);
static_assert(sizeof(Model) == 40);
static_assert(sizeof(Brush) == 12);
static_assert(sizeof(BrushSide) == 8);
static_assert(sizeof(DrawVert) == 128);
static_assert(sizeof(Surface) == 204);
static_assert(sizeof(GridPoint) == 58);
static_assert(sizeof(LightGridHeader) == 40);

inline constexpr int32_t kSurfacePolygon = 1, kSurfaceTriangles = 3;
inline constexpr int32_t kContentsSolid = 1;

}  // namespace core::rbsp
