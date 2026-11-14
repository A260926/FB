// Exact-area cellular bake tests.
//
// The reference implementation below is written independently from the
// production bake: it rebuilds every feature from the integer lattice,
// forms each stone by clipping the pixel rectangle with the competitor
// half-planes, and integrates the resulting convex polygons itself.
//
// Why the reference candidate range is enough:
//   With jitter radius J, every point of the plane has a feature within
//   D = sqrt(0.5) + J (nearest integer lattice point is within sqrt(0.5)).
//   A feature owning a point of the request is therefore within D of it;
//   any competitor that can cut that feature's eroded boundary is within
//   2D + seam of the owned point (seam <= 2). The reference enumerates the
//   proven bound E = 3D + 2 + J and then adds three full lattice rings as
//   margin. Extra features only contribute redundant half-planes (their
//   Voronoi cell never reaches the request), so the margin cannot change
//   any clipped polygon; it merely makes the reference deliberately more
//   conservative than the production candidate set.

#include "task_support/check.h"

#include <cmath>
#include <cstddef>
#include <vector>
#include <algorithm>

// Access the unchanged internals (hash, primes, random vector table) so the
// reference reuses the exact same random tables without copying them.
#define private public
#include "Cpp/FastNoiseLite.h"
#undef private

namespace
{
    const double INV_2P31 = 1.0 / 2147483648.0;

    struct RefFeature
    {
        double x, y, value;
        int k, l;
    };

    struct RefPixel
    {
        double coverage;
        double value;
    };

    typedef std::pair<double, double> RefPoint;

    int RefHash(int seed, int k, int l)
    {
        // Signed wrap-around relies on -fwrapv, exactly like FastNoiseLite.
        int hash = seed ^ (k * FastNoiseLite::PrimeX) ^ (l * FastNoiseLite::PrimeY);
        hash *= 0x27d4eb2d;
        return hash;
    }

    RefFeature MakeFeature(int seed, int k, int l, float jitterModifier)
    {
        float jitter = 0.43701595f * jitterModifier;
        int hash = RefHash(seed, k, l);
        int idx = hash & (255 << 1);

        RefFeature f;
        f.k = k;
        f.l = l;
        f.x = (double)k + (double)FastNoiseLite::Lookup<float>::RandVecs2D[idx] * (double)jitter;
        f.y = (double)l + (double)FastNoiseLite::Lookup<float>::RandVecs2D[idx | 1] * (double)jitter;
        f.value = (double)hash * INV_2P31;
        return f;
    }

    double RefPolygonArea(const std::vector<RefPoint>& poly)
    {
        size_t n = poly.size();
        if (n < 3)
            return 0.0;
        double twice = 0.0;
        for (size_t i = 0; i < n; i++)
        {
            const RefPoint& s = poly[i];
            const RefPoint& e = poly[(i + 1) % n];
            twice += s.first * e.second - e.first * s.second;
        }
        return std::fabs(twice) * 0.5;
    }

    // Keep n . p >= c.
    void RefClip(std::vector<RefPoint>& poly, double nx, double ny, double c)
    {
        std::vector<RefPoint> out;
        size_t n = poly.size();
        if (n == 0)
            return;
        for (size_t i = 0; i < n; i++)
        {
            const RefPoint& s = poly[i];
            const RefPoint& e = poly[(i + 1) % n];
            double ds = nx * s.first + ny * s.second - c;
            double de = nx * e.first + ny * e.second - c;
            bool sin = ds >= 0.0;
            bool ein = de >= 0.0;
            if (sin)
                out.push_back(s);
            if (sin != ein)
            {
                double t = ds / (ds - de);
                out.push_back(RefPoint(s.first + t * (e.first - s.first),
                    s.second + t * (e.second - s.second)));
            }
        }
        poly.swap(out);
    }

