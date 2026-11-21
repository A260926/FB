
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

// lights/portal_distribution.cpp*
#include "lights/portal_distribution.h"

namespace pbrt {

// PortalDistribution Method Definitions
Float PortalDistribution::Jacobian(Float a, Float b) {
    // For w = normalize(tan(a), tan(b), 1) the gnomonic solid angle element
    // is d_omega = (1 + ta^2)(1 + tb^2) / (1 + ta^2 + tb^2)^(3/2) da db.
    double ta = std::tan((double)a), tb = std::tan((double)b);
    double q = 1 + ta * ta + tb * tb;
    return (Float)((1 + ta * ta) * (1 + tb * tb) / (q * std::sqrt(q)));
}

Vector3f PortalDistribution::Direction(Float a, Float b) {
    return Normalize(Vector3f(std::tan(a), std::tan(b), 1));
}

PortalDistribution::PortalDistribution(const std::vector<PortalRect> &portals,
                                       int resolution, Float mix,
                                       const MIPMap<RGBSpectrum> &Lmap)
    : resolution(resolution), mix(mix), portals(portals) {
    CHECK_GE(resolution, 2);
    CHECK_LE(resolution, 512);
    CHECK(mix >= 0 && mix < 1);
    int R = resolution;
    rho.resize(R * R);
    double h = Pi / R;
    // Cell weights: environment luminance at the cell center times the
    // solid angle Jacobian, treated as a constant angular density rho over
    // the cell. (The environment map itself is left untouched.)
    for (int j = 0; j < R; ++j)
        for (int i = 0; i < R; ++i) {
            Float a = (Float)(-Pi / 2 + (i + .5) * h);
            Float b = (Float)(-Pi / 2 + (j + .5) * h);
            Vector3f w = Direction(a, b);
            Point2f st(SphericalPhi(w) * Inv2Pi, SphericalTheta(w) * InvPi);
            rho[i + j * R] = Lmap.Lookup(st).y() * Jacobian(a, b);
        }
    // 2D prefix sums over rho for O(1) rectangle mass queries.
    prefix.assign((R + 1) * (R + 1), 0.0);
    for (int j = 1; j <= R; ++j)
        for (int i = 1; i <= R; ++i)
            prefix[i + j * (R + 1)] =
                (double)rho[(i - 1) + (j - 1) * R] +
                prefix[(i - 1) + j * (R + 1)] +
                prefix[i + (j - 1) * (R + 1)] -
                prefix[(i - 1) + (j - 1) * (R + 1)];
}

PortalDistribution::Receiver PortalDistribution::BuildReceiver(
    const Point3f &p) const {
    Receiver receiver;
    const int R = resolution;
    const double h = Pi / R;
    const double halfPi = Pi / 2;
    // A run of cells along one axis with a constant per-cell coverage width
    // (edge runs keep the true partially covered width).
    struct Run {
        int c0, c1;
        double w;
    };
    for (const PortalRect &portal : portals) {
        double dz = (double)portal.z - (double)p.z;
        if (!(dz > 0)) continue;  // receiver must be strictly below portal z
        // Project the rectangle from the receiver into the angular domain.
        double a0 = std::atan2((double)portal.xmin - p.x, dz);
        double a1 = std::atan2((double)portal.xmax - p.x, dz);
        double b0 = std::atan2((double)portal.ymin - p.y, dz);
        double b1 = std::atan2((double)portal.ymax - p.y, dz);
        // Clip to the grid domain (numerical safety; the projection of a
        // plane in front of the receiver always lands inside).
        a0 = Clamp(a0, -halfPi, halfPi);
        a1 = Clamp(a1, -halfPi, halfPi);
        b0 = Clamp(b0, -halfPi, halfPi);
        b1 = Clamp(b1, -halfPi, halfPi);
        if (!(a1 > a0) || !(b1 > b0)) continue;
        // Split each axis into at most three runs (partial edge cell, full
        // cells, partial edge cell) so that cropped edge cells contribute
        // their true covered area.
        Run runsA[3], runsB[3];
        int nA = 0, nB = 0;
        auto splitAxis = [R, h](double c0, double c1, Run *runs,
                                int &n) {
            int iL = Clamp((int)std::floor(c0), 0, R - 1);
            int iR = Clamp((int)std::floor(c1), 0, R - 1);
            if (iL == iR) {
                runs[n++] = {iL, iL + 1, (c1 - c0) * h};
            } else {
                runs[n++] = {iL, iL + 1, (iL + 1 - c0) * h};
                if (iR > iL + 1) runs[n++] = {iL + 1, iR, h};
                if (c1 > iR) runs[n++] = {iR, iR + 1, (c1 - iR) * h};
            }
        };
        splitAxis((a0 + halfPi) / h, (a1 + halfPi) / h, runsA, nA);
        splitAxis((b0 + halfPi) / h, (b1 + halfPi) / h, runsB, nB);
        PortalProjection proj;
        proj.a0 = (Float)a0;
        proj.a1 = (Float)a1;
        proj.b0 = (Float)b0;
        proj.b1 = (Float)b1;
        for (int ra = 0; ra < nA; ++ra)
            for (int rb = 0; rb < nB; ++rb) {
                double mass = runsA[ra].w * runsB[rb].w *
                              RectSum(runsA[ra].c0, runsA[ra].c1,
                                      runsB[rb].c0, runsB[rb].c1);
                if (mass > 0) {
                    proj.subs.push_back({runsA[ra].c0, runsA[ra].c1,
                                         runsB[rb].c0, runsB[rb].c1, mass});
                    proj.mass += mass;
                }
            }
        if (proj.mass > 0) {
            receiver.totalMass += proj.mass;
            receiver.projections.push_back(std::move(proj));
        }
    }
    return receiver;
}

Vector3f PortalDistribution::Sample(const Receiver &receiver,
                                    const Point2f &u) const {
    const int R = resolution;
    const double h = Pi / R;
    const double halfPi = Pi / 2;
    const double oneMinusEps = 1.0 - 1e-12;
    double s = Clamp((double)u[0], 0.0, oneMinusEps);
    double t = Clamp((double)u[1], 0.0, oneMinusEps);
    // Choose a portal proportional to its cropped mass.
    double target = s * receiver.totalMass;
    const PortalProjection *proj = &receiver.projections.back();
    double cum = 0;
    for (const PortalProjection &p : receiver.projections) {
        if (target < cum + p.mass) {
            proj = &p;
            break;
        }
        cum += p.mass;
    }
    s = Clamp((target - cum) / proj->mass, 0.0, oneMinusEps);
    // Choose one of the portal's cell runs proportional to its mass.
    target = s * proj->mass;
    const PortalProjection::Sub *sub = &proj->subs.back();
    cum = 0;
    for (const PortalProjection::Sub &sb : proj->subs) {
        if (target < cum + sb.mass) {
            sub = &sb;
            break;
        }
        cum += sb.mass;
    }
    s = Clamp((target - cum) / sub->mass, 0.0, oneMinusEps);
    // Choose a row of the run by bisection on the prefix sums (O(log R)).
    int j = sub->j0;
    if (sub->j1 - sub->j0 > 1) {
        double rowTotal = RectSum(sub->i0, sub->i1, sub->j0, sub->j1);
        double rt = s * rowTotal;
        int lo = sub->j0, hi = sub->j1;
        while (hi - lo > 1) {
            int mid = (lo + hi) / 2;
            if (RectSum(sub->i0, sub->i1, sub->j0, mid) <= rt)
                lo = mid;
            else
                hi = mid;
        }
        j = lo;
        double before = RectSum(sub->i0, sub->i1, sub->j0, j);
        double cell = RectSum(sub->i0, sub->i1, j, j + 1);
        s = cell > 0 ? Clamp((rt - before) / cell, 0.0, oneMinusEps) : 0;
    }
    // Choose a column within the row, again by bisection (O(log R)).
    int i = sub->i0;
    if (sub->i1 - sub->i0 > 1) {
        double colTotal = RectSum(sub->i0, sub->i1, j, j + 1);
        double ct = s * colTotal;
        int lo = sub->i0, hi = sub->i1;
        while (hi - lo > 1) {
            int mid = (lo + hi) / 2;
            if (RectSum(sub->i0, mid, j, j + 1) <= ct)
                lo = mid;
            else
                hi = mid;
        }
        i = lo;
        double before = RectSum(sub->i0, i, j, j + 1);
        double cell = (double)rho[i + j * R];
        s = cell > 0 ? Clamp((ct - before) / cell, 0.0, oneMinusEps) : 0;
    }
    // Position the sample uniformly inside the (possibly cropped) cell.
    double aLo = std::max((double)proj->a0, -halfPi + i * h);
    double aHi = std::min((double)proj->a1, -halfPi + (i + 1) * h);
    double bLo = std::max((double)proj->b0, -halfPi + j * h);
    double bHi = std::min((double)proj->b1, -halfPi + (j + 1) * h);
    double a = aLo + s * (aHi - aLo);
    double b = bLo + t * (bHi - bLo);
    return Direction((Float)a, (Float)b);
}

Float PortalDistribution::Pdf(const Receiver &receiver,
                              const Vector3f &w) const {
    if (w.z <= 0 || receiver.totalMass <= 0) return 0;
    double a = std::atan2((double)w.x, (double)w.z);
    double b = std::atan2((double)w.y, (double)w.z);
    // Sum the contributions of every portal covering this direction.
    int n = 0;
    for (const PortalProjection &p : receiver.projections)
        if (a >= p.a0 && a <= p.a1 && b >= p.b0 && b <= p.b1) ++n;
    if (n == 0) return 0;
    const int R = resolution;
    const double h = Pi / R;
    int i = Clamp((int)((a + Pi / 2) / h), 0, R - 1);
    int j = Clamp((int)((b + Pi / 2) / h), 0, R - 1);
    double density = (double)rho[i + j * R] * n /
                     (receiver.totalMass * Jacobian((Float)a, (Float)b));
    return (Float)density;
}

}  // namespace pbrt
