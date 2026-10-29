#pragma once
#include "core/derived.h"
#include "core/math3.h"
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace core {

// Baked visibility of the rBSP export (VISIBILITY_REWRITE.md, DESIGN §12 "Visibility"): the playable zones are cut into cubic cells, big walls cut the
// cells they pass through, the regions that are not solid are the clusters, and the cluster pairs that see each other are found by tracing segments
// between sample points of the two (plus rays through narrow openings). All of it in editor space (metres, Y up); the compiler converts to Quake units.
// Cluster 0 is "outside" (everything beyond the cells): it sees and is seen by every cluster. Cluster -1 is solid space (the engine treats it as unknown).

// Where the segments are traced: Auto = a GPU when there is enough work (ray query, else a compute shader), the CPU when there is none.
enum class VisDevice { Auto, Cpu, Compute, RayQuery };

struct VisSettings {
    Vec3 cell{5, 2, 5};                       // m: size of a cell to start with (x, y up, z; grows until the visibility table fits maxBytes)
    size_t maxBytes = size_t(50) << 20;       // the visibility lump may be this big (clusters^2 / 8)
    double splitMinWidth = 2.0;               // m: a wall splits cells when its (merged, coplanar) face has no dimension below this
    int maxSplitsPerCell = 16;
    bool dilate = false;                      // a cluster also gets the view of the clusters it opens into (one ring, never through a wall)
    int samplePool = 32;                      // sample points per cluster
    int pairSegments = 64;                    // segments traced per cluster pair
    double apertureMax = 0.5;                 // m: openings up to this wide get rays aimed through them (0 = off)
    VisDevice device = VisDevice::Auto;
    size_t selfCheck = 0;                     // random segments to check the result with afterwards (the environment variable MAPEDITOR_VIS_CHECK overrides)
    size_t gpuMinPairs = 200000;              // Auto: cluster pairs below this are traced on the CPU (starting a GPU costs more)
};

// A playable zone: a convex piece (bounds + planes `n . x <= d` inside); no planes = the box.
struct VisZone {
    Vec3 mn, mx;
    std::vector<std::pair<Vec3, double>> planes;
};

struct VisInput {
    const std::vector<LightSurface>* occluders = nullptr;  // the faces that block sight (opaque, static world), pieces as the lighting sees them
    const SolidSet* solids = nullptr;                      // the volumes that block sight: sample points are not taken inside them, regions inside them are solid
    std::vector<VisZone> zones;
    VisSettings settings;
    bool trace = true;                                     // false: the clusters are made but every cluster sees every cluster
};

struct VisHalfSpace { Vec3 n; double d = 0; };            // inside: n . p <= d

struct VisLeaf {
    int cluster = -1;                                      // 0 outside, -1 solid, >= 1 a region of space
    Vec3 mn, mx;                                           // bounds
    std::vector<VisHalfSpace> sides;                       // the convex region (clusters only)
    Vec3 centroid;
};

// children[0]: n . p >= dist, children[1]: behind it. A child < 0 is leaf -(c + 1).
struct VisNode {
    Vec3 normal;
    double dist = 0;
    int32_t children[2] = {0, 0};
    Vec3 mn, mx;
};

struct VisStats {
    size_t checked = 0, misses = 0;                         // the self check: random segments tried / the ones between clusters that do not see each other
    size_t cells = 0, splitPlanes = 0, regions = 0, solidRegions = 0, apertures = 0, apertureLines = 0, pairs = 0, adjacent = 0, bytes = 0;
    Vec3 cell; double seconds = 0, secondsTree = 0, secondsPairs = 0, secondsAperture = 0, secondsDilate = 0;
    std::string device;
};

class VisResult {
public:
    std::vector<VisNode> nodes;       // node 0 is the root
    std::vector<VisLeaf> leafs;       // leaf 0 is the outside leaf (cluster 0; every `outside` face goes into it)
    int clusters = 1;                 // including cluster 0
    size_t words = 0;                 // 64 bit words per row of `bits`
    std::vector<uint64_t> bits;       // clusters x words: bit c of row r = r may see c
    std::vector<std::string> warnings;
    VisStats stats;

    // The leaf the point lies in.
    int leafAt(const Vec3& p) const;
    // The leaves with a cluster >= 1 that the convex polygon (or the box) touches; `outside` when part of it lies in a leaf of cluster 0.
    void leavesFor(const std::vector<Vec3>& polygon, std::vector<int>& out, bool& outside, double tolerance = 0.03) const;
    void leavesForBox(const Vec3& mn, const Vec3& mx, std::vector<int>& out, double tolerance = 0.03) const;
    // What the playable zones themselves can see: the union of the rows of the clusters inside a zone (and the outside). Everything outside the cells is seen.
    std::vector<uint64_t> playableSees;
    // True when the polygon lies partly outside the cells, or touches a cluster the playable zones see.
    bool seen(const std::vector<Vec3>& polygon) const;
    bool sees(int a, int b) const { return (bits[size_t(a) * words + size_t(b) / 64] >> (b % 64)) & 1; }

    // lookup of the leaves per coarse cell (built by buildVisibility)
    double indexCell = 8;
    std::vector<std::pair<uint64_t, std::vector<int>>> index;  // sorted by key
    Vec3 cellSize;
    void buildIndex();
    const std::vector<int>* indexAt(int64_t x, int64_t y, int64_t z) const;
};

using VisProgress = std::function<void(const std::string& stage, double fraction)>;
VisResult buildVisibility(const VisInput& in, const VisProgress& progress = {});

}  // namespace core