    // Independent reference bake in noise coordinates.
    // normalizeSeam=false reproduces the defective variant that forgets to
    // divide the retreat by |b-a| (uses seam instead of seam * |b-a|).
    void ReferenceBake(int seed, float jitterModifier, double seam,
        double nx0, double ny0, double ndx, double ndy,
        unsigned width, unsigned height, bool normalizeSeam, RefPixel* out)
    {
        float jitter = 0.43701595f * jitterModifier;
        double jitterRadius = std::fabs((double)jitter);
        double D = 0.70710678118654752440 + jitterRadius;
        double E = 3.0 * D + 2.0 + jitterRadius;

        double nx1 = nx0 + ndx * (double)width;
        double ny1 = ny0 + ndy * (double)height;

        int kmin = (int)std::floor(nx0 - E) - 3;
        int kmax = (int)std::floor(nx1 + E) + 3;
        int lmin = (int)std::floor(ny0 - E) - 3;
        int lmax = (int)std::floor(ny1 + E) + 3;

        std::vector<RefFeature> features;
        for (int l = lmin; l <= lmax; l++)
        {
            for (int k = kmin; k <= kmax; k++)
            {
                RefFeature f = MakeFeature(seed, k, l, jitterModifier);
                bool duplicate = false;
                for (size_t q = 0; q < features.size(); q++)
                {
                    if (features[q].x == f.x && features[q].y == f.y)
                    {
                        duplicate = true;
                        if (f.k < features[q].k || (f.k == features[q].k && f.l < features[q].l))
                            features[q] = f;
                        break;
                    }
                }
                if (!duplicate)
                    features.push_back(f);
            }
        }

        double pixelArea = ndx * ndy;
        for (unsigned j = 0; j < height; j++)
        {
            for (unsigned i = 0; i < width; i++)
            {
                double pxmin = nx0 + (double)i * ndx;
                double pymin = ny0 + (double)j * ndy;

                RefPixel& dst = out[(size_t)j * width + i];
                dst.coverage = 0.0;
                dst.value = 0.0;

                for (size_t a = 0; a < features.size(); a++)
                {
                    const RefFeature& fa = features[a];
                    std::vector<RefPoint> poly;
                    poly.push_back(RefPoint(pxmin, pymin));
                    poly.push_back(RefPoint(pxmin + ndx, pymin));
                    poly.push_back(RefPoint(pxmin + ndx, pymin + ndy));
                    poly.push_back(RefPoint(pxmin, pymin + ndy));

                    for (size_t b = 0; b < features.size(); b++)
                    {
                        if (a == b)
                            continue;
                        const RefFeature& fb = features[b];
                        double abx = fa.x - fb.x;
                        double aby = fa.y - fb.y;
                        double dist = std::sqrt(abx * abx + aby * aby);
                        if (dist == 0.0)
                            continue;
                        double gap = normalizeSeam ? seam * dist : seam;
                        double c = gap + fa.x * fa.x + fa.y * fa.y
                            - fb.x * fb.x - fb.y * fb.y;
                        RefClip(poly, 2.0 * abx, 2.0 * aby, c);
                        if (poly.empty())
                            break;
                    }

                    double area = RefPolygonArea(poly);
                    if (area <= 0.0)
                        continue;
                    dst.coverage += area / pixelArea;
                    dst.value += (area / pixelArea) * fa.value;
                }
            }
        }
    }

