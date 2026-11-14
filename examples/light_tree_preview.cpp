// Light-tree preview: a small direct-lighting renderer.
//
// The receiver, the emissive area lights, and the occluder boards are all generated
// procedurally. Primary rays and finite shadow rays use the ordinary BVH built by
// Bvh/DefaultBuilder. Direct light on diffuse surfaces is estimated with the light-tree's joint
// area density, so the estimator divides by the returned `pdf_area`. Lights emit only on their
// front side (the back side contributes nothing), while the sampling distribution remains the
// one defined by LightTree (independent of side and occlusion).
//
// Usage: light_tree_preview [output.ppm]
#include <bvh/v2/bvh.h>
#include <bvh/v2/default_builder.h>
#include <bvh/v2/light_tree.h>
#include <bvh/v2/node.h>
#include <bvh/v2/ray.h>
#include <bvh/v2/stack.h>
#include <bvh/v2/thread_pool.h>
#include <bvh/v2/tri.h>
#include <bvh/v2/vec.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

using Scalar = double;
using Vec3 = bvh::v2::Vec<Scalar, 3>;
using Tri = bvh::v2::Tri<Scalar, 3>;
using BBox = bvh::v2::BBox<Scalar, 3>;
using Node = bvh::v2::Node<Scalar, 3>;
using Bvh = bvh::v2::Bvh<Node>;
using Ray = bvh::v2::Ray<Scalar, 3>;
using LightTree = bvh::v2::LightTree<Scalar>;
using PrecomputedTri = bvh::v2::PrecomputedTri<Scalar>;

static constexpr size_t invalid_id = std::numeric_limits<size_t>::max();
static constexpr Scalar pi = Scalar(3.14159265358979323846264338327950288);

enum class MaterialKind { Diffuse, Emissive };

struct Material {
    MaterialKind kind;
    Vec3 albedo;   // for diffuse
    Vec3 emission; // for emissive
    bool sampleable_light;
    size_t light_id;
};

struct Scene {
    std::vector<Tri> triangles;
    std::vector<Material> materials;
    std::vector<Tri> light_triangles;
    std::vector<Vec3> light_radiances;
};

static Vec3 make_normal(const Tri& triangle) {
    const Vec3 cross_product =
        bvh::v2::cross(triangle.p1 - triangle.p0, triangle.p2 - triangle.p0);
    return bvh::v2::normalize(cross_product);
}

// Adds a triangle and flips its winding if needed so that its geometric normal agrees with the
// requested orientation (at least in sign).
static void add_oriented_triangle(
    Scene& scene,
    Tri triangle,
    const Vec3& desired_normal,
    const Material& material)
{
    const Vec3 normal = make_normal(triangle);
    if (bvh::v2::dot(normal, desired_normal) < 0)
        std::swap(triangle.p1, triangle.p2);
    scene.triangles.push_back(triangle);
    scene.materials.push_back(material);
}

static void add_quad(
    Scene& scene,
    const Vec3& a,
    const Vec3& b,
    const Vec3& c,
    const Vec3& d,
    const Vec3& desired_normal,
    const Material& material)
{
    add_oriented_triangle(scene, Tri(a, b, c), desired_normal, material);
    add_oriented_triangle(scene, Tri(a, c, d), desired_normal, material);
}

