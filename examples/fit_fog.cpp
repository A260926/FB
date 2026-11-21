// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0
//
// Fit a sparse fog density field to a target render: build a small sparse
// grid, render a batch of rays, then take a few gradient steps on the stored
// density parameters using the production fog::forward / fog::vjp pair.
// Writes .task-build/fog-fit/{before.ppm,after.ppm,loss.txt}.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <utility>
#include <vector>

#include <nanovdb/NanoVDB.h>
#include <nanovdb/GridHandle.h>
#include <nanovdb/HostBuffer.h>
#include <nanovdb/tools/GridBuilder.h>
#include <nanovdb/tools/CreateNanoGrid.h>
#include <nanovdb/tools/FogAdjoint.h>

namespace fog = nanovdb::tools::fog;
using nanovdb::Coord;
using nanovdb::Vec3d;

namespace {

bool writePPM(const char* path, int w, int h, const std::vector<Vec3d>& rgb)
{
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f << "P6\n" << w << " " << h << "\n255\n";
    for (const auto& c : rgb) {
        for (int i = 0; i < 3; ++i) {
            const double v = std::min(1.0, std::max(0.0, c[i]));
            const unsigned char b = static_cast<unsigned char>(v * 255.0 + 0.5);
            f.write(reinterpret_cast<const char*>(&b), 1);
        }
    }
    f.flush();
    return f.good();
}

} // unnamed namespace

int main()
{
    using BuildGrid = nanovdb::tools::build::Grid<float>;

    // sparse scene: a ball of active voxels plus one constant 8^3 block that
    // prunes into a level-1 tile during conversion
    BuildGrid src(0.0f, "fit", nanovdb::GridClass::FogVolume);
    src.setTransform(1.0, Vec3d(0.0));
    src([](const Coord& ijk) -> float {
        const double dx = ijk[0] + 3.0, dy = ijk[1] + 2.0, dz = ijk[2] - 1.0;
        if (dx * dx + dy * dy + dz * dz < 25.0) return 0.9f;
        if (ijk[0] >= 8 && ijk[0] < 16 && ijk[1] >= 8 && ijk[1] < 16 && ijk[2] >= 8 && ijk[2] < 16)
            return 0.4f;
        return 0.0f;
    },
        nanovdb::CoordBBox(Coord(-16), Coord(15)));
    nanovdb::tools::CreateNanoGrid<BuildGrid> converter(src);
    auto handle = converter.getHandle<float>();
    const auto* grid = handle.grid<float>();
    if (!grid) {
        std::fprintf(stderr, "fit_fog: failed to build the grid\n");
        return 1;
    }

    fog::Catalog catalog(*grid);
    const uint64_t P = catalog.paramCount();
    if (P == 0) {
        std::fprintf(stderr, "fit_fog: empty catalog\n");
        return 1;
    }
    std::printf("fit_fog: %llu density parameters\n", (unsigned long long)P);

    // 32x32 orthographic camera along +z
    const int W = 32, H = 32;
    std::vector<fog::Ray> rays(size_t(W) * size_t(H));
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            fog::Ray& r = rays[size_t(y) * W + x];
            r.origin = Vec3d(x - W * 0.5 + 0.5, y - H * 0.5 + 0.5, -20.0);
            r.direction = Vec3d(0.0, 0.0, 1.0);
            r.t0 = 0.0;
            r.t1 = 40.0;
            r.background = Vec3d(0.05 * x / W, 0.05 * y / H, 0.10);
        }
    }
    const double k = 0.2;
    const Vec3d e(0.05, 0.10, 0.08);
    const double h = 0.5;

    // target render: the initial field with a density bump near the origin
    // and a brighter constant tile
    std::vector<double> target = catalog.initialDensity();
    for (uint64_t p = 0; p < P; ++p) {
        const Coord o = catalog.paramOrigin(p);
        const double d2 = double(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]);
        if (d2 <= 16.0) target[p] += 0.35;
        if (catalog.paramLevel(p) == 1) target[p] += 0.15;
    }
    const fog::ForwardResult targetRes = fog::forward(catalog, target, k, e, rays, h);

    auto lossOf = [&](const std::vector<double>& d, std::vector<Vec3d>* outRGB) {
        auto res = fog::forward(catalog, d, k, e, rays, h);
        double L = 0.0;
        for (size_t i = 0; i < rays.size(); ++i) {
            const Vec3d diff = res.rgb[i] - targetRes.rgb[i];
            L += diff.dot(diff);
        }
        if (outRGB) *outRGB = std::move(res.rgb);
        return L;
    };

    std::vector<double> theta = catalog.initialDensity();
    std::vector<Vec3d> imgBefore;
    const double loss0 = lossOf(theta, &imgBefore);

    // gradient descent with the production record/VJP
    std::vector<std::pair<int, double>> trace;
    double lr = 0.5;
    double loss = loss0;
    for (int step = 0; step < 12; ++step) {
        fog::Record rec;
        auto res = fog::forward(catalog, theta, k, e, rays, h, &rec);
        std::vector<Vec3d> g(rays.size());
        std::vector<double> q(rays.size(), 0.0);
        for (size_t i = 0; i < rays.size(); ++i)
            g[i] = (res.rgb[i] - targetRes.rgb[i]) * 2.0;
        auto grad = fog::vjp(rec, g, q);
        bool accepted = false;
        for (int bt = 0; bt < 20; ++bt) {
            std::vector<double> trial(P);
            for (uint64_t p = 0; p < P; ++p)
                trial[p] = std::max(0.0, theta[p] - lr * grad.dDensity[p]);
            const double Ltrial = lossOf(trial, nullptr);
            if (Ltrial < loss) {
                theta = std::move(trial);
                loss = Ltrial;
                accepted = true;
                break;
            }
            lr *= 0.5;
        }
        trace.emplace_back(step, loss);
        if (!accepted) break;
    }
    std::vector<Vec3d> imgAfter;
    const double loss1 = lossOf(theta, &imgAfter);

    std::printf("fit_fog: loss initial=%.9g final=%.9g\n", loss0, loss1);
    if (!(loss1 < loss0)) {
        std::fprintf(stderr, "fit_fog: loss did not decrease\n");
        return 1;
    }

    const std::filesystem::path dir = std::filesystem::path(".task-build") / "fog-fit";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        std::fprintf(stderr, "fit_fog: cannot create %s: %s\n", dir.string().c_str(), ec.message().c_str());
        return 1;
    }
    const std::string beforePath = (dir / "before.ppm").string();
    const std::string afterPath = (dir / "after.ppm").string();
    const std::string lossPath = (dir / "loss.txt").string();
    if (!writePPM(beforePath.c_str(), W, H, imgBefore) || !writePPM(afterPath.c_str(), W, H, imgAfter)) {
        std::fprintf(stderr, "fit_fog: failed to write PPM files\n");
        return 1;
    }
    {
        std::ofstream f(lossPath);
        if (!f) {
            std::fprintf(stderr, "fit_fog: failed to write loss.txt\n");
            return 1;
        }
        f << "initial " << loss0 << "\n";
        for (const auto& t : trace)
            f << "step " << t.first << " " << t.second << "\n";
        f << "final " << loss1 << "\n";
        f.flush();
        if (!f.good()) {
            std::fprintf(stderr, "fit_fog: failed to write loss.txt\n");
            return 1;
        }
    }
    std::printf("fit_fog: wrote %s, %s, %s\n", beforePath.c_str(), afterPath.c_str(), lossPath.c_str());
    return 0;
}
