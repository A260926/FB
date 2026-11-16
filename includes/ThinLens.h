#ifndef THIN_LENS_H
#define THIN_LENS_H

#include <cstdint>
#include <string>
#include <vector>

#include "Point2D.h"
#include "RGBColor.h"
#include "PERSPECTIVECAMERA.H"

class Rasterizer;
class World;

// One thin-lens pupil sample.
//  - point  : 2D coordinate inside the unit disk (x^2 + y^2 <= 1), expressed
//             against the center camera's local right/up axes.
//  - weight : non-negative contribution of this pupil to the average.
// The actual camera-space pupil offset is (R * point.x, R * point.y). The
// caller supplies every sample: no random numbers are generated internally.
struct PupilSample {
  Point2D point;
  double weight;

  PupilSample() : point(0.0, 0.0), weight(0.0) {}
  PupilSample(const Point2D& p, double w) : point(p), weight(w) {}
  PupilSample(double x, double y, double w) : point(x, y), weight(w) {}
};

// Owned result of a thin-lens render. Both buffers allocate and own their own
// storage, so two results can be built and interleaved independently.
struct ThinLensImage {
  std::vector<RGBColor> color; // weighted-average float RGB (background included)
  std::vector<double> depth;   // independent center pinhole depth, never averaged
  uint32_t width;
  uint32_t height;

  ThinLensImage() : width(0), height(0) {}
};

namespace ThinLens {

// Renders a real aperture depth-of-field preview.
//
// Every pupil is treated as an off-axis perspective camera with the same
// orientation and intrinsics as center_camera, translated to the pupil point.
// Each pupil re-runs geometry coverage, perspective attribute interpolation
// and an independent depth test; the resulting float RGB frames are combined
// with the supplied non-negative weights (background pixels take part in the
// average). The returned depth buffer comes from a separate center pinhole
// geometry draw, never from the pupil average.
//
// Shadows are disabled. Orthographic cameras are rejected. On invalid
// arguments std::invalid_argument is thrown before any drawing, leaving the
// camera, renderer and world relationships unchanged; they are also restored
// on every normal or exceptional return.
//
// R == 0 performs a single center pinhole render (the pupil distribution is
// ignored) in addition to the separate center depth draw.
ThinLensImage render(PerspectiveCamera& center_camera,
                     Rasterizer& renderer,
                     World& world,
                     double focal_distance_F,
                     double aperture_radius_R,
                     const std::vector<PupilSample>& samples);

// Writes the accumulated color frame through the renderer's original BMP
// exporter. Call this only after render() has returned (the center camera is
// restored and supplies the viewport dimensions).
void exportImage(const ThinLensImage& image,
                 Rasterizer& renderer,
                 const std::string& output_path);

} // namespace ThinLens

#endif // THIN_LENS_H
