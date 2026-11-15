#pragma once

#include <ModelTriangle.h>
#include <string>
#include <vector>

// Parse a Wavefront .obj file. Resolves its own `mtllib` reference (relative to
// the .obj) and applies `usemtl` diffuse colours to the faces that follow.
std::vector<ModelTriangle> loadOBJ(const std::string &filename, float scale);