static Scene build_scene() {
    Scene scene;

    // Receiver: a diffuse vertical wall at z = 0, facing the camera (+z).
    Material wall_material;
    wall_material.kind = MaterialKind::Diffuse;
    wall_material.albedo = Vec3(0.78, 0.74, 0.68);
    wall_material.sampleable_light = false;
    wall_material.light_id = invalid_id;
    add_quad(
        scene,
        Vec3(-2.4, -1.7, 0), Vec3(2.4, -1.7, 0),
        Vec3(2.4, 1.7, 0), Vec3(-2.4, 1.7, 0),
        Vec3(0, 0, 1), wall_material);

    // Four area lights in front of the wall. Each light faces the wall (normal roughly -z), so
    // its emission is one-sided toward the receiver. Sizes, positions, and colors differ.
    struct LightSpec {
        Vec3 center;
        Scalar half_width;
        Scalar half_height;
        Scalar tilt;
        Vec3 radiance;
    };
    const LightSpec light_specs[] = {
        { Vec3(-1.15, -0.55, 1.55), 0.48, 0.34, -0.20, Vec3(9.0, 2.2, 1.1) },
        { Vec3( 0.85, -0.70, 1.20), 0.30, 0.48,  0.35, Vec3(1.2, 8.0, 2.4) },
        { Vec3(-0.55,  0.90, 2.10), 0.56, 0.26, -0.10, Vec3(1.5, 1.8, 7.5) },
        { Vec3( 1.20,  0.55, 1.85), 0.26, 0.26,  0.55, Vec3(5.5, 4.5, 0.8) },
    };
    for (const auto& spec : light_specs) {
        const Scalar cx = spec.center[0];
        const Scalar cy = spec.center[1];
        const Scalar cz = spec.center[2];
        const Scalar hw = spec.half_width;
        const Scalar hh = spec.half_height;
        const Scalar angle = spec.tilt;
        const Scalar cosine = std::cos(angle);
        const Scalar sine = std::sin(angle);
        // In-plane axes, tilted around the y axis to vary the light orientation a bit.
        const Vec3 axis_u(cosine, 0, sine);
        const Vec3 axis_v(0, 1, 0);
        const Vec3 corner0 = spec.center - axis_u * hw - axis_v * hh;
        const Vec3 corner1 = spec.center + axis_u * hw - axis_v * hh;
        const Vec3 corner2 = spec.center + axis_u * hw + axis_v * hh;
        const Vec3 corner3 = spec.center - axis_u * hw + axis_v * hh;
        (void) cx; (void) cy; (void) cz;

        Material light_material;
        light_material.kind = MaterialKind::Emissive;
        light_material.albedo = Vec3(0);
        light_material.emission = spec.radiance;
        light_material.sampleable_light = true;
        light_material.light_id = scene.light_triangles.size();

        const size_t first_triangle = scene.triangles.size();
        add_oriented_triangle(
            scene, Tri(corner0, corner1, corner2), Vec3(0, 0, -1), light_material);
        add_oriented_triangle(
            scene, Tri(corner0, corner2, corner3), Vec3(0, 0, -1), light_material);

        // The two triangles are the same light; give both faces the same light id.
        scene.light_triangles.push_back(scene.triangles[first_triangle + 0]);
        scene.light_triangles.push_back(scene.triangles[first_triangle + 1]);
        scene.light_radiances.push_back(spec.radiance);
        scene.light_radiances.push_back(spec.radiance);
    }

    // Occluders: two angled boards between the lights and the wall. They are diffuse and get
    // the same direct-lighting treatment as the wall, so they also show shadows.
    Material occluder_material;
    occluder_material.kind = MaterialKind::Diffuse;
    occluder_material.albedo = Vec3(0.22, 0.21, 0.20);
    occluder_material.sampleable_light = false;
    occluder_material.light_id = invalid_id;

    add_quad(
        scene,
        Vec3(-1.55, -1.25, 0.62), Vec3(-0.75, -1.25, 0.62),
        Vec3(-0.55, 0.55, 1.05), Vec3(-1.35, 0.55, 1.05),
        Vec3(0, 0, 1), occluder_material);
    add_quad(
        scene,
        Vec3(0.35, -0.55, 1.30), Vec3(1.05, -0.55, 1.30),
        Vec3(1.15, 1.15, 0.85), Vec3(0.45, 1.15, 0.85),
        Vec3(0, 0, 1), occluder_material);

    return scene;
}

