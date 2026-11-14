//
// SunSkySH.cpp
//
// Full-direction-coupled sky diffuse lighting via a 7-band real spherical
// harmonic projection of the complete sphere. See SunSkySH.hpp for the
// interface contract and sky_sh_basis.inc for the basis/kernel definition
// shared with the shader.
//

#include "SunSkySH.hpp"
#include "sky_sh_basis.inc"

#include <vector>
#include <cmath>

namespace
{
    const double kPiLocal = 3.14159265358979323846;

    // Gauss-Legendre nodes/weights on [-1, 1], found by Newton iteration on
    // the Legendre polynomial roots.
    void GaussLegendre(int n, std::vector<double>& nodes, std::vector<double>& weights)
    {
        nodes.resize(n);
        weights.resize(n);

        for (int i = 0; i < n; ++i)
        {
            // Initial (asymptotic) guess for the i-th root.
            double x = -std::cos(double(2 * i + 1) * kPiLocal / double(2 * n));

            for (int iteration = 0; iteration < 100; ++iteration)
            {
                // Evaluate P_n(x) and P_{n-1}(x) via the recurrence.
                double pPrev = 1.0;
                double pCur = x;

                for (int k = 2; k <= n; ++k)
                {
                    double pNext = (double(2 * k - 1) * x * pCur - double(k - 1) * pPrev) / double(k);
                    pPrev = pCur;
                    pCur = pNext;
                }

                double dp = double(n) * (x * pCur - pPrev) / (x * x - 1.0);
                double xOld = x;

                x -= pCur / dp;

                if (std::fabs(x - xOld) < 1e-15)
                    break;
            }

            // Recompute P_n, P_{n-1} at the converged root for the weight.
            double pPrev = 1.0;
            double pCur = x;

            for (int k = 2; k <= n; ++k)
            {
                double pNext = (double(2 * k - 1) * x * pCur - double(k - 1) * pPrev) / double(k);
                pPrev = pCur;
                pCur = pNext;
            }

            double dp = double(n) * (x * pCur - pPrev) / (x * x - 1.0);

            nodes[i] = x;
            weights[i] = 2.0 / ((1.0 - x * x) * dp * dp);
        }
    }

    int BandOfIndex(int i)
    {
        int l = 0;
        while ((l + 1) * (l + 1) <= i)
            ++l;
        return l;
    }

    struct HemiPoint
    {
        double z;
        double zWeight;
    };
}

namespace SSLib
{
    namespace
    {
        // Per-hemisphere Gauss-Legendre orders. A separate rule is affinely
        // mapped onto [0, 1] and [-1, 0] rather than one rule over [-1, 1],
        // so no node straddles the radiance step at the horizon.
        const int kThetaNodes = 120;
        const int kPhiNodes   = 160;
    }

    SkySH::SkySH()
    {
        for (int i = 0; i < kSkySHCoeffCount; ++i)
            mCoefficients[i] = Vec3f(vl_0);

        mEsky = Vec3f(vl_0);
    }

