#include "Sky.h"

#include <cmath>
#include <algorithm>

// Sky.cpp
//
// Compact single-scattering atmospheric sky. The approach follows the
// classic "Simulating the Colors of the Sky" / Nishita single-scattering
// model, reduced to a closed form that does not require ray marching:
//
//   1. Rayleigh scattering is wavelength dependent. Short (blue) wavelengths
//      scatter far more than long (red) ones, which is why the daytime sky
//      is blue and why the low sun looks red once its light has travelled
//      through a lot of air. We use the well known beta coefficients
//      (roughly proportional to 1 / lambda^4) for R, G and B.
//
//   2. Mie scattering is caused by larger aerosol particles. It is nearly
//      wavelength independent (grey) and strongly forward biased, which
//      creates the bright warm halo hugging the sun.
//
//   3. Both are combined with an air-mass / optical-depth approximation
//      derived from the view and sun zenith angles. More air mass means
//      more extinction of the direct sunlight and more in-scattered light
//      along the view ray.
//
// Everything below depends only on glm and <cmath>.

namespace {

// Pi as a float. Defined locally because M_PI is not part of standard C++.
const float kPi = 3.14159265358979323846f;

// Rayleigh scattering coefficients (per metre) at sea level for R, G, B.
// Blue is scattered the most, red the least. These are the standard values
// used by most real time atmosphere implementations.
const glm::vec3 kBetaR = glm::vec3(5.8e-6f, 13.5e-6f, 33.1e-6f);

// Mie scattering coefficient (per metre). Grey, i.e. the same for all three
// channels. Scaled by turbidity to represent haze.
const float kBetaMBase = 21e-6f;

// Mie asymmetry factor. Positive and close to 1 gives strong forward
// scattering, which produces the bright glow around the sun.
const float kMieG = 0.76f;

// Nominal atmospheric thickness parameters (in metres). These act as the
// integrated air column depth for a ray looking straight up. The actual
// depth for a given direction is obtained by multiplying by an air-mass
// factor that grows as the direction approaches the horizon.
const float kRayleighDepth = 8000.0f; // Rayleigh scale height column.
const float kMieDepth = 1200.0f;      // Mie scale height column.

// Overall intensity of the incoming sunlight before attenuation.
const float kSunIntensity = 22.0f;

// Rayleigh phase function: describes the angular distribution of Rayleigh
// scattered light. Symmetric, of the form (3/16pi)(1 + cos^2 theta).
float rayleighPhase(float cosTheta) {
	return (3.0f / (16.0f * kPi)) * (1.0f + cosTheta * cosTheta);
}

// Henyey-Greenstein phase function used as the Mie phase. Strongly peaked
// in the forward (toward the sun) direction for g close to 1.
float miePhase(float cosTheta, float g) {
	float g2 = g * g;
	float denom = 1.0f + g2 - 2.0f * g * cosTheta;
	// Guard against a tiny or negative denominator from floating point error.
	denom = std::max(denom, 1e-4f);
	return (1.0f - g2) / (4.0f * kPi * std::pow(denom, 1.5f));
}

// Air-mass approximation (Rozenberg style) that stays finite at the
// horizon. Returns a multiplier (>= 1) for the vertical optical depth as a
// function of the cosine of the zenith angle. Looking straight up
// (cosZenith = 1) gives ~1, and it grows rapidly as the ray tends toward
// and past the horizon without diverging to infinity. cosZenith is the
// cosine of the angle from the vertical (up, +y) axis.
float airMassRozenberg(float cosZenith) {
	float c = std::max(cosZenith, -0.1f);
	return 1.0f / (c + 0.15f * std::pow(93.885f - std::acos(std::clamp(c, -1.0f, 1.0f)) * 180.0f / kPi, -1.253f));
}

} // namespace

