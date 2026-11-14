//
// SunSkySH.hpp
//
// Full-direction-coupled sky diffuse lighting path. Unlike SkyBRDF, which
// convolves the separable theta and gamma Hosek terms independently, SkySH
// projects the radiance of the complete sphere onto a 7-band (49 coefficient)
// real spherical harmonic basis, so the non-axisymmetric components created
// by the product of the theta and gamma terms are retained.
//
// The lower hemisphere carries coloured ground bounce: a constant radiance
// equal to the given linear RGB ground albedo times the upper-hemisphere sky
// irradiance Esky / pi.
//
// The legacy SkyTable/SkyBRDF classes and their texture layouts are
// unaffected.
//

#ifndef SUN_SKY_SH_H
#define SUN_SKY_SH_H

#include "VL234f.hpp"
#include "SunSky.hpp"

namespace SSLib
{
    constexpr int kSkySHBands       = 7;
    constexpr int kSkySHCoeffCount  = kSkySHBands * kSkySHBands;   // 49

    class SkySH
    {
    public:
        SkySH();

        // Project the full-sphere radiance of the given Hosek model.
        //
        // z >= 0: radiance is sky.SkyRGB(d) (sky diffuse only, no sun disc).
        // z <  0: radiance is groundAlbedoRGB * Esky / pi per channel, where
        //         Esky is the solid-angle integral of SkyRGB(d) * d.z over
        //         the upper hemisphere. groundAlbedoRGB is linear RGB with
        //         components in [0, 1]; the model's XYZ mAlbedo is not used.
        //
        // The result is snapshotted: later changes to the model do not affect
        // this object, and a subsequent Project() replaces every coefficient.
        void        Project(const SkyHosek& sky, const Vec3f& groundAlbedoRGB);

        // Access the 49 linear RGB coefficients, indexed l*l + l + m with
        // 0 <= l <= 6 and -l <= m <= l.
        const Vec3f* Coefficients() const;

        // Replace the snapshot with externally supplied coefficients.
        void        LoadCoefficients(const Vec3f coefficients[kSkySHCoeffCount]);

        // Band-limited reconstruction of the radiance convolved with the
        // rotationally symmetric cosine-power kernel for the given roughness.
        // roughness is clamped to [0, 1]; unitDirection must be a finite unit
        // vector. Negative and HDR values are preserved.
        Vec3f       Evaluate(const Vec3f& unitDirection, float roughness) const;

        // Fill a 49 x 1 RGBAF32 texture: pixel i holds coefficient i already
        // multiplied by the kernel weight of its band, with alpha = 0. With
        // the default roughness of 0 the raw coefficients are exported. RGB is
        // not quantised. Shader code must not convolve these again.
        void        FillTexture(float rgba[kSkySHCoeffCount][4], float roughness = 0.0f) const;

        // Upper-hemisphere sky irradiance snapshotted by the last Project(),
        // per linear RGB channel. Exposed for testing/validation.
        Vec3f       SkyIrradiance() const;

    private:
        Vec3f       mCoefficients[kSkySHCoeffCount];
        Vec3f       mEsky;
    };
}

#endif
