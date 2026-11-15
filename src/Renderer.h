#pragma once

#include "Camera.h"
#include "Light.h"
#include "Scene.h"
#include <Canvas.h>
#include <ModelTriangle.h>
#include <glm/glm.hpp>
#include <vector>

// Flat uses the face normal; Gouraud lights each vertex and interpolates the
// result; Phong interpolates the normal per pixel.
enum class ShadingModel { Flat, Gouraud, Phong };

// Three ways to draw the same model. Each clears the canvas and draws into it.
// Both raster paths clip triangles against the near plane; the rasteriser can
// optionally cull back-facing triangles.
void renderWireframe(const std::vector<ModelTriangle> &model, const Camera &camera, Canvas &canvas);
void renderRasterised(const std::vector<ModelTriangle> &model, const Camera &camera, Canvas &canvas,
                      bool backfaceCull = false);

// Rasteriser with shadow mapping: a depth map rendered from `lightPos` shadows
// the scene, plus simple diffuse lighting.
void renderShadowMapped(const std::vector<ModelTriangle> &model, const Camera &camera, Canvas &canvas,
                        const glm::vec3 &lightPos);

// Rasteriser with stencil shadow volumes (z-fail): occluders are extruded away
// from `lightPos` into volumes counted in a stencil buffer to mark shadowed
// pixels. A different technique from shadow mapping (no depth-map resolution
// limit, exact silhouettes).
void renderStencilShadowVolumes(const std::vector<ModelTriangle> &model, const Camera &camera, Canvas &canvas,
                                const glm::vec3 &lightPos);

// The ray tracer shades each hit over all `lights` (point/directional/spot, with
// soft shadows for area lights), adding specular and an ambient floor; mirror
// and glass materials reflect / refract recursively. An empty `lights` uses a
// single default soft area light.
// Enable/disable the physically-based atmospheric sky as the ray/path tracer
// background (miss rays). sunDir is the direction TO the sun; pass (0,0,0) to
// keep the current sun. When disabled, a simple gradient sky is used.
void setSkyModel(bool enabled, const glm::vec3 &sunDir = glm::vec3(0.0f));

void renderRaytraced(const std::vector<ModelTriangle> &model, const Camera &camera, Canvas &canvas,
                     ShadingModel shading = ShadingModel::Phong, const std::vector<Light> &lights = {},
                     const Primitives &prims = {}, float fogDensity = 0.0f,
                     const glm::vec3 &fogColour = glm::vec3(200.0f, 210.0f, 230.0f));

// Monte-Carlo path tracer: `samples` jittered paths per pixel give global
// illumination (colour bleeding), soft shadows, and anti-aliasing together.
// aperture > 0 enables depth of field focused at `focusDistance`; a non-zero
// cameraMotion adds motion blur along that vector.
// apertureBlades shapes the depth-of-field bokeh: 0/<3 = round lens, >=3 = a
// regular-polygon aperture (5 = pentagon, 6 = hexagon, ...).
void renderPathTraced(const std::vector<ModelTriangle> &model, const Camera &camera, Canvas &canvas, int samples = 32,
                      const std::vector<Light> &lights = {}, float aperture = 0.0f, float focusDistance = 4.0f,
                      const glm::vec3 &cameraMotion = glm::vec3(0.0f), const Primitives &prims = {},
                      int apertureBlades = 0);

// Photon mapping: emit `numPhotons` from the light (reflecting/refracting through
// mirrors and glass, which focuses caustics), then gather them for indirect
// light + caustics on top of direct lighting.
// gatherRays > 0 enables a final gather (smoother indirect, more cost).
void renderPhotonMapped(const std::vector<ModelTriangle> &model, const Camera &camera, Canvas &canvas,
                        int numPhotons = 200000, const std::vector<Light> &lights = {}, const Primitives &prims = {},
                        int gatherRays = 0);

// Participating media: ray-march the scene through a uniform fog of the given
// density, accumulating single-scattered light so unoccluded beams form visible
// shafts (god-rays). `steps` is the march resolution.
void renderVolumetric(const std::vector<ModelTriangle> &model, const Camera &camera, Canvas &canvas,
                      const std::vector<Light> &lights = {}, const Primitives &prims = {}, float density = 0.6f,
                      int steps = 48);

// Stochastic (delta-tracking) Monte-Carlo volume rendering of a Perlin cloud
// sphere lit by a directional sun: finds a scatter event by delta tracking and
// single-scatters to the sun with ratio-tracking transmittance. densityScale is
// the maximum extinction; more samples reduce the MC noise.
void renderVolumetricMC(const std::vector<ModelTriangle> &model, const Camera &camera, Canvas &canvas,
                        const std::vector<Light> &lights = {}, const Primitives &prims = {}, float densityScale = 6.0f,
                        int samples = 64);

// Accumulation buffer: average many (jittered) render passes into one image, for
// progressive anti-aliasing / depth of field / motion blur / noise reduction.
class AccumBuffer {
public:
	AccumBuffer(int width, int height);
	void add(const Canvas &canvas);  // fold in one pass
	void resolve(Canvas &out) const; // write the running average

private:
	int w_, h_, count_;
	std::vector<glm::vec3> sum_;
};

// Post-filters over a rendered canvas.
void toneMap(Canvas &canvas, float exposure = 1.0f, float gamma = 2.2f);
void applyFXAA(Canvas &canvas);
void applyBloom(Canvas &canvas, float threshold = 0.75f, float intensity = 0.7f);
