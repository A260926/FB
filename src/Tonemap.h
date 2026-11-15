#pragma once

#include <Canvas.h>
#include <glm/glm.hpp>

// A small colour-management / exposure module: work in linear light, apply an
// exposure (in photographic stops), tone-map the high dynamic range down to
// displayable [0,1], then encode to sRGB. Based on Scratchapixel's BRDF /
// linearity / exposure lesson.

// Scale a linear colour by an exposure value in stops (EV): c * 2^ev.
glm::vec3 applyExposure(const glm::vec3 &linear, float ev);

// ACES filmic tone-mapping curve (Narkowicz fit): compresses HDR to [0,1] with a
// filmic shoulder/toe, keeping highlights from clipping harshly.
glm::vec3 acesFilmic(const glm::vec3 &linear);

// Linear <-> sRGB transfer (per channel), for correct display encoding/decoding.
glm::vec3 linearToSRGB(const glm::vec3 &linear);
glm::vec3 sRGBToLinear(const glm::vec3 &srgb);

// Grade a rendered Canvas in place: treat its stored 0xAARRGGBB bytes as linear
// [0,255], apply exposure, tone-map (ACES if aces, else none), and sRGB-encode.
void gradeCanvas(Canvas &canvas, float ev = 0.0f, bool aces = true);
