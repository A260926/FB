#pragma once

#include "Camera.h"
#include <Canvas.h>
#include <ModelTriangle.h>
#include <vector>

// Bidirectional path tracing (diffuse). Each sample traces a subpath from the
// camera and a subpath from a ceiling area light, then connects every pair of
// vertices with a visibility ray and the geometry term, summing the strategies
// with a uniform MIS weight. Connecting from both ends captures light paths a
// unidirectional tracer samples poorly. `exposure` scales the physically-based
// result for display.
void renderBidirectional(const std::vector<ModelTriangle> &model, const Camera &camera, Canvas &canvas,
                         int samples = 64, float exposure = 40.0f);
