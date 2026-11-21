// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0
//
// Task tests for nanovdb/tools/FogAdjoint: primal (catalog + forward),
// adjoint (analytic VJP vs finite differences) and snapshot (record
// ownership) groups. Registered through task_cases/task_case_count; the
// driver provides main().

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <stdexcept>
#include <vector>

#include <nanovdb/NanoVDB.h>
#include <nanovdb/GridHandle.h>
#include <nanovdb/HostBuffer.h>
#include <nanovdb/math/Ray.h>
#include <nanovdb/tools/GridBuilder.h>
#include <nanovdb/tools/CreateNanoGrid.h>
#include <nanovdb/tools/CreatePrimitives.h>
#include <nanovdb/tools/FogAdjoint.h>

#include "task_support/check.h"

namespace fog = nanovdb::tools::fog;
using nanovdb::Coord;
using nanovdb::Vec3d;

void runNanoVDB(nanovdb::GridHandle<nanovdb::HostBuffer>&, int, int, int, nanovdb::HostBuffer&);

namespace {

// --------------------------------------------------------------------------
// scene construction on top of nanovdb::tools::build::Grid
// --------------------------------------------------------------------------

using BuildGrid = nanovdb::tools::build::Grid<float>;
using Node1 = BuildGrid::Node1;
using Node2 = BuildGrid::Node2;
using BuildRoot = BuildGrid::RootNodeType;
using Handle = nanovdb::GridHandle<nanovdb::HostBuffer>;

struct Region {
    int   level; // 0 = voxel, 1/2/3 = constant tiles of edge 8/128/4096
    Coord origin;
    float value;
};

struct Scene {
    std::unique_ptr<BuildGrid> grid;
    std::vector<Region> regions; // active regions in insertion order
};

int edgeOf(int level) { return 1 << (level == 0 ? 0 : level == 1 ? 3 : level == 2 ? 7 : 12); }

std::unique_ptr<Scene> makeScene(double scale, const Vec3d& translate)
{
    auto s = std::make_unique<Scene>();
    s->grid.reset(new BuildGrid(0.0f, "scene", nanovdb::GridClass::FogVolume));
    s->grid->setTransform(scale, translate);
    return s;
}

void addVoxel(Scene& s, const Coord& ijk, float value, bool active = true)
{
    auto acc = s.grid->getAccessor();
    auto* leaf = acc.setValue(ijk, value); // always sets the active bit
    if (active) {
        s.regions.push_back(Region{0, ijk, value});
    } else { // keep the stored value but clear the state bit
        leaf->mValueMask.setOff(BuildGrid::Node0::CoordToOffset(ijk));
    }
}

Node2* ensureUpper(BuildGrid& g, const Coord& ijk)
{
    auto& root = g.mRoot;
    const Coord key = BuildRoot::CoordToKey(ijk);
    auto it = root.mTable.find(key);
    if (it == root.mTable.end())
        it = root.mTable.emplace(key, BuildRoot::Tile(new Node2(ijk, 0.0f, false))).first;
    return it->second.child;
}

void addTile(Scene& s, int level, const Coord& origin, float value, bool active = true)
{
    if (level == 3) {
        const Coord key = BuildRoot::CoordToKey(origin);
        s.grid->mRoot.mTable[key] = BuildRoot::Tile(value, active);
        if (active) s.regions.push_back(Region{3, key, value});
        return;
    }
    Node2* upper = ensureUpper(*s.grid, origin);
    if (level == 2) {
        const uint32_t n = Node2::CoordToOffset(origin);
        upper->mTable[n].value = value;
        if (active) {
            upper->mValueMask.setOn(n);
            s.regions.push_back(Region{2, origin & ~uint32_t(127), value});
        }
        return;
    }
    const uint32_t n2 = Node2::CoordToOffset(origin);
    Node1* lower = nullptr;
    if (upper->mChildMask.isOn(n2)) {
        lower = upper->mTable[n2].child;
    } else {
        lower = new Node1(origin, 0.0f, false);
        upper->mTable[n2].child = lower;
        upper->mChildMask.setOn(n2);
    }
    const uint32_t n1 = Node1::CoordToOffset(origin);
    lower->mTable[n1].value = value;
    if (active) {
        lower->mValueMask.setOn(n1);
        s.regions.push_back(Region{1, origin & ~uint32_t(7), value});
    }
}

Handle finalize(Scene& s)
{
    nanovdb::tools::CreateNanoGrid<BuildGrid> converter(*s.grid);
    return converter.getHandle<float>();
}

std::vector<Region> sortedRegions(const Scene& s)
{
    auto r = s.regions;
    std::sort(r.begin(), r.end(), [](const Region& a, const Region& b) {
        if (a.level != b.level) return a.level < b.level;
        for (int i = 0; i < 3; ++i)
            if (a.origin[i] != b.origin[i]) return a.origin[i] < b.origin[i];
        return false;
    });
    return r;
}

// --------------------------------------------------------------------------
// independent dense trilinear midpoint integrator (no empty-space skipping)
// --------------------------------------------------------------------------

int refResolve(const std::vector<Region>& sorted, int32_t x, int32_t y, int32_t z)
{
    for (size_t i = 0; i < sorted.size(); ++i) {
        const Region& r = sorted[i];
        const int32_t e = edgeOf(r.level);
        if (x >= r.origin[0] && x < r.origin[0] + e &&
            y >= r.origin[1] && y < r.origin[1] + e &&
            z >= r.origin[2] && z < r.origin[2] + e)
            return static_cast<int>(i);
    }
    return -1;
}

void refForward(const std::vector<Region>& sorted, const std::vector<double>& density,
                double k, const Vec3d& e, const fog::Ray& ray, double h,
                double scale, const Vec3d& tr, Vec3d& rgbOut, double& tOut)
{
    const double lenD = ray.direction.length();
    const double D = (ray.t1 - ray.t0) * lenD;
    uint64_t n = static_cast<uint64_t>(std::floor(D / h));
    if (n * h < D) ++n;
    if (n == 0) n = 1;
    const double lastL = D - static_cast<double>(n - 1) * h;
    double C[3] = {0.0, 0.0, 0.0};
    double T = 1.0;
    for (uint64_t i = 0; i < n; ++i) {
        const bool last = (i == n - 1);
        const double m = last ? (static_cast<double>(n - 1) * h + 0.5 * lastL)
                              : ((static_cast<double>(i) + 0.5) * h);
        const double t = ray.t0 + m / lenD;
        const double px = ray.origin[0] + t * ray.direction[0];
        const double py = ray.origin[1] + t * ray.direction[1];
        const double pz = ray.origin[2] + t * ray.direction[2];
        const double ix = (px - tr[0]) / scale;
        const double iy = (py - tr[1]) / scale;
        const double iz = (pz - tr[2]) / scale;
        const int32_t bx = static_cast<int32_t>(std::floor(ix));
        const int32_t by = static_cast<int32_t>(std::floor(iy));
        const int32_t bz = static_cast<int32_t>(std::floor(iz));
        const double fx = ix - std::floor(ix);
        const double fy = iy - std::floor(iy);
        const double fz = iz - std::floor(iz);
        double rho = 0.0;
        for (int c = 0; c < 8; ++c) {
            const int dx = c & 1, dy = (c >> 1) & 1, dz = (c >> 2) & 1;
            const double w = (dx ? fx : 1.0 - fx) * (dy ? fy : 1.0 - fy) * (dz ? fz : 1.0 - fz);
            const int p = refResolve(sorted, bx + dx, by + dy, bz + dz);
            if (p >= 0) rho += w * density[static_cast<size_t>(p)];
        }
        const double L = last ? lastL : h;
        const double x = rho * k * L;
        const double a = std::exp(-x);
        const double f = (rho * k != 0.0) ? (-std::expm1(-x)) / (rho * k) : L;
        const double s = rho * f;
        C[0] += T * s * e[0];
        C[1] += T * s * e[1];
        C[2] += T * s * e[2];
        T *= a;
    }
    rgbOut = Vec3d(C[0] + T * ray.background[0], C[1] + T * ray.background[1], C[2] + T * ray.background[2]);
    tOut = T;
}

// --------------------------------------------------------------------------
// shared bits
// --------------------------------------------------------------------------

bool close(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

// the mixed topology used by several cases
std::unique_ptr<Scene> makeMixedScene(double scale, const Vec3d& translate)
{
    auto s = makeScene(scale, translate);
    addVoxel(*s, Coord(-2, 0, 0), 0.8f);
    addVoxel(*s, Coord(-1, 0, 0), 0.0f); // active zero keeps its parameter
    addVoxel(*s, Coord(0, 0, 0), 1.2f);
    addVoxel(*s, Coord(6, 0, 0), 0.5f);
    addVoxel(*s, Coord(7, 0, 0), 0.9f);
    addVoxel(*s, Coord(8, 0, 0), 0.3f); // across the leaf boundary
    addVoxel(*s, Coord(9, 0, 0), 1.1f);
    addVoxel(*s, Coord(3, 3, 3), 2.0f);
    addVoxel(*s, Coord(4, 0, 0), 7.0f, false); // inactive non-zero -> no parameter, reads as 0
    addTile(*s, 1, Coord(16, 0, 0), 0.6f);
    addTile(*s, 1, Coord(-16, -8, -8), 0.9f);
    addTile(*s, 1, Coord(32, 0, 0), 5.0f, false); // inactive non-zero tile
    addTile(*s, 2, Coord(128, 0, 0), 0.2f);
    addTile(*s, 3, Coord(4096, -4096, -4096), 0.5f);
    return s;
}

double evalLoss(const fog::Catalog& cat, const std::vector<double>& density, double k,
                const Vec3d& e, const std::vector<fog::Ray>& rays, double h,
                const std::vector<Vec3d>& g, const std::vector<double>& q)
{
    auto res = fog::forward(cat, density, k, e, rays, h);
    double v = 0.0;
    for (size_t i = 0; i < rays.size(); ++i)
        v += g[i].dot(res.rgb[i]) + q[i] * res.transmittance[i];
    return v;
}

// finite-difference check of one scalar gradient component; prints steps and errors
bool checkGrad(const char* label, double analytic, const std::function<double(double)>& eval,
               double s1, double s2, bool central)
{
    double fd1, fd2;
    if (central) {
        fd1 = (eval(s1) - eval(-s1)) / (2.0 * s1);
        fd2 = (eval(s2) - eval(-s2)) / (2.0 * s2);
    } else {
        const double f0 = eval(0.0);
        fd1 = (eval(s1) - f0) / s1;
        fd2 = (eval(s2) - f0) / s2;
    }
    const double e1 = std::fabs(analytic - fd1);
    const double e2 = std::fabs(analytic - fd2);
    const double tol = 2e-5 + 2e-5 * std::fabs(analytic);
    std::printf("  %-24s analytic=% .9g fd1=% .9g fd2=% .9g steps=(%.1e,%.1e) err=(%.3g,%.3g) tol=%.3g\n",
                label, analytic, fd1, fd2, s1, s2, e1, e2, tol);
    return e1 <= tol && e2 <= tol;
}

// --------------------------------------------------------------------------
// primal group
// --------------------------------------------------------------------------

int case_catalog_params()
{
    auto s = makeScene(2.5, Vec3d(10.0, -4.0, 0.5));
    addVoxel(*s, Coord(0, 0, 0), 1.5f);
    addVoxel(*s, Coord(-3, 2, 7), 0.0f); // active zero keeps a parameter
    addVoxel(*s, Coord(8, 0, 0), 2.0f);
    addVoxel(*s, Coord(1, 1, 1), 9.0f, false); // inactive non-zero: no parameter
    addTile(*s, 1, Coord(-16, 8, 0), 0.7f);
    addTile(*s, 2, Coord(128, -256, 0), 0.3f);
    addTile(*s, 3, Coord(-4096, 0, 8192), 1.1f);
    addTile(*s, 1, Coord(16, 16, 16), 5.0f, false); // inactive tile: no parameter
    auto handle = finalize(*s);
    const auto* grid = handle.grid<float>();
    TASK_CHECK(grid != nullptr);

    fog::Catalog cat(*grid);
    TASK_CHECK(cat.paramCount() == 6);
    const int levels[6] = {0, 0, 0, 1, 2, 3};
    const int32_t origins[6][3] = {{-3, 2, 7}, {0, 0, 0}, {8, 0, 0}, {-16, 8, 0}, {128, -256, 0}, {-4096, 0, 8192}};
    const double values[6] = {0.0, 1.5, 2.0, 0.7, 0.3, 1.1};
    const auto& init = cat.initialDensity();
    TASK_CHECK(init.size() == 6);
    for (uint64_t i = 0; i < 6; ++i) {
        TASK_CHECK(cat.paramLevel(i) == levels[i]);
        for (int a = 0; a < 3; ++a)
            TASK_CHECK(cat.paramOrigin(i)[a] == origins[i][a]);
        TASK_CHECK(close(init[i], values[i], 1e-6));
    }
    TASK_CHECK(close(cat.mapScale(), 2.5, 1e-12));
    TASK_CHECK(close(cat.mapTranslate()[0], 10.0, 1e-12) && close(cat.mapTranslate()[1], -4.0, 1e-12));

    // non-uniform / negative / rotated maps must be rejected
    auto tryMap = [&](std::function<void(nanovdb::Map&)> edit) {
        auto s2 = makeScene(1.0, Vec3d(0.0));
        addVoxel(*s2, Coord(0, 0, 0), 1.0f);
        edit(s2->grid->mMap);
        auto h2 = finalize(*s2);
        bool thrown = false;
        try {
            fog::Catalog c2(*h2.grid<float>());
        } catch (const std::invalid_argument&) {
            thrown = true;
        }
        return thrown;
    };
    TASK_CHECK(tryMap([](nanovdb::Map& m) { m.mMatD[1] = 0.25; })); // off-diagonal
    TASK_CHECK(tryMap([](nanovdb::Map& m) { m.mMatD[4] = 2.0; }));   // non-uniform
    TASK_CHECK(tryMap([](nanovdb::Map& m) { m.mMatD[0] = m.mMatD[4] = m.mMatD[8] = -1.0; })); // negative
    TASK_CHECK(tryMap([](nanovdb::Map& m) { m.mMatD[0] = m.mMatD[4] = m.mMatD[8] = 0.0; }));  // zero
    return 0;
}

int case_forward_mixed()
{
    auto s = makeMixedScene(1.0, Vec3d(0.0));
    auto sorted = sortedRegions(*s);
    auto handle = finalize(*s);
    fog::Catalog cat(*handle.grid<float>());
    TASK_CHECK(cat.paramCount() == sorted.size());
    const auto& density = cat.initialDensity();
    for (size_t i = 0; i < sorted.size(); ++i)
        TASK_CHECK(close(density[i], sorted[i].value, 1e-6));

    std::vector<fog::Ray> rays(5);
    rays[0].origin = Vec3d(-4.0, 0.5, 0.5);
    rays[0].direction = Vec3d(1.0, 0.0, 0.0);
    rays[0].t0 = 0.0;
    rays[0].t1 = 140.3; // remainder last segment
    rays[0].background = Vec3d(0.1, 0.2, 0.3);
    rays[1] = rays[0]; // same world path, non-unit direction
    rays[1].direction = Vec3d(2.0, 0.0, 0.0);
    rays[1].t1 = 70.15;
    rays[2].origin = Vec3d(-4.0, -4.0, -4.0);
    rays[2].direction = Vec3d(1.0, 1.0, 1.0);
    rays[2].t0 = 0.0;
    rays[2].t1 = 10.0;
    rays[2].background = Vec3d(0.0, 0.5, 0.25);
    rays[3].origin = Vec3d(30.0, 0.5, 0.5);
    rays[3].direction = Vec3d(-1.0, 0.0, 0.0);
    rays[3].t0 = 0.0;
    rays[3].t1 = 50.0;
    rays[3].background = Vec3d(0.6, 0.1, 0.1);
    rays[4].origin = Vec3d(0.0, 100.0, 0.0); // misses everything
    rays[4].direction = Vec3d(0.0, 1.0, 0.0);
    rays[4].t0 = 0.0;
    rays[4].t1 = 10.0;
    rays[4].background = Vec3d(0.2, 0.4, 0.6);

    const double k = 0.35;
    const Vec3d e(0.2, 0.5, 0.9);
    const double h = 0.5;
    auto res = fog::forward(cat, density, k, e, rays, h);
    TASK_CHECK(res.rgb.size() == rays.size() && res.transmittance.size() == rays.size());
    for (size_t i = 0; i < rays.size(); ++i) {
        Vec3d rgb;
        double T;
        refForward(sorted, density, k, e, rays[i], h, 1.0, Vec3d(0.0), rgb, T);
        for (int c = 0; c < 3; ++c)
            TASK_CHECK(close(res.rgb[i][c], rgb[c], 1e-9));
        TASK_CHECK(close(res.transmittance[i], T, 1e-9));
    }
    // same world path with a non-unit direction must give the same answer
    for (int c = 0; c < 3; ++c)
        TASK_CHECK(close(res.rgb[0][c], res.rgb[1][c], 1e-9));
    TASK_CHECK(close(res.transmittance[0], res.transmittance[1], 1e-9));
    // the missing ray sees only its background
    TASK_CHECK(close(res.rgb[4][0], 0.2, 1e-12) && close(res.transmittance[4], 1.0, 1e-12));
    TASK_CHECK(res.sampleEvaluations > 0 && res.hierarchyVisits > 0);
    return 0;
}

int case_forward_scaled()
{
    const double scale = 0.5;
    const Vec3d tr(1.0, 2.0, 3.0);
    auto s = makeMixedScene(scale, tr);
    auto sorted = sortedRegions(*s);
    auto handle = finalize(*s);
    fog::Catalog cat(*handle.grid<float>());
    const auto& density = cat.initialDensity();

    std::vector<fog::Ray> rays(3);
    rays[0].origin = Vec3d(0.0, 2.25, 3.25);
    rays[0].direction = Vec3d(1.0, 0.0, 0.0);
    rays[0].t0 = 0.0;
    rays[0].t1 = 70.0;
    rays[0].background = Vec3d(0.3, 0.1, 0.0);
    rays[1].origin = Vec3d(-1.0, 0.0, 1.0);
    rays[1].direction = Vec3d(0.5, 0.5, 0.5);
    rays[1].t0 = 0.0;
    rays[1].t1 = 20.0;
    rays[1].background = Vec3d(0.0, 0.2, 0.4);
    rays[2].origin = Vec3d(16.0, 2.25, 3.25);
    rays[2].direction = Vec3d(-1.0, 0.0, 0.0);
    rays[2].t0 = 0.0;
    rays[2].t1 = 25.0;
    rays[2].background = Vec3d(0.5, 0.5, 0.1);

    const double k = 0.45;
    const Vec3d e(0.7, 0.1, 0.3);
    const double h = 0.25;
    auto res = fog::forward(cat, density, k, e, rays, h);
    for (size_t i = 0; i < rays.size(); ++i) {
        Vec3d rgb;
        double T;
        refForward(sorted, density, k, e, rays[i], h, scale, tr, rgb, T);
        for (int c = 0; c < 3; ++c)
            TASK_CHECK(close(res.rgb[i][c], rgb[c], 1e-9));
        TASK_CHECK(close(res.transmittance[i], T, 1e-9));
    }
    return 0;
}

int case_validation()
{
    auto s = makeMixedScene(1.0, Vec3d(0.0));
    auto handle = finalize(*s);
    fog::Catalog cat(*handle.grid<float>());
    const auto density = cat.initialDensity();
    std::vector<fog::Ray> rays(1);
    rays[0].origin = Vec3d(0.0, 0.5, 0.5);
    rays[0].direction = Vec3d(1.0, 0.0, 0.0);
    rays[0].t0 = 0.0;
    rays[0].t1 = 10.0;
    const Vec3d e(0.1, 0.2, 0.3);

    auto throwsInvalid = [&](std::function<void()> fn) {
        try {
            fn();
        } catch (const std::invalid_argument&) {
            return true;
        } catch (...) {
            return false;
        }
        return false;
    };
    TASK_CHECK(throwsInvalid([&] { fog::forward(cat, std::vector<double>(density.size() + 1, 0.0), 0.1, e, rays, 0.5); }));
    TASK_CHECK(throwsInvalid([&] { auto d = density; d[0] = -0.5; fog::forward(cat, d, 0.1, e, rays, 0.5); }));
    TASK_CHECK(throwsInvalid([&] { auto d = density; d[1] = NAN; fog::forward(cat, d, 0.1, e, rays, 0.5); }));
    TASK_CHECK(throwsInvalid([&] { fog::forward(cat, density, -0.1, e, rays, 0.5); }));
    TASK_CHECK(throwsInvalid([&] { fog::forward(cat, density, NAN, e, rays, 0.5); }));
    TASK_CHECK(throwsInvalid([&] { fog::forward(cat, density, 0.1, Vec3d(-0.1, 0.0, 0.0), rays, 0.5); }));
    TASK_CHECK(throwsInvalid([&] { fog::forward(cat, density, 0.1, e, rays, 0.0); }));
    TASK_CHECK(throwsInvalid([&] { fog::forward(cat, density, 0.1, e, rays, -1.0); }));
    TASK_CHECK(throwsInvalid([&] { fog::forward(cat, density, 0.1, e, rays, NAN); }));
    TASK_CHECK(throwsInvalid([&] { auto r = rays; r[0].direction = Vec3d(0.0); fog::forward(cat, density, 0.1, e, r, 0.5); }));
    TASK_CHECK(throwsInvalid([&] { auto r = rays; r[0].t1 = r[0].t0; fog::forward(cat, density, 0.1, e, r, 0.5); }));
    TASK_CHECK(throwsInvalid([&] { auto r = rays; r[0].t1 = r[0].t0 - 1.0; fog::forward(cat, density, 0.1, e, r, 0.5); }));
    TASK_CHECK(throwsInvalid([&] { auto r = rays; r[0].origin[1] = INFINITY; fog::forward(cat, density, 0.1, e, r, 0.5); }));
    TASK_CHECK(throwsInvalid([&] { auto r = rays; r[0].background[2] = NAN; fog::forward(cat, density, 0.1, e, r, 0.5); }));
    TASK_CHECK(!throwsInvalid([&] { fog::forward(cat, density, 0.1, e, rays, 0.5); }));
    TASK_CHECK(!throwsInvalid([&] { fog::forward(cat, density, 0.0, Vec3d(0.0), rays, 0.5); }));
    return 0;
}

int case_gap_counts()
{
    auto s = makeScene(1.0, Vec3d(0.0));
    addVoxel(*s, Coord(0, 0, 0), 1.0f);
    addVoxel(*s, Coord(1048576, 0, 0), 2.0f); // 2^20 away along x
    auto sorted = sortedRegions(*s);
    auto handle = finalize(*s);
    fog::Catalog cat(*handle.grid<float>());
    const auto& density = cat.initialDensity();
    TASK_CHECK(cat.paramCount() == 2);

    std::vector<fog::Ray> rays(1);
    rays[0].origin = Vec3d(-2.0, 0.37, 0.41);
    rays[0].direction = Vec3d(1.0, 0.0, 0.0);
    rays[0].t0 = 0.0;
    rays[0].t1 = 1048580.0; // two units past both voxels
    rays[0].background = Vec3d(0.7, 0.5, 0.3);
    const double k = 0.1;
    const Vec3d e(0.3, 0.2, 0.1);
    const double h = 0.25;
    auto res = fog::forward(cat, density, k, e, rays, h);

    std::printf("  gap: sampleEvaluations=%llu hierarchyVisits=%llu\n",
                (unsigned long long)res.sampleEvaluations, (unsigned long long)res.hierarchyVisits);
    TASK_CHECK(res.sampleEvaluations <= 128);
    TASK_CHECK(res.hierarchyVisits <= 2048);
    TASK_CHECK(res.sampleEvaluations > 0 && res.hierarchyVisits > 0);

    // dense reference marches every one of the ~4.2M samples from t0: matching
    // it proves the skipping kept the original sampling phase and values
    Vec3d rgb;
    double T;
    refForward(sorted, density, k, e, rays[0], h, 1.0, Vec3d(0.0), rgb, T);
    for (int c = 0; c < 3; ++c)
        TASK_CHECK(close(res.rgb[0][c], rgb[c], 1e-9));
    TASK_CHECK(close(res.transmittance[0], T, 1e-9));
    return 0;
}

// --------------------------------------------------------------------------
// adjoint group
// --------------------------------------------------------------------------

int case_vjp_central()
{
    auto s = makeMixedScene(1.0, Vec3d(0.0));
    auto handle = finalize(*s);
    fog::Catalog cat(*handle.grid<float>());
    const auto density = cat.initialDensity();
    const uint64_t P = cat.paramCount();

    std::vector<fog::Ray> rays(3);
    rays[0].origin = Vec3d(-4.0, 0.5, 0.5);
    rays[0].direction = Vec3d(1.0, 0.0, 0.0);
    rays[0].t0 = 0.0;
    rays[0].t1 = 140.3;
    rays[0].background = Vec3d(0.1, 0.2, 0.3);
    rays[1].origin = Vec3d(-4.0, -4.0, -4.0);
    rays[1].direction = Vec3d(1.0, 1.0, 1.0);
    rays[1].t0 = 0.0;
    rays[1].t1 = 10.0;
    rays[1].background = Vec3d(0.0, 0.5, 0.25);
    rays[2].origin = Vec3d(30.0, 0.5, 0.5);
    rays[2].direction = Vec3d(-1.0, 0.0, 0.0);
    rays[2].t0 = 0.0;
    rays[2].t1 = 50.0;
    rays[2].background = Vec3d(0.6, 0.1, 0.1);

    const double k = 0.35;
    const Vec3d e(0.2, 0.5, 0.9);
    const double h = 0.5;
    const std::vector<Vec3d> g = {Vec3d(0.7, -0.4, 0.25), Vec3d(-0.2, 0.9, 0.5), Vec3d(0.1, 0.3, -0.8)};
    const std::vector<double> q = {0.3, -0.6, 0.45};

    fog::Record rec;
    fog::forward(cat, density, k, e, rays, h, &rec);
    TASK_CHECK(rec.paramCount() == P && rec.rayCount() == rays.size() && rec.segmentCount() > 0);
    auto grad = fog::vjp(rec, g, q);
    TASK_CHECK(grad.dDensity.size() == P);

    auto lossOf = [&](std::vector<double> d, double kk, Vec3d ee) {
        return evalLoss(cat, d, kk, ee, rays, h, g, q);
    };
    bool ok = true;
    for (uint64_t p = 0; p < P; ++p) {
        const bool positive = density[p] > 4e-4;
        auto eval = [&](double delta) {
            auto d = density;
            d[p] += delta;
            return lossOf(d, k, e);
        };
        char label[64];
        std::snprintf(label, sizeof(label), "density[%llu]", (unsigned long long)p);
        ok &= checkGrad(label, grad.dDensity[p], eval, 1e-4, 2e-4, positive);
    }
    ok &= checkGrad("extinction", grad.dExtinction,
                    [&](double delta) { return lossOf(density, k + delta, e); }, 1e-4, 2e-4, true);
    for (int c = 0; c < 3; ++c) {
        char label[64];
        std::snprintf(label, sizeof(label), "emission[%d]", c);
        ok &= checkGrad(label, grad.dEmission[c],
                        [&](double delta) {
                            auto ee = e;
                            ee[c] += delta;
                            return lossOf(density, k, ee);
                        },
                        1e-4, 2e-4, true);
    }
    TASK_CHECK(ok);
    return 0;
}

int case_vjp_zero_limits()
{
    auto s = makeScene(1.0, Vec3d(0.0));
    addVoxel(*s, Coord(0, 0, 0), 0.0f); // zero density in the front segment
    addVoxel(*s, Coord(4, 0, 0), 1.0f);
    addVoxel(*s, Coord(8, 0, 0), 0.5f);
    addTile(*s, 1, Coord(16, 0, 0), 0.8f);
    auto handle = finalize(*s);
    fog::Catalog cat(*handle.grid<float>());
    const auto density = cat.initialDensity();
    const uint64_t P = cat.paramCount();
    TASK_CHECK(P == 4);

    std::vector<fog::Ray> rays(1);
    rays[0].origin = Vec3d(-2.0, 0.5, 0.5);
    rays[0].direction = Vec3d(1.0, 0.0, 0.0);
    rays[0].t0 = 0.0;
    rays[0].t1 = 30.0;
    rays[0].background = Vec3d(0.1, 0.2, 0.3);
    const Vec3d e(0.6, 0.4, 0.2);
    const double h = 0.5;
    const std::vector<Vec3d> g = {Vec3d(0.5, -0.3, 0.8)};
    const std::vector<double> q = {0.4};

    bool ok = true;
    for (int variant = 0; variant < 2; ++variant) {
        const double k = variant == 0 ? 0.4 : 0.0;
        fog::Record rec;
        fog::forward(cat, density, k, e, rays, h, &rec);
        auto grad = fog::vjp(rec, g, q);
        // the zero-density front parameter must see a non-zero gradient
        TASK_CHECK(std::fabs(grad.dDensity[0]) > 1e-6);
        auto lossOf = [&](std::vector<double> d, double kk, Vec3d ee) {
            return evalLoss(cat, d, kk, ee, rays, h, g, q);
        };
        for (uint64_t p = 0; p < P; ++p) {
            const bool positive = density[p] > 4e-4;
            auto eval = [&](double delta) {
                auto d = density;
                d[p] += delta;
                return lossOf(d, k, e);
            };
            char label[64];
            std::snprintf(label, sizeof(label), "k=%.1f density[%llu]", k, (unsigned long long)p);
            ok &= checkGrad(label, grad.dDensity[p], eval, 1e-6, 2e-6, positive);
        }
        // k = 0 can only be probed from the right
        ok &= checkGrad("extinction", grad.dExtinction,
                        [&](double delta) { return lossOf(density, k + delta, e); },
                        1e-6, 2e-6, variant == 0);
        for (int c = 0; c < 3; ++c) {
            char label[64];
            std::snprintf(label, sizeof(label), "k=%.1f emission[%d]", k, c);
            ok &= checkGrad(label, grad.dEmission[c],
                            [&](double delta) {
                                auto ee = e;
                                ee[c] += delta;
                                return lossOf(density, k, ee);
                            },
                            1e-6, 2e-6, true);
        }
    }
    TASK_CHECK(ok);
    return 0;
}

int case_vjp_shared_tile()
{
    auto s = makeScene(1.0, Vec3d(0.0));
    addTile(*s, 1, Coord(0, 0, 0), 0.9f); // one tile referenced by many corners/samples/rays
    auto handle = finalize(*s);
    fog::Catalog cat(*handle.grid<float>());
    const auto density = cat.initialDensity();
    TASK_CHECK(cat.paramCount() == 1);

    std::vector<fog::Ray> rays(3);
    rays[0].origin = Vec3d(-2.0, 0.5, 0.5);
    rays[0].direction = Vec3d(1.0, 0.0, 0.0);
    rays[0].t0 = 0.0;
    rays[0].t1 = 12.0;
    rays[0].background = Vec3d(0.2, 0.1, 0.0);
    rays[1].origin = Vec3d(0.5, -2.0, 3.5);
    rays[1].direction = Vec3d(0.0, 1.0, 0.0);
    rays[1].t0 = 0.0;
    rays[1].t1 = 12.0;
    rays[1].background = Vec3d(0.0, 0.3, 0.1);
    rays[2].origin = Vec3d(-2.0, -2.0, -2.0);
    rays[2].direction = Vec3d(1.0, 1.0, 1.0);
    rays[2].t0 = 0.0;
    rays[2].t1 = 6.0;
    rays[2].background = Vec3d(0.1, 0.0, 0.4);

    const double k = 0.3;
    const Vec3d e(0.5, 0.25, 0.75);
    const std::vector<Vec3d> g = {Vec3d(0.6, 0.2, -0.1), Vec3d(-0.3, 0.4, 0.9), Vec3d(0.8, -0.5, 0.3)};
    const std::vector<double> q = {0.2, 0.7, -0.4};

    fog::Record rec;
    fog::forward(cat, density, k, e, rays, 0.5, &rec);
    auto grad = fog::vjp(rec, g, q);

    // multi-ray reduction: the batched gradient equals the sum of per-ray VJPs
    double perRaySum = 0.0;
    for (size_t i = 0; i < rays.size(); ++i) {
        fog::Record ri;
        fog::forward(cat, density, k, e, std::vector<fog::Ray>{rays[i]}, 0.5, &ri);
        auto gi = fog::vjp(ri, {g[i]}, {q[i]});
        perRaySum += gi.dDensity[0];
    }
    TASK_CHECK(close(grad.dDensity[0], perRaySum, 1e-12));

    auto lossOf = [&](double delta) {
        return evalLoss(cat, {density[0] + delta}, k, e, rays, 0.5, g, q);
    };
    TASK_CHECK(checkGrad("shared tile", grad.dDensity[0], lossOf, 1e-4, 2e-4, true));
    return 0;
}

// --------------------------------------------------------------------------
// snapshot group
// --------------------------------------------------------------------------

bool sameVJP(const fog::VJPResult& a, const fog::VJPResult& b)
{
    if (a.dDensity.size() != b.dDensity.size()) return false;
    for (size_t i = 0; i < a.dDensity.size(); ++i)
        if (a.dDensity[i] != b.dDensity[i]) return false;
    return a.dExtinction == b.dExtinction && a.dEmission == b.dEmission;
}

int case_snapshot_records()
{
    std::unique_ptr<fog::Record> recA(new fog::Record());
    std::unique_ptr<fog::Record> recB(new fog::Record());
    fog::VJPResult expectA, expectB;
    std::vector<Vec3d> gA, gB;
    std::vector<double> qA, qB;
    uint64_t P = 0;
    {
        auto s = makeMixedScene(1.0, Vec3d(0.0));
        auto handle = finalize(*s);
        fog::Catalog cat(*handle.grid<float>());
        P = cat.paramCount();
        auto d0 = cat.initialDensity();

        std::vector<fog::Ray> raysA(2);
        raysA[0].origin = Vec3d(-4.0, 0.5, 0.5);
        raysA[0].direction = Vec3d(1.0, 0.0, 0.0);
        raysA[0].t0 = 0.0;
        raysA[0].t1 = 60.0;
        raysA[0].background = Vec3d(0.1, 0.0, 0.2);
        raysA[1].origin = Vec3d(-4.0, -4.0, -4.0);
        raysA[1].direction = Vec3d(1.0, 1.0, 1.0);
        raysA[1].t0 = 0.0;
        raysA[1].t1 = 10.0;
        raysA[1].background = Vec3d(0.0, 0.1, 0.1);
        std::vector<fog::Ray> raysB(1);
        raysB[0].origin = Vec3d(30.0, 0.5, 0.5);
        raysB[0].direction = Vec3d(-1.0, 0.0, 0.0);
        raysB[0].t0 = 0.0;
        raysB[0].t1 = 50.0;
        raysB[0].background = Vec3d(0.3, 0.3, 0.0);

        gA = {Vec3d(0.5, 0.25, -0.75), Vec3d(-0.2, 0.6, 0.1)};
        qA = {0.35, -0.15};
        gB = {Vec3d(0.9, -0.1, 0.4)};
        qB = {0.55};

        auto d1 = d0;
        for (auto& v : d1) v = v * 0.5 + 0.1;
        fog::forward(cat, d0, 0.3, Vec3d(0.4, 0.2, 0.1), raysA, 0.5, recA.get());
        fog::forward(cat, d1, 0.15, Vec3d(0.1, 0.1, 0.4), raysB, 0.25, recB.get());
        expectA = fog::vjp(*recA, gA, qA);
        expectB = fog::vjp(*recB, gB, qB);
        // mutate the input vectors after recording; the records must not notice
        for (auto& v : d0) v = 999.0;
        for (auto& v : d1) v = -5.0;
    } // grid handle, catalog and density vectors are destroyed here

    TASK_CHECK(recA->paramCount() == P && recB->paramCount() == P);
    TASK_CHECK(recA->segmentCount() > 0 && recB->segmentCount() > 0);
    // repeated and interleaved VJPs on the orphaned records stay consistent
    auto a1 = fog::vjp(*recA, gA, qA);
    auto b1 = fog::vjp(*recB, gB, qB);
    auto a2 = fog::vjp(*recA, gA, qA);
    auto b2 = fog::vjp(*recB, gB, qB);
    auto a3 = fog::vjp(*recA, gA, qA);
    TASK_CHECK(sameVJP(a1, expectA));
    TASK_CHECK(sameVJP(b1, expectB));
    TASK_CHECK(sameVJP(a2, expectA));
    TASK_CHECK(sameVJP(b2, expectB));
    TASK_CHECK(sameVJP(a3, expectA));
    // and they still match a fresh record made from a rebuilt catalog
    {
        auto s = makeMixedScene(1.0, Vec3d(0.0));
        auto handle = finalize(*s);
        fog::Catalog cat(*handle.grid<float>());
        std::vector<fog::Ray> raysA(2);
        raysA[0].origin = Vec3d(-4.0, 0.5, 0.5);
        raysA[0].direction = Vec3d(1.0, 0.0, 0.0);
        raysA[0].t0 = 0.0;
        raysA[0].t1 = 60.0;
        raysA[0].background = Vec3d(0.1, 0.0, 0.2);
        raysA[1].origin = Vec3d(-4.0, -4.0, -4.0);
        raysA[1].direction = Vec3d(1.0, 1.0, 1.0);
        raysA[1].t0 = 0.0;
        raysA[1].t1 = 10.0;
        raysA[1].background = Vec3d(0.0, 0.1, 0.1);
        fog::Record fresh;
        fog::forward(cat, cat.initialDensity(), 0.3, Vec3d(0.4, 0.2, 0.1), raysA, 0.5, &fresh);
        TASK_CHECK(sameVJP(fog::vjp(fresh, gA, qA), expectA));
    }
    return 0;
}

int case_example_same_path()
{
    auto handle = nanovdb::tools::createFogVolumeSphere<float>(6.0, Vec3d(0.0), 1.0, 3.0);
    auto* grid = handle.grid<float>();
    TASK_CHECK(grid != nullptr);
    const int W = 8, H = 6;
    nanovdb::HostBuffer image(size_t(W) * H * sizeof(float));
    runNanoVDB(handle, 1, W, H, image); // the original CPU example entry point
    const float* pixels = reinterpret_cast<const float*>(image.data());

    // replicate the example's ray generation (RayGenOp, world-bounds clip,
    // checkerboard background) and run the shared forward directly
    fog::Catalog cat(*grid);
    const auto& wBBox = grid->worldBBox();
    const double wBBoxDimZ = wBBox.dim()[2] * 2.0;
    const Vec3d center = wBBox.min() + wBBox.dim() * 0.5;
    std::vector<fog::Ray> rays(size_t(W) * H);
    std::vector<char> hits(size_t(W) * H, 0);
    std::vector<float> bgs(size_t(W) * H, 0.0f);
    for (int i = 0; i < W * H; ++i) {
        const int x = i % W, y = i / W;
        const float u = (float(x) + 0.5f) / W;
        const float v = (float(y) + 0.5f) / H;
        const float aspect = W / float(H);
        const float Px = (2.f * u - 1.f) * std::tan(22.5f * 3.14159265358979323846f / 180.f) * aspect;
        const float Py = (2.f * v - 1.f) * std::tan(22.5f * 3.14159265358979323846f / 180.f);
        const Vec3d origin = center + Vec3d(0.0, 0.0, wBBoxDimZ);
        Vec3d dir(Px, Py, -1.0);
        dir.normalize();
        const int mask = 1 << 7;
        const float bg = ((x & mask) ^ (y & mask)) ? 1.0f : 0.5f;
        bgs[size_t(i)] = bg;
        auto& fr = rays[size_t(i)];
        fr.origin = origin;
        fr.direction = dir;
        fr.background = Vec3d(double(bg));
        nanovdb::math::Ray<double> wRay(origin, dir);
        if (wRay.clip(wBBox)) {
            fr.t0 = wRay.t0();
            fr.t1 = wRay.t1();
            hits[size_t(i)] = 1;
        } else {
            fr.t0 = 0.0;
            fr.t1 = 1.0;
        }
    }
    auto res = fog::forward(cat, cat.initialDensity(), 0.1, Vec3d(0.0), rays, 0.5);
    bool ok = true;
    for (int i = 0; i < W * H; ++i) {
        const double expected = hits[size_t(i)] ? res.rgb[size_t(i)][0] : double(bgs[size_t(i)]);
        if (!close(pixels[i], expected, 1e-5)) {
            std::printf("  pixel %d: example=%g forward=%g\n", i, pixels[i], expected);
            ok = false;
        }
    }
    TASK_CHECK(ok);
    return 0;
}

} // unnamed namespace

const struct task_case task_cases[] = {
    {"primal", "catalog_params", case_catalog_params},
    {"primal", "forward_mixed", case_forward_mixed},
    {"primal", "forward_scaled", case_forward_scaled},
    {"primal", "validation", case_validation},
    {"primal", "gap_counts", case_gap_counts},
    {"adjoint", "vjp_central", case_vjp_central},
    {"adjoint", "vjp_zero_limits", case_vjp_zero_limits},
    {"adjoint", "vjp_shared_tile", case_vjp_shared_tile},
    {"snapshot", "records", case_snapshot_records},
    {"snapshot", "example_same_path", case_example_same_path},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
