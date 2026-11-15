#pragma once

#include "Camera.h"
#include <Canvas.h>
#include <ModelTriangle.h>
#include <vector>

// Classic finite-element radiosity for diffuse scenes. Subdivides the model into
// `subdivLevel` levels of patches, adds a bright ceiling emitter, solves the
// radiosity equation B_i = E_i + rho_i * <incoming B> by Monte-Carlo gathering
// (`samples` hemisphere rays per patch, `iterations` Jacobi sweeps), then
// renders each camera ray by displaying the hit patch's converged radiosity.
// A view-independent, noise-free (given enough samples) global-illumination
// solution with the classic Cornell colour bleeding.
// progressive = true uses the shooting solver (Cohen-Greenberg progressive
// refinement) instead of Monte-Carlo Jacobi gathering.
void renderRadiosity(const std::vector<ModelTriangle> &model, const Camera &camera, Canvas &canvas, int subdivLevel = 3,
                     int iterations = 8, int samples = 200, bool progressive = false);
