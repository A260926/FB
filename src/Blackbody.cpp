#include "Blackbody.h"

#include <algorithm>
#include <cmath>

// Blackbody radiation to linear RGB.
//
// Pipeline:
//   Kelvin -> CIE 1931 xy chromaticity (Planckian locus approximation)
//          -> CIE XYZ (assuming unit luminance Y = 1)
//          -> linear sRGB (clamp negatives, normalise).
//
// The Planckian locus is approximated with the well-known Kim et al. (2002)
// piecewise cubic for x, and a cubic in x for y. This avoids integrating
// Planck's law against the CIE colour matching functions while staying accurate
// over roughly 1667K to 25000K. We clamp the input to a slightly wider useful
// range so callers cannot produce NaNs at the extremes.

namespace {

// CIE x chromaticity along the Planckian locus as a function of temperature.
// Kim, Weyrich, Kautz "Modeling and Optimization of Adaptive Color Constancy"
// style piecewise fit (a common tabulated approximation). `t` is in Kelvin.
float planckianX(float t) {
	// Work in inverse mega-kelvin friendly terms: coefficients are expressed
	// against 1e9 / t^3, 1e6 / t^2, 1e3 / t.
	const double invT = 1.0 / static_cast<double>(t);
	const double invT2 = invT * invT;
	const double invT3 = invT2 * invT;
	if (t < 4000.0f) {
		// Low colour temperature branch (roughly 1667K to 4000K).
		return static_cast<float>(-0.2661239e9 * invT3 - 0.2343589e6 * invT2 + 0.8776956e3 * invT + 0.179910);
	}
	// High colour temperature branch (roughly 4000K to 25000K).
	return static_cast<float>(-3.0258469e9 * invT3 + 2.1070379e6 * invT2 + 0.2226347e3 * invT + 0.240390);
}

// CIE y chromaticity as a cubic in the already-computed x, using the standard
// three piecewise fits keyed on temperature.
float planckianY(float t, float x) {
	const double x2 = static_cast<double>(x) * x;
	const double x3 = x2 * x;
	if (t < 2222.0f) {
		return static_cast<float>(-1.1063814 * x3 - 1.34811020 * x2 + 2.18555832 * static_cast<double>(x) - 0.20219683);
	}
	if (t < 4000.0f) {
		return static_cast<float>(-0.9549476 * x3 - 1.37418593 * x2 + 2.09137015 * static_cast<double>(x) - 0.16748867);
	}
	return static_cast<float>(3.0817580 * x3 - 5.87338670 * x2 + 3.75112997 * static_cast<double>(x) - 0.37001483);
}

} // namespace

glm::vec3 blackbodyRGB(float kelvin) {
	// Clamp to the useful, numerically safe range.
	const float t = std::clamp(kelvin, 1000.0f, 40000.0f);

	// Step 1: chromaticity on the Planckian locus.
	const float x = planckianX(t);
	const float y = planckianY(t, x);

	// Step 2: xy + luminance Y=1 -> CIE XYZ.
	// Guard against a degenerate y (never happens in range, but be safe).
	const float yy = (std::abs(y) < 1e-6f) ? 1e-6f : y;
	const float X = x / yy; // X = (x / y) * Y, with Y = 1
	const float Y = 1.0f;
	const float Z = (1.0f - x - y) / yy;

	// Step 3: CIE XYZ -> linear sRGB (sRGB / Rec.709 primaries, D65 white).
	float r = 3.2404542f * X - 1.5371385f * Y - 0.4985314f * Z;
	float g = -0.9692660f * X + 1.8760108f * Y + 0.0415560f * Z;
	float b = 0.0556434f * X - 0.2040259f * Y + 1.0572252f * Z;

	// Clamp negative components (colours just outside the sRGB gamut).
	r = std::max(0.0f, r);
	g = std::max(0.0f, g);
	b = std::max(0.0f, b);

	// Normalise so the brightest channel is ~1, preserving hue.
	const float maxc = std::max(r, std::max(g, b));
	if (maxc > 0.0f) {
		r /= maxc;
		g /= maxc;
		b /= maxc;
	}

	return glm::vec3(r, g, b);
}

glm::vec3 blackbodyRGB(float kelvin, float intensity) {
	return blackbodyRGB(kelvin) * intensity;
}
