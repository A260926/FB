#include "ThinLens.h"

#include <cmath>
#include <memory>
#include <stdexcept>

#include "Rasterizer.h"
#include "Renderer.h"
#include "World.h"
#include "OrthographicCamera.h"
#include "OffAxisPerspectiveCamera.h"

namespace ThinLens {

namespace {

// Restores the renderer/world camera relationships on every return path,
// including exceptions. Created only after input validation has succeeded, so a
// rejected configuration never mutates any state.
class CameraRelationGuard {
public:
  CameraRelationGuard(Rasterizer& renderer, World& world)
    : m_renderer(renderer), m_world(world),
      m_renderer_camera(renderer.get_camera()),
      m_world_camera(world.m_camera) {}

  ~CameraRelationGuard() {
    m_renderer.set_camera(m_renderer_camera);
    m_world.m_camera = m_world_camera;
  }

  CameraRelationGuard(const CameraRelationGuard&) = delete;
  CameraRelationGuard& operator=(const CameraRelationGuard&) = delete;

private:
  Rasterizer& m_renderer;
  World& m_world;
  Camera* m_renderer_camera;
  Camera* m_world_camera;
};

void validate(const PerspectiveCamera& center_camera,
              const World& world,
              double focal_distance_F,
              double aperture_radius_R,
              const std::vector<PupilSample>& samples) {
  // F and R must be finite; the focal plane must lie beyond the near plane
  // and the radius cannot be negative.
  if (!std::isfinite(focal_distance_F) || !std::isfinite(aperture_radius_R)) {
    throw std::invalid_argument("ThinLens: focal distance and aperture radius must be finite");
  }
  if (!(focal_distance_F > static_cast<double>(center_camera.get_near_plane()))) {
    throw std::invalid_argument("ThinLens: focal distance F must be greater than the near plane");
  }
  if (aperture_radius_R < 0.0) {
    throw std::invalid_argument("ThinLens: aperture radius R must be non-negative");
  }
  if (samples.empty()) {
    throw std::invalid_argument("ThinLens: pupil sample list must not be empty");
  }
  // Only the perspective thin-lens model is supported.
  if (dynamic_cast<const OrthographicCamera*>(world.m_camera) != nullptr) {
    throw std::invalid_argument("ThinLens: orthographic cameras are not supported");
  }

  double total_weight = 0.0;
  for (const PupilSample& sample : samples) {
    const double px = sample.point.x;
    const double py = sample.point.y;
    if (!std::isfinite(px) || !std::isfinite(py)) {
      throw std::invalid_argument("ThinLens: pupil point coordinates must be finite");
    }
    if (px * px + py * py > 1.0 + 1e-12) {
      throw std::invalid_argument("ThinLens: pupil point must lie inside the unit disk");
    }
    if (!std::isfinite(sample.weight) || sample.weight < 0.0) {
      throw std::invalid_argument("ThinLens: pupil weights must be finite and non-negative");
    }
    total_weight += sample.weight;
  }
  if (!std::isfinite(total_weight) || !(total_weight > 0.0)) {
    throw std::invalid_argument("ThinLens: total pupil weight must be a finite positive number");
  }
}

} // namespace

ThinLensImage render(PerspectiveCamera& center_camera,
                     Rasterizer& renderer,
                     World& world,
                     double focal_distance_F,
                     double aperture_radius_R,
                     const std::vector<PupilSample>& samples) {
  // All validation runs before any camera/renderer/world state is touched.
  validate(center_camera, world, focal_distance_F, aperture_radius_R, samples);

  CameraRelationGuard guard(renderer, world);

  const uint32_t width = center_camera.get_width();
  const uint32_t height = center_camera.get_height();
  const uint32_t pixel_count = width * height;

  // Accumulate in double precision so the float RGB average is not clamped or
  // rounded by the RGBColor arithmetic. Background frames contribute too.
  std::vector<double> accumulated_r(pixel_count, 0.0);
  std::vector<double> accumulated_g(pixel_count, 0.0);
  std::vector<double> accumulated_b(pixel_count, 0.0);
  double total_weight = 0.0;

  auto accumulate_frame = [&](const Camera& frame_camera, double weight) {
    std::vector<RGBColor> frame_color;
    std::vector<double> frame_depth;
    // One full, independent draw: coverage, perspective attributes and depth
    // are all evaluated through frame_camera, which is also the shading camera.
    renderer.renderPupilFrame(frame_camera, frame_color, frame_depth);
    for (uint32_t i = 0; i < pixel_count; ++i) {
      accumulated_r[i] += static_cast<double>(frame_color[i].r) * weight;
      accumulated_g[i] += static_cast<double>(frame_color[i].g) * weight;
      accumulated_b[i] += static_cast<double>(frame_color[i].b) * weight;
    }
    total_weight += weight;
  };

  if (aperture_radius_R == 0.0) {
    // Pinhole: a single center render. The sample distribution is ignored, but
    // it was still fully validated above.
    renderer.set_camera(&center_camera);
    world.m_camera = &center_camera;
    accumulate_frame(center_camera, 1.0);
  } else {
    const Point3D center_position = center_camera.get_position();
    const Vector3D center_right = center_camera.get_right();
    const Vector3D center_up = center_camera.get_up();

    for (const PupilSample& sample : samples) {
      if (sample.weight == 0.0) {
        continue; // validated non-negative; zero-weight pupils change nothing
      }
      // Real camera-space offset is R times the unit-disk coordinate, applied
      // along the center camera's local right/up axes (same orientation).
      const double offset_a = aperture_radius_R * sample.point.x;
      const double offset_b = aperture_radius_R * sample.point.y;
      const Point3D pupil_position(
        center_position.x + center_right.x * offset_a + center_up.x * offset_b,
        center_position.y + center_right.y * offset_a + center_up.y * offset_b,
        center_position.z + center_right.z * offset_a + center_up.z * offset_b);

      // The constructor registers this camera on the renderer; bind it on the
      // world as well so both relationships name the current sample camera.
      std::unique_ptr<OffAxisPerspectiveCamera> pupil_camera(
        new OffAxisPerspectiveCamera(center_camera, pupil_position,
                                     offset_a, offset_b, focal_distance_F,
                                     &renderer));
      world.m_camera = pupil_camera.get();
      accumulate_frame(*pupil_camera, sample.weight);
    }
  }

  ThinLensImage result;
  result.width = width;
  result.height = height;
  result.color.resize(pixel_count);
  for (uint32_t i = 0; i < pixel_count; ++i) {
    result.color[i] = RGBColor(
      static_cast<float>(accumulated_r[i] / total_weight),
      static_cast<float>(accumulated_g[i] / total_weight),
      static_cast<float>(accumulated_b[i] / total_weight));
  }

  // Independent center pinhole geometry draw. This depth is produced on its
  // own and is neither the averaged depth nor the last pupil sample's depth.
  renderer.set_camera(&center_camera);
  world.m_camera = &center_camera;
  result.depth = renderer.renderCenterDepth(center_camera);

  return result;
}

void exportImage(const ThinLensImage& image,
                 Rasterizer& renderer,
                 const std::string& output_path) {
  renderer.exportPupilImage(image.color, output_path);
}

} // namespace ThinLens
