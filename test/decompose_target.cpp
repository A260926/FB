// decompose_target.cpp
//
// Task test suite for the morph-to-skin decomposition. No main(): the cases
// are driven by task_support/driver.c through task_cases / task_case_count.
//
// Three groups are registered:
//   optimization - numerical behaviour of the alternating joint solver,
//                  checked against independent small enumerations;
//   asset        - cgltf sampling, export and reload behaviour on hand-built
//                  in-memory glTF assets (sparse, stride, multi primitive);
//   contracts    - input validation, determinism and failure isolation.
//
// Everything here uses the C++17 standard library and the in-repository
// cgltf reader/writer only.
#include "task_support/check.h"

#include "../examples/cgltf_skin_decompose.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace skin_decompose;

namespace decompose_tests {

// ---------------------------------------------------------------------------
// Generic numerical helpers.
// ---------------------------------------------------------------------------

constexpr double kPi = 3.14159265358979323846;

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

void rot_z(double angle, double R[9]) {
    const double c = std::cos(angle), s = std::sin(angle);
    R[0] = c;  R[1] = -s; R[2] = 0;
    R[3] = s;  R[4] = c;  R[5] = 0;
    R[6] = 0;  R[7] = 0;  R[8] = 1;
}

double det3(const double m[9]) {
    return m[0] * (m[4] * m[8] - m[5] * m[7]) -
           m[1] * (m[3] * m[8] - m[5] * m[6]) +
           m[2] * (m[3] * m[7] - m[4] * m[6]);
}

// Independent rigid reference for planar data: enumerate z-rotation angles
// and pick the best weighted translation for each candidate rotation.
struct EnumeratedRigid {
    double angle;
    double objective;
    double R[9];
    double t[3];
};

EnumeratedRigid enumerate_rigid_z(const std::vector<double>& a,
                                  const std::vector<double>& b,
                                  const std::vector<double>& c,
                                  const std::vector<std::size_t>& members) {
    double wsum = 0.0, abar[3] = {0, 0, 0}, bbar[3] = {0, 0, 0};
    for (std::size_t idx : members) {
        wsum += c[idx];
        for (int r = 0; r < 3; ++r) {
            abar[r] += c[idx] * a[idx * 3 + r];
            bbar[r] += c[idx] * b[idx * 3 + r];
        }
    }
    for (int r = 0; r < 3; ++r) {
        abar[r] /= wsum;
        bbar[r] /= wsum;
    }
    EnumeratedRigid best{};
    best.objective = std::numeric_limits<double>::infinity();
    constexpr int kSteps = 200001;
    for (int i = 0; i < kSteps; ++i) {
        const double angle = -kPi + 2.0 * kPi * i / (kSteps - 1);
        double R[9];
        rot_z(angle, R);
        double t[3];
        for (int r = 0; r < 3; ++r)
            t[r] = bbar[r] - (R[r * 3] * abar[0] + R[r * 3 + 1] * abar[1] +
                              R[r * 3 + 2] * abar[2]);
        double objective = 0.0;
        for (std::size_t idx : members) {
            for (int r = 0; r < 3; ++r) {
                const double y = R[r * 3] * a[idx * 3] +
                                 R[r * 3 + 1] * a[idx * 3 + 1] +
                                 R[r * 3 + 2] * a[idx * 3 + 2] + t[r];
                const double d = b[idx * 3 + r] - y;
                objective += c[idx] * d * d;
            }
        }
        if (objective < best.objective) {
            best.objective = objective;
            best.angle = angle;
            std::memcpy(best.R, R, sizeof(R));
            std::memcpy(best.t, t, sizeof(t));
        }
    }
    return best;
}

double rigid_objective(const std::vector<double>& a,
                       const std::vector<double>& b,
                       const std::vector<double>& c,
                       const std::vector<std::size_t>& members,
                       const RigidTransform& tf) {
    double objective = 0.0;
    for (std::size_t idx : members) {
        for (int r = 0; r < 3; ++r) {
            const double y = tf.rotation[r * 3] * a[idx * 3] +
                             tf.rotation[r * 3 + 1] * a[idx * 3 + 1] +
                             tf.rotation[r * 3 + 2] * a[idx * 3 + 2] +
                             tf.translation[r];
            const double d = b[idx * 3 + r] - y;
            objective += c[idx] * d * d;
        }
    }
    return objective;
}

// Two-region ground truth: +1 region follows +theta, -1 region follows
// -theta, zero region is an exact 50/50 blend.
struct BlendScene {
    std::size_t S = 5, V = 9, K = 2;
    std::vector<double> rest;
    std::vector<double> observed;
    std::vector<int> labels;
    std::vector<double> truth_weights;
};

BlendScene make_blend_scene() {
    BlendScene scene;
    const int region[9] = {+1, +1, +1, 0, 0, +1, 0, -1, -1};
    const double points[9][3] = {
        {1.0, 0.0, 0.0}, {0.8, 0.3, 0.1}, {0.6, 0.6, -0.2},
        {0.3, 0.8, 0.2}, {-0.2, 0.9, 0.0}, {-0.6, 0.5, 0.1},
        {0.0, -0.7, 0.4}, {-0.5, -0.6, -0.3}, {-0.9, -0.2, 0.2}};
    scene.rest.resize(scene.V * 3);
    scene.truth_weights.resize(scene.V * scene.K, 0.0);
    scene.labels.resize(scene.V);
    for (std::size_t v = 0; v < scene.V; ++v) {
        for (int r = 0; r < 3; ++r) scene.rest[v * 3 + r] = points[v][r];
        scene.labels[v] = region[v] < 0 ? 1 : 0;
        if (region[v] > 0) scene.truth_weights[v * 2] = 1.0;
        else if (region[v] < 0) scene.truth_weights[v * 2 + 1] = 1.0;
        else {
            scene.truth_weights[v * 2] = 0.5;
            scene.truth_weights[v * 2 + 1] = 0.5;
        }
    }
    scene.observed.resize(scene.S * scene.V * 3);
    for (std::size_t s = 0; s < scene.S; ++s) {
        const double theta = 0.3 * static_cast<double>(s) / (scene.S - 1);
        double Rp[9], Rm[9];
        rot_z(theta, Rp);
        rot_z(-theta, Rm);
        for (std::size_t v = 0; v < scene.V; ++v) {
            for (int r = 0; r < 3; ++r) {
                double yp = 0.0, ym = 0.0;
                for (int j = 0; j < 3; ++j) {
                    yp += Rp[r * 3 + j] * scene.rest[v * 3 + j];
                    ym += Rm[r * 3 + j] * scene.rest[v * 3 + j];
                }
                scene.observed[(s * scene.V + v) * 3 + r] =
                    scene.truth_weights[v * 2] * yp +
                    scene.truth_weights[v * 2 + 1] * ym;
            }
        }
    }
    return scene;
}

SkinProblem problem_from_scene(const BlendScene& scene, std::size_t iterations,
                               double max_error) {
    SkinProblem problem;
    problem.frame_count = scene.S;
    problem.vertex_count = scene.V;
    problem.bone_count = scene.K;
    problem.max_influences = 2;
    problem.iterations = iterations;
    problem.max_error = max_error;
    problem.rest_positions = scene.rest;
    problem.observed_positions = scene.observed;
    problem.labels = scene.labels;
    return problem;
}

// ---------------------------------------------------------------------------
// In-memory glTF construction.
// ---------------------------------------------------------------------------

std::string base64(const std::vector<unsigned char>& bytes) {
    static const char* table =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
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

template <class T>
void append_bytes(std::vector<unsigned char>& bin, const T& value) {
    const auto* p = reinterpret_cast<const unsigned char*>(&value);
    bin.insert(bin.end(), p, p + sizeof(T));
}

struct GltfBuilder {
    std::vector<unsigned char> bin;
    struct View {
        std::size_t offset;
        std::size_t size;
        std::size_t stride;
    };
    std::vector<View> views;
    std::vector<std::string> accessors;

    std::size_t push_view(const std::vector<unsigned char>& bytes,
                          std::size_t stride = 0) {
        while (bin.size() % 4 != 0) bin.push_back(0);
        const std::size_t offset = bin.size();
        bin.insert(bin.end(), bytes.begin(), bytes.end());
        views.push_back(View{offset, bytes.size(), stride});
        return views.size() - 1;
    }

    std::size_t add_float_view(const std::vector<float>& values,
                               std::size_t stride = 0) {
        std::vector<unsigned char> bytes(values.size() * sizeof(float));
        std::memcpy(bytes.data(), values.data(), bytes.size());
        return push_view(bytes, stride ? stride : values.size() * sizeof(float));
    }

    std::size_t add_index_view(const std::vector<uint32_t>& indices,
                               int component_type) {
        std::vector<unsigned char> bytes;
        if (component_type == 5121) {
            bytes.resize(indices.size());
            for (std::size_t i = 0; i < indices.size(); ++i)
                bytes[i] = static_cast<unsigned char>(indices[i]);
        } else if (component_type == 5123) {
            bytes.resize(indices.size() * 2);
            for (std::size_t i = 0; i < indices.size(); ++i) {
                uint16_t v = static_cast<uint16_t>(indices[i]);
                std::memcpy(bytes.data() + i * 2, &v, 2);
            }
        } else {
            bytes.resize(indices.size() * 4);
            for (std::size_t i = 0; i < indices.size(); ++i) {
                uint32_t v = indices[i];
                std::memcpy(bytes.data() + i * 4, &v, 4);
            }
        }
        return push_view(bytes, component_type == 5121 ? 1 :
                                        component_type == 5123 ? 2 : 4);
    }

    std::size_t add_accessor(std::size_t view, int component_type,
                             const char* type, std::size_t count) {
        std::ostringstream o;
        o << "{\"bufferView\":" << view << ",\"componentType\":" << component_type
          << ",\"count\":" << count << ",\"type\":\"" << type << "\"}";
        accessors.push_back(o.str());
        return accessors.size() - 1;
    }

    // Sparse FLOAT accessor with an implicit zero base.
    std::size_t add_sparse_float_accessor(
        std::size_t index_view, int index_component_type,
        std::size_t value_view, const char* type, std::size_t count,
        std::size_t sparse_count) {
        std::ostringstream o;
        o << "{\"componentType\":5126,\"count\":" << count << ",\"type\":\""
          << type << "\",\"sparse\":{\"count\":" << sparse_count
          << ",\"indices\":{\"bufferView\":" << index_view
          << ",\"componentType\":" << index_component_type
          << "},\"values\":{\"bufferView\":" << value_view << "}}}";
        accessors.push_back(o.str());
        return accessors.size() - 1;
    }

    std::string finalize(const std::string& meshes,
                         const std::string& nodes,
                         const std::string& scenes,
                         const std::string& animations,
                         const std::string& materials = "",
                         const std::string& textures = "",
                         const std::string& images = "",
                         const std::string& extensions_used = "") const {
        std::ostringstream o;
        o << "{\"asset\":{\"version\":\"2.0\"},\"buffers\":[{\"byteLength\":"
          << bin.size()
          << ",\"uri\":\"data:application/octet-stream;base64," << base64(bin)
          << "\"}],\"bufferViews\":[";
        for (std::size_t i = 0; i < views.size(); ++i) {
            if (i) o << ",";
            o << "{\"buffer\":0,\"byteOffset\":" << views[i].offset
              << ",\"byteLength\":" << views[i].size;
            if (views[i].stride) o << ",\"byteStride\":" << views[i].stride;
            o << "}";
        }
        o << "],\"accessors\":[";
        for (std::size_t i = 0; i < accessors.size(); ++i) {
            if (i) o << ",";
            o << accessors[i];
        }
        o << "]";
        if (!materials.empty()) o << ",\"materials\":" << materials;
        if (!textures.empty()) o << ",\"textures\":" << textures;
        if (!images.empty()) o << ",\"images\":" << images;
        o << ",\"meshes\":" << meshes << ",\"nodes\":" << nodes
          << ",\"scenes\":" << scenes << ",\"scene\":0";
        if (!animations.empty()) o << ",\"animations\":" << animations;
        if (!extensions_used.empty())
            o << ",\"extensionsUsed\":" << extensions_used;
        o << "}";
        return o.str();
    }
};

cgltf_data* parse_and_load(const std::string& json) {
    cgltf_options options{};
    cgltf_data* data = nullptr;
    cgltf_result pr = cgltf_parse(&options, json.data(), json.size(), &data);
    if (pr != cgltf_result_success) {
        std::fprintf(stderr, "parse failed %d json=%s\n", (int)pr, json.c_str());
        return nullptr;
    }
    cgltf_result lr = cgltf_load_buffers(&options, data, nullptr);
    if (lr != cgltf_result_success) {
        std::fprintf(stderr, "load failed %d\n", (int)lr);
        cgltf_free(data);
        return nullptr;
    }
    return data;
}

std::vector<float> read_accessor_floats(const cgltf_accessor* accessor) {
    const cgltf_size count = cgltf_accessor_unpack_floats(accessor, nullptr, 0);
    std::vector<float> values(count);
    cgltf_accessor_unpack_floats(accessor, values.data(), count);
    return values;
}

// Column-major 4x4 transform of a point.
void transform_point(const double m[16], const double p[3], double out[3]) {
    for (int r = 0; r < 3; ++r)
        out[r] = m[r] * p[0] + m[4 + r] * p[1] + m[8 + r] * p[2] + m[12 + r];
}

void quat_to_matrix(const double q[4], double R[9]) {
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

// Independent glTF skinning evaluation of a reloaded file at keyframe s.
std::vector<double> skin_at_frame(cgltf_data* data, std::size_t s) {
    cgltf_primitive& prim = data->meshes[0].primitives[0];
    const cgltf_accessor* pos_acc = nullptr;
    const cgltf_accessor* joints_acc = nullptr;
    const cgltf_accessor* weights_acc = nullptr;
    for (cgltf_size i = 0; i < prim.attributes_count; ++i) {
        if (prim.attributes[i].type == cgltf_attribute_type_position)
            pos_acc = prim.attributes[i].data;
        if (prim.attributes[i].type == cgltf_attribute_type_joints)
            joints_acc = prim.attributes[i].data;
        if (prim.attributes[i].type == cgltf_attribute_type_weights)
            weights_acc = prim.attributes[i].data;
    }
    std::vector<float> pos = read_accessor_floats(pos_acc);
    std::vector<float> w4 = read_accessor_floats(weights_acc);
    std::vector<float> j4 = read_accessor_floats(joints_acc);
    cgltf_skin& skin = data->skins[0];
    const cgltf_size K = skin.joints_count;
    std::vector<double> R(K * 9), t(K * 3);
    cgltf_animation& animation = data->animations[0];
    for (cgltf_size c = 0; c < animation.channels_count; ++c) {
        cgltf_animation_channel& channel = animation.channels[c];
        cgltf_size joint = K;
        for (cgltf_size k = 0; k < K; ++k)
            if (skin.joints[k] == channel.target_node) joint = k;
        if (joint == K) continue;
        std::vector<float> values =
            read_accessor_floats(channel.sampler->output);
        if (channel.target_path == cgltf_animation_path_type_translation)
            for (int r = 0; r < 3; ++r)
                t[joint * 3 + r] = values[s * 3 + r];
        else if (channel.target_path == cgltf_animation_path_type_rotation) {
            double q[4] = {values[s * 4], values[s * 4 + 1],
                           values[s * 4 + 2], values[s * 4 + 3]};
            quat_to_matrix(q, &R[joint * 9]);
        }
    }
    std::vector<double> out(pos_acc->count * 3);
    for (cgltf_size v = 0; v < pos_acc->count; ++v) {
        for (int r = 0; r < 3; ++r) out[v * 3 + r] = 0.0;
        for (int slot = 0; slot < 4; ++slot) {
            const float w = w4[v * 4 + slot];
            if (w == 0.0f) continue;
            const int k = static_cast<int>(j4[v * 4 + slot]);
            for (int r = 0; r < 3; ++r) {
                const double y = R[k * 9 + r * 3] * pos[v * 3] +
                                 R[k * 9 + r * 3 + 1] * pos[v * 3 + 1] +
                                 R[k * 9 + r * 3 + 2] * pos[v * 3 + 2] +
                                 t[k * 3 + r];
                out[v * 3 + r] += w * y;
            }
        }
    }
    return out;
}

// ===========================================================================
// optimization group
// ===========================================================================

int case_one_round_matches_rigid_enumeration() {
    // Strictly planar scene: every proper-rotation optimum is a z rotation,
    // so angle enumeration is a complete independent rigid reference.
    const std::size_t S = 4, V = 6, K = 2;
    const double rest6[6][3] = {
        {1.0, 0.0, 0.0}, {0.7, 0.5, 0.0}, {0.2, 0.9, 0.0},
        {-0.4, 0.8, 0.0}, {-0.9, 0.3, 0.0}, {-0.6, -0.6, 0.0}};
    // Vertices 0..2 follow bone 0, 3..4 bone 1, vertex 5 is a true 50/50
    // boundary blend.
    const int region[6] = {+1, +1, +1, -1, -1, 0};
    std::vector<double> rest(V * 3), observed(S * V * 3);
    std::vector<int> labels(V);
    for (std::size_t v = 0; v < V; ++v) {
        for (int r = 0; r < 3; ++r) rest[v * 3 + r] = rest6[v][r];
        labels[v] = region[v] < 0 ? 1 : 0;
    }
    for (std::size_t s = 0; s < S; ++s) {
        const double a0 = 0.25 * static_cast<double>(s);
        const double a1 = -0.20 * static_cast<double>(s);
        double R0[9], R1[9];
        rot_z(a0, R0);
        rot_z(a1, R1);
        for (std::size_t v = 0; v < V; ++v) {
            double w0 = 1.0, w1 = 0.0;
            if (region[v] < 0) { w0 = 0.0; w1 = 1.0; }
            else if (region[v] == 0) { w0 = w1 = 0.5; }
            for (int r = 0; r < 3; ++r) {
                double y0 = R0[r * 3] * rest[v * 3] +
                            R0[r * 3 + 1] * rest[v * 3 + 1];
                double y1 = R1[r * 3] * rest[v * 3] +
                            R1[r * 3 + 1] * rest[v * 3 + 1];
                observed[(s * V + v) * 3 + r] = w0 * y0 + w1 * y1;
            }
        }
    }
    SkinProblem problem;
    problem.frame_count = S;
    problem.vertex_count = V;
    problem.bone_count = K;
    problem.max_influences = 2;
    problem.iterations = 1;
    problem.max_error = 1.0;
    problem.rest_positions = rest;
    problem.observed_positions = observed;
    problem.labels = labels;
    SkinSolution solution = solve(problem);
    TASK_CHECK(solution.ok);
    TASK_CHECK(solution.reports.size() == 2);
    TASK_CHECK(solution.reports[1].energy <= solution.reports[0].energy);

    // Independent initialization reference: one-hot weights decouple the
    // objective into one rigid fit per (frame, bone).
    std::vector<double> unit_weight(V, 1.0);
    double reference_initial = 0.0;
    for (std::size_t k = 0; k < K; ++k) {
        std::vector<std::size_t> members;
        for (std::size_t v = 0; v < V; ++v)
            if (labels[v] == static_cast<int>(k)) members.push_back(v);
        for (std::size_t s = 0; s < S; ++s) {
            std::vector<double> frame_targets(V * 3);
            for (std::size_t v = 0; v < V; ++v)
                for (int r = 0; r < 3; ++r)
                    frame_targets[v * 3 + r] =
                        observed[(s * V + v) * 3 + r];
            EnumeratedRigid reference =
                enumerate_rigid_z(rest, frame_targets, unit_weight, members);
            reference_initial += reference.objective;
        }
    }
    TASK_CHECK(near(solution.reports[0].energy, reference_initial,
                    1e-9 * std::max(1.0, reference_initial)));

    // One weight block must have moved at least one labelled vertex off its
    // one-hot initialization.
    bool became_mixed = false;
    for (std::size_t v = 0; v < V; ++v)
        if (positive_influence_count(solution, v) >= 2) became_mixed = true;
    TASK_CHECK(became_mixed);
    return 0;
}

int case_reflection_is_refused() {
    BlendScene scene = make_blend_scene();
    SkinProblem problem = problem_from_scene(scene, 0, 1.0);
    // Every target is the reflected rest pose (det -1 spatial map).
    for (std::size_t s = 0; s < scene.S; ++s)
        for (std::size_t v = 0; v < scene.V; ++v)
            for (int r = 0; r < 3; ++r)
                problem.observed_positions[(s * scene.V + v) * 3 + r] =
                    -scene.rest[v * 3 + r];
    SkinSolution solution = solve(problem);
    TASK_CHECK(solution.ok);
    for (const RigidTransform& tf : solution.transforms) {
        double RtR[9] = {0};
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                for (int r = 0; r < 3; ++r)
                    RtR[i * 3 + j] += tf.rotation[r * 3 + i] *
                                      tf.rotation[r * 3 + j];
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                TASK_CHECK(near(RtR[i * 3 + j], i == j ? 1.0 : 0.0, 1e-10));
        TASK_CHECK(det3(tf.rotation) > 0.0);
    }
    // Independent reference: best proper planar rotation is a 180-degree
    // z rotation, which reproduces -I on the planar coordinates exactly.
    std::vector<std::size_t> members;
    for (std::size_t v = 0; v < scene.V; ++v)
        if (scene.labels[v] == 0) members.push_back(v);
    std::vector<double> c(scene.V, 1.0);
    EnumeratedRigid reference =
        enumerate_rigid_z(scene.rest, problem.observed_positions, c, members);
    const double got = rigid_objective(scene.rest, problem.observed_positions,
                                       c, members, solution.transforms[0]);
    TASK_CHECK(got <= reference.objective + 1e-9);
    TASK_CHECK(near(reference.angle, kPi, 2e-5) ||
                near(reference.angle, -kPi, 2e-5));
    return 0;
}

int case_rank_deficient_and_duplicate_trajectories() {
    // Bone 0 sees identical rest points (rank 0); bone 1 sees collinear
    // points (rank 1). Both must still yield deterministic proper rotations.
    const std::size_t S = 3, V = 6, K = 2;
    SkinProblem problem;
    problem.frame_count = S;
    problem.vertex_count = V;
    problem.bone_count = K;
    problem.max_influences = 2;
    problem.iterations = 4;
    problem.max_error = 1.0;
    problem.rest_positions = {
        1, 1, 0, 1, 1, 0, 1, 1, 0,
        2, 0, 0, 3, 0, 0, 4, 0, 0};
    problem.labels = {0, 0, 0, 1, 1, 1};
    problem.observed_positions.resize(S * V * 3);
    for (std::size_t s = 0; s < S; ++s) {
        const double shift = 0.1 * static_cast<double>(s);
        for (std::size_t v = 0; v < V; ++v)
            for (int r = 0; r < 3; ++r)
                problem.observed_positions[(s * V + v) * 3 + r] =
                    problem.rest_positions[v * 3 + r] +
                    (r == 0 ? shift : 0.0);
    }
    SkinSolution a = solve(problem);
    SkinSolution b = solve(problem);
    TASK_CHECK(a.ok && b.ok);
    TASK_CHECK(a.weights == b.weights);
    TASK_CHECK(a.transforms.size() == b.transforms.size());
    for (std::size_t i = 0; i < a.transforms.size(); ++i) {
        for (int j = 0; j < 9; ++j)
            TASK_CHECK(a.transforms[i].rotation[j] ==
                       b.transforms[i].rotation[j]);
        TASK_CHECK(det3(a.transforms[i].rotation) > 0.0);
    }
    return 0;
}

int case_mixed_influence_improves_and_recomputes() {
    BlendScene scene = make_blend_scene();
    SkinSolution zero = solve(problem_from_scene(scene, 0, 1.0));
    SkinSolution grown = solve(problem_from_scene(scene, 60, 1e-6));
    TASK_CHECK(zero.ok && grown.ok);
    TASK_CHECK(grown.final_energy < zero.final_energy);

    // Recomputed energy must match the reported value.
    SkinProblem problem = problem_from_scene(scene, 60, 1e-6);
    const double recomputed = evaluate_energy(problem, grown);
    TASK_CHECK(near(recomputed, grown.final_energy,
                    1e-12 * std::max(1.0, grown.final_energy)));

    // Not all one-hot: the boundary vertices must keep two influences.
    std::size_t mixed = 0;
    for (std::size_t v = 0; v < scene.V; ++v)
        if (positive_influence_count(grown, v) >= 2) ++mixed;
    TASK_CHECK(mixed >= 2);

    // Not translation-only: some rotation must differ noticeably from identity.
    bool rotated = false;
    for (const RigidTransform& tf : grown.transforms) {
        double deviation = 0.0;
        for (int i = 0; i < 9; ++i) {
            const double identity = (i % 4 == 0) ? 1.0 : 0.0;
            deviation = std::max(deviation,
                                 std::fabs(tf.rotation[i] - identity));
        }
        if (deviation > 0.05) rotated = true;
    }
    TASK_CHECK(rotated);

    // Recovered weights track the designed blend on the boundary vertices.
    for (std::size_t v = 0; v < scene.V; ++v) {
        if (scene.truth_weights[v * 2] == 0.5) {
            TASK_CHECK(grown.weights[v * 2] > 0.25);
            TASK_CHECK(grown.weights[v * 2 + 1] > 0.25);
        }
    }
    return 0;
}

int case_monotone_reports_and_constraints() {
    BlendScene scene = make_blend_scene();
    SkinSolution solution = solve(problem_from_scene(scene, 20, 1.0));
    TASK_CHECK(solution.ok);
    TASK_CHECK(solution.reports.size() == 21);
    for (std::size_t i = 1; i < solution.reports.size(); ++i) {
        const double previous = solution.reports[i - 1].energy;
        TASK_CHECK(solution.reports[i].energy <=
                   previous + 1e-8 * std::max(1.0, previous));
        TASK_CHECK(solution.reports[i].non_finite == 0);
    }
    for (std::size_t v = 0; v < scene.V; ++v) {
        double sum = 0.0;
        for (std::size_t k = 0; k < scene.K; ++k) {
            TASK_CHECK(solution.weights[v * scene.K + k] >= 0.0);
            sum += solution.weights[v * scene.K + k];
        }
        TASK_CHECK(near(sum, 1.0, 1e-9));
        TASK_CHECK(positive_influence_count(solution, v) <= 2);
    }
    return 0;
}

// ===========================================================================
// asset group
// ===========================================================================

// Builds a two-primitive morph asset exercising stride, offset, sparse
// morph deltas, U8/U16 indices and a non-indexed primitive.
struct MorphAsset {
    std::string json;
    std::size_t V0 = 5;
    std::size_t V1 = 3;
    std::size_t total = 8;
    std::vector<double> rest;
    std::vector<double> observed;
    double world[16];
};

MorphAsset make_sparse_morph_asset() {
    MorphAsset asset;
    GltfBuilder b;
    const std::size_t V0 = asset.V0, V1 = asset.V1, M = 2, S = 3;
    asset.rest = {
        1, 0, 0, 0.8f, 0.4f, 0, 0.5f, 0.7f, 0, 0.2f, 0.9f, 0, -0.3f, 0.6f, 0,
        0, -0.5f, 0.2f, -0.4f, -0.7f, 0.1f, -0.8f, -0.2f, -0.3f};
    // Primitive 0 positions use stride 16 (vec3 + one padding float).
    std::vector<unsigned char> pos0_bytes;
    for (std::size_t v = 0; v < V0; ++v) {
        for (int r = 0; r < 3; ++r)
            append_bytes<float>(pos0_bytes,
                                static_cast<float>(asset.rest[v * 3 + r]));
        append_bytes<float>(pos0_bytes, 0.0f);
    }
    const std::size_t pos0_view = b.push_view(pos0_bytes, 16);
    const std::size_t pos0_acc = b.add_accessor(pos0_view, 5126, "VEC3", V0);

    // Primitive 0 morph target 0: dense deltas, stride 12.
    std::vector<float> delta0(V0 * 3, 0.0f);
    for (std::size_t v = 0; v < V0; ++v) {
        delta0[v * 3 + 0] = 0.10f * static_cast<float>(v);
        delta0[v * 3 + 1] = -0.05f;
    }
    const std::size_t d0_view = b.add_float_view(delta0, 12);
    const std::size_t d0_acc = b.add_accessor(d0_view, 5126, "VEC3", V0);

    // Primitive 0 morph target 1: implicit-zero sparse accessor, touching only
    // vertices 1 and 3.
    std::vector<uint16_t> sparse_indices = {1, 3};
    std::vector<unsigned char> idx_bytes(sparse_indices.size() * 2);
    for (std::size_t i = 0; i < sparse_indices.size(); ++i)
        std::memcpy(idx_bytes.data() + i * 2, &sparse_indices[i], 2);
    const std::size_t si_view = b.push_view(idx_bytes, 2);
    std::vector<float> sparse_values = {0.0f, 0.0f, 0.20f,
                                        0.0f, 0.0f, -0.20f};
    const std::size_t sv_view = b.add_float_view(sparse_values, 12);
    const std::size_t d1_acc = b.add_sparse_float_accessor(
        si_view, 5123, sv_view, "VEC3", V0, sparse_indices.size());

    // U8 indices for primitive 0 (two triangles + padding pattern).
    const std::size_t u8_view =
        b.add_index_view({0, 1, 2, 2, 3, 4}, 5121);
    const std::size_t u8_acc = b.add_accessor(u8_view, 5121, "SCALAR", 6);

    // Primitive 1: non-indexed, dense positions.
    std::vector<float> pos1(V1 * 3);
    for (std::size_t v = 0; v < V1; ++v)
        for (int r = 0; r < 3; ++r)
            pos1[v * 3 + r] =
                static_cast<float>(asset.rest[(V0 + v) * 3 + r]);
    const std::size_t pos1_view = b.add_float_view(pos1, 12);
    const std::size_t pos1_acc = b.add_accessor(pos1_view, 5126, "VEC3", V1);
    std::vector<float> delta1(V1 * 3, 0.0f);
    for (std::size_t v = 0; v < V1; ++v) delta1[v * 3 + 2] = 0.08f;
    const std::size_t d1b_view = b.add_float_view(delta1, 12);
    const std::size_t d1b_acc = b.add_accessor(d1b_view, 5126, "VEC3", V1);
    std::vector<float> delta2(V1 * 3, 0.0f);
    for (std::size_t v = 0; v < V1; ++v) delta2[v * 3 + 0] = -0.07f;
    const std::size_t d2b_view = b.add_float_view(delta2, 12);
    const std::size_t d2b_acc = b.add_accessor(d2b_view, 5126, "VEC3", V1);

    // Animation: times and flattened weights (LINEAR).
    std::vector<float> times = {0.0f, 1.0f, 2.0f};
    const std::size_t time_view = b.add_float_view(times, 4);
    const std::size_t time_acc = b.add_accessor(time_view, 5126, "SCALAR", S);
    std::vector<float> weights = {
        0.0f, 0.0f, 0.5f, 0.5f, 1.0f, 0.0f};
    const std::size_t weight_view = b.add_float_view(weights, 4);
    const std::size_t weight_acc =
        b.add_accessor(weight_view, 5126, "SCALAR", S * M);

    std::ostringstream meshes;
    meshes << "[{\"primitives\":["
           << "{\"attributes\":{\"POSITION\":" << pos0_acc
           << "},\"indices\":" << u8_acc << ",\"targets\":[{\"POSITION\":"
           << d0_acc << "},{\"POSITION\":" << d1_acc << "}]},"
           << "{\"attributes\":{\"POSITION\":" << pos1_acc
           << "},\"targets\":[{\"POSITION\":" << d1b_acc
           << "},{\"POSITION\":" << d2b_acc << "}]}]}]";
    // Non-identity root: translation plus a z rotation.
    const double qz = std::sin(0.15 / 2.0), qw = std::cos(0.15 / 2.0);
    std::ostringstream nodes;
    nodes << "[{\"translation\":[2, -1, 0.5],\"rotation\":[0,0," << qz
          << "," << qw << "],\"children\":[1]},{\"mesh\":0}]";
    const std::string scenes = "[{\"nodes\":[0]}]";
    std::ostringstream anim;
    anim << "[{\"samplers\":[{\"input\":" << time_acc << ",\"output\":"
         << weight_acc
         << ",\"interpolation\":\"LINEAR\"}],\"channels\":[{\"sampler\":0,"
            "\"target\":{\"node\":1,\"path\":\"weights\"}}]}]";
    asset.json = b.finalize(meshes.str(), nodes.str(), scenes, anim.str());

    // Independent expected samples at the four requested times. The sampler
    // clamps outside the keyframe span and interpolates LINEAR inside it.
    const std::vector<double> sample_times = {0.0, 0.5, 1.0, 2.0};
    const std::vector<float> key_weights = {
        0.0f, 0.0f, 0.5f, 0.5f, 1.0f, 0.0f};
    asset.observed.resize(sample_times.size() * asset.total * 3);
    auto delta_at = [&](std::size_t global_v, std::size_t target,
                        float out[3]) {
        out[0] = out[1] = out[2] = 0.0f;
        if (global_v < V0) {
            if (target == 0) {
                out[0] = 0.10f * static_cast<float>(global_v);
                out[1] = -0.05f;
            } else if (global_v == 1) {
                out[2] = 0.20f;
            } else if (global_v == 3) {
                out[2] = -0.20f;
            }
        } else {
            if (target == 0) out[2] = 0.08f;
            else out[0] = -0.07f;
        }
    };
    for (std::size_t s = 0; s < sample_times.size(); ++s) {
        double w[2];
        const double t = sample_times[s];
        if (t <= times.front()) {
            w[0] = key_weights[0];
            w[1] = key_weights[1];
        } else if (t >= times.back()) {
            w[0] = key_weights[(S - 1) * 2];
            w[1] = key_weights[(S - 1) * 2 + 1];
        } else {
            const std::size_t ki = static_cast<std::size_t>(t); // keys at integers
            const double alpha = t - static_cast<double>(ki);
            for (int m = 0; m < 2; ++m)
                w[m] = key_weights[ki * 2 + m] +
                       alpha * (key_weights[(ki + 1) * 2 + m] -
                                key_weights[ki * 2 + m]);
        }
        for (std::size_t v = 0; v < asset.total; ++v) {
            float d0[3], d1[3];
            delta_at(v, 0, d0);
            delta_at(v, 1, d1);
            for (int r = 0; r < 3; ++r)
                asset.observed[(s * asset.total + v) * 3 + r] =
                    asset.rest[v * 3 + r] + w[0] * d0[r] + w[1] * d1[r];
        }
    }
    // Expected static world matrix (column major).
    const double c = std::cos(0.15), sn = std::sin(0.15);
    std::fill(std::begin(asset.world), std::end(asset.world), 0.0);
    asset.world[0] = c;  asset.world[1] = sn;
    asset.world[4] = -sn; asset.world[5] = c;
    asset.world[10] = 1.0;
    asset.world[12] = 2.0;
    asset.world[13] = -1.0;
    asset.world[14] = 0.5;
    asset.world[15] = 1.0;
    return asset;
}

int case_multi_primitive_sparse_export_reload() {
    MorphAsset asset = make_sparse_morph_asset();
    cgltf_data* data = parse_and_load(asset.json);
    TASK_CHECK(data != nullptr);
    std::vector<int> labels(asset.total);
    for (std::size_t v = 0; v < asset.total; ++v) labels[v] = v % 2;
    DecomposeOptions options;
    options.data = data;
    options.target_node = &data->nodes[1];
    options.animation_index = 0;
    options.sample_times = {0.0, 0.5, 1.0, 2.0};
    options.bone_count = 2;
    options.max_influences = 2;
    options.labels = labels;
    options.iterations = 10;
    options.max_error = 1.0;
    DecomposeReport report = decompose_morph_skin(options);
    TASK_CHECK(report.ok);
    TASK_CHECK(report.vertices == asset.total);
    TASK_CHECK(report.primitives.size() == 2);
    TASK_CHECK(report.primitives[0].indexed);
    TASK_CHECK(!report.primitives[1].indexed);
    // Sampled positions match the independently computed expected values.
    for (std::size_t i = 0; i < asset.observed.size(); ++i)
        TASK_CHECK(near(report.observed_positions[i], asset.observed[i], 1e-6));
    for (std::size_t i = 0; i < 16; ++i)
        TASK_CHECK(near(report.world_matrix[i], asset.world[i], 1e-6));

    const std::string path = "decompose_target_sparse.gltf";
    std::string error;
    TASK_CHECK(export_decomposed_gltf(report, path.c_str(), &error) ==
               cgltf_result_success);

    // Reload after releasing the source asset.
    cgltf_free(data);
    data = nullptr;
    cgltf_options cg_options{};
    cgltf_data* reloaded = nullptr;
    TASK_CHECK(cgltf_parse_file(&cg_options, path.c_str(), &reloaded) ==
               cgltf_result_success);
    TASK_CHECK(cgltf_load_buffers(&cg_options, reloaded, path.c_str()) ==
               cgltf_result_success);
    TASK_CHECK(cgltf_validate(reloaded) == cgltf_result_success);
    TASK_CHECK(reloaded->meshes[0].primitives[0].targets_count == 0);
    TASK_CHECK(reloaded->meshes[0].primitives_count == 2);

    // U8 indices survived the round trip.
    std::vector<uint32_t> reloaded_indices;
    const cgltf_accessor* indices =
        reloaded->meshes[0].primitives[0].indices;
    TASK_CHECK(indices != nullptr);
    TASK_CHECK(indices->component_type == cgltf_component_type_r_8u);
    for (cgltf_size i = 0; i < indices->count; ++i)
        reloaded_indices.push_back(
            static_cast<uint32_t>(cgltf_accessor_read_index(indices, i)));
    const std::vector<uint32_t> expected_indices = {0, 1, 2, 2, 3, 4};
    TASK_CHECK(reloaded_indices == expected_indices);

    // Real skinning in local space reproduces the sampled observations within
    // the report's own measured worst-case error (which the export verified).
    TASK_CHECK(std::isfinite(report.max_local_error));
    for (std::size_t s = 0; s < report.frames; ++s) {
        std::vector<double> local = skin_at_frame(reloaded, s);
        // skin_at_frame only covers primitive 0; compare its vertices.
        for (std::size_t v = 0; v < asset.V0; ++v)
            for (int r = 0; r < 3; ++r) {
                const double expected =
                    report.observed_positions[(s * asset.total + v) * 3 + r];
                TASK_CHECK(near(local[v * 3 + r], expected,
                                report.max_local_error + 1e-9));
            }
    }
    // The common root carries the original static world matrix.
    const cgltf_node& root = reloaded->nodes[0];
    TASK_CHECK(root.has_matrix);
    for (int i = 0; i < 16; ++i)
        TASK_CHECK(near(root.matrix[i], asset.world[i], 1e-6));
    cgltf_free(reloaded);
    std::remove(path.c_str());
    return 0;
}

int case_non_identity_root_world_space() {
    MorphAsset asset = make_sparse_morph_asset();
    cgltf_data* data = parse_and_load(asset.json);
    TASK_CHECK(data != nullptr);
    std::vector<int> labels(asset.total);
    for (std::size_t v = 0; v < asset.total; ++v) labels[v] = v % 2;
    DecomposeOptions options;
    options.data = data;
    options.target_node = &data->nodes[1];
    options.animation_index = 0;
    options.sample_times = {0.0, 1.0, 2.0};
    options.bone_count = 2;
    options.max_influences = 2;
    options.labels = labels;
    options.iterations = 5;
    options.max_error = 1.0;
    DecomposeReport report = decompose_morph_skin(options);
    TASK_CHECK(report.ok);
    const std::string path = "decompose_target_root.gltf";
    std::string error;
    TASK_CHECK(export_decomposed_gltf(report, path.c_str(), &error) ==
               cgltf_result_success);
    cgltf_free(data);

    cgltf_options options2{};
    cgltf_data* reloaded = nullptr;
    TASK_CHECK(cgltf_parse_file(&options2, path.c_str(), &reloaded) ==
               cgltf_result_success);
    TASK_CHECK(cgltf_load_buffers(&options2, reloaded, path.c_str()) ==
               cgltf_result_success);
    double root[16];
    for (int i = 0; i < 16; ++i) root[i] = reloaded->nodes[0].matrix[i];
    for (std::size_t s = 0; s < report.frames; ++s) {
        std::vector<double> local = skin_at_frame(reloaded, s);
        for (std::size_t v = 0; v < asset.V0; ++v) {
            double world_skinned[3];
            transform_point(root, &local[v * 3], world_skinned);
            double world_observed[3];
            transform_point(
                root,
                &report.observed_positions[(s * asset.total + v) * 3],
                world_observed);
            for (int r = 0; r < 3; ++r)
                TASK_CHECK(near(world_skinned[r], world_observed[r],
                                report.max_local_error + 1e-9));
        }
    }
    cgltf_free(reloaded);
    std::remove(path.c_str());
    return 0;
}

int case_retry_after_rejection_keeps_old_file() {
    // An impossibly tight export tolerance must fail without replacing an
    // existing destination file; a retry with a workable tolerance must then
    // replace it and produce a reloadable asset.
    MorphAsset asset = make_sparse_morph_asset();
    cgltf_data* data = parse_and_load(asset.json);
    TASK_CHECK(data != nullptr);
    std::vector<int> labels(asset.total);
    for (std::size_t v = 0; v < asset.total; ++v) labels[v] = v % 2;
    auto make_options = [&](double tolerance) {
        DecomposeOptions options;
        options.data = data;
        options.target_node = &data->nodes[1];
        options.animation_index = 0;
        options.sample_times = {0.0, 1.0, 2.0};
        options.bone_count = 2;
        options.max_influences = 2;
        options.labels = labels;
        options.iterations = 4;
        options.max_error = tolerance;
        return options;
    };
    const std::string path = "decompose_target_retry.gltf";
    {
        std::ofstream seed(path, std::ios::binary);
        seed << "SEED_CONTENTS_KEEP_ME";
    }
    DecomposeReport rejected = decompose_morph_skin(make_options(1e-12));
    TASK_CHECK(rejected.ok);
    std::string error;
    const cgltf_result failure =
        export_decomposed_gltf(rejected, path.c_str(), &error);
    TASK_CHECK(failure != cgltf_result_success);
    TASK_CHECK(!error.empty());
    std::ifstream after(path, std::ios::binary);
    std::stringstream kept;
    kept << after.rdbuf();
    TASK_CHECK(kept.str() == "SEED_CONTENTS_KEEP_ME");

    // Retry with an achievable tolerance: the old file is replaced and the
    // reloaded result validates.
    DecomposeReport accepted = decompose_morph_skin(make_options(1.0));
    TASK_CHECK(accepted.ok);
    TASK_CHECK(export_decomposed_gltf(accepted, path.c_str(), &error) ==
               cgltf_result_success);
    cgltf_free(data);
    cgltf_options cg_options{};
    cgltf_data* reloaded = nullptr;
    TASK_CHECK(cgltf_parse_file(&cg_options, path.c_str(), &reloaded) ==
               cgltf_result_success);
    TASK_CHECK(cgltf_load_buffers(&cg_options, reloaded, path.c_str()) ==
               cgltf_result_success);
    TASK_CHECK(cgltf_validate(reloaded) == cgltf_result_success);
    cgltf_free(reloaded);
    std::remove(path.c_str());
    return 0;
}

int case_step_clamp_and_default_weights() {
    GltfBuilder b;
    const std::size_t V = 2, M = 2, S_keys = 2;
    std::vector<float> base = {1, 0, 0, 0, 1, 0};
    const std::size_t base_view = b.add_float_view(base, 12);
    const std::size_t base_acc = b.add_accessor(base_view, 5126, "VEC3", V);
    std::vector<float> d0 = {0.5f, 0, 0, 0, 0.5f, 0};
    const std::size_t d0_view = b.add_float_view(d0, 12);
    const std::size_t d0_acc = b.add_accessor(d0_view, 5126, "VEC3", V);
    std::vector<float> d1 = {0, 0.25f, 0, 0.25f, 0, 0};
    const std::size_t d1_view = b.add_float_view(d1, 12);
    const std::size_t d1_acc = b.add_accessor(d1_view, 5126, "VEC3", V);
    std::vector<float> times = {1.0f, 3.0f};
    const std::size_t time_view = b.add_float_view(times, 4);
    const std::size_t time_acc =
        b.add_accessor(time_view, 5126, "SCALAR", S_keys);
    // Sparse implicit-base weights on the flattened SCALAR output
    // (count = keyframes * targets). The single sparse entry overrides only
    // the first component of key 0; every other slot keeps the mesh defaults.
    std::vector<uint16_t> sparse_index = {0};
    std::vector<unsigned char> si_bytes(2);
    uint16_t zero16 = 0;
    std::memcpy(si_bytes.data(), &zero16, 2);
    const std::size_t si_view = b.push_view(si_bytes, 2);
    std::vector<float> sparse_value = {0.8f};
    const std::size_t sv_view = b.add_float_view(sparse_value, 4);
    const std::size_t weight_acc = b.add_sparse_float_accessor(
        si_view, 5123, sv_view, "SCALAR", S_keys * M, 1);
    std::ostringstream meshes;
    meshes << "[{\"weights\":[0,0.4],\"primitives\":[{\"attributes\":{"
              "\"POSITION\":" << base_acc << "},\"targets\":[{\"POSITION\":"
           << d0_acc << "},{\"POSITION\":" << d1_acc << "}]}]}]";
    const std::string nodes = "[{\"mesh\":0}]";
    const std::string scenes = "[{\"nodes\":[0]}]";
    std::ostringstream anim;
    anim << "[{\"samplers\":[{\"input\":" << time_acc << ",\"output\":"
         << weight_acc
         << ",\"interpolation\":\"STEP\"}],\"channels\":[{\"sampler\":0,"
            "\"target\":{\"node\":0,\"path\":\"weights\"}}]}]";
    const std::string json = b.finalize(meshes.str(), nodes, scenes, anim.str());
    cgltf_data* data = parse_and_load(json);
    TASK_CHECK(data != nullptr);
    DecomposeOptions options;
    options.data = data;
    options.target_node = &data->nodes[0];
    options.animation_index = 0;
    // t=0 clamps to the first key, t=2 holds the STEP value, t=4 clamps the
    // last (implicit default) key.
    options.sample_times = {0.0, 2.0, 4.0};
    options.bone_count = 1;
    options.max_influences = 1;
    options.labels = {0, 0};
    options.iterations = 0;
    options.max_error = 1.0;
    DecomposeReport report = decompose_morph_skin(options);
    TASK_CHECK(report.ok);
    const double w00 = report.morph_weights[0 * 2];
    const double w01 = report.morph_weights[0 * 2 + 1];
    const double w10 = report.morph_weights[1 * 2];
    const double w11 = report.morph_weights[1 * 2 + 1];
    const double w20 = report.morph_weights[2 * 2];
    const double w21 = report.morph_weights[2 * 2 + 1];
    // Weights originate from float accessors, so compare at float precision.
    TASK_CHECK(near(w00, 0.8, 1e-6) && near(w01, 0.4, 1e-6));
    TASK_CHECK(near(w10, 0.8, 1e-6) && near(w11, 0.4, 1e-6));
    // The second key has no sparse override: both components come from mesh
    // defaults (target 0 default 0, target 1 default 0.4).
    TASK_CHECK(near(w20, 0.0, 1e-6) && near(w21, 0.4, 1e-6));
    // Spot-check one observed vertex at t=0: base + 0.8*d0 + 0.4*d1.
    TASK_CHECK(near(report.observed_positions[0], 1.0 + 0.8 * 0.5, 1e-6));
    TASK_CHECK(near(report.observed_positions[1], 0.4 * 0.25, 1e-6));
    cgltf_free(data);
    return 0;
}

int case_single_bone_export() {
    MorphAsset asset = make_sparse_morph_asset();
    cgltf_data* data = parse_and_load(asset.json);
    TASK_CHECK(data != nullptr);
    DecomposeOptions options;
    options.data = data;
    options.target_node = &data->nodes[1];
    options.animation_index = 0;
    options.sample_times = {0.0, 1.0, 2.0};
    options.bone_count = 1;
    options.max_influences = 1;
    options.labels.assign(asset.total, 0);
    options.iterations = 6;
    options.max_error = 1.0;
    DecomposeReport report = decompose_morph_skin(options);
    TASK_CHECK(report.ok);
    TASK_CHECK(report.bones == 1);
    TASK_CHECK(report.influences == 1);
    // Every vertex is one-hot on the single bone.
    for (std::size_t v = 0; v < asset.total; ++v) {
        TASK_CHECK(positive_influence_count(report.solution, v) == 1);
        TASK_CHECK(near(report.solution.weights[v], 1.0, 1e-12));
    }
    const std::string path = "decompose_target_single.gltf";
    std::string error;
    TASK_CHECK(export_decomposed_gltf(report, path.c_str(), &error) ==
               cgltf_result_success);
    cgltf_free(data);
    cgltf_options cg_options{};
    cgltf_data* reloaded = nullptr;
    TASK_CHECK(cgltf_parse_file(&cg_options, path.c_str(), &reloaded) ==
               cgltf_result_success);
    TASK_CHECK(cgltf_load_buffers(&cg_options, reloaded, path.c_str()) ==
               cgltf_result_success);
    TASK_CHECK(cgltf_validate(reloaded) == cgltf_result_success);
    TASK_CHECK(reloaded->skins[0].joints_count == 1);
    TASK_CHECK(reloaded->animations[0].channels_count == 2);
    cgltf_free(reloaded);
    std::remove(path.c_str());
    return 0;
}

void matrix_from_solution_rotation(const RigidTransform& tf, double R[9]) {
    for (int i = 0; i < 9; ++i) R[i] = tf.rotation[i];
}

int case_worst_location_and_float_tiebreak() {
    // Drive the real pipeline, then independently recompute the float-rounded
    // per-(frame, vertex) error and confirm the reported worst location and
    // value, including the smallest-frame/smallest-vertex tie ordering.
    MorphAsset asset = make_sparse_morph_asset();
    cgltf_data* data = parse_and_load(asset.json);
    TASK_CHECK(data != nullptr);
    std::vector<int> labels(asset.total);
    for (std::size_t v = 0; v < asset.total; ++v) labels[v] = v % 2;
    DecomposeOptions options;
    options.data = data;
    options.target_node = &data->nodes[1];
    options.animation_index = 0;
    options.sample_times = {0.0, 0.5, 1.0, 2.0};
    options.bone_count = 2;
    options.max_influences = 2;
    options.labels = labels;
    options.iterations = 8;
    options.max_error = 1.0;
    DecomposeReport report = decompose_morph_skin(options);
    TASK_CHECK(report.ok);
    cgltf_free(data);

    const SkinSolution& sol = report.solution;
    double worst = -1.0;
    std::size_t wf = 0, wv = 0;
    for (std::size_t s = 0; s < sol.frames; ++s) {
        std::vector<double> Rm(sol.bones * 9);
        std::vector<std::array<float, 3>> tf(sol.bones);
        for (std::size_t k = 0; k < sol.bones; ++k) {
            matrix_from_solution_rotation(sol.transforms[s * sol.bones + k],
                                          &Rm[k * 9]);
            const double* t = sol.transforms[s * sol.bones + k].translation;
            tf[k] = {static_cast<float>(t[0]), static_cast<float>(t[1]),
                     static_cast<float>(t[2])};
        }
        for (std::size_t v = 0; v < sol.vertices; ++v) {
            const double* p = &report.rest_positions[v * 3];
            double skinned[3] = {0, 0, 0};
            for (std::size_t k = 0; k < sol.bones; ++k) {
                const float w = static_cast<float>(sol.weights[v * sol.bones + k]);
                if (w == 0.0f) continue;
                for (int r = 0; r < 3; ++r) {
                    const double y = Rm[k * 9 + r * 3] * p[0] +
                                     Rm[k * 9 + r * 3 + 1] * p[1] +
                                     Rm[k * 9 + r * 3 + 2] * p[2] + tf[k][r];
                    skinned[r] += static_cast<double>(w) * y;
                }
            }
            double norm2 = 0.0;
            for (int r = 0; r < 3; ++r) {
                const double d =
                    report.observed_positions[(s * sol.vertices + v) * 3 + r] -
                    skinned[r];
                norm2 += d * d;
            }
            const double e = std::sqrt(norm2);
            if (e > worst) {
                worst = e;
                wf = s;
                wv = v;
            }
        }
    }
    // The production value rounds rotation through float quaternions too, so
    // compare at float scale; the argmax location must still agree.
    TASK_CHECK(near(report.max_local_error, worst, 1e-5));
    TASK_CHECK(report.worst_frame == wf);
    TASK_CHECK(report.worst_vertex == wv);
    TASK_CHECK(report.worst_frame < sol.frames);
    TASK_CHECK(report.worst_vertex < sol.vertices);
    return 0;
}

int case_node_default_weights_take_precedence() {
    GltfBuilder b;
    const std::size_t V = 2, M = 2;
    std::vector<float> base = {1, 0, 0, 0, 1, 0};
    const std::size_t base_acc =
        b.add_accessor(b.add_float_view(base, 12), 5126, "VEC3", V);
    std::vector<float> d0 = {0.5f, 0, 0, 0, 0.5f, 0};
    const std::size_t d0_acc =
        b.add_accessor(b.add_float_view(d0, 12), 5126, "VEC3", V);
    std::vector<float> d1 = {0, 0.25f, 0, 0.25f, 0, 0};
    const std::size_t d1_acc =
        b.add_accessor(b.add_float_view(d1, 12), 5126, "VEC3", V);
    std::vector<float> times = {0.0f, 2.0f};
    const std::size_t time_acc =
        b.add_accessor(b.add_float_view(times, 4), 5126, "SCALAR", 2);
    // Flattened implicit-base sparse weight output (2 keys * 2 targets). Only
    // key 0 / target 0 is overridden; every other slot falls back to defaults.
    std::vector<uint16_t> sparse_index = {0};
    std::vector<unsigned char> si_bytes(2);
    uint16_t zero16 = 0;
    std::memcpy(si_bytes.data(), &zero16, 2);
    const std::size_t si_view = b.push_view(si_bytes, 2);
    std::vector<float> sparse_value = {0.8f};
    const std::size_t sv_view = b.add_float_view(sparse_value, 4);
    const std::size_t weight_acc = b.add_sparse_float_accessor(
        si_view, 5123, sv_view, "SCALAR", 2 * M, 1);
    // Mesh defaults differ from node defaults; node must win.
    const std::string meshes =
        "[{\"weights\":[0.25,0.4],\"primitives\":[{\"attributes\":{\"POSITION\":" +
        std::to_string(base_acc) + "},\"targets\":[{\"POSITION\":" +
        std::to_string(d0_acc) + "},{\"POSITION\":" +
        std::to_string(d1_acc) + "}]}]}]";
    const std::string nodes = "[{\"mesh\":0,\"weights\":[0.75,0.5]}]";
    const std::string scenes = "[{\"nodes\":[0]}]";
    const std::string anim =
        "[{\"samplers\":[{\"input\":" + std::to_string(time_acc) +
        ",\"output\":" + std::to_string(weight_acc) +
        ",\"interpolation\":\"LINEAR\"}],\"channels\":[{\"sampler\":0,"
        "\"target\":{\"node\":0,\"path\":\"weights\"}}]}]";
    cgltf_data* data = parse_and_load(b.finalize(meshes, nodes, scenes, anim));
    TASK_CHECK(data != nullptr);
    DecomposeOptions options;
    options.data = data;
    options.target_node = &data->nodes[0];
    options.animation_index = 0;
    // t=0 is the overridden key; t=2 clamps onto the un-overridden last key,
    // whose components must come from the node (not mesh) defaults.
    options.sample_times = {0.0, 2.0};
    options.bone_count = 1;
    options.max_influences = 1;
    options.labels = {0, 0};
    options.iterations = 0;
    options.max_error = 1.0;
    DecomposeReport report = decompose_morph_skin(options);
    TASK_CHECK(report.ok);
    // Key 0: target 0 overridden to 0.8, target 1 keeps node default 0.5.
    TASK_CHECK(near(report.morph_weights[0], 0.8, 1e-6));
    TASK_CHECK(near(report.morph_weights[1], 0.5, 1e-6));
    // Key 1 (un-overridden): both components use node defaults.
    TASK_CHECK(near(report.morph_weights[2], 0.75, 1e-6));
    TASK_CHECK(near(report.morph_weights[3], 0.5, 1e-6));
    cgltf_free(data);
    return 0;
}

// ===========================================================================
// contracts group
// ===========================================================================

int case_zero_iterations_return_initialization() {
    BlendScene scene = make_blend_scene();
    SkinSolution solution = solve(problem_from_scene(scene, 0, 1.0));
    TASK_CHECK(solution.ok);
    TASK_CHECK(solution.reports.size() == 1);
    TASK_CHECK(solution.reports[0].iteration == 0);
    for (std::size_t v = 0; v < scene.V; ++v)
        for (std::size_t k = 0; k < scene.K; ++k)
            TASK_CHECK(solution.weights[v * scene.K + k] ==
                       (scene.labels[v] == static_cast<int>(k) ? 1.0 : 0.0));
    return 0;
}

int case_solver_input_validation() {
    BlendScene scene = make_blend_scene();
    SkinProblem base = problem_from_scene(scene, 1, 1.0);
    {
        SkinProblem p = base;
        p.bone_count = 0;
        TASK_CHECK(!solve(p).ok);
    }
    {
        SkinProblem p = base;
        p.bone_count = 9;
        TASK_CHECK(!solve(p).ok);
    }
    {
        SkinProblem p = base;
        p.max_influences = 3;
        p.bone_count = 2; // min(4, K) = 2
        TASK_CHECK(!solve(p).ok);
    }
    {
        SkinProblem p = base;
        p.labels = scene.labels;
        p.labels[0] = 7;
        TASK_CHECK(!solve(p).ok);
    }
    {
        SkinProblem p = base;
        p.labels = scene.labels;
        for (int& label : p.labels)
            if (label == 1) label = 0;
        TASK_CHECK(!solve(p).ok); // bone 1 label missing
    }
    {
        SkinProblem p = base;
        p.max_error = std::numeric_limits<double>::quiet_NaN();
        TASK_CHECK(!solve(p).ok);
    }
    {
        SkinProblem p = base;
        p.observed_positions[0] =
            std::numeric_limits<double>::infinity();
        TASK_CHECK(!solve(p).ok);
    }
    {
        SkinProblem p = base;
        p.rest_positions.pop_back();
        TASK_CHECK(!solve(p).ok);
    }
    return 0;
}

int case_failure_does_not_overwrite_held_result() {
    BlendScene scene = make_blend_scene();
    SkinSolution good = solve(problem_from_scene(scene, 5, 1.0));
    TASK_CHECK(good.ok);
    const std::vector<double> saved_weights = good.weights;
    const double saved_energy = good.final_energy;

    SkinProblem bad = problem_from_scene(scene, 5, 1.0);
    bad.bone_count = 0;
    SkinSolution failed = solve(bad);
    TASK_CHECK(!failed.ok);
    TASK_CHECK(!failed.error.empty());

    // The previously held result is untouched.
    TASK_CHECK(good.ok);
    TASK_CHECK(good.weights == saved_weights);
    TASK_CHECK(good.final_energy == saved_energy);
    return 0;
}

// Assets and options for the contract rejections.
struct ContractAsset {
    std::string json;
};

ContractAsset make_basic_contract_asset(const char* interpolation,
                                        bool add_trs_ancestor = false,
                                        bool non_position_target = false,
                                        bool already_skin = false) {
    GltfBuilder b;
    const std::size_t V = 3, M = 1;
    std::vector<float> base = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    const std::size_t base_view = b.add_float_view(base, 12);
    const std::size_t base_acc = b.add_accessor(base_view, 5126, "VEC3", V);
    std::vector<float> delta = {0, 0, 0.1f, 0, 0, 0.1f, 0, 0, 0.1f};
    const std::size_t delta_view = b.add_float_view(delta, 12);
    const std::size_t delta_acc =
        b.add_accessor(delta_view, 5126, "VEC3", V);
    std::vector<float> normal_delta = {0, 1, 0, 0, 1, 0, 0, 1, 0};
    const std::size_t normal_view = b.add_float_view(normal_delta, 12);
    const std::size_t normal_acc =
        b.add_accessor(normal_view, 5126, "VEC3", V);
    std::vector<float> times = {0.0f, 1.0f};
    const std::size_t time_view = b.add_float_view(times, 4);
    const std::size_t time_acc = b.add_accessor(time_view, 5126, "SCALAR", 2);
    std::vector<float> weights = {0.0f, 1.0f};
    const std::size_t weight_view = b.add_float_view(weights, 4);
    const std::size_t weight_acc =
        b.add_accessor(weight_view, 5126, "SCALAR", 2 * M);
    std::ostringstream mesh;
    mesh << "[{\"primitives\":[{\"attributes\":{\"POSITION\":" << base_acc
         << "},\"targets\":[";
    if (non_position_target)
        mesh << "{\"NORMAL\":" << normal_acc << "}";
    else
        mesh << "{\"POSITION\":" << delta_acc << "}";
    mesh << "]}]}]";
    std::ostringstream nodes;
    if (add_trs_ancestor)
        nodes << "[{\"translation\":[0,0,0],\"children\":[1]},{\"mesh\":0}]";
    else
        nodes << "[{\"mesh\":0}]";
    const std::string scenes = "[{\"nodes\":[0]}]";
    std::ostringstream anim;
    anim << "[{\"samplers\":[{\"input\":" << time_acc << ",\"output\":"
         << weight_acc << ",\"interpolation\":\"" << interpolation
         << "\"}],\"channels\":[{\"sampler\":0,\"target\":{\"node\":"
         << (add_trs_ancestor ? 1 : 0) << ",\"path\":\"weights\"}}";
    if (add_trs_ancestor)
        anim << ",{\"sampler\":0,\"target\":{\"node\":0,\"path\":"
                "\"translation\"}}";
    anim << "]}]";
    ContractAsset asset;
    asset.json = b.finalize(mesh.str(), nodes.str(), scenes, anim.str());
    (void)already_skin;
    return asset;
}

int case_asset_contract_rejections() {
    // Unsupported interpolation.
    {
        ContractAsset asset =
            make_basic_contract_asset("CUBICSPLINE");
        cgltf_data* data = parse_and_load(asset.json);
        TASK_CHECK(data != nullptr);
        DecomposeOptions o;
        o.data = data;
        o.target_node = &data->nodes[0];
        o.animation_index = 0;
        o.sample_times = {0.0, 1.0};
        o.bone_count = 1;
        o.max_influences = 1;
        o.labels = {0, 0, 0};
        o.iterations = 0;
        o.max_error = 1.0;
        DecomposeReport report = decompose_morph_skin(o);
        TASK_CHECK(!report.ok);
        TASK_CHECK(report.error.find("interpolation") != std::string::npos);
        cgltf_free(data);
    }
    // TRS animation on an ancestor.
    {
        ContractAsset asset =
            make_basic_contract_asset("LINEAR", true);
        cgltf_data* data = parse_and_load(asset.json);
        TASK_CHECK(data != nullptr);
        DecomposeOptions o;
        o.data = data;
        o.target_node = &data->nodes[1];
        o.animation_index = 0;
        o.sample_times = {0.0, 1.0};
        o.bone_count = 1;
        o.max_influences = 1;
        o.labels = {0, 0, 0};
        o.iterations = 0;
        o.max_error = 1.0;
        DecomposeReport report = decompose_morph_skin(o);
        TASK_CHECK(!report.ok);
        TASK_CHECK(report.error.find("TRS") != std::string::npos);
        cgltf_free(data);
    }
    // Non-POSITION morph target.
    {
        ContractAsset asset =
            make_basic_contract_asset("LINEAR", false, true);
        cgltf_data* data = parse_and_load(asset.json);
        TASK_CHECK(data != nullptr);
        DecomposeOptions o;
        o.data = data;
        o.target_node = &data->nodes[0];
        o.animation_index = 0;
        o.sample_times = {0.0, 1.0};
        o.bone_count = 1;
        o.max_influences = 1;
        o.labels = {0, 0, 0};
        o.iterations = 0;
        o.max_error = 1.0;
        DecomposeReport report = decompose_morph_skin(o);
        TASK_CHECK(!report.ok);
        TASK_CHECK(report.error.find("POSITION") != std::string::npos);
        cgltf_free(data);
    }
    return 0;
}

int case_option_contract_rejections() {
    MorphAsset asset = make_sparse_morph_asset();
    cgltf_data* data = parse_and_load(asset.json);
    TASK_CHECK(data != nullptr);
    auto base_options = [&] {
        DecomposeOptions o;
        o.data = data;
        o.target_node = &data->nodes[1];
        o.animation_index = 0;
        o.sample_times = {0.0, 1.0, 2.0};
        o.bone_count = 2;
        o.max_influences = 2;
        o.labels.resize(asset.total);
        for (std::size_t v = 0; v < asset.total; ++v) o.labels[v] = v % 2;
        o.iterations = 1;
        o.max_error = 1.0;
        return o;
    };
    {
        DecomposeOptions o = base_options();
        o.data = nullptr;
        TASK_CHECK(!decompose_morph_skin(o).ok);
    }
    {
        DecomposeOptions o = base_options();
        o.target_node = nullptr;
        TASK_CHECK(!decompose_morph_skin(o).ok);
    }
    {
        DecomposeOptions o = base_options();
        o.bone_count = 9;
        TASK_CHECK(!decompose_morph_skin(o).ok);
    }
    {
        DecomposeOptions o = base_options();
        o.max_influences = 5;
        TASK_CHECK(!decompose_morph_skin(o).ok);
    }
    {
        DecomposeOptions o = base_options();
        o.sample_times = {0.0, 0.0, 2.0};
        TASK_CHECK(!decompose_morph_skin(o).ok);
    }
    {
        DecomposeOptions o = base_options();
        o.sample_times = {0.0, -1.0, 2.0};
        TASK_CHECK(!decompose_morph_skin(o).ok);
    }
    {
        DecomposeOptions o = base_options();
        o.sample_times = {0.0,
                          std::numeric_limits<double>::quiet_NaN(), 2.0};
        TASK_CHECK(!decompose_morph_skin(o).ok);
    }
    {
        DecomposeOptions o = base_options();
        o.sample_times = {0.0};
        TASK_CHECK(!decompose_morph_skin(o).ok);
    }
    {
        DecomposeOptions o = base_options();
        o.labels[0] = 5;
        TASK_CHECK(!decompose_morph_skin(o).ok);
    }
    {
        DecomposeOptions o = base_options();
        o.labels.resize(asset.total - 1);
        TASK_CHECK(!decompose_morph_skin(o).ok);
    }
    {
        DecomposeOptions o = base_options();
        o.max_error = -1.0;
        TASK_CHECK(!decompose_morph_skin(o).ok);
    }
    {
        DecomposeOptions o = base_options();
        o.animation_index = 999;
        TASK_CHECK(!decompose_morph_skin(o).ok);
    }
    cgltf_free(data);
    return 0;
}

int case_textured_material_and_compression_rejected() {
    // Textured material.
    {
        GltfBuilder b;
        std::vector<float> base = {0, 0, 0, 1, 0, 0, 0, 1, 0};
        const std::size_t base_view = b.add_float_view(base, 12);
        const std::size_t base_acc =
            b.add_accessor(base_view, 5126, "VEC3", 3);
        std::vector<float> delta = {0, 0, 0.1f, 0, 0, 0.1f, 0, 0, 0.1f};
        const std::size_t delta_view = b.add_float_view(delta, 12);
        const std::size_t delta_acc =
            b.add_accessor(delta_view, 5126, "VEC3", 3);
        std::vector<float> times = {0.0f, 1.0f}, weights = {0.0f, 1.0f};
        const std::size_t time_acc =
            b.add_accessor(b.add_float_view(times, 4), 5126, "SCALAR", 2);
        const std::size_t weight_acc = b.add_accessor(
            b.add_float_view(weights, 4), 5126, "SCALAR", 2);
        const std::string meshes =
            "[{\"primitives\":[{\"material\":0,\"attributes\":{\"POSITION\":" +
            std::to_string(base_acc) + "},\"targets\":[{\"POSITION\":" +
            std::to_string(delta_acc) + "}]}]}]";
        const std::string materials =
            "[{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0}}}]";
        const std::string textures = "[{\"source\":0}]";
        const std::string images =
            "[{\"uri\":\"data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEA"
            "AAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5E"
            "rkJggg==\"}]";
        const std::string nodes = "[{\"mesh\":0}]";
        const std::string scenes = "[{\"nodes\":[0]}]";
        const std::string anim =
            "[{\"samplers\":[{\"input\":" + std::to_string(time_acc) +
            ",\"output\":" + std::to_string(weight_acc) +
            ",\"interpolation\":\"LINEAR\"}],\"channels\":[{\"sampler\":0,"
            "\"target\":{\"node\":0,\"path\":\"weights\"}}]}]";
        const std::string json = b.finalize(meshes, nodes, scenes, anim,
                                            materials, textures, images);
        cgltf_data* data = parse_and_load(json);
        TASK_CHECK(data != nullptr);
        DecomposeOptions o;
        o.data = data;
        o.target_node = &data->nodes[0];
        o.animation_index = 0;
        o.sample_times = {0.0, 1.0};
        o.bone_count = 1;
        o.max_influences = 1;
        o.labels = {0, 0, 0};
        o.iterations = 0;
        o.max_error = 1.0;
        DecomposeReport report = decompose_morph_skin(o);
        TASK_CHECK(!report.ok);
        TASK_CHECK(report.error.find("texture") != std::string::npos);
        cgltf_free(data);
    }
    // Compression extension declared on the asset.
    {
        ContractAsset asset = make_basic_contract_asset("LINEAR");
        GltfBuilder b;
        std::string with_ext = asset.json;
        with_ext.insert(with_ext.size() - 1,
                        ",\"extensionsUsed\":[\"EXT_meshopt_compression\"]");
        cgltf_data* data = parse_and_load(with_ext);
        TASK_CHECK(data != nullptr);
        DecomposeOptions o;
        o.data = data;
        o.target_node = &data->nodes[0];
        o.animation_index = 0;
        o.sample_times = {0.0, 1.0};
        o.bone_count = 1;
        o.max_influences = 1;
        o.labels = {0, 0, 0};
        o.iterations = 0;
        o.max_error = 1.0;
        DecomposeReport report = decompose_morph_skin(o);
        TASK_CHECK(!report.ok);
        TASK_CHECK(report.error.find("compress") != std::string::npos);
        cgltf_free(data);
    }
    return 0;
}

int case_deterministic_solve() {
    BlendScene scene = make_blend_scene();
    SkinSolution a = solve(problem_from_scene(scene, 12, 1.0));
    SkinSolution b = solve(problem_from_scene(scene, 12, 1.0));
    TASK_CHECK(a.ok && b.ok);
    TASK_CHECK(a.weights == b.weights);
    TASK_CHECK(a.transforms.size() == b.transforms.size());
    for (std::size_t i = 0; i < a.transforms.size(); ++i) {
        for (int j = 0; j < 9; ++j)
            TASK_CHECK(a.transforms[i].rotation[j] ==
                       b.transforms[i].rotation[j]);
        for (int j = 0; j < 3; ++j)
            TASK_CHECK(a.transforms[i].translation[j] ==
                       b.transforms[i].translation[j]);
    }
    TASK_CHECK(a.reports.size() == b.reports.size());
    for (std::size_t i = 0; i < a.reports.size(); ++i)
        TASK_CHECK(a.reports[i].energy == b.reports[i].energy);
    return 0;
}

// ===========================================================================
// Registration. The driver is a C translation unit, so the registry symbols
// and every entry point need C linkage. Thin trampolines forward to the
// C++ test functions above.
// ===========================================================================

} // namespace decompose_tests

using namespace decompose_tests;

const task_case task_cases[] = {
    // optimization
    {"optimization", "one_round_rigid_enumeration",
     case_one_round_matches_rigid_enumeration},
    {"optimization", "reflection_refused", case_reflection_is_refused},
    {"optimization", "rank_deficient_deterministic",
     case_rank_deficient_and_duplicate_trajectories},
    {"optimization", "mixed_influence_improves",
     case_mixed_influence_improves_and_recomputes},
    {"optimization", "monotone_reports_constraints",
     case_monotone_reports_and_constraints},
    {"optimization", "deterministic_solve", case_deterministic_solve},
    // asset
    {"asset", "multi_primitive_sparse_reload",
     case_multi_primitive_sparse_export_reload},
    {"asset", "non_identity_root_world_space",
     case_non_identity_root_world_space},
    {"asset", "retry_after_rejection", case_retry_after_rejection_keeps_old_file},
    {"asset", "step_clamp_default_weights", case_step_clamp_and_default_weights},
    {"asset", "single_bone_export", case_single_bone_export},
    {"asset", "worst_location_tiebreak", case_worst_location_and_float_tiebreak},
    {"asset", "node_default_weight_precedence",
     case_node_default_weights_take_precedence},
    // contracts
    {"contracts", "zero_iterations_initialization",
     case_zero_iterations_return_initialization},
    {"contracts", "solver_input_validation", case_solver_input_validation},
    {"contracts", "failure_keeps_held_result",
     case_failure_does_not_overwrite_held_result},
    {"contracts", "asset_contract_rejections", case_asset_contract_rejections},
    {"contracts", "option_contract_rejections", case_option_contract_rejections},
    {"contracts", "texture_compression_rejected",
     case_textured_material_and_compression_rejected},
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
