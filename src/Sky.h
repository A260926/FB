#pragma once

#include <glm/glm.hpp>

// Sky.h
//
// A compact physically-based atmospheric sky model using Rayleigh and Mie
// single scattering (Nishita / Preetham style). Given a normalised view
// direction and a normalised direction to the sun it returns a linear RGB
// sky colour. The result is blue overhead, brighter and warmer toward the
// horizon and around the sun, and turns red when the sun is low (sunset).
//
// The returned colour is linear RGB and is NOT clamped to 1: the sun disc
// and the region immediately around it can be significantly brighter than
// white so that a tone mapper can recover a plausible highlight.

// Compute the sky colour for a given view direction.
//
//  dir       normalised view direction. dir.y > 0 points up into the sky,
//            dir.y <= 0 points toward or below the horizon.
//  sunDir    normalised direction TO the sun.
//  turbidity haze factor. ~2 is a very clear sky, higher values add more
//            Mie (aerosol) scattering for a hazier look.
//
// Returns a linear (unclamped) RGB colour.
glm::vec3 skyColour(const glm::vec3 &dir, const glm::vec3 &sunDir, float turbidity = 2.0f);

// Compute the attenuated colour of the sun as seen through the atmosphere,
// suitable for use as the colour of a directional light. As the sun drops
// toward the horizon the extra air mass removes blue light and the result
// warms toward orange and red.
//
//  sunDir    normalised direction TO the sun.
//
// Returns a linear RGB colour.
glm::vec3 sunLight(const glm::vec3 &sunDir);
