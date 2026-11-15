#ifndef PROJECTIVE_BAKER_H
#define PROJECTIVE_BAKER_H

#include "geometry.h"
#include "model.h"
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

// Windowless projective texturing bake: source views are first captured into
// independent depth snapshots with the same rasterization kernel as the
// regular Phong path, then their linear RGB images are projected back onto
// the per-face corner UVs of the same geometry into a floating point atlas.
namespace projective_baker {

struct bake_error : std::runtime_error {
    explicit bake_error(const std::string &message) : std::runtime_error(message) {}
};

struct OverlapConflict {
    int x = 0, y = 0;              // atlas texel covered by two different faces
    int model_a = 0, face_a = 0;   // face that owns the texel
    int model_b = 0, face_b = 0;   // second face covering the same texel
};

struct BakeResult {
    int width = 0, height = 0;
    std::vector<std::uint8_t> covered;   // width*height, 1 where some face covers the texel
    std::vector<int> owner_model;        // width*height, -1 when not covered
    std::vector<int> owner_face;         // width*height, -1 when not covered
    std::vector<double> rgb;             // width*height*3 row-major linear RGB, 0 where weight is 0
    std::vector<double> weight;          // width*height, accumulated valid source weight
};

// Independent snapshot of everything the baker needs from the Models:
// world triangle corners, corner UVs and the unit world geometric normal.
class ProjectiveGeometry {
public:
    struct Face {
        vec3 world[3] = {};
        vec2 uv[3] = {};
        vec3 normal = {};
    };
    struct Mesh {
        std::vector<Face> faces;
    };

    ProjectiveGeometry() = default;
    explicit ProjectiveGeometry(const std::vector<const Model *> &models);

    const std::vector<Mesh> &meshes() const { return meshes_; }

private:
    std::vector<Mesh> meshes_;
};

// A captured source view. The object owns its own copies of the geometry,
// projection data, linear RGB image and captured depth buffer: destroying or
// editing the caller's Models/images, changing the global rasterizer state or
// interleaving other captures does not affect an existing capture.
class SourceCapture {
public:
    SourceCapture() = default;
    SourceCapture(const ProjectiveGeometry &geometry,
                  const mat<4, 4> &world_to_clip, const vec3 &eye,
                  const std::vector<double> &rgb, int width, int height);

    int width() const { return width_; }
    int height() const { return height_; }
    const vec3 &eye() const { return eye_; }
    const mat<4, 4> &world_to_clip() const { return world_to_clip_; }
    bool has_depth(int x, int y) const;
    double depth(int x, int y) const;
    const std::vector<double> &image() const { return rgb_; }

private:
    ProjectiveGeometry geometry_;
    mat<4, 4> world_to_clip_ = {};
    vec3 eye_ = {};
    int width_ = 0, height_ = 0;
    std::vector<double> rgb_;
    std::vector<double> depth_;
    std::vector<std::uint8_t> depth_covered_; // 1 where rasterize_depth committed a depth
};

// Bake the projective texture atlas. An empty source list is legal: covered
// texels keep their coverage and ownership but get zero color and weight.
// Conflicts (two distinct faces truly covering the same atlas texel) are
// reported instead of being silently resolved by submission order. When
// conflicts is non-null, every conflicted texel is appended once.
BakeResult bake_atlas(const ProjectiveGeometry &geometry,
                      const std::vector<const SourceCapture *> &sources,
                      int atlas_width, int atlas_height,
                      double depth_eps,
                      std::vector<OverlapConflict> *conflicts);

} // namespace projective_baker

#endif // PROJECTIVE_BAKER_H
