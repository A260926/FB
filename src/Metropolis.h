#pragma once

#include "Camera.h"
#include <Canvas.h>
#include <ModelTriangle.h>
#include <vector>

// Primary-sample-space Metropolis light transport (PSSMLT) for a diffuse scene.
// The path tracer is viewed as a function of a vector of [0,1) random numbers;
// a Metropolis-Hastings Markov chain explores that space, preferentially
// mutating toward high-contribution paths and depositing them onto the image.
// A bootstrap pass estimates the normalisation so the result is correctly
// exposed. `mutations` is the total chain length across all chains.
void renderMetropolis(const std::vector<ModelTriangle> &model, const Camera &camera, Canvas &canvas,
                      int mutations = 4000000, float exposure = 16.0f);