glm::vec3 skyColour(const glm::vec3 &dir, const glm::vec3 &sunDir, float turbidity) {
	// Cosine of the angle between the view ray and the sun. This drives both
	// phase functions: 1 means looking straight at the sun.
	float cosTheta = glm::dot(dir, sunDir);
	cosTheta = std::clamp(cosTheta, -1.0f, 1.0f);

	// Zenith cosines (angle from the up axis, +y). Used for the air mass of
	// the view ray and of the sunlight reaching the scattering point.
	float cosViewZenith = dir.y;
	float cosSunZenith = sunDir.y;

	// Air-mass multipliers. The view path controls how much atmosphere we
	// look through; the sun path controls how reddened the incoming light
	// is before it scatters toward us.
	float viewAir = airMassRozenberg(cosViewZenith);
	float sunAir = airMassRozenberg(cosSunZenith);

	// Mie coefficient scaled by turbidity (haze).
	float betaM = kBetaMBase * std::max(turbidity, 1.0f) * 0.5f;
	glm::vec3 betaMVec = glm::vec3(betaM);

	// Optical depth (extinction) along the view ray for each channel.
	// depth = coefficient * columnHeight * airMass.
	glm::vec3 tauR = kBetaR * kRayleighDepth * viewAir;
	glm::vec3 tauM = betaMVec * kMieDepth * viewAir;

	// Optical depth for the sunlight travelling down to the scattering
	// column. This is what reddens a low sun.
	glm::vec3 sunTauR = kBetaR * kRayleighDepth * sunAir;
	glm::vec3 sunTauM = betaMVec * kMieDepth * sunAir;

	// Transmittance of the incoming sunlight (Beer-Lambert). Blue is removed
	// fastest, so a long sun path leaves warm, reddened light.
	glm::vec3 sunTransmittance = glm::exp(-(sunTauR + sunTauM));

	// Phase functions.
	float phaseR = rayleighPhase(cosTheta);
	float phaseM = miePhase(cosTheta, kMieG);

	// In-scattered light. The (1 - exp(-tau)) term is the amount of light
	// scattered into the eye along the view ray for each mechanism, times
	// the phase function and the (reddened) incoming sun colour.
	glm::vec3 scatterR = (glm::vec3(1.0f) - glm::exp(-tauR)) * phaseR;
	glm::vec3 scatterM = (glm::vec3(1.0f) - glm::exp(-tauM)) * phaseM;

	// Weight each mechanism by its scattering coefficient so the relative
	// colours stay physical (Rayleigh stays blue biased, Mie stays grey).
	glm::vec3 inscatter = scatterR * kBetaR / (kBetaR + betaMVec) + scatterM * betaMVec / (kBetaR + betaMVec);

	glm::vec3 colour = kSunIntensity * sunTransmittance * inscatter;

	// Add an explicit bright sun disc / near-sun glow so that looking almost
	// straight at the sun returns a bright warm value. The exponent makes it
	// fall off quickly away from the solar direction.
	if (cosTheta > 0.0f) {
		float disc = std::pow(cosTheta, 1200.0f);              // sharp central disc
		float glow = std::pow(std::max(cosTheta, 0.0f), 8.0f); // soft surrounding glow
		glm::vec3 sunCol = sunTransmittance * kSunIntensity;
		colour += sunCol * (disc * 40.0f + glow * 0.6f);
	}

	// Guarantee non-negative output.
	colour = glm::max(colour, glm::vec3(0.0f));
	return colour;
}

glm::vec3 sunLight(const glm::vec3 &sunDir) {
	// Air mass of the sun path.
	float sunAir = airMassRozenberg(sunDir.y);

	// Extinction of the direct beam. Same coefficients as the sky model.
	float betaM = kBetaMBase * 0.5f;
	glm::vec3 tau = kBetaR * kRayleighDepth * sunAir + glm::vec3(betaM) * kMieDepth * sunAir;

	glm::vec3 transmittance = glm::exp(-tau);

	// Scale to a usable directional-light intensity.
	return transmittance * kSunIntensity * 0.6f;
}