    // Degraded "bake": the whole pixel takes the identity of its center.
    void CenterSamplingBake(int seed, float jitterModifier, double seam,
        double nx0, double ny0, double ndx, double ndy,
        unsigned width, unsigned height, RefPixel* out)
    {
        float jitter = 0.43701595f * jitterModifier;
        double jitterRadius = std::fabs((double)jitter);
        double D = 0.70710678118654752440 + jitterRadius;
        double E = 3.0 * D + 2.0 + jitterRadius;

        double nx1 = nx0 + ndx * (double)width;
        double ny1 = ny0 + ndy * (double)height;

        int kmin = (int)std::floor(nx0 - E) - 3;
        int kmax = (int)std::floor(nx1 + E) + 3;
        int lmin = (int)std::floor(ny0 - E) - 3;
        int lmax = (int)std::floor(ny1 + E) + 3;

        std::vector<RefFeature> features;
        for (int l = lmin; l <= lmax; l++)
            for (int k = kmin; k <= kmax; k++)
                features.push_back(MakeFeature(seed, k, l, jitterModifier));

        for (unsigned j = 0; j < height; j++)
        {
            for (unsigned i = 0; i < width; i++)
            {
                double px = nx0 + (i + 0.5) * ndx;
                double py = ny0 + (j + 0.5) * ndy;

                size_t best = 0;
                double bestDist = 1e300;
                for (size_t a = 0; a < features.size(); a++)
                {
                    double ddx = features[a].x - px;
                    double ddy = features[a].y - py;
                    double d = ddx * ddx + ddy * ddy;
                    if (d < bestDist)
                    {
                        bestDist = d;
                        best = a;
                    }
                }

                const RefFeature& fa = features[best];
                bool inside = true;
                for (size_t b = 0; b < features.size(); b++)
                {
                    if (b == best)
                        continue;
                    const RefFeature& fb = features[b];
                    double abx = fa.x - fb.x;
                    double aby = fa.y - fb.y;
                    double dist = std::sqrt(abx * abx + aby * aby);
                    if (dist == 0.0)
                        continue;
                    double c = seam * dist + fa.x * fa.x + fa.y * fa.y
                        - fb.x * fb.x - fb.y * fb.y;
                    if (2.0 * abx * px + 2.0 * aby * py < c)
                    {
                        inside = false;
                        break;
                    }
                }

                RefPixel& dst = out[(size_t)j * width + i];
                dst.coverage = inside ? 1.0 : 0.0;
                dst.value = inside ? fa.value : 0.0;
            }
        }
    }

    bool Close(double a, double b, double eps)
    {
        return std::fabs(a - b) <= eps;
    }

    void NoiseRect(const FastNoiseLite& noise, double x0, double y0,
        double dx, double dy, double& nx0, double& ny0,
        double& ndx, double& ndy)
    {
        double f = (double)noise.mFrequency;
        nx0 = x0 * f;
        ny0 = y0 * f;
        ndx = dx * f;
        ndy = dy * f;
    }

    // Compare a full bake against the independent reference.
    bool MatchesReference(const FastNoiseLite& noise, double seam,
        double x0, double y0, double dx, double dy,
        unsigned width, unsigned height, double eps)
    {
        std::vector<FastNoiseLite::CellularPixel2D> got((size_t)width * height);
        noise.BakeCellular2D(x0, y0, dx, dy, width, height, seam, &got[0]);

        double nx0, ny0, ndx, ndy;
        NoiseRect(noise, x0, y0, dx, dy, nx0, ny0, ndx, ndy);
        std::vector<RefPixel> ref((size_t)width * height);
        ReferenceBake(noise.mSeed, noise.mCellularJitterModifier, seam,
            nx0, ny0, ndx, ndy, width, height, true, &ref[0]);

        for (size_t q = 0; q < got.size(); q++)
        {
            if (!Close(got[q].coverage, ref[q].coverage, eps))
                return false;
            if (!Close(got[q].value, ref[q].value, eps))
                return false;
        }
        return true;
    }

    FastNoiseLite MakeNoise(int seed, float frequency, float jitter)
    {
        FastNoiseLite noise(seed);
        noise.SetFrequency(frequency);
        noise.SetCellularJitter(jitter);
        // These settings must have no influence on the bake.
        noise.SetNoiseType(FastNoiseLite::NoiseType_Perlin);
        noise.SetFractalType(FastNoiseLite::FractalType_FBm);
        noise.SetFractalOctaves(5);
        noise.SetCellularReturnType(FastNoiseLite::CellularReturnType_Distance2);
        noise.SetCellularDistanceFunction(FastNoiseLite::CellularDistanceFunction_Manhattan);
        return noise;
    }

