#pragma once

#include <glm/glm.hpp>

// Blackbody radiation to linear RGB.
//
// Converts a colour temperature in Kelvin into a linear RGB colour lying on the
// Planckian locus (the path a heated blackbody radiator traces through colour
// space). This lets lights and emitters use physically-plausible colours, for
// example 2700K warm tungsten, 6500K daylight, or 10000K cool blue.
//
// The returned colour is LINEAR RGB (not gamma-encoded sRGB) and is normalised
// so that its maximum component is approximately 1. Multiply by an intensity /
// radiance factor before feeding it into a physically based light.
//
// Valid roughly over 1000K to 40000K. Inputs outside that range are clamped.

// Normalised linear RGB (max component ~1) for the given temperature.
glm::vec3 blackbodyRGB(float kelvin);

// Same as above, then scaled uniformly by `intensity`.
glm::vec3 blackbodyRGB(float kelvin, float intensity);
