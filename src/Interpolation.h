#pragma once

#include <glm/glm.hpp>
#include <vector>

// Evenly spaced values from `from` to `to` inclusive. Guards numberOfValues <= 1.
std::vector<float> interpolateSingleFloats(float from, float to, size_t numberOfValues);

// Evenly spaced vec3 values from `from` to `to` inclusive. Guards numberOfValues <= 1.
std::vector<glm::vec3> interpolateThreeElementValues(const glm::vec3 &from, const glm::vec3 &to, int numberOfValues);

// Scalar linear map of x from [x1, x2] onto [y1, y2]. Degenerate source range -> y1.
float interpolation(float x, float x1, float x2, float y1, float y2);