    // Purely analytic jitter = 0 bake: every feature sits on its integer
    // lattice point (k, l), so its Voronoi cell is the unit square centered
    // there, [k-0.5, k+0.5] x [l-0.5, l+0.5], eroded by seam/2 on every
    // side. Each stone/pixel overlap is therefore an axis-aligned rectangle
    // whose area is an exact product of differences. Greyscales come from
    // an independently recomputed signed hash.
    void AnalyticLatticeBake(int seed, double seam,
        double x0, double y0, double dx, double dy,
        unsigned width, unsigned height, RefPixel* out)
    {
        double half = 0.5 * seam;
        double pixelArea = dx * dy;
        for (unsigned j = 0; j < height; j++)
        {
            for (unsigned i = 0; i < width; i++)
            {
                double px0 = x0 + (double)i * dx;
                double px1 = px0 + dx;
                double py0 = y0 + (double)j * dy;
                double py1 = py0 + dy;

                RefPixel& dst = out[(size_t)j * width + i];
                dst.coverage = 0.0;
                dst.value = 0.0;

                // Enumerate centered cells with a one-cell margin so boxes
                // straddling a bisector are covered.
                int k0 = (int)std::floor(px0) - 1;
                int k1 = (int)std::floor(px1) + 1;
                int l0 = (int)std::floor(py0) - 1;
                int l1 = (int)std::floor(py1) + 1;
                for (int l = l0; l <= l1; l++)
                {
                    double iy0 = std::max(py0, (double)l - 0.5 + half);
                    double iy1 = std::min(py1, (double)l + 0.5 - half);
                    if (iy1 <= iy0)
                        continue;
                    for (int k = k0; k <= k1; k++)
                    {
                        double ix0 = std::max(px0, (double)k - 0.5 + half);
                        double ix1 = std::min(px1, (double)k + 0.5 - half);
                        if (ix1 <= ix0)
                            continue;
                        double stoneArea = (ix1 - ix0) * (iy1 - iy0);
                        double gray = (double)RefHash(seed, k, l) * INV_2P31;
                        dst.coverage += stoneArea / pixelArea;
                        dst.value += (stoneArea / pixelArea) * gray;
                    }
                }
            }
        }
    }

    // ---------------------------------------------------------------
    // lattice group: jitter = 0, analytic integer-square answers
    // ---------------------------------------------------------------

    int lattice_interior_square()
    {
        FastNoiseLite noise = MakeNoise(7, 1.0f, 0.0f);
        FastNoiseLite::CellularPixel2D px;
        // Pixel fully inside the eroded stone of lattice cell (3, 3).
        noise.BakeCellular2D(3.2, 3.2, 0.1, 0.1, 1, 1, 0.2, &px);
        double expectedValue = (double)RefHash(7, 3, 3) * INV_2P31;
        TASK_CHECK(Close(px.coverage, 1.0, 1e-12));
        TASK_CHECK(Close(px.value, expectedValue, 1e-12));
        return 0;
    }

    int lattice_seam_area()
    {
        FastNoiseLite noise = MakeNoise(7, 1.0f, 0.0f);
        FastNoiseLite::CellularPixel2D px;
        // Pixel straddling the vertical seam between cells (3,3) and (4,3).
        // Eroded edges sit at x = 3.4 and x = 3.6; the pixel spans
        // [3.3, 3.7] x [3.2, 3.35], fully inside row l = 3.
        noise.BakeCellular2D(3.3, 3.2, 0.4, 0.15, 1, 1, 0.2, &px);
        double vLeft = (double)RefHash(7, 3, 3) * INV_2P31;
        double vRight = (double)RefHash(7, 4, 3) * INV_2P31;
        TASK_CHECK(Close(px.coverage, 0.5, 1e-12));
        // Black seam included: each stone covers a quarter of the pixel.
        TASK_CHECK(Close(px.value, 0.25 * (vLeft + vRight), 1e-12));
        TASK_CHECK(std::fabs(px.value - 0.5 * (vLeft + vRight)) > 1e-8);
        return 0;
    }

