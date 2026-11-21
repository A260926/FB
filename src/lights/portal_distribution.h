
/*
    pbrt source code is Copyright(c) 1998-2016
                        Matt Pharr, Greg Humphreys, and Wenzel Jakob.

    This file is part of pbrt.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are
    met:

    - Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.

    - Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
    IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
    TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
    PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
    HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
    SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
    LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
    DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
    THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
    OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

 */

#if defined(_MSC_VER)
#define NOMINMAX
#pragma once
#endif

#ifndef PBRT_LIGHTS_PORTAL_DISTRIBUTION_H
#define PBRT_LIGHTS_PORTAL_DISTRIBUTION_H

// lights/portal_distribution.h*
#include "pbrt.h"
#include "geometry.h"
#include "mipmap.h"
#include "spectrum.h"
#include <vector>

namespace pbrt {

// PortalRect Declarations
// A rectangular portal parallel to the light-local XY plane. A portal is
// only active for receiver points whose light-local z is strictly less than
// the portal's z.
struct PortalRect {
    Float xmin, ymin, xmax, ymax, z;
};

// PortalDistribution Declarations
//
// Receiver-conditioned angular-domain sampling distribution used to
// concentrate environment-map samples on rectangular portals. Directions in
// the light-local +Z hemisphere are parameterized by
//   a = atan2(wx, wz), b = atan2(wy, wz),
//   w = normalize(tan(a), tan(b), 1),
// with (a, b) in (-Pi/2, Pi/2)^2. A resolution x resolution grid of
// equal-width cells covers that domain; each cell carries the constant
// angular density rho = luminance(L_env(center)) * d_omega/(da db) evaluated
// at the cell center. Every portal is projected from the current receiver
// point into an (a, b) rectangle and the grid is cropped to it, counting the
// true covered area of partially covered edge cells. All valid portals with
// nonzero cropped mass form a single sampling branch whose density is the
// sum of the cropped portal densities (so directions covered by several
// portals get the summed density).
//
// Construction is O(R^2); per-receiver work is O(P) and each sampling or
// density query is O(P + log R), so no ray ever scans the whole angular
// map. All query results are per-steradian densities. The class is
// immutable after construction and every query takes its receiver state by
// value/reference, so concurrent threads and interleaved receiver points
// never touch shared mutable caches.
class PortalDistribution {
  public:
    // PortalDistribution Public Methods
    PortalDistribution(const std::vector<PortalRect> &portals, int resolution,
                       Float mix, const MIPMap<RGBSpectrum> &Lmap);

    // Per-receiver projection of one portal into the angular domain.
    struct PortalProjection {
        Float a0, a1, b0, b1;  // projected (and grid-clipped) rectangle
        double mass = 0;       // cropped mass of this portal
        // Disjoint runs of grid cells (with constant per-cell coverage
        // width) whose union is the cropped rectangle; used for sampling.
        struct Sub {
            int i0, i1, j0, j1;  // cell index ranges [i0,i1) x [j0,j1)
            double mass;
        };
        std::vector<Sub> subs;
    };

    // Per-receiver state: the valid, nonzero-mass portal projections and
    // their total mass. Constructed fresh for each receiver point.
    struct Receiver {
        std::vector<PortalProjection> projections;
        double totalMass = 0;
    };

    // Project all portals valid for the light-local receiver point _p_
    // into the angular domain. O(P) via precomputed prefix sums.
    Receiver BuildReceiver(const Point3f &pLight) const;

    // Sample the combined portal branch for _receiver_. _u_ is a fresh
    // uniform 2D sample. Returns a light-local direction; the caller is
    // responsible for folding in the mixture weights and the environment
    // branch (see InfiniteAreaLight).
    Vector3f Sample(const Receiver &receiver, const Point2f &u) const;

    // Per-steradian density of the combined portal branch (not including
    // the mixture weight or the environment branch) for the light-local
    // direction _w_.
    Float Pdf(const Receiver &receiver, const Vector3f &wLight) const;

    Float Mix() const { return mix; }
    int Resolution() const { return resolution; }
    // Constant angular density rho of cell (i, j), per unit (da db).
    Float CellDensity(int i, int j) const { return rho[i + j * resolution]; }
    // Solid angle Jacobian d_omega/(da db) of the angular parameterization.
    static Float Jacobian(Float a, Float b);
    // Light-local direction for angular coordinates (a, b).
    static Vector3f Direction(Float a, Float b);

  private:
    // PortalDistribution Private Methods
    // Sum of rho over the cell range [i0,i1) x [j0,j1) in O(1).
    double RectSum(int i0, int i1, int j0, int j1) const {
        int R1 = resolution + 1;
        return prefix[i1 + j1 * R1] - prefix[i0 + j1 * R1] -
               prefix[i1 + j0 * R1] + prefix[i0 + j0 * R1];
    }

    // PortalDistribution Private Data
    int resolution;
    Float mix;
    std::vector<PortalRect> portals;
    std::vector<Float> rho;      // resolution^2 cell densities
    std::vector<double> prefix;  // (resolution+1)^2 prefix sums of rho
};

}  // namespace pbrt

#endif  // PBRT_LIGHTS_PORTAL_DISTRIBUTION_H
