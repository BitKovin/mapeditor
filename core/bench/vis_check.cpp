// Checks the baked visibility of a map against what can really be seen: false negatives (a cluster that can be seen but the table hides) and false positives
// (clusters the table shows that no sample can see: what the culling gives away).
//
//   vis_check <assetsRoot> <map.map.json> [--sections file.json] [--clusters N] [--views V] [--targets P] [--seed S] [--list K]
//             [--cell X Y Z] [--extend N] [--segments N] [--samples N] [--aperture M] [--dilate] [--device cpu|compute|rayquery] [--split-min M]
//
// The map is exported without a bake (the real export path, so the table is the one the game gets). Then for N analysed clusters A (random, spread over the sections):
//   truth(A)  = the clusters B for which a segment between one of V viewpoints inside A and one of P points inside B is clear (any-hit on the BVH of the faces that block
//               sight; the pairs the table shows and the sampling did not confirm are tried again with 8x more viewpoints and targets)
//   false negative: B in truth(A) but not in the table's row of A (the game would cull something the player can see: must be 0)
//   false positive: B in the row of A but not in truth(A) (shown for nothing)
//   cull ratio    : 1 - |row(A)| / |all clusters of A's section|
// `--sections` (written by tools/gen_vis_testmap.py) groups the numbers by section; without it everything is one group.
#include "core/bsp_compile.h"
#include "core/bsp_export.h"
#include "core/bsp_vis.h"
#include "core/exporter.h"
#include "core/parallel.h"
#include "core/raytrace.h"
#include "core/scene.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <nlohmann/json.hpp>
#include <random>
#include <string>
#include <vector>

using namespace core;
using Clock = std::chrono::steady_clock;