    int lattice_grid_against_reference()
    {
        FastNoiseLite noise = MakeNoise(7, 1.0f, 0.0f);
        // Tile crossing integer cell boundaries on every row/column.
        TASK_CHECK(MatchesReference(noise, 0.1, -2.2, -1.7, 0.13, 0.17, 31, 23, 1e-10));
        return 0;
    }

    int lattice_analytic_full_output()
    {
        // Entire output checked against the closed-form rectangle areas and
        // independently recomputed hashes, with and without a seam, and with
        // a non-unit frequency so the coordinate transform is exercised.
        const int seeds[] = {7, -1234, 987654};
        const double seams[] = {0.0, 0.35};
        for (size_t s = 0; s < 3; s++)
        {
            for (size_t t = 0; t < 2; t++)
            {
                FastNoiseLite noise = MakeNoise(seeds[s], 1.7f, 0.0f);
                const unsigned w = 19, h = 13;
                const double x0 = -3.27, y0 = -2.61, dx = 0.23, dy = 0.31;
                std::vector<FastNoiseLite::CellularPixel2D> got((size_t)w * h);
                noise.BakeCellular2D(x0, y0, dx, dy, w, h, seams[t], &got[0]);

                double nx0, ny0, ndx, ndy;
                NoiseRect(noise, x0, y0, dx, dy, nx0, ny0, ndx, ndy);
                std::vector<RefPixel> analytic((size_t)w * h);
                AnalyticLatticeBake(seeds[s], seams[t],
                    nx0, ny0, ndx, ndy, w, h, &analytic[0]);

                for (size_t q = 0; q < got.size(); q++)
                {
                    TASK_CHECK(Close(got[q].coverage, analytic[q].coverage, 1e-10));
                    TASK_CHECK(Close(got[q].value, analytic[q].value, 1e-10));
                    TASK_CHECK(got[q].coverage >= 0.0 && got[q].coverage <= 1.0 + 1e-12);
                }
            }
        }
        return 0;
    }

    // ---------------------------------------------------------------
    // jitter group: independent half-plane clipped reference
    // ---------------------------------------------------------------

    int jitter_reference_typical()
    {
        FastNoiseLite noise = MakeNoise(11, 1.0f, 1.0f);
        TASK_CHECK(MatchesReference(noise, 0.3, -1.7, -2.3, 0.25, 0.3, 24, 20, 1e-8));
        return 0;
    }

    int jitter_reference_high()
    {
        FastNoiseLite noise = MakeNoise(23, 1.0f, 4.0f);
        TASK_CHECK(MatchesReference(noise, 0.15, -3.4, 0.6, 0.2, 0.22, 26, 19, 1e-8));
        return 0;
    }

    int jitter_large_pixels_negative()
    {
        // Pixels much larger than a cell, spanning many Voronoi cells and
        // sitting entirely in negative coordinate territory.
        FastNoiseLite noise = MakeNoise(29, 0.5f, 1.0f);
        TASK_CHECK(MatchesReference(noise, 0.2, -8.0, -6.0, 1.5, 1.5, 4, 3, 1e-8));
        return 0;
    }

    int jitter_empty_cells()
    {
        // seam = 2 with jitter = 0 erodes every unit square down to nothing.
        FastNoiseLite lattice = MakeNoise(7, 1.0f, 0.0f);
        std::vector<FastNoiseLite::CellularPixel2D> out(36);
        lattice.BakeCellular2D(-1.2, -1.2, 0.4, 0.4, 6, 6, 2.0, &out[0]);
        for (size_t q = 0; q < out.size(); q++)
        {
            TASK_CHECK(out[q].coverage == 0.0);
            TASK_CHECK(out[q].value == 0.0);
        }
        // Jittered case still agrees with the reference even when many
        // stones vanish.
        FastNoiseLite jittered = MakeNoise(31, 1.0f, 1.0f);
        TASK_CHECK(MatchesReference(jittered, 2.0, -2.0, -2.0, 0.5, 0.5, 8, 8, 1e-8));
        return 0;
    }

