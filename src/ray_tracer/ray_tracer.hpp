#ifndef RAY_TRACER_HPP_
#define RAY_TRACER_HPP_

#include "common.hpp"
#include "ppm_writer.hpp"
#include "objects.hpp"
#include "light.hpp"
#include "camera.hpp"
#include "ray.hpp"
#include <vector>
#include <memory>
#include <type_traits>
#include <utility>
#include <limits> // numeric_limits
#include <cmath>
#include <cstdint>


// incremental preview (see src/incremental/ray_cache.hpp and README)
namespace incremental {
class Engine;
struct SphereEdit;
struct FrameStats;
struct NodeInfo;
}

struct TraceRecord {
  Vec3u8 color{0, 0, 0};      // final color
  bool hit{false};        
  float t{std::numeric_limits<float>::infinity()}; // hit distance
  Vec3f hit_point{};          // where the hit occured
  Vec3f normal{};             // surface normal at hit
  const Object* obj{nullptr}; // hit object (nullptr if no hit)
};

class RayTracer {
public:
  RayTracer(const Camera& camera, Lights& lights);
  ~RayTracer();

  // variadic template to store any object derived from Object
  // pass Args to allow constructing objects in place (c/tor with arguments)
  template <typename T, typename... Args>
  T& AddObject(Args&&... args) {
    static_assert(std::is_base_of_v<Object, T>,
                  "AddObject<T>: T must derive from Object");
    // with std::forward to preserve rvalue/lvalue nature
    auto obj = std::make_unique<T>(std::forward<Args>(args)...);
    T& ref = *obj;
    objects_.push_back(std::move(obj));
    ++objects_version_;
    return ref;
  }

  const Image& image();

  void Trace(int max_reflections = 5);

  // gamma < 1 brightens the low intensitites of the image,
  // gamma > 1 darkens the high intensities
  void GammaCorrect(float gamma);

  // background can be a PPM image or a solid color
  void SetBackground(const std::string& filename);
  void SetBackground(uint8_t r, uint8_t g, uint8_t b) {
    has_background_ = false;
    background_color_ = {r, g, b};
    ++background_version_;
  }

  //-------------------------------------------------------------------
  // incremental preview (optional; see README "Incremental preview")
  //-------------------------------------------------------------------
  // enable with a finite edit domain enclosing all objects' finite
  // bounds; returns false (and stays disabled) if the constraint fails
  bool EnableIncremental(const AABB& edit_domain);
  void DisableIncremental();
  bool IncrementalEnabled() const;
  // atomically replace several registered spheres' center/radius/
  // material; on rejection geometry, image and cache stay untouched
  bool ApplySphereEdits(const std::vector<incremental::SphereEdit>& edits);
  // per-frame work counters of the last Trace() call
  incremental::FrameStats IncrementalStats() const;
  // live ray records (monotonic ids, parents, children, types, slots)
  std::vector<incremental::NodeInfo> IncrementalNodes() const;
  bool IncrementalNodeInfo(uint64_t id, incremental::NodeInfo& out) const;
  // ray record id of a pixel's root node (0 when nothing is recorded)
  uint64_t IncrementalRootId(unsigned row, unsigned col) const;

private:
  // get the corrent IOR (index of refraction) and normal arrangement
  // for refraction calculations
  struct OrientationInfo {
    bool entering{true};
    // oriented normal, i.e. pointing away from refracted medium
    Vec3f normal{};
    float n1{1.0f};
    float n2{1.0f};
    float eta{1.0f};
    float cos_i{0.0f};
  };

  // probe the refractive index of the surrounding medium slightly off
  // the surface
  float SurroundingIOR(const Vec3f& where,
                       const Object* self,
                       const Vec3f& outward_normal) const;

  // determine normal orientation and the IOR (index of refraction)
  // pair, given that the normal ray should point towadds the incident
  // plane
  OrientationInfo RayOrientation(const TraceRecord& record, 
                                 const Vec3f& I,
                                 float ior_current) const;

  // returns refracted vector (Snell's vectorized law) and whether TIR
  // occured
    std::pair<bool, Vec3f>
    Refract(const Vec3f &incident, /* incident ray direction */
      const Vec3f &N,              /* normal at intersection */
      float eta,                   /* relative refr index n1/n2 */
      float cosi                   /* cos of incident ray */) const;

  // Schlick's approximation of Fresnel reflectance
  float Schlick(float n1, float n2, float cos_i) const;

  TraceRecord TraceRay(const Ray& ray, int depth, float ior_current = 1.0f) const;

  // the original full-frame trace (used whenever the incremental path
  // is disabled); kept byte-identical to the historical behavior
  void TraceFull(int max_reflections);

  // camera-plane sampling shared by the full trace and the recorder
  struct Raster {
    Vec3f tl, span_h, span_v;
  };
  Raster CurrentRaster() const;
  Vec3u8 TracePixel(const Raster& raster, std::size_t row, std::size_t col,
                    int max_reflections) const;

  // simple glow halo: add emission for rays that pass near emissive spheres
  Vec3u8 EmissiveGlow(const Ray& ray,
                      float t_max = std::numeric_limits<float>::infinity()) const;

  // map ray direction to background image using equirectangular mapping
  Vec3u8 SampleBackground(const Vec3f& dir_world) const;

  const Camera &camera_;
  std::vector<std::unique_ptr<Object>> objects_;
  // image buffer to store the final colors
  Image image_;
  Lights& lights_;
  // how much to scale the eps of a normal to avoid self-intersection
  static constexpr const float eps_factor = 40.0;
  // cached camera basis for mapping rays to background image
  Vec3f cam_right_{};
  Vec3f cam_up_{};
  Vec3f cam_forward_{};
  // emissive spheres, cached at the start of Trace() for the glow pass
  std::vector<const Sphere*> emissive_{};
  // spherical background
  Image background_{};
  bool has_background_{false};
  // solid background color when no background image is used
  Vec3u8 background_color_{0, 0, 0};
  // bumped whenever the background or the object set changes, so the
  // incremental engine can detect it and fully rebuild its cache
  uint64_t background_version_{0};
  uint64_t objects_version_{0};
  // incremental preview engine (see src/incremental/ray_cache.hpp);
  // rec_ is non-null only while the engine is recording/replaying
  std::unique_ptr<incremental::Engine> incr_;
  mutable incremental::Engine* rec_{nullptr};
  friend class incremental::Engine;
};

#endif // RAY_TRACER_HPP_