namespace {

struct Section {
    std::string name;
    Vec3 mn, mx;
    std::vector<int> clusters;
};

struct Region {
    int leaf = -1;
    Vec3 mn, mx;
};

bool inside(const VisLeaf& l, const Vec3& p) {
    for (const VisHalfSpace& h : l.sides)
        if (h.n.dot(p) > h.d) return false;
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: vis_check <assetsRoot> <map.map.json> [--sections file.json] [--clusters N] [--views V] [--targets P] [--seed S] [--list K] [--cell X Y Z] [--extend N] "
                     "[--segments N] [--samples N] [--aperture M] [--dilate] [--device cpu|compute|rayquery] [--split-min M]\n";
        return 2;
    }
    BspExportRequest req;
    req.assetsRoot = argv[1];
    req.mapPath = argv[2];
    req.outPath = "maps/zz_vis_check.bsp";
    req.bake = false;
    std::string sectionsFile;
    size_t analyse = 48;
    int views = 16, targets = 32, listN = 8;
    unsigned seed = 7;
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : "0"; };
        if (a == "--sections") sectionsFile = next();
        else if (a == "--clusters") analyse = size_t(std::atoi(next()));
        else if (a == "--views") views = std::atoi(next());
        else if (a == "--targets") targets = std::atoi(next());
        else if (a == "--seed") seed = unsigned(std::atoi(next()));
        else if (a == "--list") listN = std::atoi(next());
        else if (a == "--cell") { const double x = std::atof(next()), y = std::atof(next()), z = std::atof(next()); req.settings.visCell = Vec3{x, y, z}; }
        else if (a == "--extend") req.settings.visExtendRounds = std::atoi(next());
        else if (a == "--segments") req.settings.visPairSegments = std::atoi(next());
        else if (a == "--samples") req.settings.visSamplePool = std::atoi(next());
        else if (a == "--aperture") req.settings.visApertureMax = std::atof(next());
        else if (a == "--split-min") req.settings.visSplitMinWidth = std::atof(next());
        else if (a == "--dilate") req.settings.visDilate = true;
        else if (a == "--device") req.settings.visDevice = next();
        else { std::cerr << "unknown argument " << a << "\n"; return 2; }
    }

    Scene scene{req.assetsRoot};
    scene.loadGameProfile();
    scene.openDocument(req.mapPath);
    req.document = scene.documentSyncJson()["doc"];
    const Exporter& exporter = exporterFor(scene.profile().exporter);
    req.format = exporter.id;
    req.outPath = withExporterExtension(req.outPath, exporter);
    const auto t0 = Clock::now();
    const ExportResult res = exporter.run(req, {}, {});
    if (!res.ok || !res.vis) {
        std::cerr << "export failed: " << res.error << "\n";
        return 1;
    }
    std::error_code ec;
    std::vector<uint8_t> bspBytes;
    {
        std::ifstream in(req.assetsRoot / req.outPath, std::ios::binary);
        bspBytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    std::filesystem::remove(req.assetsRoot / req.outPath, ec);
    std::filesystem::remove_all(req.assetsRoot / std::filesystem::path(req.outPath).replace_extension(""), ec);
    const VisResult& vis = *res.vis;
    const VisStats& vs = vis.stats;
    std::printf("export %.1f s; visibility %.2f s (tree %.2f, pairs %.2f, aperture %.2f, extend %.2f) on %s; %d clusters, %zu leafs, cell %.2f x %.2f x %.2f, %zu pairs, %zu extended, %zu bytes\n",
                std::chrono::duration<double>(Clock::now() - t0).count(), vs.seconds, vs.secondsTree, vs.secondsPairs, vs.secondsAperture, vs.secondsExtend, vs.device.c_str(), vis.clusters,
                vis.leafs.size(), vs.cell.x, vs.cell.y, vs.cell.z, vs.pairs, vs.extendedPairs, vs.bytes);
    for (const std::string& w : vis.warnings) std::printf("  warning: %s\n", w.c_str());

    // ---- the geometry that blocks sight, as the table was made from it
    LightingClasses cls;
    cls.castShadows = {"worldspawn"};
    std::vector<LightSurface> occ = scene.lightSurfaces();
    const BspContentRules rules = BspContentRules::fromJson(scene.profile().bsp);
    std::vector<LightSurface> blockers;
    for (LightSurface& s : occ) {
        if (s.classname != "worldspawn" && s.classname != "func_group" && s.classname != "func_detail") continue;
        if (rules.isSky(s.texture) || !rules.blocksSight(s.texture)) continue;
        s.classname = "worldspawn";
        s.relief.reset();
        blockers.push_back(s);
    }
    const OccluderMesh mesh = buildOccluders(blockers, cls);
    const TriangleBvh bvh(mesh);
    const std::shared_ptr<SolidSet> solids = scene.lightSolids();
    auto clear = [&](const Vec3& p, const Vec3& q) {
        const Vec3 d = q - p;
        const double len = d.length();
        if (len < 1e-6) return true;
        const double e = std::min(0.002 / len, 0.25);
        return !bvh.occluded(p + d * e, d * (1 - 2 * e), 1.0);
    };

    // ---- sections
    std::vector<Section> sections;
    if (!sectionsFile.empty()) {
        std::ifstream f(sectionsFile);
        nlohmann::json j = nlohmann::json::parse(f);
        for (const auto& s : j) sections.push_back({s["name"], {s["mn"][0], s["mn"][1], s["mn"][2]}, {s["mx"][0], s["mx"][1], s["mx"][2]}, {}});
    } else {
        sections.push_back({"all", {-1e9, -1e9, -1e9}, {1e9, 1e9, 1e9}, {}});
    }
    std::vector<Region> region(size_t(vis.clusters));
    for (size_t li = 0; li < vis.leafs.size(); ++li) {
        const VisLeaf& l = vis.leafs[li];
        if (l.cluster < 1) continue;
        region[size_t(l.cluster)] = {int(li), l.mn, l.mx};
        const Vec3 c = l.centroid;
        for (Section& s : sections)
            if (c.x >= s.mn.x && c.x <= s.mx.x && c.y >= s.mn.y && c.y <= s.mx.y && c.z >= s.mn.z && c.z <= s.mx.z) { s.clusters.push_back(l.cluster); break; }
    }

    // random points inside a cluster (not in solid)
    auto pointsIn = [&](int cluster, int count, std::mt19937_64& rng) {
        std::vector<Vec3> out;
        const Region& rg = region[size_t(cluster)];
        const VisLeaf& l = vis.leafs[size_t(rg.leaf)];
        std::uniform_real_distribution<double> u(0.0, 1.0);
        for (int tries = 0; tries < count * 40 && int(out.size()) < count; ++tries) {
            const Vec3 p{rg.mn.x + u(rng) * (rg.mx.x - rg.mn.x), rg.mn.y + u(rng) * (rg.mx.y - rg.mn.y), rg.mn.z + u(rng) * (rg.mx.z - rg.mn.z)};
            if (!inside(l, p) || (solids && solids->inside(p))) continue;
            out.push_back(p);
        }
        return out;
    };

    std::printf("%-10s %8s %9s %7s %8s %8s %8s %8s %8s\n", "section", "clusters", "analysed", "row", "cull %", "truth", "falseNeg", "falsePos", "fp %row");
    std::mt19937_64 rng(seed);
    size_t totalFn = 0, totalFp = 0, totalRow = 0, totalTruth = 0;
    struct Example { double dist; Vec3 a, b; int ca, cb; };
    std::vector<Example> fpExamples, fnExamples;
    for (Section& sec : sections) {
        if (sec.clusters.empty()) continue;
        std::vector<int> pick = sec.clusters;
        std::shuffle(pick.begin(), pick.end(), rng);
        if (pick.size() > analyse / std::max<size_t>(1, sections.size())) pick.resize(std::max<size_t>(2, analyse / std::max<size_t>(1, sections.size())));
        // targets for every cluster of the section (fixed per run)
        std::map<int, std::vector<Vec3>> tgt;
        for (int c : sec.clusters) tgt[c] = pointsIn(c, targets, rng);
        size_t sumRow = 0, sumTruth = 0, fn = 0, fp = 0, rowAll = 0;
        for (int a : pick) {
            const std::vector<Vec3> from = pointsIn(a, views, rng);
            if (from.empty()) continue;
            std::vector<int> others;
            for (int c : sec.clusters) if (c != a) others.push_back(c);
            std::vector<char> truth(others.size(), 0), shown(others.size(), 0);
            for (size_t k = 0; k < others.size(); ++k) shown[k] = vis.sees(size_t(a), size_t(others[k])) ? 1 : 0;
            parallelFor(others.size(), 4, [&](size_t i0, size_t i1) {
                for (size_t k = i0; k < i1; ++k) {
                    const std::vector<Vec3>& to = tgt[others[k]];
                    bool seen = false;
                    for (const Vec3& p : from) {
                        for (const Vec3& q : to) if (clear(p, q)) { seen = true; break; }
                        if (seen) break;
                    }
                    truth[k] = seen ? 1 : 0;
                }
            });
            // a pair the table shows and the sampling did not confirm: look harder (8x the viewpoints and the targets)
            std::vector<size_t> doubtful;
            for (size_t k = 0; k < others.size(); ++k) if (shown[k] && !truth[k]) doubtful.push_back(k);
            if (!doubtful.empty()) {
                std::mt19937_64 rng2(rng());
                const std::vector<Vec3> from2 = pointsIn(a, views * 8, rng2);
                std::vector<std::vector<Vec3>> to2(doubtful.size());
                for (size_t d = 0; d < doubtful.size(); ++d) to2[d] = pointsIn(others[doubtful[d]], targets * 8, rng2);
                parallelFor(doubtful.size(), 1, [&](size_t i0, size_t i1) {
                    for (size_t d = i0; d < i1; ++d) {
                        bool seen = false;
                        for (const Vec3& p : from2) {
                            for (const Vec3& q : to2[d]) if (clear(p, q)) { seen = true; break; }
                            if (seen) break;
                        }
                        if (seen) truth[doubtful[d]] = 1;
                    }
                });
            }
            size_t row = 0, tr = 0;
            const Vec3 ca = vis.leafs[size_t(region[size_t(a)].leaf)].centroid;
            for (size_t k = 0; k < others.size(); ++k) {
                row += shown[k];
                tr += truth[k];
                const Vec3 cb = vis.leafs[size_t(region[size_t(others[k])].leaf)].centroid;
                if (truth[k] && !shown[k]) { ++fn; fnExamples.push_back({(cb - ca).length(), ca, cb, a, others[k]}); }
                if (shown[k] && !truth[k]) { ++fp; fpExamples.push_back({(cb - ca).length(), ca, cb, a, others[k]}); }
            }
            sumRow += row;
            sumTruth += tr;
            rowAll += others.size();
        }
        const double n = double(pick.size());
        std::printf("%-10s %8zu %9zu %7.1f %8.1f %8.1f %8zu %8zu %8.1f\n", sec.name.c_str(), sec.clusters.size(), pick.size(), sumRow / n, rowAll ? 100.0 * (1.0 - double(sumRow) / double(rowAll)) : 0.0,
                    sumTruth / n, fn, fp, sumRow ? 100.0 * double(fp) / double(sumRow) : 0.0);
        totalFn += fn;
        totalFp += fp;
        totalRow += sumRow;
        totalTruth += sumTruth;
    }
    // ---- surfaces: what the engine draws from a cluster (the surfaces of the leaves of the clusters its table row shows, read back from the exported file) against what can be
    // seen from inside the cluster (sample points on the triangles of each surface)
    {
        BspView bv;
        std::string err;
        if (!bv.open(bspBytes, &err)) std::printf("\ncannot read the exported bsp: %s\n", err.c_str());
        else {
            const auto leafs = bv.lump<rbsp::Leaf>(rbsp::kLeafs);
            const auto leafSurfs = bv.lump<int32_t>(rbsp::kLeafSurfaces);
            const auto surfs = bv.lump<rbsp::Surface>(rbsp::kSurfaces);
            const auto dverts = bv.lump<rbsp::DrawVert>(rbsp::kDrawVerts);
            const auto idx = bv.lump<int32_t>(rbsp::kDrawIndexes);
            const auto models = bv.lump<rbsp::Model>(rbsp::kModels);
            const auto visb = bv.lump<uint8_t>(rbsp::kVisibility);
            int32_t nClusters = 0, bytesPer = 0;
            if (visb.size() >= 8) { std::memcpy(&nClusters, visb.data(), 4); std::memcpy(&bytesPer, visb.data() + 4, 4); }
            auto bit = [&](int a, int c) { return (visb[8 + size_t(a) * size_t(bytesPer) + size_t(c) / 8] >> (c % 8)) & 1; };

            struct Surf { std::vector<int> clusters; std::vector<Vec3> pts; Vec3 n, c; double area = 0; int section = -1; int index = 0; int tris = 0; };
            std::vector<Surf> list;
            std::vector<int> listOf(surfs.size(), -1);
            const int first = models.empty() ? 0 : models[0].firstSurface, count = models.empty() ? int(surfs.size()) : models[0].numSurfaces;
            std::mt19937_64 rngF(seed + 99);
            std::uniform_real_distribution<double> u(0.0, 1.0);
            for (int s = first; s < first + count; ++s) {
                const rbsp::Surface& sf = surfs[size_t(s)];
                if (sf.numIndexes < 3) continue;
                Surf f;
                f.index = s;
                f.tris = sf.numIndexes / 3;
                f.n = fromQuake(Vec3{sf.normal[0], sf.normal[1], sf.normal[2]}) * double(rbsp::kUnitsPerMeter);
                if (f.n.length() > 1e-9) f.n = f.n * (1.0 / f.n.length());
                std::vector<std::array<Vec3, 3>> tris;
                std::vector<double> cum;
                for (int t = 0; t + 2 < sf.numIndexes; t += 3) {
                    std::array<Vec3, 3> tr;
                    for (int k = 0; k < 3; ++k) {
                        const rbsp::DrawVert& dv = dverts[size_t(sf.firstVert + idx[size_t(sf.firstIndex + t + k)])];
                        tr[size_t(k)] = fromQuake(Vec3{dv.xyz[0], dv.xyz[1], dv.xyz[2]});
                    }
                    f.area += 0.5 * (tr[1] - tr[0]).cross(tr[2] - tr[0]).length();
                    cum.push_back(f.area);
                    tris.push_back(tr);
                    f.c = f.c + (tr[0] + tr[1] + tr[2]) * (1.0 / 3.0);
                }
                if (tris.empty() || f.area < 1e-6) continue;
                f.c = f.c * (1.0 / double(tris.size()));
                const int n = std::clamp(int(f.area / 4.0), 3, 40);   // one sample per 4 m2, spread by area over the triangles
                for (int k = 0; k < n; ++k) {
                    const double target = u(rngF) * f.area;
                    const size_t t = size_t(std::lower_bound(cum.begin(), cum.end(), target) - cum.begin()) % tris.size();
                    double a = u(rngF), b = u(rngF);
                    if (a + b > 1) { a = 1 - a; b = 1 - b; }
                    f.pts.push_back(tris[t][0] + (tris[t][1] - tris[t][0]) * a + (tris[t][2] - tris[t][0]) * b);
                }
                for (size_t si = 0; si < sections.size(); ++si)
                    if (f.c.x >= sections[si].mn.x && f.c.x <= sections[si].mx.x && f.c.z >= sections[si].mn.z && f.c.z <= sections[si].mx.z) { f.section = int(si); break; }
                listOf[size_t(s)] = int(list.size());
                list.push_back(std::move(f));
            }
            for (const rbsp::Leaf& l : leafs) {
                if (l.cluster < 0) continue;
                for (int k = 0; k < l.numLeafSurfaces; ++k) {
                    const int s = leafSurfs[size_t(l.firstLeafSurface + k)];
                    if (s < 0 || size_t(s) >= listOf.size() || listOf[size_t(s)] < 0) continue;
                    std::vector<int>& cl = list[size_t(listOf[size_t(s)])].clusters;
                    if (std::find(cl.begin(), cl.end(), l.cluster) == cl.end()) cl.push_back(l.cluster);
                }
            }
            {   // the surfaces of the outside leaf: how much of them lies inside the cells (a surface with one point outside is drawn from everywhere, whole)
                size_t n0 = 0, nMostly = 0, t0 = 0, tAll = 0;
                double a0 = 0, aInside = 0, aMostly = 0, aAll = 0;
                for (const Surf& f : list) {
                    aAll += f.area;
                    tAll += size_t(f.tris);
                    if (std::find(f.clusters.begin(), f.clusters.end(), 0) == f.clusters.end()) continue;
                    size_t in = 0;
                    for (const Vec3& p : f.pts) if (vis.leafs[size_t(vis.leafAt(p + f.n * 0.02))].cluster > 0) ++in;
                    const double frac = double(in) / double(f.pts.size());
                    ++n0;
                    t0 += size_t(f.tris);
                    a0 += f.area;
                    aInside += f.area * frac;
                    if (frac > 0.5) { ++nMostly; aMostly += f.area; }
                }
                std::printf("\nthe outside leaf (always drawn): %zu of %zu surfaces, %.0f of %.0f m2 (%.1f %%); %.0f m2 of them lie inside the cells (sampled); %zu surfaces (%.0f m2) are mostly inside\n",
                            n0, list.size(), a0, aAll, 100.0 * a0 / aAll, aInside, nMostly, aMostly);
                std::printf("triangles: %zu of %zu (%.1f %%) are in the outside leaf\n", t0, tAll, 100.0 * double(t0) / double(std::max<size_t>(tAll, 1)));
            }
            struct Missing { int a; size_t surf; Vec3 p, q; };
            std::vector<Missing> missingList;
            for (int scope = 0; scope < 2; ++scope) {   // 0: every surface, 1: only those of the cells (the outside leaf, drawn from everywhere by design, left out)
            std::printf("\nsurfaces of the exported file, %s (what the engine draws from a cluster, by area; always = in the outside cluster 0, the table shows it from everywhere):\n%-10s %7s %9s %9s %8s %8s %8s %8s %8s %8s\n",
                        scope ? "the cells only" : "all", "section", "surfaces", "m2/surf", "always %", "drawn %", "drawn n%", "seen %", "excess", "of >50m2", "missing");
            std::mt19937_64 rngA(seed + 5);
            for (size_t si = 0; si < sections.size(); ++si) {
                const Section& sec = sections[si];
                double total = 0, always = 0;
                std::vector<size_t> mine;
                for (size_t i = 0; i < list.size(); ++i)
                    if ((list[i].section == int(si) || sections.size() == 1) && !(scope && std::find(list[i].clusters.begin(), list[i].clusters.end(), 0) != list[i].clusters.end())) {
                        mine.push_back(i);
                        total += list[i].area;
                        if (std::find(list[i].clusters.begin(), list[i].clusters.end(), 0) != list[i].clusters.end()) always += list[i].area;
                    }
                if (mine.empty() || sec.clusters.empty()) continue;
                std::vector<int> pick = sec.clusters;
                std::shuffle(pick.begin(), pick.end(), rngA);
                pick.resize(std::min<size_t>(pick.size(), 12));
                double drawn = 0, seen = 0, excess = 0, missing = 0, excessBig = 0, drawnCount = 0, drawnTris = 0, allTris = 0, seenTris = 0;
                for (int a : pick) {
                    const std::vector<Vec3> from = pointsIn(a, views, rngA);
                    if (from.empty()) continue;
                    std::vector<char> shown(mine.size(), 0), vis2(mine.size(), 0);
                    std::vector<Vec3> hitP(mine.size()), hitQ(mine.size());
                    parallelFor(mine.size(), 16, [&](size_t i0, size_t i1) {
                        for (size_t k = i0; k < i1; ++k) {
                            const Surf& f = list[mine[k]];
                            bool d = false;
                            for (int c : f.clusters) if (c < nClusters && bit(a, c)) { d = true; break; }
                            shown[k] = d;
                            bool sn = false;
                            for (const Vec3& p : from) {
                                for (const Vec3& q : f.pts) {
                                    if (f.n.dot(p - q) <= 0.01) continue;   // behind the surface
                                    if (clear(p, q)) { sn = true; hitP[k] = p; hitQ[k] = q; break; }
                                }
                                if (sn) break;
                            }
                            vis2[k] = sn;
                        }
                    });
                    for (size_t k = 0; k < mine.size(); ++k) {
                        const double ar = list[mine[k]].area;
                        drawn += shown[k] ? ar : 0;
                        drawnCount += shown[k] ? 1 : 0;
                        drawnTris += shown[k] ? list[mine[k]].tris : 0;
                        allTris += list[mine[k]].tris;
                        seenTris += vis2[k] ? list[mine[k]].tris : 0;
                        seen += vis2[k] ? ar : 0;
                        excess += shown[k] && !vis2[k] ? ar : 0;
                        excessBig += shown[k] && !vis2[k] && ar > 50.0 ? ar : 0;
                        missing += !shown[k] && vis2[k] ? ar : 0;
                        if (!shown[k] && vis2[k] && scope == 0) missingList.push_back({a, mine[k], hitP[k], hitQ[k]});
                    }
                }
                const double n = double(pick.size()) * total;
                std::printf("%-10s %7zu %9.1f %9.1f %8.1f %8.1f %8.1f %8.1f %8.1f %8.2f\n", sec.name.c_str(), mine.size(), total / double(mine.size()), 100.0 * always / total, 100.0 * drawn / n,
                            100.0 * drawnCount / (double(pick.size()) * double(mine.size())), 100.0 * seen / n, 100.0 * excess / n, 100.0 * excessBig / n, 100.0 * missing / n);
                std::printf("           triangles drawn from a cluster: %.0f of %.0f (%.1f %%), really seen (sampled): %.0f\n", drawnTris / double(std::max<size_t>(pick.size(), 1)), allTris / double(std::max<size_t>(pick.size(), 1)), 100.0 * drawnTris / std::max(allTris, 1.0), seenTris / double(std::max<size_t>(pick.size(), 1)));
            }
            }
            std::sort(missingList.begin(), missingList.end(), [&](const Missing& x, const Missing& y) { return list[x.surf].area > list[y.surf].area; });
            for (int i = 0; i < listN && i < int(missingList.size()); ++i) {
                const Missing& m = missingList[size_t(i)];
                const Surf& f = list[m.surf];
                const Vec3 qq = m.q + f.n * 0.02;
                const int cq = vis.leafs[size_t(vis.leafAt(qq))].cluster, cp = vis.leafs[size_t(vis.leafAt(m.p))].cluster;
                const bool inList = std::find(f.clusters.begin(), f.clusters.end(), cq) != f.clusters.end();
                std::printf("  MISSING SURFACE %d from cluster %d (viewpoint in %d): area %.1f n (%.2f %.2f %.2f) c (%.1f %.1f %.1f), in %zu clusters; seen at (%.2f %.2f %.2f) in cluster %d (listed there: %d, table sees it: %d)\n",
                            f.index, m.a, cp, f.area, f.n.x, f.n.y, f.n.z, f.c.x, f.c.y, f.c.z, f.clusters.size(), m.q.x, m.q.y, m.q.z, cq, int(inList), cq >= 0 ? int(vis.sees(size_t(m.a), size_t(cq))) : -1);
            }
        }
    }
    std::printf("total: false negatives %zu, false positives %zu of %zu shown (%.1f %%), truth %zu\n", totalFn, totalFp, totalRow, totalRow ? 100.0 * double(totalFp) / double(totalRow) : 0.0, totalTruth);
    std::sort(fnExamples.begin(), fnExamples.end(), [](const Example& a, const Example& b) { return a.dist > b.dist; });
    std::sort(fpExamples.begin(), fpExamples.end(), [](const Example& a, const Example& b) { return a.dist > b.dist; });
    for (int i = 0; i < listN && i < int(fnExamples.size()); ++i) {
        const Example& e = fnExamples[size_t(i)];
        std::printf("  FALSE NEGATIVE %d -> %d  (%.1f %.1f %.1f) -> (%.1f %.1f %.1f)\n", e.ca, e.cb, e.a.x, e.a.y, e.a.z, e.b.x, e.b.y, e.b.z);
    }
    for (int i = 0; i < listN && i < int(fpExamples.size()); ++i) {
        const Example& e = fpExamples[size_t(i)];
        std::printf("  false positive %d -> %d  dist %.1f  (%.1f %.1f %.1f) -> (%.1f %.1f %.1f)\n", e.ca, e.cb, e.dist, e.a.x, e.a.y, e.a.z, e.b.x, e.b.y, e.b.z);
    }
    return totalFn == 0 ? 0 : 3;
}