    int jitter_center_sampling_rejected()
    {
        // A degraded center-sampling bake must disagree with the area bake
        // where the seam cuts through pixels.
        FastNoiseLite noise = MakeNoise(41, 1.0f, 1.0f);
        const unsigned w = 16, h = 16;
        std::vector<FastNoiseLite::CellularPixel2D> got((size_t)w * h);
        noise.BakeCellular2D(-1.0, -1.0, 0.125, 0.125, w, h, 0.4, &got[0]);

        std::vector<RefPixel> center((size_t)w * h);
        CenterSamplingBake(noise.mSeed, noise.mCellularJitterModifier, 0.4,
            -1.0, -1.0, 0.125, 0.125, w, h, &center[0]);

        double maxCoverageGap = 0.0;
        double maxValueGap = 0.0;
        for (size_t q = 0; q < got.size(); q++)
        {
            maxCoverageGap = std::max(maxCoverageGap,
                std::fabs(got[q].coverage - center[q].coverage));
            maxValueGap = std::max(maxValueGap,
                std::fabs(got[q].value - center[q].value));
        }
        TASK_CHECK(maxCoverageGap > 1e-3);
        TASK_CHECK(maxValueGap > 1e-3);
        return 0;
    }

    int jitter_unnormalized_seam_rejected()
    {
        // Forgetting the |b-a| factor on oblique (non unit-distance) edges
        // must produce the wrong physical gap and fail numerically.
        FastNoiseLite noise = MakeNoise(43, 1.0f, 1.0f);
        const unsigned w = 18, h = 18;
        std::vector<FastNoiseLite::CellularPixel2D> got((size_t)w * h);
        noise.BakeCellular2D(-1.5, -1.5, 0.2, 0.2, w, h, 0.25, &got[0]);

        std::vector<RefPixel> bad((size_t)w * h);
        ReferenceBake(noise.mSeed, noise.mCellularJitterModifier, 0.25,
            -1.5, -1.5, 0.2, 0.2, w, h, false, &bad[0]);

        double maxGap = 0.0;
        for (size_t q = 0; q < got.size(); q++)
            maxGap = std::max(maxGap, std::fabs(got[q].coverage - bad[q].coverage));
        TASK_CHECK(maxGap > 1e-6);
        return 0;
    }

    // ---------------------------------------------------------------
    // seams group
    // ---------------------------------------------------------------

    int seams_zero_covers_everything()
    {
        // Without a seam the Voronoi cells partition the whole plane.
        float jitters[] = {0.0f, 1.0f, 4.0f};
        for (size_t t = 0; t < 3; t++)
        {
            FastNoiseLite noise = MakeNoise(53 + (int)t, 1.0f, jitters[t]);
            const unsigned w = 20, h = 17;
            std::vector<FastNoiseLite::CellularPixel2D> out((size_t)w * h);
            noise.BakeCellular2D(-2.7, -1.9, 0.19, 0.23, w, h, 0.0, &out[0]);
            for (size_t q = 0; q < out.size(); q++)
            {
                TASK_CHECK(Close(out[q].coverage, 1.0, 1e-8));
                TASK_CHECK(out[q].coverage >= 0.0 && out[q].coverage <= 1.0 + 1e-12);
            }
        }
        return 0;
    }

