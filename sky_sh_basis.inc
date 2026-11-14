//
// sky_sh_basis.inc
//
// Shared definition of the 7-band (49 coefficient) real spherical harmonic
// basis used by the SunSkySH diffuse lighting path. The same code is compiled
// as C++ (see SunSkySH.cpp and tests/sky_sh_target.cpp) and as GLSL (see
// sky.sh), so the CPU and shader cannot drift apart.
//
// Coefficient/index convention:
//
//   index(l, m) = l*l + l + m,   0 <= l <= 6,  -l <= m <= l
//
//   N(l, m) = sqrt((2l+1)/(4*pi) * (l-|m|)! / (l+|m|)!)
//   m = 0 : Y = N * P_l^m(cos theta)
//   m > 0 : Y = sqrt(2) * N * P_l^m(cos theta) * cos(m phi)
//   m < 0 : Y = sqrt(2) * N * P_l^|m|(cos theta) * sin(|m| phi)
//
// P_l^m includes the Condon-Shortley phase, phi = atan2(y, x), +Z is up.
//
// skySHKernelWeight() returns the rotationally symmetric cosine-power
// convolution weight for band l at roughness r:
//
//   r = 0 -> 1 for every band
//   r > 0 -> n = 2/r^2 - 1, weight(l) = (n+1) * integral_0^1 t^n P_l(t) dt
//
// GPU textures produced by SkySH::FillTexture() already hold coefficients
// multiplied by these weights, so shader code must not convolve again.
//

#ifndef SKY_SH_BASIS_INC
#define SKY_SH_BASIS_INC

#define SKY_SH_BAND_COUNT 7
#define SKY_SH_COEFF_COUNT 49

#ifdef __cplusplus
#ifndef SKY_SH_FORCE_GLSL

#include <cmath>

namespace SkySHBasis
{
    constexpr int kBandCount  = SKY_SH_BAND_COUNT;
    constexpr int kCoeffCount = SKY_SH_COEFF_COUNT;

    inline int Index(int l, int m)
    {
        return l * l + l + m;
    }

    inline float Normalizer(int l, int am)
    {
        // (l-am)! / (l+am)! built as a product so no factorial table is needed.
        double ratio = 1.0;
        for (int k = l - am + 1; k <= l + am; ++k)
            ratio /= double(k);

        return float(std::sqrt(double(2 * l + 1) / (4.0 * 3.14159265358979323846) * ratio));
    }

    inline void StoreBand(int l, int am, float plm, float phi, float out[kCoeffCount])
    {
        const float n = Normalizer(l, am);

        if (am == 0)
        {
            out[Index(l, 0)] = n * plm;
        }
        else
        {
            const float s = 1.4142135623730951f * n * plm;  // sqrt(2)
            out[Index(l,  am)] = s * std::cos(float(am) * phi);
            out[Index(l, -am)] = s * std::sin(float(am) * phi);
        }
    }

    // Evaluate all 49 real SH basis functions for unit direction (x, y, z).
    inline void EvalBasis(float x, float y, float z, float out[kCoeffCount])
    {
        for (int i = 0; i < kCoeffCount; ++i)
            out[i] = 0.0f;

        if (z > 1.0f) z = 1.0f;
        else if (z < -1.0f) z = -1.0f;

        const float phi = std::atan2(y, x);
        const float sinTheta = std::sqrt(std::max(0.0f, 1.0f - z * z));

        // P_m^m with Condon-Shortley phase, then the standard (l, m) recurrence.
        float pmm = 1.0f;

        for (int m = 0; m < kBandCount; ++m)
        {
            if (m > 0)
                pmm = -(2.0f * float(m) - 1.0f) * sinTheta * pmm;

            StoreBand(m, m, pmm, phi, out);

            if (m + 1 < kBandCount)
            {
                float pPrev = pmm;
                float pCur = float(2 * m + 1) * z * pmm;   // P_{m+1}^m

                StoreBand(m + 1, m, pCur, phi, out);

                for (int l = m + 2; l < kBandCount; ++l)
                {
                    const float pNext =
                        (float(2 * l - 1) * z * pCur - float(l + m - 1) * pPrev)
                        / float(l - m);

                    StoreBand(l, m, pNext, phi, out);

                    pPrev = pCur;
                    pCur = pNext;
                }
            }
        }
    }

    // Cosine-power convolution weight for band l.
    inline float KernelWeight(int l, float r)
    {
        if (r <= 0.0f)
            return 1.0f;
        if (r > 1.0f)
            r = 1.0f;

        const float n = 2.0f / (r * r) - 1.0f;

        // Moments M_k = integral_0^1 t^(n+k) dt = 1 / (n+k+1).
        const float m0 = 1.0f / (n + 1.0f);
        const float m1 = 1.0f / (n + 2.0f);
        const float m2 = 1.0f / (n + 3.0f);
        const float m3 = 1.0f / (n + 4.0f);
        const float m4 = 1.0f / (n + 5.0f);
        const float m5 = 1.0f / (n + 6.0f);
        const float m6 = 1.0f / (n + 7.0f);

        float integral;

        switch (l)
        {
        case 0:
            integral = m0;
            break;
        case 1:
            integral = m1;
            break;
        case 2:
            integral = 0.5f * (3.0f * m2 - m0);
            break;
        case 3:
            integral = 0.5f * (5.0f * m3 - 3.0f * m1);
            break;
        case 4:
            integral = 0.125f * (35.0f * m4 - 30.0f * m2 + 3.0f * m0);
            break;
        case 5:
            integral = 0.125f * (63.0f * m5 - 70.0f * m3 + 15.0f * m1);
            break;
        default:
            integral = 0.0625f * (231.0f * m6 - 315.0f * m4 + 105.0f * m2 - 5.0f * m0);
            break;
        }

        return (n + 1.0f) * integral;
    }