static Vec3 random_in_unit_cube(uint64_t& state, size_t index, size_t count) {
    (void) state;
    // Deterministic, low-discrepancy-ish coordinates in [0, 1). The caller combines these for
    // the three light-tree uniforms.
    const Scalar base2 = Scalar(0.5);
    const Scalar base3 = Scalar(1) / Scalar(3);
    Scalar x = Scalar(0);
    Scalar y = Scalar(0);
    Scalar z = Scalar(0);
    Scalar fraction;
    size_t n;
    fraction = base2;
    for (n = index + 1; n > 0; n >>= 1, fraction *= base2) {
        if (n & 1)
            x += fraction;
    }
    fraction = base3;
    for (n = index + 1; n > 0; n /= 3, fraction *= base3)
        y += static_cast<Scalar>(n % 3) * fraction;
    // A tiny per-pixel scramble (the caller passes a hash in `state`) decorrelates neighbors.
    z = (static_cast<Scalar>(state & 0xFFFFFF) + Scalar(0.5)) / Scalar(0x1000000);
    (void) count;
    return Vec3(
        std::min(x, std::nextafter(Scalar(1), Scalar(0))),
        std::min(y, std::nextafter(Scalar(1), Scalar(0))),
        std::min(z, std::nextafter(Scalar(1), Scalar(0))));
}

static uint64_t hash2(uint64_t x, uint64_t y, uint64_t z) {
    uint64_t h = x * 0x9E3779B97F4A7C15ull;
    h ^= y * 0xBF58476D1CE4E5B9ull;
    h ^= z * 0x94D049BB133111EBull;
    h ^= h >> 30;
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 27;
    h *= 0x94D049BB133111EBull;
    h ^= h >> 31;
    return h;
}

// Prepared scene acceleration structure: the ordinary BVH plus permuted data for leaf traversal.
struct PreparedScene {
    Bvh bvh;
    std::vector<PrecomputedTri> precomputed;
    std::vector<size_t> original_id;
    std::vector<const Material*> material;
};

static PreparedScene prepare_scene(const Scene& scene) {
    std::vector<BBox> bboxes(scene.triangles.size());
    std::vector<Vec3> centers(scene.triangles.size());
    for (size_t i = 0; i < scene.triangles.size(); ++i) {
        bboxes[i] = scene.triangles[i].get_bbox();
        centers[i] = scene.triangles[i].get_center();
    }

    bvh::v2::ThreadPool thread_pool;
    typename bvh::v2::DefaultBuilder<Node>::Config config;
    config.quality = bvh::v2::DefaultBuilder<Node>::Quality::High;
    auto bvh = bvh::v2::DefaultBuilder<Node>::build(thread_pool, bboxes, centers, config);

    PreparedScene prepared;
    prepared.bvh = std::move(bvh);
    prepared.precomputed.resize(scene.triangles.size());
    prepared.original_id.resize(scene.triangles.size());
    prepared.material.resize(scene.triangles.size());
    for (size_t slot = 0; slot < scene.triangles.size(); ++slot) {
        const size_t original = prepared.bvh.prim_ids[slot];
        prepared.original_id[slot] = original;
        prepared.precomputed[slot] = scene.triangles[original];
        prepared.material[slot] = &scene.materials[original];
    }
    return prepared;
}

struct Hit {
    size_t original_id = invalid_id;
    Scalar distance = 0;
};

static Hit closest_hit(const PreparedScene& prepared, const Ray& ray) {
    using Stack = bvh::v2::SmallStack<Bvh::Index, 64>;
    static constexpr bool is_any_hit = false;
    static constexpr bool is_robust = true;

    Stack stack;
    Hit hit;
    Ray mutable_ray = ray;
    prepared.bvh.intersect<is_any_hit, is_robust>(
        mutable_ray, prepared.bvh.get_root().index, stack,
        [&] (size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                if (auto result = prepared.precomputed[i].intersect(mutable_ray)) {
                    hit.original_id = prepared.original_id[i];
                    hit.distance = std::get<0>(*result);
                    std::tie(mutable_ray.tmax, std::ignore, std::ignore) = *result;
                }
            }
            return hit.original_id != invalid_id;
        });
    return hit;
}