    int seams_monotonic_coverage()
    {
        FastNoiseLite noise = MakeNoise(59, 1.0f, 1.0f);
        const unsigned w = 14, h = 11;
        double seamValues[] = {0.0, 0.05, 0.1, 0.2, 0.4, 0.8, 1.5};
        std::vector<FastNoiseLite::CellularPixel2D> prev((size_t)w * h);
        for (size_t s = 0; s < 7; s++)
        {
            std::vector<FastNoiseLite::CellularPixel2D> cur((size_t)w * h);
            noise.BakeCellular2D(-1.3, -0.9, 0.21, 0.27, w, h, seamValues[s], &cur[0]);
            for (size_t q = 0; q < cur.size(); q++)
            {
                TASK_CHECK(cur[q].coverage >= -1e-12 && cur[q].coverage <= 1.0 + 1e-12);
                if (s > 0)
                    TASK_CHECK(cur[q].coverage <= prev[q].coverage + 1e-12);
            }
            prev = cur;
        }
        return 0;
    }

    int seams_oblique_physical_width()
    {
        // Analytic check on an oblique (diagonal neighbour) boundary:
        // the eroded boundary must pass exactly seam/2 from the midpoint
        // along the a-b line, which only holds when the retreat is
        // seam * |b-a| in the squared-distance inequality.
        RefFeature a = MakeFeature(61, 0, 0, 1.0f);
        RefFeature b = MakeFeature(61, 1, 1, 1.0f);
        double abx = a.x - b.x;
        double aby = a.y - b.y;
        double dist = std::sqrt(abx * abx + aby * aby);
        TASK_CHECK(dist > 1.05); // genuinely oblique, not a unit axial edge

        double mx = 0.5 * (a.x + b.x);
        double my = 0.5 * (a.y + b.y);
        double ux = abx / dist;
        double uy = aby / dist;
        const double seam = 0.4;
        double px = mx + 0.5 * seam * ux;
        double py = my + 0.5 * seam * uy;

        double lhs = (px - b.x) * (px - b.x) + (py - b.y) * (py - b.y)
            - ((px - a.x) * (px - a.x) + (py - a.y) * (py - a.y));
        TASK_CHECK(Close(lhs, seam * dist, 1e-12));

        // The midpoint itself lies inside the black gap: it fails both
        // eroded half-planes.
        double midForA = 2.0 * abx * mx + 2.0 * aby * my
            - (seam * dist + a.x * a.x + a.y * a.y - b.x * b.x - b.y * b.y);
        double midForB = -2.0 * abx * mx - 2.0 * aby * my
            - (seam * dist + b.x * b.x + b.y * b.y - a.x * a.x - a.y * a.y);
        TASK_CHECK(midForA < 0.0 && midForB < 0.0);
        return 0;
    }

    int seams_black_counts_in_average()
    {
        FastNoiseLite noise = MakeNoise(67, 1.0f, 0.0f);
        FastNoiseLite::CellularPixel2D px;
        noise.BakeCellular2D(3.3, 3.2, 0.4, 0.15, 1, 1, 0.2, &px);
        double vLeft = (double)RefHash(67, 3, 3) * INV_2P31;
        double vRight = (double)RefHash(67, 4, 3) * INV_2P31;
        // Whole-pixel average, black seam included (not divided by coverage).
        TASK_CHECK(Close(px.value, 0.25 * (vLeft + vRight), 1e-12));
        TASK_CHECK(Close(px.coverage, 0.5, 1e-12));
        return 0;
    }

    // ---------------------------------------------------------------
    // tiles group: stitching and coordinate scaling
    // ---------------------------------------------------------------

