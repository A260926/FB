#include "cgltf_skin_decompose.h"

#include "cgltf_write.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <numeric>
#include <string>

namespace skin_decompose {

namespace {

// ---------------------------------------------------------------------------
// Small helpers.
// ---------------------------------------------------------------------------

std::string attribute_name(cgltf_attribute_type type, int index, const char* fallback) {
    switch (type) {
        case cgltf_attribute_type_position: return "POSITION";
        case cgltf_attribute_type_normal: return "NORMAL";
        case cgltf_attribute_type_tangent: return "TANGENT";
        case cgltf_attribute_type_texcoord:
            return "TEXCOORD_" + std::to_string(index);
        case cgltf_attribute_type_color:
            return "COLOR_" + std::to_string(index);
        case cgltf_attribute_type_joints:
            return "JOINTS_" + std::to_string(index);
        case cgltf_attribute_type_weights:
            return "WEIGHTS_" + std::to_string(index);
        default:
            return fallback ? std::string(fallback) : std::string("CUSTOM");
    }
}

std::vector<float> unpack_accessor(const cgltf_accessor* accessor) {
    const cgltf_size count = cgltf_accessor_unpack_floats(accessor, nullptr, 0);
    std::vector<float> values(count);
    if (cgltf_accessor_unpack_floats(accessor, values.data(), count) != count)
        return {};
    return values;
}

// Column-major 4x4 inverse. Returns false when the matrix is not invertible.
bool inverse4(const double m[16], double out[16]) {
    auto minor = [&](int r0, int r1, int r2, int c0, int c1, int c2) {
        return m[r0 * 4 + c0] *
                   (m[r1 * 4 + c1] * m[r2 * 4 + c2] -
                    m[r1 * 4 + c2] * m[r2 * 4 + c1]) -
               m[r0 * 4 + c1] *
                   (m[r1 * 4 + c0] * m[r2 * 4 + c2] -
                    m[r1 * 4 + c2] * m[r2 * 4 + c0]) +
               m[r0 * 4 + c2] *
                   (m[r1 * 4 + c0] * m[r2 * 4 + c1] -
                    m[r1 * 4 + c1] * m[r2 * 4 + c0]);
    };
    double inv[16];
    for (int col = 0; col < 4; ++col) {
        const int c0 = (col + 1) % 4, c1 = (col + 2) % 4, c2 = (col + 3) % 4;
        for (int row = 0; row < 4; ++row) {
            const int r0 = (row + 1) % 4, r1 = (row + 2) % 4,
                      r2 = (row + 3) % 4;
            double cofactor = minor(r0, r1, r2, c0, c1, c2);
            if ((row + col) % 2 != 0) cofactor = -cofactor;
            inv[col * 4 + row] = cofactor; // transpose built into the layout
        }
    }
    double det = 0.0;
    for (int col = 0; col < 4; ++col)
        det += m[col * 4 + 0] * inv[col * 4 + 0];
    if (!std::isfinite(det) || det == 0.0) return false;
    const double inv_det = 1.0 / det;
    for (int i = 0; i < 16; ++i) {
        out[i] = inv[i] * inv_det;
        if (!std::isfinite(out[i])) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Quaternion / rotation conversion shared by the error check and the writer.
// ---------------------------------------------------------------------------

void rotation_to_quaternion(const double R[9], double q[4]) {
    const double trace = R[0] + R[4] + R[8];
    double vals[4];
    if (trace > 0.0) {
        double s = std::sqrt(trace + 1.0) * 2.0;
        vals[3] = 0.25 * s;
        vals[0] = (R[7] - R[5]) / s;
        vals[1] = (R[2] - R[6]) / s;
        vals[2] = (R[3] - R[1]) / s;
    } else if (R[0] > R[4] && R[0] > R[8]) {
        double s = std::sqrt(1.0 + R[0] - R[4] - R[8]) * 2.0;
        vals[3] = (R[7] - R[5]) / s;
        vals[0] = 0.25 * s;
        vals[1] = (R[1] + R[3]) / s;
        vals[2] = (R[2] + R[6]) / s;
    } else if (R[4] > R[8]) {
        double s = std::sqrt(1.0 + R[4] - R[0] - R[8]) * 2.0;
        vals[3] = (R[2] - R[6]) / s;
        vals[0] = (R[1] + R[3]) / s;
        vals[1] = 0.25 * s;
        vals[2] = (R[5] + R[7]) / s;
    } else {
        double s = std::sqrt(1.0 + R[8] - R[0] - R[4]) * 2.0;
        vals[3] = (R[3] - R[1]) / s;
        vals[0] = (R[2] + R[6]) / s;
        vals[1] = (R[5] + R[7]) / s;
        vals[2] = 0.25 * s;
    }
    double n = std::sqrt(vals[0] * vals[0] + vals[1] * vals[1] +
                         vals[2] * vals[2] + vals[3] * vals[3]);
    for (int i = 0; i < 4; ++i) q[i] = vals[i] / n;
}

void quaternion_to_matrix(const double q[4], double R[9]) {
    const double x = q[0], y = q[1], z = q[2], w = q[3];
    R[0] = 1 - 2 * (y * y + z * z);
    R[1] = 2 * (x * y - z * w);
    R[2] = 2 * (x * z + y * w);
    R[3] = 2 * (x * y + z * w);
    R[4] = 1 - 2 * (x * x + z * z);
    R[5] = 2 * (y * z - x * w);
    R[6] = 2 * (x * z - y * w);
    R[7] = 2 * (y * z + x * w);
    R[8] = 1 - 2 * (x * x + y * y);
}

// Float-rounded, hemisphere-consistent keyframe quaternions for one bone.
std::vector<std::array<float, 4>> build_bone_quaternions(
        const SkinSolution& solution, std::size_t bone) {
    std::vector<std::array<float, 4>> out;
    out.reserve(solution.frames);
    double previous[4] = {0.0, 0.0, 0.0, 1.0};
    for (std::size_t s = 0; s < solution.frames; ++s) {
        const RigidTransform& tf = solution.transforms[s * solution.bones + bone];
        double q[4];
        rotation_to_quaternion(tf.rotation, q);
        if (s > 0) {
            double dot = q[0] * previous[0] + q[1] * previous[1] +
                         q[2] * previous[2] + q[3] * previous[3];
            if (dot < 0.0)
                for (double& c : q) c = -c;
        }
        float fq[4] = {static_cast<float>(q[0]), static_cast<float>(q[1]),
                       static_cast<float>(q[2]), static_cast<float>(q[3])};
        float n = std::sqrt(fq[0] * fq[0] + fq[1] * fq[1] +
                            fq[2] * fq[2] + fq[3] * fq[3]);
        for (float& c : fq) c /= n;
        out.push_back({fq[0], fq[1], fq[2], fq[3]});
        for (int i = 0; i < 4; ++i) previous[i] = fq[i];
    }
    return out;
}

// Recompute the local-space error using exactly the float values that will be
// written to the output glTF.
void compute_float_error(const DecomposeReport& report, double& max_error,
                         std::size_t& worst_frame, std::size_t& worst_vertex) {
    const SkinSolution& sol = report.solution;
    const std::size_t S = sol.frames, V = sol.vertices, K = sol.bones;

    std::vector<std::vector<std::array<float, 4>>> quats(K);
    std::vector<std::array<float, 3>> translations(S * K);
    for (std::size_t k = 0; k < K; ++k) {
        quats[k] = build_bone_quaternions(sol, k);
        for (std::size_t s = 0; s < S; ++s) {
            const double* t = sol.transforms[s * K + k].translation;
            translations[s * K + k] = {
                static_cast<float>(t[0]), static_cast<float>(t[1]),
                static_cast<float>(t[2])};
        }
    }
    std::vector<float> float_weights(sol.weights.begin(), sol.weights.end());

    max_error = -1.0;
    worst_frame = 0;
    worst_vertex = 0;
    for (std::size_t s = 0; s < S; ++s) {
        std::vector<double> Rm(K * 9);
        for (std::size_t k = 0; k < K; ++k) {
            double qd[4] = {quats[k][s][0], quats[k][s][1], quats[k][s][2],
                            quats[k][s][3]};
            quaternion_to_matrix(qd, &Rm[k * 9]);
        }
        for (std::size_t v = 0; v < V; ++v) {
            const double* p = &report.rest_positions[v * 3];
            double skinned[3] = {0.0, 0.0, 0.0};
            for (std::size_t k = 0; k < K; ++k) {
                const float w = float_weights[v * K + k];
                if (w == 0.0f) continue;
                const double* R = &Rm[k * 9];
                const auto& t = translations[s * K + k];
                for (int r = 0; r < 3; ++r) {
                    const double y = R[r * 3 + 0] * p[0] +
                                     R[r * 3 + 1] * p[1] +
                                     R[r * 3 + 2] * p[2] + t[r];
                    skinned[r] += static_cast<double>(w) * y;
                }
            }
            double norm2 = 0.0;
            for (int r = 0; r < 3; ++r) {
                const double d = report.observed_positions[(s * V + v) * 3 + r] -
                                 skinned[r];
                norm2 += d * d;
            }
            const double err = std::sqrt(norm2);
            // Iteration order is (frame, vertex), so the first maximum keeps
            // the smallest frame and, within it, the smallest vertex.
            if (err > max_error) {
                max_error = err;
                worst_frame = s;
                worst_vertex = v;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Morph weight sampling.
// ---------------------------------------------------------------------------

bool find_weight_sampler(const cgltf_data* data, const cgltf_node* node,
                         cgltf_size animation_index,
                         const cgltf_animation_sampler*& sampler_out,
                         std::string& error) {
    if (animation_index >= data->animations_count) {
        error = "animation index is out of range";
        return false;
    }
    const cgltf_animation& animation = data->animations[animation_index];
    const cgltf_animation_sampler* found = nullptr;
    for (cgltf_size i = 0; i < animation.channels_count; ++i) {
        const cgltf_animation_channel& channel = animation.channels[i];
        if (channel.target_node != node ||
            channel.target_path != cgltf_animation_path_type_weights)
            continue;
        if (found) {
            error = "more than one morph weight channel targets the node";
            return false;
        }
        found = channel.sampler;
    }
    if (!found) {
        error = "the selected animation has no morph weight channel for the node";
        return false;
    }
    if (found->interpolation != cgltf_interpolation_type_linear &&
        found->interpolation != cgltf_interpolation_type_step) {
        error = "unsupported morph weight interpolation (only STEP and LINEAR are supported)";
        return false;
    }
    sampler_out = found;
    return true;
}

// Flattened weights for every sample time, S * M entries.
//
// Dense samplers (the common case) fully define the weights at every keyframe.
// When the output accessor has an implicit base (no buffer view) and only
// sparse overrides, components that no sparse entry touches fall back to the
// node/mesh default weights, with node weights taking precedence.
bool sample_morph_weights(const cgltf_animation_sampler& sampler,
                          const std::vector<double>& defaults,
                          const std::vector<double>& sample_times,
                          std::size_t target_count,
                          std::vector<double>& out_weights,
                          std::string& error) {
    std::vector<float> time_f = unpack_accessor(sampler.input);
    if (time_f.empty()) {
        error = "failed to unpack the morph weight sampler times";
        return false;
    }
    const cgltf_size key_count = sampler.input->count;
    if (key_count == 0) {
        error = "morph weight sampler has no keys";
        return false;
    }
    std::vector<double> key_values(
        static_cast<std::size_t>(key_count) * target_count);
    const cgltf_accessor* output = sampler.output;
    const bool implicit_sparse =
        output->is_sparse && output->buffer_view == nullptr;
    if (implicit_sparse) {
        for (cgltf_size i = 0; i < key_count; ++i)
            for (std::size_t m = 0; m < target_count; ++m)
                key_values[i * target_count + m] = defaults[m];
        const cgltf_accessor_sparse& sparse = output->sparse;
        const uint8_t* index_bytes =
            cgltf_buffer_view_data(sparse.indices_buffer_view);
        if (!index_bytes) {
            error = "failed to read sparse morph weight indices";
            return false;
        }
        index_bytes += sparse.indices_byte_offset;
        const cgltf_size component_bytes =
            cgltf_component_size(sparse.indices_component_type);
        const cgltf_size components = cgltf_num_components(output->type);
        if (components > 16) {
            error = "morph weight sampler has an impossible component count";
            return false;
        }
        for (cgltf_size i = 0; i < sparse.count; ++i) {
            cgltf_size key = 0;
            const uint8_t* p = index_bytes + i * component_bytes;
            switch (sparse.indices_component_type) {
                case cgltf_component_type_r_8u:
                    key = p[0];
                    break;
                case cgltf_component_type_r_16u: {
                    uint16_t v;
                    std::memcpy(&v, p, sizeof(v));
                    key = v;
                    break;
                }
                case cgltf_component_type_r_32u: {
                    uint32_t v;
                    std::memcpy(&v, p, sizeof(v));
                    key = v;
                    break;
                }
                default:
                    error = "unsupported sparse index component type";
                    return false;
            }
            if (key >= key_count) {
                error = "sparse morph weight index is out of range";
                return false;
            }
            float element[16] = {0};
            if (!cgltf_accessor_read_float(output, key, element, components)) {
                error = "failed to read a sparse morph weight key";
                return false;
            }
            for (cgltf_size c = 0; c < components && c < target_count; ++c)
                key_values[key * target_count + c] = element[c];
        }
    } else {
        std::vector<float> value_f = unpack_accessor(output);
        if (value_f.empty()) {
            error = "failed to unpack the morph weight sampler values";
            return false;
        }
        if (value_f.size() !=
            static_cast<std::size_t>(key_count) * target_count) {
            error = "morph weight sampler output does not match the target count";
            return false;
        }
        for (std::size_t i = 0; i < value_f.size(); ++i)
            key_values[i] = value_f[i];
    }
    std::vector<double> keys(key_count);
    for (cgltf_size i = 0; i < key_count; ++i) keys[i] = time_f[i];

    out_weights.clear();
    out_weights.reserve(sample_times.size() * target_count);
    for (double t : sample_times) {
        if (t <= keys.front()) {
            for (std::size_t m = 0; m < target_count; ++m)
                out_weights.push_back(key_values[m]);
            continue;
        }
        if (t >= keys.back()) {
            for (std::size_t m = 0; m < target_count; ++m)
                out_weights.push_back(
                    key_values[(key_count - 1) * target_count + m]);
            continue;
        }
        cgltf_size lo = 0, hi = key_count - 1;
        while (lo + 1 < hi) {
            const cgltf_size mid = (lo + hi) / 2;
            if (keys[mid] <= t) lo = mid;
            else hi = mid;
        }
        double alpha = 0.0;
        if (sampler.interpolation == cgltf_interpolation_type_linear &&
            keys[lo + 1] > keys[lo]) {
            alpha = (t - keys[lo]) / (keys[lo + 1] - keys[lo]);
        }
        for (std::size_t m = 0; m < target_count; ++m) {
            const double a = key_values[lo * target_count + m];
            const double b = key_values[(lo + 1) * target_count + m];
            const double w =
                sampler.interpolation == cgltf_interpolation_type_step
                    ? a
                    : a + (b - a) * alpha;
            if (!std::isfinite(w)) {
                error = "non-finite morph weight produced by sampling";
                return false;
            }
            out_weights.push_back(w);
        }
    }
    return true;
}

bool material_uses_texture(const cgltf_material& material) {
    if (material.has_pbr_metallic_roughness) {
        const auto& pbr = material.pbr_metallic_roughness;
        if (pbr.base_color_texture.texture ||
            pbr.metallic_roughness_texture.texture)
            return true;
    }
    return material.normal_texture.texture || material.occlusion_texture.texture ||
           material.emissive_texture.texture;
}

} // namespace

DecomposeReport decompose_morph_skin(const DecomposeOptions& options) {
    DecomposeReport report;
    auto fail = [&](const std::string& message) -> DecomposeReport {
        DecomposeReport bad;
        bad.ok = false;
        bad.error = message;
        return bad;
    };

    cgltf_data* data = options.data;
    if (!data) return fail("cgltf_data must not be null");
    cgltf_node* node = options.target_node;
    if (!node) return fail("target node must not be null");
    if (!node->mesh) return fail("target node does not reference a mesh");
    if (node->skin) return fail("target node already has a skin");
    if (options.bone_count < 1 || options.bone_count > 8)
        return fail("bone count K must be in [1, 8]");
    if (options.max_influences < 1 ||
        options.max_influences > std::min<cgltf_size>(4, options.bone_count))
        return fail("max influences P must be in [1, min(4, K)]");
    if (!std::isfinite(options.max_error) || options.max_error < 0.0)
        return fail("max error must be finite and non-negative");
    if (options.sample_times.size() < 2 || options.sample_times.size() > 64)
        return fail("sample count must be in [2, 64]");
    for (std::size_t i = 0; i < options.sample_times.size(); ++i) {
        if (!std::isfinite(options.sample_times[i]) ||
            options.sample_times[i] < 0.0)
            return fail("sample times must be finite and non-negative");
        if (i > 0 && options.sample_times[i] <= options.sample_times[i - 1])
            return fail("sample times must be strictly increasing");
    }

    cgltf_mesh& mesh = *node->mesh;
    if (mesh.primitives_count == 0)
        return fail("target mesh has no primitives");
    const cgltf_size target_count = mesh.primitives[0].targets_count;
    if (target_count == 0)
        return fail("target mesh has no morph targets");

    // No compression anywhere on the data we are about to read.
    for (cgltf_size e = 0; e < data->extensions_used_count; ++e) {
        const char* name = data->extensions_used[e];
        if (name && (std::strcmp(name, "KHR_draco_mesh_compression") == 0 ||
                     std::strcmp(name, "EXT_meshopt_compression") == 0))
            return fail("compressed meshes are not supported");
    }

    cgltf_size total_vertices = 0;
    for (cgltf_size pi = 0; pi < mesh.primitives_count; ++pi) {
        cgltf_primitive& prim = mesh.primitives[pi];
        if (prim.type != cgltf_primitive_type_triangles)
            return fail("every primitive must use TRIANGLES");
        if (prim.has_draco_mesh_compression)
            return fail("compressed primitives are not supported");
        if (prim.targets_count != target_count)
            return fail("all primitives must expose the same number of morph targets");
        const cgltf_accessor* position = nullptr;
        for (cgltf_size a = 0; a < prim.attributes_count; ++a) {
            if (prim.attributes[a].type == cgltf_attribute_type_position)
                position = prim.attributes[a].data;
            if (prim.attributes[a].data->buffer_view &&
                prim.attributes[a].data->buffer_view->has_meshopt_compression)
                return fail("compressed accessors are not supported");
        }
        if (!position) return fail("a primitive is missing its POSITION attribute");
        if (position->type != cgltf_type_vec3)
            return fail("POSITION must be a VEC3 accessor");
        if (prim.indices) {
            const auto ct = prim.indices->component_type;
            if (ct != cgltf_component_type_r_8u &&
                ct != cgltf_component_type_r_16u &&
                ct != cgltf_component_type_r_32u)
                return fail("index accessors must be U8/U16/U32");
        }
        for (cgltf_size ti = 0; ti < target_count; ++ti) {
            const cgltf_morph_target& target = prim.targets[ti];
            if (target.attributes_count != 1 ||
                target.attributes[0].type != cgltf_attribute_type_position)
                return fail("only POSITION morph targets are supported");
            if (target.attributes[0].data->count != position->count)
                return fail("morph target size does not match POSITION");
            if (target.attributes[0].data->buffer_view &&
                target.attributes[0].data->buffer_view->has_meshopt_compression)
                return fail("compressed accessors are not supported");
        }
        total_vertices += position->count;
    }

    if (options.labels.size() != total_vertices)
        return fail("label count must equal the connected POSITION vertex count");
    std::vector<int> label_seen(options.bone_count, 0);
    for (int label : options.labels) {
        if (label < 0 || static_cast<cgltf_size>(label) >= options.bone_count)
            return fail("every label must be in [0, K)");
        ++label_seen[label];
    }
    for (cgltf_size k = 0; k < options.bone_count; ++k)
        if (label_seen[k] == 0)
            return fail("every bone label must occur at least once");

    // The target node and every ancestor must be free of TRS animation.
    std::vector<const cgltf_node*> chain;
    for (const cgltf_node* cursor = node; cursor; cursor = cursor->parent)
        chain.push_back(cursor);
    for (cgltf_size ai = 0; ai < data->animations_count; ++ai) {
        for (cgltf_size ci = 0; ci < data->animations[ai].channels_count; ++ci) {
            const cgltf_animation_channel& channel =
                data->animations[ai].channels[ci];
            if (!channel.target_node) continue;
            if (channel.target_path != cgltf_animation_path_type_translation &&
                channel.target_path != cgltf_animation_path_type_rotation &&
                channel.target_path != cgltf_animation_path_type_scale)
                continue;
            for (const cgltf_node* ancestor : chain)
                if (channel.target_node == ancestor)
                    return fail("the target node or an ancestor is driven by a TRS animation");
        }
    }

    const cgltf_animation_sampler* weight_sampler = nullptr;
    if (!find_weight_sampler(data, node, options.animation_index, weight_sampler,
                             report.error))
        return fail(report.error);

    // Default morph weights: node weights win over mesh weights.
    std::vector<double> default_weights(target_count, 0.0);
    if (node->weights && node->weights_count == target_count) {
        for (cgltf_size m = 0; m < target_count; ++m)
            default_weights[m] = node->weights[m];
    } else if (mesh.weights && mesh.weights_count == target_count) {
        for (cgltf_size m = 0; m < target_count; ++m)
            default_weights[m] = mesh.weights[m];
    }

    const std::size_t S = options.sample_times.size();
    std::vector<double> frame_weights;
    if (!sample_morph_weights(*weight_sampler, default_weights,
                              options.sample_times, target_count,
                              frame_weights, report.error))
        return fail(report.error);

    // Static world matrix of the target node: must be finite and invertible.
    float world_f[16];
    cgltf_node_transform_world(node, world_f);
    double world[16];
    for (int i = 0; i < 16; ++i) {
        world[i] = world_f[i];
        if (!std::isfinite(world[i]))
            return fail("target node world matrix contains a non-finite entry");
    }
    double world_inverse[16];
    if (!inverse4(world, world_inverse))
        return fail("target node world matrix is not invertible");

    // Sample rest positions, morph deltas and observed positions, while
    // archiving everything needed to rebuild the glTF.
    std::vector<double> rest;
    std::vector<double> observed(S * total_vertices * 3, 0.0);
    rest.reserve(total_vertices * 3);
    report.morph_weights = frame_weights;

    // Material archive (textured materials are rejected). Distinct material
    // pointers are kept distinct even when their names happen to match.
    std::vector<int> primitive_material_index(mesh.primitives_count, -1);
    std::vector<const cgltf_material*> material_pointers;
    for (cgltf_size pi = 0; pi < mesh.primitives_count; ++pi) {
        cgltf_material* material = mesh.primitives[pi].material;
        if (!material) continue;
        if (material_uses_texture(*material))
            return fail("textured materials are not supported");
        int slot = -1;
        for (std::size_t i = 0; i < material_pointers.size(); ++i) {
            if (material_pointers[i] == material) {
                slot = static_cast<int>(i);
                break;
            }
        }
        if (slot < 0) {
            ArchivedMaterial archived;
            archived.name = material->name ? material->name : "";
            archived.has_pbr = material->has_pbr_metallic_roughness != 0;
            if (archived.has_pbr) {
                const auto& pbr = material->pbr_metallic_roughness;
                std::memcpy(archived.base_color_factor, pbr.base_color_factor,
                            sizeof(float) * 4);
                archived.metallic_factor = pbr.metallic_factor;
                archived.roughness_factor = pbr.roughness_factor;
            }
            std::memcpy(archived.emissive_factor, material->emissive_factor,
                        sizeof(float) * 3);
            archived.double_sided = material->double_sided != 0;
            archived.unlit = material->unlit != 0;
            archived.alpha_mode = material->alpha_mode;
            archived.alpha_cutoff = material->alpha_cutoff;
            report.materials.push_back(archived);
            material_pointers.push_back(material);
            slot = static_cast<int>(report.materials.size()) - 1;
        }
        primitive_material_index[pi] = slot;
    }

    cgltf_size vertex_offset = 0;
    for (cgltf_size pi = 0; pi < mesh.primitives_count; ++pi) {
        cgltf_primitive& prim = mesh.primitives[pi];
        const cgltf_accessor* position_accessor = nullptr;
        for (cgltf_size a = 0; a < prim.attributes_count; ++a)
            if (prim.attributes[a].type == cgltf_attribute_type_position)
                position_accessor = prim.attributes[a].data;
        const cgltf_size count = position_accessor->count;

        ArchivedPrimitive archived_prim;
        archived_prim.vertex_count = count;
        archived_prim.material_index = primitive_material_index[pi];
        if (prim.indices) {
            archived_prim.indexed = true;
            archived_prim.index_component_type =
                prim.indices->component_type;
            archived_prim.indices.reserve(prim.indices->count);
            for (cgltf_size i = 0; i < prim.indices->count; ++i)
                archived_prim.indices.push_back(
                    static_cast<uint32_t>(cgltf_accessor_read_index(prim.indices, i)));
        }

        // Static attributes (skip any pre-existing skinning sets; the new skin
        // replaces them).
        for (cgltf_size a = 0; a < prim.attributes_count; ++a) {
            const cgltf_attribute& attr = prim.attributes[a];
            if (attr.type == cgltf_attribute_type_joints ||
                attr.type == cgltf_attribute_type_weights)
                continue;
            std::vector<float> values = unpack_accessor(attr.data);
            if (values.empty())
                return fail("failed to unpack a vertex attribute");
            ArchivedAttribute archived;
            archived.type = attr.type;
            archived.index = attr.index;
            archived.name = attribute_name(attr.type, attr.index, attr.name);
            archived.accessor_type = attr.data->type;
            archived.values = std::move(values);
            archived_prim.attributes.push_back(std::move(archived));
        }

        std::vector<float> base = unpack_accessor(position_accessor);
        if (base.size() != count * 3)
            return fail("failed to unpack POSITION");
        std::vector<std::vector<float>> deltas(target_count);
        for (cgltf_size ti = 0; ti < target_count; ++ti) {
            deltas[ti] = unpack_accessor(
                prim.targets[ti].attributes[0].data);
            if (deltas[ti].size() != count * 3)
                return fail("failed to unpack a POSITION morph target");
        }

        for (cgltf_size v = 0; v < count; ++v) {
            const cgltf_size global_v = vertex_offset + v;
            for (int r = 0; r < 3; ++r) {
                const double p = base[v * 3 + r];
                if (!std::isfinite(p))
                    return fail("non-finite rest position in POSITION");
                rest.push_back(p);
            }
            for (std::size_t s = 0; s < S; ++s) {
                for (int r = 0; r < 3; ++r) {
                    double x = base[v * 3 + r];
                    for (cgltf_size m = 0; m < target_count; ++m) {
                        const double d = deltas[m][v * 3 + r];
                        if (!std::isfinite(d))
                            return fail("non-finite morph delta");
                        x += frame_weights[s * target_count + m] * d;
                    }
                    if (!std::isfinite(x))
                        return fail("non-finite sampled position");
                    observed[(s * total_vertices + global_v) * 3 + r] = x;
                }
            }
        }
        report.primitives.push_back(std::move(archived_prim));
        vertex_offset += count;
    }

    // Hand the sampled problem to the joint solver.
    SkinProblem problem;
    problem.frame_count = S;
    problem.vertex_count = total_vertices;
    problem.bone_count = options.bone_count;
    problem.max_influences = options.max_influences;
    problem.iterations = options.iterations;
    problem.max_error = options.max_error;
    problem.rest_positions = rest;
    problem.observed_positions = observed;
    problem.labels = options.labels;

    SkinSolution solution = solve(problem);
    if (!solution.ok)
        return fail(std::string("solver failure: ") + solution.error);

    // Fill the self-contained report.
    report.ok = true;
    report.error.clear();
    report.sample_times = options.sample_times;
    report.frames = S;
    report.vertices = total_vertices;
    report.bones = options.bone_count;
    report.influences = options.max_influences;
    report.max_error = options.max_error;
    report.rest_positions = std::move(rest);
    report.observed_positions = std::move(observed);
    report.solution = std::move(solution);
    for (int i = 0; i < 16; ++i) report.world_matrix[i] = world[i];

    compute_float_error(report, report.max_local_error, report.worst_frame,
                        report.worst_vertex);
    return report;
}

namespace {

// ---------------------------------------------------------------------------
// Construction of the standalone output cgltf_data. All allocations are owned
// by the builder and released when it goes out of scope; the source asset is
// untouched.
// ---------------------------------------------------------------------------

struct ExportBuilder {
    std::vector<void*> owned;
    std::vector<unsigned char> bin;
    std::vector<cgltf_buffer> buffers;
    std::vector<cgltf_buffer_view> buffer_views;
    std::vector<cgltf_accessor> accessors;
    std::vector<cgltf_material> materials;
    std::vector<cgltf_mesh> meshes;
    std::vector<cgltf_node> nodes;
    std::vector<cgltf_skin> skins;
    std::vector<cgltf_animation> animations;
    std::vector<cgltf_animation_sampler> samplers;
    std::vector<cgltf_animation_channel> channels;
    std::vector<cgltf_scene> scenes;
    std::vector<std::vector<cgltf_attribute>> attribute_storage;
    std::vector<cgltf_primitive> primitive_storage;
    cgltf_data data{};

    template <class T> T* allocate(std::size_t count = 1) {
        T* ptr = static_cast<T*>(std::calloc(count, sizeof(T)));
        if (ptr) owned.push_back(ptr);
        return ptr;
    }

    char* dup_string(const std::string& value) {
        char* ptr = allocate<char>(value.size() + 1);
        if (ptr) std::memcpy(ptr, value.c_str(), value.size() + 1);
        return ptr;
    }

    void align4() {
        while (bin.size() % 4 != 0) bin.push_back(0);
    }

    cgltf_size add_view(const void* bytes, std::size_t byte_size,
                        cgltf_size stride) {
        align4();
        const cgltf_size offset = static_cast<cgltf_size>(bin.size());
        const auto* p = static_cast<const unsigned char*>(bytes);
        bin.insert(bin.end(), p, p + byte_size);
        cgltf_buffer_view view{};
        view.offset = offset;
        view.size = byte_size;
        view.stride = stride;
        view.type = cgltf_buffer_view_type_vertices;
        buffer_views.push_back(view);
        return buffer_views.size() - 1;
    }

    cgltf_size add_float_accessor(const std::vector<float>& values,
                                  cgltf_type type, cgltf_size components,
                                  const float* min_values = nullptr,
                                  const float* max_values = nullptr) {
        const cgltf_size count = values.size() / components;
        const cgltf_size stride = components * sizeof(float);
        const cgltf_size view_index =
            add_view(values.data(), values.size() * sizeof(float), stride);
        cgltf_accessor accessor{};
        accessor.component_type = cgltf_component_type_r_32f;
        accessor.type = type;
        accessor.count = count;
        accessor.stride = stride;
        accessor.offset = 0;
        if (min_values && max_values) {
            accessor.has_min = 1;
            accessor.has_max = 1;
            for (cgltf_size i = 0; i < components; ++i) {
                accessor.min[i] = min_values[i];
                accessor.max[i] = max_values[i];
            }
        }
        accessors.push_back(accessor);
        (void)view_index;
        accessors.back().buffer_view = nullptr; // wired up after allocation
        return accessors.size() - 1;
    }

    cgltf_size add_index_accessor(const std::vector<uint32_t>& indices,
                                  cgltf_component_type component_type) {
        cgltf_accessor accessor{};
        accessor.type = cgltf_type_scalar;
        accessor.count = indices.size();
        accessor.offset = 0;
        if (component_type == cgltf_component_type_r_8u) {
            std::vector<uint8_t> small(indices.begin(), indices.end());
            accessor.component_type = cgltf_component_type_r_8u;
            accessor.stride = sizeof(uint8_t);
            add_view(small.data(), small.size(), sizeof(uint8_t));
        } else if (component_type == cgltf_component_type_r_16u) {
            std::vector<uint16_t> small(indices.begin(), indices.end());
            accessor.component_type = cgltf_component_type_r_16u;
            accessor.stride = sizeof(uint16_t);
            add_view(small.data(), small.size() * sizeof(uint16_t),
                     sizeof(uint16_t));
        } else {
            accessor.component_type = cgltf_component_type_r_32u;
            accessor.stride = sizeof(uint32_t);
            add_view(indices.data(), indices.size() * sizeof(uint32_t),
                     sizeof(uint32_t));
        }
        accessors.push_back(accessor);
        accessors.back().buffer_view = nullptr;
        return accessors.size() - 1;
    }

    cgltf_size add_u8vec4_accessor(const std::vector<uint8_t>& values) {
        const cgltf_size view_index =
            add_view(values.data(), values.size(), sizeof(uint8_t) * 4);
        cgltf_accessor accessor{};
        accessor.component_type = cgltf_component_type_r_8u;
        accessor.type = cgltf_type_vec4;
        accessor.count = values.size() / 4;
        accessor.stride = 4;
        accessor.offset = 0;
        accessors.push_back(accessor);
        accessors.back().buffer_view = nullptr;
        (void)view_index;
        return accessors.size() - 1;
    }

    ~ExportBuilder() {
        for (void* ptr : owned) std::free(ptr);
    }
};

std::string base64_encode(const std::vector<unsigned char>& bytes) {
    static const char* table =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((bytes.size() + 2) / 3) * 4);
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const unsigned b0 = bytes[i];
        const unsigned b1 = i + 1 < bytes.size() ? bytes[i + 1] : 0;
        const unsigned b2 = i + 2 < bytes.size() ? bytes[i + 2] : 0;
        const unsigned triple = (b0 << 16) | (b1 << 8) | b2;
        out.push_back(table[(triple >> 18) & 63]);
        out.push_back(table[(triple >> 12) & 63]);
        out.push_back(i + 1 < bytes.size() ? table[(triple >> 6) & 63] : '=');
        out.push_back(i + 2 < bytes.size() ? table[triple & 63] : '=');
    }
    return out;
}

// Verification on the re-parsed output file: real glTF skinning at every
// sample keyframe.
bool verify_written_file(cgltf_data* data, const DecomposeReport& report,
                         double& measured_error, std::string& error) {
    if (cgltf_validate(data) != cgltf_result_success) {
        error = "re-parsed output failed cgltf_validate";
        return false;
    }
    if (data->meshes_count != 1 || data->skins_count != 1 ||
        data->animations_count != 1 || data->scenes_count != 1) {
        error = "re-parsed output has an unexpected top-level structure";
        return false;
    }
    cgltf_mesh& mesh = data->meshes[0];
    cgltf_skin& skin = data->skins[0];
    const cgltf_size K = skin.joints_count;
    const cgltf_size S = report.frames;
    if (K != report.bones) {
        error = "re-parsed output bone count mismatch";
        return false;
    }
    if (mesh.primitives_count != report.primitives.size()) {
        error = "re-parsed output primitive count mismatch";
        return false;
    }
    if (mesh.primitives[0].targets_count != 0) {
        error = "re-parsed output still contains morph targets";
        return false;
    }

    // Map each joint's translation/rotation channel by target node and path.
    std::vector<const cgltf_accessor*> translation_acc(K, nullptr);
    std::vector<const cgltf_accessor*> rotation_acc(K, nullptr);
    cgltf_animation& animation = data->animations[0];
    for (cgltf_size c = 0; c < animation.channels_count; ++c) {
        cgltf_animation_channel& channel = animation.channels[c];
        cgltf_size joint = K;
        for (cgltf_size k = 0; k < K; ++k)
            if (skin.joints[k] == channel.target_node) joint = k;
        if (joint == K) continue;
        if (channel.target_path == cgltf_animation_path_type_translation)
            translation_acc[joint] = channel.sampler->output;
        else if (channel.target_path == cgltf_animation_path_type_rotation)
            rotation_acc[joint] = channel.sampler->output;
        if (channel.sampler->interpolation != cgltf_interpolation_type_linear) {
            error = "bone animation must use LINEAR interpolation";
            return false;
        }
    }
    for (cgltf_size k = 0; k < K; ++k)
        if (!translation_acc[k] || !rotation_acc[k]) {
            error = "a bone is missing a translation or rotation channel";
            return false;
        }

    std::vector<std::vector<float>> trans_f(K), rot_f(K);
    for (cgltf_size k = 0; k < K; ++k) {
        trans_f[k] = unpack_accessor(translation_acc[k]);
        rot_f[k] = unpack_accessor(rotation_acc[k]);
        if (trans_f[k].size() != S * 3 || rot_f[k].size() != S * 4) {
            error = "bone animation key count does not match the sample count";
            return false;
        }
        for (cgltf_size s = 1; s < S; ++s) {
            double dot = 0.0;
            for (int r = 0; r < 4; ++r)
                dot += static_cast<double>(rot_f[k][(s - 1) * 4 + r]) *
                       rot_f[k][s * 4 + r];
            if (dot < 0.0) {
                error = "adjacent rotation quaternions leave the same hemisphere";
                return false;
            }
        }
    }

    // Identity inverse-bind check.
    std::vector<float> ibm = unpack_accessor(skin.inverse_bind_matrices);
    if (ibm.size() != K * 16) {
        error = "inverse-bind accessor has the wrong size";
        return false;
    }
    for (cgltf_size k = 0; k < K; ++k)
        for (int i = 0; i < 16; ++i) {
            const float want = (i % 5 == 0) ? 1.0f : 0.0f;
            if (std::fabs(ibm[k * 16 + i] - want) > 1e-6f) {
                error = "inverse-bind matrices must be identity";
                return false;
            }
        }

    measured_error = -1.0;
    cgltf_size global_vertex = 0;
    for (cgltf_size pi = 0; pi < mesh.primitives_count; ++pi) {
        cgltf_primitive& prim = mesh.primitives[pi];
        const cgltf_accessor* position_acc = nullptr;
        const cgltf_accessor* joints_acc = nullptr;
        const cgltf_accessor* weights_acc = nullptr;
        for (cgltf_size a = 0; a < prim.attributes_count; ++a) {
            if (prim.attributes[a].type == cgltf_attribute_type_position)
                position_acc = prim.attributes[a].data;
            if (prim.attributes[a].type == cgltf_attribute_type_joints)
                joints_acc = prim.attributes[a].data;
            if (prim.attributes[a].type == cgltf_attribute_type_weights)
                weights_acc = prim.attributes[a].data;
        }
        if (!position_acc || !joints_acc || !weights_acc) {
            error = "output primitive is missing POSITION/JOINTS_0/WEIGHTS_0";
            return false;
        }
        std::vector<float> positions = unpack_accessor(position_acc);
        std::vector<float> weights = unpack_accessor(weights_acc);
        std::vector<float> joints_f = unpack_accessor(joints_acc);
        const cgltf_size count = position_acc->count;
        if (positions.size() != count * 3 || weights.size() != count * 4 ||
            joints_f.size() != count * 4) {
            error = "output vertex attribute size mismatch";
            return false;
        }

        for (std::size_t s = 0; s < S; ++s) {
            std::vector<double> R(K * 9), t(K * 3);
            for (cgltf_size k = 0; k < K; ++k) {
                double q[4] = {rot_f[k][s * 4 + 0], rot_f[k][s * 4 + 1],
                               rot_f[k][s * 4 + 2], rot_f[k][s * 4 + 3]};
                quaternion_to_matrix(q, &R[k * 9]);
                for (int r = 0; r < 3; ++r)
                    t[k * 3 + r] = trans_f[k][s * 3 + r];
            }
            for (cgltf_size v = 0; v < count; ++v) {
                const cgltf_size global_v = global_vertex + v;
                const float* p = &positions[v * 3];
                double skinned[3] = {0.0, 0.0, 0.0};
                for (int slot = 0; slot < 4; ++slot) {
                    const float w = weights[v * 4 + slot];
                    if (w == 0.0f) continue;
                    const int k = static_cast<int>(joints_f[v * 4 + slot]);
                    if (k < 0 || static_cast<cgltf_size>(k) >= K) {
                        error = "joint index out of range in output";
                        return false;
                    }
                    for (int r = 0; r < 3; ++r) {
                        const double y = R[k * 9 + r * 3 + 0] * p[0] +
                                         R[k * 9 + r * 3 + 1] * p[1] +
                                         R[k * 9 + r * 3 + 2] * p[2] +
                                         t[k * 3 + r];
                        skinned[r] += static_cast<double>(w) * y;
                    }
                }
                double norm2 = 0.0;
                for (int r = 0; r < 3; ++r) {
                    const double d =
                        report.observed_positions[(s * report.vertices + global_v) * 3 + r] -
                        skinned[r];
                    norm2 += d * d;
                }
                measured_error = std::max(measured_error, std::sqrt(norm2));
            }
        }
        global_vertex += count;
    }
    if (global_vertex != report.vertices) {
        error = "re-parsed output vertex count mismatch";
        return false;
    }
    if (!std::isfinite(measured_error)) {
        error = "non-finite error while verifying the written skin";
        return false;
    }
    return true;
}

} // namespace

cgltf_result export_decomposed_gltf(const DecomposeReport& report,
                                    const char* output_path,
                                    std::string* error_out) {
    auto fail = [&](cgltf_result code, const std::string& message) {
        if (error_out) *error_out = message;
        return code;
    };
    if (!report.ok)
        return fail(cgltf_result_invalid_options,
                    "cannot export a report that is not ok");
    if (!output_path || !*output_path)
        return fail(cgltf_result_invalid_options, "output path must not be empty");

    const SkinSolution& sol = report.solution;
    const cgltf_size S = report.frames;
    const cgltf_size K = report.bones;

    ExportBuilder b;

    // ----- Materials (scalar fields only). ---------------------------------
    for (const ArchivedMaterial& src : report.materials) {
        cgltf_material material{};
        material.name = b.dup_string(src.name);
        material.has_pbr_metallic_roughness = src.has_pbr ? 1 : 0;
        if (src.has_pbr) {
            auto& pbr = material.pbr_metallic_roughness;
            std::memcpy(pbr.base_color_factor, src.base_color_factor,
                        sizeof(float) * 4);
            pbr.metallic_factor = src.metallic_factor;
            pbr.roughness_factor = src.roughness_factor;
        }
        std::memcpy(material.emissive_factor, src.emissive_factor,
                    sizeof(float) * 3);
        material.double_sided = src.double_sided ? 1 : 0;
        material.unlit = src.unlit ? 1 : 0;
        material.alpha_mode = src.alpha_mode;
        material.alpha_cutoff = src.alpha_cutoff;
        b.materials.push_back(material);
    }

    // ----- Mesh primitives. JOINTS_0 / WEIGHTS_0 are per primitive so every
    // attribute of a primitive shares its vertex count. ----------------------
    b.attribute_storage.resize(report.primitives.size());
    cgltf_size vertex_cursor = 0;
    for (cgltf_size pi = 0; pi < report.primitives.size(); ++pi) {
        const ArchivedPrimitive& src = report.primitives[pi];
        cgltf_primitive prim{};
        prim.type = cgltf_primitive_type_triangles;
        if (src.material_index >= 0)
            prim.material = nullptr; // wired later by index

        auto& attrs = b.attribute_storage[pi];
        for (const ArchivedAttribute& src_attr : src.attributes) {
            const cgltf_size components =
                cgltf_num_components(src_attr.accessor_type);
            const float* min_values = nullptr;
            const float* max_values = nullptr;
            std::array<float, 3> lo{}, hi{};
            if (src_attr.type == cgltf_attribute_type_position) {
                lo = {std::numeric_limits<float>::infinity(),
                      std::numeric_limits<float>::infinity(),
                      std::numeric_limits<float>::infinity()};
                hi = {-std::numeric_limits<float>::infinity(),
                      -std::numeric_limits<float>::infinity(),
                      -std::numeric_limits<float>::infinity()};
                for (cgltf_size i = 0; i < src.vertex_count; ++i)
                    for (int r = 0; r < 3; ++r) {
                        lo[r] = std::min(lo[r], src_attr.values[i * 3 + r]);
                        hi[r] = std::max(hi[r], src_attr.values[i * 3 + r]);
                    }
                min_values = lo.data();
                max_values = hi.data();
            }
            b.add_float_accessor(src_attr.values, src_attr.accessor_type,
                                 components, min_values, max_values);
            cgltf_attribute attr{};
            attr.type = src_attr.type;
            attr.index = src_attr.index;
            attr.name = b.dup_string(src_attr.name);
            attr.data = nullptr; // wired later
            attrs.push_back(attr);
        }
        // Per-primitive skinning attributes sliced from the connected vertex
        // numbering, which assigns primitive pi the range
        // [vertex_cursor, vertex_cursor + vertex_count).
        std::vector<uint8_t> joints_bytes(src.vertex_count * 4, 0);
        std::vector<float> weights_float(src.vertex_count * 4, 0.0f);
        for (cgltf_size local_v = 0; local_v < src.vertex_count; ++local_v) {
            const cgltf_size global_v = vertex_cursor + local_v;
            cgltf_size slot = 0;
            for (cgltf_size k = 0; k < K && slot < 4; ++k) {
                const double w = sol.weights[global_v * K + k];
                if (w <= 0.0) continue;
                joints_bytes[local_v * 4 + slot] = static_cast<uint8_t>(k);
                weights_float[local_v * 4 + slot] = static_cast<float>(w);
                ++slot;
            }
        }
        b.add_u8vec4_accessor(joints_bytes);
        attrs.push_back(cgltf_attribute{});
        attrs.back().type = cgltf_attribute_type_joints;
        attrs.back().index = 0;
        attrs.back().name = b.dup_string("JOINTS_0");
        b.add_float_accessor(weights_float, cgltf_type_vec4, 4);
        attrs.push_back(cgltf_attribute{});
        attrs.back().type = cgltf_attribute_type_weights;
        attrs.back().index = 0;
        attrs.back().name = b.dup_string("WEIGHTS_0");

        if (src.indexed)
            b.add_index_accessor(src.indices, src.index_component_type);
        prim.attributes_count = attrs.size();
        b.primitive_storage.push_back(prim);
        vertex_cursor += src.vertex_count;
    }

    cgltf_mesh mesh{};
    mesh.primitives_count = b.primitive_storage.size();
    b.meshes.push_back(mesh);

    // ----- Identity inverse bind matrices. ---------------------------------
    std::vector<float> identity(K * 16, 0.0f);
    for (cgltf_size k = 0; k < K; ++k)
        for (int i = 0; i < 4; ++i) identity[k * 16 + i * 4 + i] = 1.0f;
    b.add_float_accessor(identity, cgltf_type_mat4, 16);

    // ----- Bone animation: shared time input, per-bone TRS outputs. --------
    std::vector<float> times(S);
    for (cgltf_size s = 0; s < S; ++s)
        times[s] = static_cast<float>(report.sample_times[s]);
    b.add_float_accessor(times, cgltf_type_scalar, 1);

    std::vector<std::vector<std::array<float, 4>>> bone_quats(K);
    for (cgltf_size k = 0; k < K; ++k) {
        std::vector<float> translations(S * 3);
        for (std::size_t s = 0; s < S; ++s) {
            const double* t = sol.transforms[s * K + k].translation;
            translations[s * 3 + 0] = static_cast<float>(t[0]);
            translations[s * 3 + 1] = static_cast<float>(t[1]);
            translations[s * 3 + 2] = static_cast<float>(t[2]);
        }
        b.add_float_accessor(translations, cgltf_type_vec3, 3);

        bone_quats[k] = build_bone_quaternions(sol, k);
        std::vector<float> rotations(S * 4);
        for (std::size_t s = 0; s < S; ++s)
            for (int r = 0; r < 4; ++r)
                rotations[s * 4 + r] = bone_quats[k][s][r];
        b.add_float_accessor(rotations, cgltf_type_vec4, 4);
    }

    // ----- Nodes: common root, mesh node, K independent joints. ------------
    // Node indices: 0 = root, 1 = mesh node, 2..K+1 = joints.
    const cgltf_size root_index = 0;
    const cgltf_size mesh_node_index = 1;
    const cgltf_size joint_node_start = 2;

    std::vector<cgltf_node*> root_children;
    root_children.reserve(1 + K);
    b.nodes.resize(2 + K);
    for (cgltf_size i = 0; i < 2 + K; ++i) {
        cgltf_node& n = b.nodes[i];
        n.has_translation = 0;
        n.has_rotation = 0;
        n.has_scale = 0;
        n.has_matrix = (i == root_index) ? 1 : 0;
    }
    for (int i = 0; i < 16; ++i)
        b.nodes[root_index].matrix[i] = static_cast<float>(report.world_matrix[i]);

    // ----- Skin. ------------------------------------------------------------
    cgltf_skin skin{};
    skin.joints_count = K;
    b.skins.push_back(skin);

    // ----- Animation structure. --------------------------------------------
    cgltf_animation animation{};
    animation.samplers_count = 2 * K;
    animation.channels_count = 2 * K;
    b.animations.push_back(animation);
    for (cgltf_size k = 0; k < K; ++k) {
        cgltf_animation_sampler ts{};
        ts.interpolation = cgltf_interpolation_type_linear;
        b.samplers.push_back(ts);
        cgltf_animation_sampler rs{};
        rs.interpolation = cgltf_interpolation_type_linear;
        b.samplers.push_back(rs);

        cgltf_animation_channel tc{};
        tc.target_path = cgltf_animation_path_type_translation;
        b.channels.push_back(tc);
        cgltf_animation_channel rc{};
        rc.target_path = cgltf_animation_path_type_rotation;
        b.channels.push_back(rc);
    }

    cgltf_scene scene{};
    b.scenes.push_back(scene);

    // ----- Single embedded buffer with a base64 data URI. ------------------
    cgltf_buffer buffer{};
    buffer.size = b.bin.size();
    buffer.data = b.allocate<unsigned char>(b.bin.size());
    if (!buffer.data)
        return fail(cgltf_result_out_of_memory, "out of memory while exporting");
    std::memcpy(buffer.data, b.bin.data(), b.bin.size());
    buffer.data_free_method = cgltf_data_free_method_none;
    const std::string uri =
        "data:application/octet-stream;base64," + base64_encode(b.bin);
    buffer.uri = b.dup_string(uri);
    b.buffers.push_back(buffer);

    // ----- Allocate the flat arrays and wire every pointer. ----------------
    cgltf_data& out = b.data;
    out.asset.version = b.dup_string("2.0");
    out.asset.generator = b.dup_string("cgltf-skin-decompose");

    out.buffers_count = b.buffers.size();
    out.buffers = b.allocate<cgltf_buffer>(out.buffers_count);
    std::memcpy(out.buffers, b.buffers.data(),
                out.buffers_count * sizeof(cgltf_buffer));

    out.buffer_views_count = b.buffer_views.size();
    out.buffer_views = b.allocate<cgltf_buffer_view>(out.buffer_views_count);
    std::memcpy(out.buffer_views, b.buffer_views.data(),
                out.buffer_views_count * sizeof(cgltf_buffer_view));
    for (cgltf_size i = 0; i < out.buffer_views_count; ++i)
        out.buffer_views[i].buffer = &out.buffers[0];

    out.accessors_count = b.accessors.size();
    out.accessors = b.allocate<cgltf_accessor>(out.accessors_count);
    std::memcpy(out.accessors, b.accessors.data(),
                out.accessors_count * sizeof(cgltf_accessor));
    // Accessor i corresponds to buffer view i: every accessor got its own view.
    for (cgltf_size i = 0; i < out.accessors_count; ++i)
        out.accessors[i].buffer_view = &out.buffer_views[i];

    out.materials_count = b.materials.size();
    if (out.materials_count) {
        out.materials = b.allocate<cgltf_material>(out.materials_count);
        std::memcpy(out.materials, b.materials.data(),
                    out.materials_count * sizeof(cgltf_material));
    }

    // Primitives: resolve attribute and material pointers. Accessors were
    // created in strict order (each attribute and each index set gets one),
    // so they map onto the accessor array one-to-one from the start.
    cgltf_size accessor_cursor = 0;
    out.meshes_count = 1;
    out.meshes = b.allocate<cgltf_mesh>(1);
    cgltf_primitive* out_prims =
        b.allocate<cgltf_primitive>(b.primitive_storage.size());
    for (cgltf_size pi = 0; pi < b.primitive_storage.size(); ++pi) {
        const ArchivedPrimitive& src = report.primitives[pi];
        cgltf_primitive prim = b.primitive_storage[pi];
        auto& attr_vec = b.attribute_storage[pi];
        cgltf_attribute* out_attrs =
            b.allocate<cgltf_attribute>(attr_vec.size());
        for (std::size_t a = 0; a < attr_vec.size(); ++a) {
            out_attrs[a] = attr_vec[a];
            out_attrs[a].data = &out.accessors[accessor_cursor++];
        }
        prim.attributes = out_attrs;
        prim.attributes_count = attr_vec.size();
        if (src.indexed)
            prim.indices = &out.accessors[accessor_cursor++];
        if (src.material_index >= 0)
            prim.material = &out.materials[src.material_index];
        out_prims[pi] = prim;
    }
    out.meshes[0] = b.meshes[0];
    out.meshes[0].primitives = out_prims;
    out.meshes[0].primitives_count = b.primitive_storage.size();

    // Remaining accessors after the primitive block: IBM, times, bone outputs.
    const cgltf_size ibm_index = accessor_cursor++;
    const cgltf_size time_index = accessor_cursor++;
    std::vector<cgltf_size> trans_index(K), rot_index(K);
    for (cgltf_size k = 0; k < K; ++k) {
        trans_index[k] = accessor_cursor++;
        rot_index[k] = accessor_cursor++;
    }
    if (accessor_cursor != out.accessors_count)
        return fail(cgltf_result_invalid_gltf,
                    "internal export accessor bookkeeping mismatch");

    // Nodes.
    out.nodes_count = b.nodes.size();
    out.nodes = b.allocate<cgltf_node>(out.nodes_count);
    std::memcpy(out.nodes, b.nodes.data(),
                out.nodes_count * sizeof(cgltf_node));
    cgltf_node** root_child_ptrs = b.allocate<cgltf_node*>(1 + K);
    root_child_ptrs[0] = &out.nodes[mesh_node_index];
    for (cgltf_size k = 0; k < K; ++k)
        root_child_ptrs[1 + k] = &out.nodes[joint_node_start + k];
    out.nodes[root_index].children = root_child_ptrs;
    out.nodes[root_index].children_count = 1 + K;
    out.nodes[mesh_node_index].mesh = &out.meshes[0];

    // Skin.
    out.skins_count = 1;
    out.skins = b.allocate<cgltf_skin>(1);
    out.skins[0] = b.skins[0];
    cgltf_node** joint_ptrs = b.allocate<cgltf_node*>(K);
    for (cgltf_size k = 0; k < K; ++k)
        joint_ptrs[k] = &out.nodes[joint_node_start + k];
    out.skins[0].joints = joint_ptrs;
    out.skins[0].inverse_bind_matrices = &out.accessors[ibm_index];
    out.nodes[mesh_node_index].skin = &out.skins[0];

    // Animation.
    out.animations_count = 1;
    out.animations = b.allocate<cgltf_animation>(1);
    out.animations[0] = b.animations[0];
    cgltf_animation_sampler* out_samplers =
        b.allocate<cgltf_animation_sampler>(b.samplers.size());
    cgltf_animation_channel* out_channels =
        b.allocate<cgltf_animation_channel>(b.channels.size());
    for (cgltf_size k = 0; k < K; ++k) {
        cgltf_animation_sampler& ts = out_samplers[2 * k];
        ts = b.samplers[2 * k];
        ts.input = &out.accessors[time_index];
        ts.output = &out.accessors[trans_index[k]];
        cgltf_animation_sampler& rs = out_samplers[2 * k + 1];
        rs = b.samplers[2 * k + 1];
        rs.input = &out.accessors[time_index];
        rs.output = &out.accessors[rot_index[k]];

        cgltf_animation_channel& tc = out_channels[2 * k];
        tc = b.channels[2 * k];
        tc.sampler = &out_samplers[2 * k];
        tc.target_node = &out.nodes[joint_node_start + k];
        cgltf_animation_channel& rc = out_channels[2 * k + 1];
        rc = b.channels[2 * k + 1];
        rc.sampler = &out_samplers[2 * k + 1];
        rc.target_node = &out.nodes[joint_node_start + k];
    }
    out.animations[0].samplers = out_samplers;
    out.animations[0].samplers_count = b.samplers.size();
    out.animations[0].channels = out_channels;
    out.animations[0].channels_count = b.channels.size();

    // Scene.
    out.scenes_count = 1;
    out.scenes = b.allocate<cgltf_scene>(1);
    out.scenes[0] = b.scenes[0];
    cgltf_node** scene_nodes = b.allocate<cgltf_node*>(1);
    scene_nodes[0] = &out.nodes[root_index];
    out.scenes[0].nodes = scene_nodes;
    out.scenes[0].nodes_count = 1;
    out.scene = &out.scenes[0];

    // ----- Write to a temporary file, reload and verify. -------------------
    const std::string temp_path = std::string(output_path) + ".tmp";
    cgltf_options options{};
    cgltf_result write_result = cgltf_write_file(&options, temp_path.c_str(), &out);
    if (write_result != cgltf_result_success) {
        std::remove(temp_path.c_str());
        return fail(write_result, "cgltf_write_file failed for the temporary output");
    }

    cgltf_data* reloaded = nullptr;
    cgltf_result parse_result = cgltf_parse_file(&options, temp_path.c_str(), &reloaded);
    if (parse_result != cgltf_result_success) {
        std::remove(temp_path.c_str());
        return fail(parse_result, "failed to re-parse the temporary output");
    }
    cgltf_result load_result = cgltf_load_buffers(&options, reloaded, temp_path.c_str());
    if (load_result != cgltf_result_success) {
        cgltf_free(reloaded);
        std::remove(temp_path.c_str());
        return fail(load_result, "failed to load buffers of the temporary output");
    }

    double measured_error = 0.0;
    std::string verify_error;
    const bool verified =
        verify_written_file(reloaded, report, measured_error, verify_error);
    cgltf_free(reloaded);
    if (!verified) {
        std::remove(temp_path.c_str());
        return fail(cgltf_result_invalid_gltf, verify_error);
    }
    if (measured_error > report.max_error) {
        std::remove(temp_path.c_str());
        return fail(cgltf_result_invalid_gltf,
                    "written skin error " + std::to_string(measured_error) +
                        " exceeds the allowed max error " +
                        std::to_string(report.max_error));
    }

    if (std::rename(temp_path.c_str(), output_path) != 0) {
        std::remove(temp_path.c_str());
        return fail(cgltf_result_io_error,
                    "failed to replace the destination file atomically");
    }
    if (error_out) error_out->clear();
    return cgltf_result_success;
}

} // namespace skin_decompose