static bool any_hit(const PreparedScene& prepared, const Ray& ray) {
    using Stack = bvh::v2::SmallStack<Bvh::Index, 64>;
    static constexpr bool is_any_hit = true;
    static constexpr bool is_robust = true;

    Stack stack;
    bool hit = false;
    Ray mutable_ray = ray;
    prepared.bvh.intersect<is_any_hit, is_robust>(
        mutable_ray, prepared.bvh.get_root().index, stack,
        [&] (size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                if (prepared.precomputed[i].intersect(mutable_ray)) {
                    hit = true;
                    break;
                }
            }
            return hit;
        });
    return hit;
}

static Vec3 trace_direct(
    const PreparedScene& prepared,
    const LightTree& light_tree,
    const Vec3& position,
    const Vec3& normal,
    const Vec3& albedo,
    uint64_t pixel_seed,
    size_t sample_count)
{
    Vec3 incoming(Scalar(0));
    for (size_t sample = 0; sample < sample_count; ++sample) {
        uint64_t state = hash2(pixel_seed, sample, 0x51ED2701ull);
        const Vec3 uniforms = random_in_unit_cube(state, sample, sample_count);
        const auto light_sample = light_tree.sample(
            position,
            Scalar(0.35),
            uniforms[0], uniforms[1], uniforms[2]);
        if (!light_sample || !(light_sample->pdf_area > 0))
            continue;

        const Vec3 to_light = light_sample->position - position;
        const Scalar squared_distance = bvh::v2::dot(to_light, to_light);
        if (!(squared_distance > 0))
            continue;
        const Scalar distance = std::sqrt(squared_distance);
        const Vec3 direction = to_light * (Scalar(1) / distance);

        // One-sided emission: the light must face the receiver.
        const Scalar light_cosine = -bvh::v2::dot(light_sample->normal, direction);
        if (!(light_cosine > 0))
            continue;
        const Scalar receiver_cosine = bvh::v2::dot(normal, direction);
        if (!(receiver_cosine > 0))
            continue;

        // Finite shadow ray against the ordinary BVH, ending just short of the light.
        const Vec3 shadow_origin = position + normal * Scalar(1e-3);
        const Scalar shadow_length = distance * (Scalar(1) - Scalar(1e-4)) - Scalar(1e-3);
        if (!(shadow_length > 0))
            continue;
        const Ray shadow_ray(shadow_origin, direction, Scalar(0), shadow_length);
        if (any_hit(prepared, shadow_ray))
            continue;

        const Scalar geometry = receiver_cosine * light_cosine / squared_distance;
        // Lambertian BRDF = albedo / pi, and the light-tree sample is already a joint area density,
        // so the estimator divides by that density.
        incoming = incoming + light_sample->radiance *
            (albedo * (geometry / light_sample->pdf_area));
    }
    return incoming * (Scalar(1) / static_cast<Scalar>(sample_count) * (Scalar(1) / pi));
}

static Vec3 trace_primary(
    const PreparedScene& prepared,
    const Scene& scene,
    const LightTree& light_tree,
    const Ray& ray,
    uint64_t pixel_seed,
    size_t direct_samples)
{
    const Hit hit = closest_hit(prepared, ray);
    if (hit.original_id == invalid_id)
        return Vec3(Scalar(0.012), Scalar(0.016), Scalar(0.025)); // dim background

    const Vec3 position = ray.org + ray.dir * hit.distance;
    const Material& material = scene.materials[hit.original_id];
    if (material.kind == MaterialKind::Emissive) {
        // Single-sided emission: only the front side emits. The camera is on the wall side, i.e.
        // behind these lights, so their front side is not visible here and they read as dark.
        const Vec3 normal = make_normal(scene.triangles[hit.original_id]);
        if (bvh::v2::dot(normal, ray.dir) < 0)
            return material.emission;
        return Vec3(Scalar(0.01), 0.01, 0.01);
    }

    const Vec3 normal = make_normal(scene.triangles[hit.original_id]);
    const Vec3 oriented_normal =
        bvh::v2::dot(normal, ray.dir) < 0 ? normal : -normal;
    const Vec3 ambient = material.albedo * Scalar(0.025);
    return ambient + trace_direct(
        prepared, light_tree, position, oriented_normal, material.albedo,
        pixel_seed, direct_samples);
}