    int tiles_stitch_subrectangles()
    {
        FastNoiseLite noise = MakeNoise(71, 1.0f, 1.0f);
        const unsigned w = 7, h = 5;
        const double x0 = -2.13, y0 = -1.77, dx = 0.31, dy = 0.27;
        const double seam = 0.2;

        std::vector<FastNoiseLite::CellularPixel2D> full((size_t)w * h);
        noise.BakeCellular2D(x0, y0, dx, dy, w, h, seam, &full[0]);

        // Split into four sub-bakes along pixel grid lines, including
        // boundaries that cut through cells and seams.
        const unsigned splitI = 3, splitJ = 2;
        std::vector<FastNoiseLite::CellularPixel2D> piece((size_t)w * h);
        unsigned cols[2] = {splitI, w - splitI};
        unsigned rows[2] = {splitJ, h - splitJ};
        for (int sy = 0; sy < 2; sy++)
        {
            for (int sx = 0; sx < 2; sx++)
            {
                unsigned jBase = sy == 0 ? 0 : splitJ;
                unsigned iBase = sx == 0 ? 0 : splitI;
                std::vector<FastNoiseLite::CellularPixel2D> sub((size_t)cols[sx] * rows[sy]);
                noise.BakeCellular2D(x0 + iBase * dx, y0 + jBase * dy, dx, dy,
                    cols[sx], rows[sy], seam, &sub[0]);
                for (unsigned j = 0; j < rows[sy]; j++)
                    for (unsigned i = 0; i < cols[sx]; i++)
                        piece[(size_t)(jBase + j) * w + (iBase + i)] =
                            sub[(size_t)j * cols[sx] + i];
            }
        }

        for (size_t q = 0; q < full.size(); q++)
        {
            TASK_CHECK(Close(full[q].coverage, piece[q].coverage, 1e-10));
            TASK_CHECK(Close(full[q].value, piece[q].value, 1e-10));
        }
        return 0;
    }

    int tiles_frequency_world_scale()
    {
        // Frequency and world coordinates cancel: doubling the frequency
        // while halving the world extents must bake identical pixels.
        const double seam = 0.18;
        FastNoiseLite a(73);
        a.SetFrequency(2.0f);
        a.SetCellularJitter(1.0f);
        FastNoiseLite b(73);
        b.SetFrequency(1.0f);
        b.SetCellularJitter(1.0f);

        const unsigned w = 9, h = 7;
        std::vector<FastNoiseLite::CellularPixel2D> outA((size_t)w * h);
        std::vector<FastNoiseLite::CellularPixel2D> outB((size_t)w * h);
        a.BakeCellular2D(-1.25, 0.75, 0.1, 0.12, w, h, seam, &outA[0]);
        b.BakeCellular2D(-2.5, 1.5, 0.2, 0.24, w, h, seam, &outB[0]);

        for (size_t q = 0; q < outA.size(); q++)
        {
            TASK_CHECK(Close(outA[q].coverage, outB[q].coverage, 1e-10));
            TASK_CHECK(Close(outA[q].value, outB[q].value, 1e-10));
        }
        return 0;
    }

    int tiles_negative_region_reference()
    {
        FastNoiseLite noise = MakeNoise(79, 1.3f, 1.0f);
        TASK_CHECK(MatchesReference(noise, 0.22, -6.7, -8.1, 0.17, 0.19, 21, 17, 1e-8));
        return 0;
    }
}

extern const struct task_case task_cases[] = {
    {"lattice", "interior_square", lattice_interior_square},
    {"lattice", "seam_area", lattice_seam_area},
    {"lattice", "grid_reference", lattice_grid_against_reference},
    {"lattice", "analytic_full_output", lattice_analytic_full_output},

    {"jitter", "reference_typical", jitter_reference_typical},
    {"jitter", "reference_high", jitter_reference_high},
    {"jitter", "large_pixels_negative", jitter_large_pixels_negative},
    {"jitter", "empty_cells", jitter_empty_cells},
    {"jitter", "center_sampling_rejected", jitter_center_sampling_rejected},
    {"jitter", "unnormalized_seam_rejected", jitter_unnormalized_seam_rejected},

    {"seams", "zero_covers_everything", seams_zero_covers_everything},
    {"seams", "monotonic_coverage", seams_monotonic_coverage},
    {"seams", "oblique_physical_width", seams_oblique_physical_width},
    {"seams", "black_counts_in_average", seams_black_counts_in_average},

    {"tiles", "stitch_subrectangles", tiles_stitch_subrectangles},
    {"tiles", "frequency_world_scale", tiles_frequency_world_scale},
    {"tiles", "negative_region_reference", tiles_negative_region_reference},
};

extern const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
