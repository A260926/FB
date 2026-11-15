#pragma once

#include <glm/glm.hpp>

// Worley (cellular) noise, after Steven Worley's "A Cellular Texture Basis
// Function" and ryg's cellular-texture notes. Space is divided into a unit
// integer grid; each cell holds one deterministic jittered feature point. For a
// query point we scan the cell it falls in plus its immediate neighbours and
// measure the Euclidean distance to every feature point, keeping the two
// smallest: f1 (nearest) and f2 (second nearest). Everything is a pure function
// of the input, so the same point always yields the same result (no RNG at call
// time). `density` scales the input, i.e. the number of cells per unit.
struct WorleyResult {
	float f1;
	float f2;
};

// 2D cellular noise. Scans the 3x3 block of cells around the query point.
WorleyResult worley(const glm::vec2 &p, float density = 1.0f);

// Convenience: just the nearest-feature distance f1.
float worleyF1(const glm::vec2 &p, float density = 1.0f);

// ryg's brightness-equalised cell value (f2 - f1) / (f2 + f1). This is 0 at cell
// centres and rises towards 1 near cell borders, giving crisp cell walls that do
// not fade with distance. Guarded against divide-by-zero (returns 0).
float worleyCells(const glm::vec2 &p, float density = 1.0f);

// 3D cellular noise. Scans the 3x3x3 block of cells around the query point.
WorleyResult worley(const glm::vec3 &p, float density = 1.0f);