static void write_ppm(const std::string& path, size_t width, size_t height,
    const std::vector<Vec3>& pixels)
{
    std::ofstream file(path, std::ios::binary);
    if (!file)
        throw std::runtime_error("cannot open output file: " + path);
    file << "P6\n" << width << " " << height << "\n255\n";
    for (const auto& pixel : pixels) {
        for (size_t channel = 0; channel < 3; ++channel) {
            const Scalar value = pixel[channel];
            const Scalar mapped = std::isfinite(value)
                ? (Scalar(1) - std::exp(-std::max(value, Scalar(0))))
                : Scalar(1);
            const Scalar gamma_corrected = std::pow(mapped, Scalar(1) / Scalar(2.2));
            const auto byte = static_cast<unsigned>(
                std::clamp(std::floor(gamma_corrected * Scalar(255) + Scalar(0.5)),
                    Scalar(0), Scalar(255)));
            file.put(static_cast<char>(byte));
        }
    }
    if (!file)
        throw std::runtime_error("failed while writing output file: " + path);
}

int main(int argc, char** argv) {
    const std::string output_path = argc > 1 ? argv[1] : "light-tree.ppm";
    const size_t width = 300;
    const size_t height = 220;
    const size_t direct_samples = 64;

    const Scene scene = build_scene();
    const PreparedScene prepared = prepare_scene(scene);

    LightTree::Config light_config;
    light_config.min_leaf_size = 2;
    light_config.max_leaf_size = 8;
    const LightTree light_tree(scene.light_triangles, scene.light_radiances, light_config);

    const Vec3 eye(0, 0, 7.5);
    const Vec3 look_at(0, 0, 0);
    const Vec3 up(0, 1, 0);
    const Vec3 forward = bvh::v2::normalize(look_at - eye);
    const Vec3 right = bvh::v2::normalize(bvh::v2::cross(forward, up));
    const Vec3 camera_up = bvh::v2::cross(right, forward);
    const Scalar field_of_view = Scalar(48) * pi / Scalar(180);
    const Scalar half_tan = std::tan(field_of_view * Scalar(0.5));
    const Scalar aspect = static_cast<Scalar>(width) / static_cast<Scalar>(height);

    std::vector<Vec3> pixels;
    pixels.reserve(width * height);
    for (size_t y = 0; y < height; ++y) {
        for (size_t x = 0; x < width; ++x) {
            const Scalar sx =
                ((static_cast<Scalar>(x) + Scalar(0.5)) / static_cast<Scalar>(width) * Scalar(2) - Scalar(1)) *
                half_tan * aspect;
            const Scalar sy =
                (Scalar(1) - (static_cast<Scalar>(y) + Scalar(0.5)) / static_cast<Scalar>(height) * Scalar(2)) *
                half_tan;
            const Vec3 direction = bvh::v2::normalize(
                forward + right * sx + camera_up * sy);
            const Ray ray(eye, direction, Scalar(0), Scalar(1e6));
            const uint64_t seed = hash2(x, y, 0x1234ABCDull);
            pixels.push_back(trace_primary(
                prepared, scene, light_tree, ray, seed, direct_samples));
        }
    }

    write_ppm(output_path, width, height, pixels);

    std::printf(
        "LIGHT_TREE_PREVIEW ok ppm=%s resolution=%zux%zu light_faces=%zu "
        "light_nodes=%zu direct_samples_per_pixel=%zu\n",
        output_path.c_str(), width, height,
        light_tree.face_count(), light_tree.node_count(), direct_samples);
    return 0;
}