    void SkySH::Project(const SkyHosek& sky, const Vec3f& groundAlbedoRGB)
    {
        std::vector<double> tNodes;
        std::vector<double> tWeights;
        std::vector<double> phiNodes;
        std::vector<double> phiWeights;

        GaussLegendre(kThetaNodes, tNodes, tWeights);
        GaussLegendre(kPhiNodes,   phiNodes, phiWeights);

        // Map the phi nodes from [-1, 1] to [0, 2 pi]. The transformed weight
        // carries the pi Jacobian.
        for (int i = 0; i < kPhiNodes; ++i)
        {
            phiNodes[i] = (phiNodes[i] + 1.0) * kPiLocal;
            phiWeights[i] *= kPiLocal;
        }

        // One theta node set is affinely mapped into each hemisphere.
        // upper: z = 0.5 (t + 1), lower: z = 0.5 (t - 1), weight *= 0.5.
        std::vector<HemiPoint> upper(kThetaNodes);
        std::vector<HemiPoint> lower(kThetaNodes);

        for (int i = 0; i < kThetaNodes; ++i)
        {
            upper[i].z       = 0.5 * (tNodes[i] + 1.0);
            upper[i].zWeight = 0.5 * tWeights[i];
            lower[i].z       = 0.5 * (tNodes[i] - 1.0);
            lower[i].zWeight = 0.5 * tWeights[i];
        }

        // Pass 1: upper-hemisphere sky irradiance
        //   Esky = integral_{z >= 0} SkyRGB(d) * z dOmega.
        // With t = cos(theta), dOmega = dphi dt.
        double esky[3] = { 0.0, 0.0, 0.0 };

        for (int ti = 0; ti < kThetaNodes; ++ti)
        {
            const double z = upper[ti].z;
            const double sinTheta = std::sqrt(1.0 - z * z);

            for (int pi = 0; pi < kPhiNodes; ++pi)
            {
                const double phi = phiNodes[pi];
                const double w = upper[ti].zWeight * phiWeights[pi];

                Vec3f radiance = sky.SkyRGB(Vec3f
                (
                    float(sinTheta * std::cos(phi)),
                    float(sinTheta * std::sin(phi)),
                    float(z)
                ));

                esky[0] += double(radiance.x) * z * w;
                esky[1] += double(radiance.y) * z * w;
                esky[2] += double(radiance.z) * z * w;
            }
        }

        // Constant lower-hemisphere ground radiance, per channel, linear RGB.
        const Vec3f groundRadiance
        (
            groundAlbedoRGB.x * float(esky[0] / kPiLocal),
            groundAlbedoRGB.y * float(esky[1] / kPiLocal),
            groundAlbedoRGB.z * float(esky[2] / kPiLocal)
        );

        // Pass 2: full-sphere SH projection of the piecewise radiance.
        double accum[kSkySHCoeffCount][3];

        for (int i = 0; i < kSkySHCoeffCount; ++i)
            for (int c = 0; c < 3; ++c)
                accum[i][c] = 0.0;

        for (int hemisphere = 0; hemisphere < 2; ++hemisphere)
        {
            const std::vector<HemiPoint>& points = hemisphere == 0 ? upper : lower;

            for (int ti = 0; ti < kThetaNodes; ++ti)
            {
                const double z = points[ti].z;
                const double sinTheta = std::sqrt(std::max(0.0, 1.0 - z * z));

                for (int pi = 0; pi < kPhiNodes; ++pi)
                {
                    const double phi = phiNodes[pi];
                    const double w = points[ti].zWeight * phiWeights[pi];

                    const float fx = float(sinTheta * std::cos(phi));
                    const float fy = float(sinTheta * std::sin(phi));
                    const float fz = float(z);

                    const Vec3f radiance = hemisphere == 0
                        ? sky.SkyRGB(Vec3f(fx, fy, fz))
                        : groundRadiance;

                    float basis[kSkySHCoeffCount];
                    SkySHBasis::EvalBasis(fx, fy, fz, basis);

                    for (int i = 0; i < kSkySHCoeffCount; ++i)
                    {
                        const double b = double(basis[i]) * w;

                        accum[i][0] += double(radiance.x) * b;
                        accum[i][1] += double(radiance.y) * b;
                        accum[i][2] += double(radiance.z) * b;
                    }
                }
            }
        }

        // Replace the whole snapshot.
        for (int i = 0; i < kSkySHCoeffCount; ++i)
            mCoefficients[i] = Vec3f(float(accum[i][0]), float(accum[i][1]), float(accum[i][2]));

        mEsky = Vec3f(float(esky[0]), float(esky[1]), float(esky[2]));
    }

    const Vec3f* SkySH::Coefficients() const
    {
        return mCoefficients;
    }

    void SkySH::LoadCoefficients(const Vec3f coefficients[kSkySHCoeffCount])
    {
        for (int i = 0; i < kSkySHCoeffCount; ++i)
            mCoefficients[i] = coefficients[i];

        mEsky = Vec3f(vl_0);
    }

    Vec3f SkySH::Evaluate(const Vec3f& unitDirection, float roughness) const
    {
        float basis[kSkySHCoeffCount];
        SkySHBasis::EvalBasis(unitDirection.x, unitDirection.y, unitDirection.z, basis);

        Vec3f result(vl_0);

        for (int l = 0; l < kSkySHBands; ++l)
        {
            const float weight = SkySHBasis::KernelWeight(l, roughness);

            for (int m = -l; m <= l; ++m)
            {
                const int i = SkySHBasis::Index(l, m);
                result += mCoefficients[i] * (weight * basis[i]);
            }
        }

        return result;
    }

    void SkySH::FillTexture(float rgba[kSkySHCoeffCount][4], float roughness) const
    {
        for (int i = 0; i < kSkySHCoeffCount; ++i)
        {
            const int l = BandOfIndex(i);
            const float weight = SkySHBasis::KernelWeight(l, roughness);
            const Vec3f weighted = mCoefficients[i] * weight;

            rgba[i][0] = weighted.x;
            rgba[i][1] = weighted.y;
            rgba[i][2] = weighted.z;
            rgba[i][3] = 0.0f;
        }
    }

    Vec3f SkySH::SkyIrradiance() const
    {
        return mEsky;
    }
}