    // Reconstruct a scalar signal from its SH coefficients, optionally with
    // the roughness kernel weights applied. This is exactly what the GLSL
    // skyColourSH() entry does.
    inline float Reconstruct
    (
        const float coefficients[kCoeffCount],
        float x, float y, float z,
        float roughness = 0.0f
    )
    {
        float basis[kCoeffCount];
        EvalBasis(x, y, z, basis);

        float result = 0.0f;

        for (int l = 0; l < kBandCount; ++l)
        {
            const float w = KernelWeight(l, roughness);

            for (int m = -l; m <= l; ++m)
            {
                const int i = Index(l, m);
                result += coefficients[i] * w * basis[i];
            }
        }

        return result;
    }
}

#endif  // !SKY_SH_FORCE_GLSL
#endif  // __cplusplus C++ branch

#if !defined(__cplusplus) || defined(SKY_SH_FORCE_GLSL)

// GLSL branch
int skySHIndex(int l, int m)
{
    return l * l + l + m;
}

float skySHNormalizer(int l, int am)
{
    // Fixed loop bound so this stays legal in GLSL 1.20.
    float ratio = 1.0;
    for (int k = 1; k <= 2 * SKY_SH_BAND_COUNT; k++)
    {
        if (k > l - am && k <= l + am)
            ratio /= float(k);
    }

    return sqrt(float(2 * l + 1) / (4.0 * 3.14159265358979323846) * ratio);
}

void skySHStoreBand(int l, int am, float plm, float phi, inout float basis[SKY_SH_COEFF_COUNT])
{
    float n = skySHNormalizer(l, am);

    if (am == 0)
    {
        basis[skySHIndex(l, 0)] = n * plm;
    }
    else
    {
        float s = 1.4142135623730951 * n * plm;  // sqrt(2)
        basis[skySHIndex(l,  am)] = s * cos(float(am) * phi);
        basis[skySHIndex(l, -am)] = s * sin(float(am) * phi);
    }
}

// Evaluate all 49 real SH basis functions for unit direction d.
void skySHEvalBasis(vec3 d, out float basis[SKY_SH_COEFF_COUNT])
{
    for (int i = 0; i < SKY_SH_COEFF_COUNT; i++)
        basis[i] = 0.0;

    float z = clamp(d.z, -1.0, 1.0);
    float phi = atan(d.y, d.x);
    float sinTheta = sqrt(max(0.0, 1.0 - z * z));

    // P_m^m with Condon-Shortley phase, then the standard (l, m) recurrence.
    float pmm = 1.0;

    for (int m = 0; m < SKY_SH_BAND_COUNT; m++)
    {
        if (m > 0)
            pmm = -(2.0 * float(m) - 1.0) * sinTheta * pmm;

        skySHStoreBand(m, m, pmm, phi, basis);

        if (m + 1 < SKY_SH_BAND_COUNT)
        {
            float pPrev = pmm;
            float pCur = float(2 * m + 1) * z * pmm;   // P_{m+1}^m

            skySHStoreBand(m + 1, m, pCur, phi, basis);

            for (int l = m + 2; l < SKY_SH_BAND_COUNT; l++)
            {
                float pNext =
                    (float(2 * l - 1) * z * pCur - float(l + m - 1) * pPrev)
                    / float(l - m);

                skySHStoreBand(l, m, pNext, phi, basis);

                pPrev = pCur;
                pCur = pNext;
            }
        }
    }
}

// Cosine-power convolution weight for band l.
float skySHKernelWeight(int l, float r)
{
    if (r <= 0.0)
        return 1.0;
    if (r > 1.0)
        r = 1.0;

    float n = 2.0 / (r * r) - 1.0;

    // Moments M_k = integral_0^1 t^(n+k) dt = 1 / (n+k+1).
    float m0 = 1.0 / (n + 1.0);
    float m1 = 1.0 / (n + 2.0);
    float m2 = 1.0 / (n + 3.0);
    float m3 = 1.0 / (n + 4.0);
    float m4 = 1.0 / (n + 5.0);
    float m5 = 1.0 / (n + 6.0);
    float m6 = 1.0 / (n + 7.0);

    float integral;

    if (l == 0)
        integral = m0;
    else if (l == 1)
        integral = m1;
    else if (l == 2)
        integral = 0.5 * (3.0 * m2 - m0);
    else if (l == 3)
        integral = 0.5 * (5.0 * m3 - 3.0 * m1);
    else if (l == 4)
        integral = 0.125 * (35.0 * m4 - 30.0 * m2 + 3.0 * m0);
    else if (l == 5)
        integral = 0.125 * (63.0 * m5 - 70.0 * m3 + 15.0 * m1);
    else
        integral = 0.0625 * (231.0 * m6 - 315.0 * m4 + 105.0 * m2 - 5.0 * m0);

    return (n + 1.0) * integral;
}

#endif  // __cplusplus

#endif  // SKY_SH_BASIS_INC
