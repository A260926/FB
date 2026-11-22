// decompose_example.cpp
//
// End-to-end demonstration: build an in-memory morph asset with two rotating
// regions and blended boundary vertices, decompose it into a rigid skin,
// export a standalone .gltf, then reload that file and verify the samples.
//
// Usage: decompose_example <output.gltf>
//
// Standard library plus the in-repository cgltf reader/writer only.
#include "cgltf_skin_decompose.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace {

using skin_decompose::DecomposeOptions;
using skin_decompose::DecomposeReport;
using skin_decompose::export_decomposed_gltf;
using skin_decompose::decompose_morph_skin;

template <class T>
void append(std::vector<unsigned char>& bytes, const T& value) {
    const auto* p = reinterpret_cast<const unsigned char*>(&value);
    bytes.insert(bytes.end(), p, p + sizeof(T));
}

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

void rotz(double angle, const double p[3], double out[3]) {
    const double c = std::cos(angle), s = std::sin(angle);
    out[0] = c * p[0] - s * p[1];
    out[1] = s * p[0] + c * p[1];
    out[2] = p[2];
}

// Builds the complete glTF document text for the demo asset. Every view uses
// a tight stride; the flattened per-keyframe morph weights are one-hot, so at
// each sample time the posed geometry equals the designed target exactly.
std::string build_document(const std::vector<unsigned char>& bin,
                           const std::vector<cgltf_size>& view_offsets,
                           const std::vector<cgltf_size>& view_sizes) {
    auto view = [&](cgltf_size i) {
        std::ostringstream o;
        o << "{\"buffer\":0,\"byteOffset\":" << view_offsets[i]
          << ",\"byteLength\":" << view_sizes[i] << "}";
        return o.str();
    };
    std::ostringstream json;
    json << "{"
         << R"("asset":{"version":"2.0"},"buffers":[{)"
         << "\"byteLength\":" << bin.size()
         << ",\"uri\":\"data:application/octet-stream;base64," << base64(bin)
         << R"("}],"bufferViews":[)";
    for (std::size_t i = 0; i < view_offsets.size(); ++i) {
        if (i) json << ",";
        json << view(i);
    }
    json << R"(],"accessors":[)"
         // 0: prim0 POSITION (6 verts), 1: prim0 TEXCOORD_0.
         << R"({"componentType":5126,"count":6,"type":"VEC3","bufferView":0},)"
         << R"({"componentType":5126,"count":6,"type":"VEC2","bufferView":1},)"
         // 2..5: prim0 morph targets.
         << R"({"componentType":5126,"count":6,"type":"VEC3","bufferView":2},)"
         << R"({"componentType":5126,"count":6,"type":"VEC3","bufferView":3},)"
         << R"({"componentType":5126,"count":6,"type":"VEC3","bufferView":4},)"
         << R"({"componentType":5126,"count":6,"type":"VEC3","bufferView":5},)"
         // 6: prim0 U16 indices, 7: times, 8: flattened weights.
         << R"({"componentType":5123,"count":6,"type":"SCALAR","bufferView":6},)"
         << R"({"componentType":5126,"count":4,"type":"SCALAR","bufferView":7},)"
         << R"({"componentType":5126,"count":16,"type":"SCALAR","bufferView":8},)"
         // 9: prim1 POSITION (3 verts), 10: prim1 TEXCOORD_0.
         << R"({"componentType":5126,"count":3,"type":"VEC3","bufferView":9},)"
         << R"({"componentType":5126,"count":3,"type":"VEC2","bufferView":10},)"
         // 11..14: prim1 morph targets.
         << R"({"componentType":5126,"count":3,"type":"VEC3","bufferView":11},)"
         << R"({"componentType":5126,"count":3,"type":"VEC3","bufferView":12},)"
         << R"({"componentType":5126,"count":3,"type":"VEC3","bufferView":13},)"
         << R"({"componentType":5126,"count":3,"type":"VEC3","bufferView":14})"
         << R"(],"meshes":[{"primitives":[)"
         // Primitive 0: indexed, two static attributes and four targets.
         << R"({"attributes":{"POSITION":0,"TEXCOORD_0":1},"indices":6,"targets":[)"
         << R"({"POSITION":2},{"POSITION":3},{"POSITION":4},{"POSITION":5}]},)"
         // Primitive 1: non-indexed.
         << R"({"attributes":{"POSITION":9,"TEXCOORD_0":10},"targets":[)"
         << R"({"POSITION":11},{"POSITION":12},{"POSITION":13},{"POSITION":14}]})"
         << R"(]}],"nodes":[)"
         // Root carries a non-identity static world transform.
         << R"({"translation":[1.0,2.0,3.0],"rotation":[0.0,0.0,0.14943813247359922,0.9887710779360422],"children":[1]},)"
         << R"({"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0,)"
         << R"("animations":[{"samplers":[{"input":7,"output":8,"interpolation":"LINEAR"}],)"
         << R"("channels":[{"sampler":0,"target":{"node":1,"path":"weights"}}]}]})";
    return json.str();
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <output.gltf>\n",
                     argc > 0 ? argv[0] : "decompose_example");
        return 2;
    }

    constexpr cgltf_size S = 4;
    constexpr cgltf_size K = 2;
    constexpr cgltf_size V0 = 6;
    constexpr cgltf_size V1 = 3;
    constexpr cgltf_size V = V0 + V1;

    // Region membership: +1 rotates with bone 0, -1 with bone 1, boundary
    // vertices are an exact 50/50 blend.
    const int region[V] = {+1, +1, +1, 0, 0, +1, 0, -1, -1};
    double rest[V][3] = {
        {1.0, 0.0, 0.0}, {0.8, 0.3, 0.1}, {0.6, 0.6, -0.2},
        {0.3, 0.8, 0.2}, {-0.2, 0.9, 0.0}, {-0.6, 0.5, 0.1},
        {0.0, -0.7, 0.4}, {-0.5, -0.6, -0.3}, {-0.9, -0.2, 0.2}};

    std::vector<unsigned char> bin;
    auto record_view = [&](std::vector<cgltf_size>& offsets,
                           std::vector<cgltf_size>& sizes) {
        offsets.push_back(bin.size());
        sizes.push_back(0);
    };
    std::vector<cgltf_size> offsets, sizes;
    auto close_view = [&] { sizes.back() = bin.size() - offsets.back(); };

    // View 0: prim0 positions.
    record_view(offsets, sizes);
    for (cgltf_size v = 0; v < V0; ++v)
        for (int r = 0; r < 3; ++r) append<float>(bin, static_cast<float>(rest[v][r]));
    close_view();
    // View 1: prim0 UV.
    record_view(offsets, sizes);
    for (cgltf_size v = 0; v < V0; ++v) {
        append<float>(bin, static_cast<float>(v) * 0.1f);
        append<float>(bin, 0.25f);
    }
    close_view();
    // Views 2..5: prim0 morph deltas at each sample.
    for (cgltf_size s = 0; s < S; ++s) {
        record_view(offsets, sizes);
        const double angle = 0.2 * static_cast<double>(s);
        for (cgltf_size v = 0; v < V0; ++v) {
            double plus[3], minus[3], posed[3];
            rotz(angle, rest[v], plus);
            rotz(-angle, rest[v], minus);
            if (region[v] > 0)
                std::memcpy(posed, plus, sizeof(posed));
            else if (region[v] < 0)
                std::memcpy(posed, minus, sizeof(posed));
            else
                for (int r = 0; r < 3; ++r) posed[r] = 0.5 * (plus[r] + minus[r]);
            for (int r = 0; r < 3; ++r)
                append<float>(bin, static_cast<float>(posed[r] - rest[v][r]));
        }
        close_view();
    }
    // View 6: prim0 U16 indices (two triangles).
    record_view(offsets, sizes);
    const uint16_t indices[V0] = {0, 1, 2, 3, 4, 5};
    for (uint16_t i : indices) append<uint16_t>(bin, i);
    close_view();
    // View 7: sample times.
    record_view(offsets, sizes);
    for (cgltf_size s = 0; s < S; ++s)
        append<float>(bin, static_cast<float>(s));
    close_view();
    // View 8: flattened one-hot morph weights per keyframe.
    record_view(offsets, sizes);
    for (cgltf_size s = 0; s < S; ++s)
        for (cgltf_size m = 0; m < S; ++m)
            append<float>(bin, s == m ? 1.0f : 0.0f);
    close_view();
    // View 9: prim1 positions.
    record_view(offsets, sizes);
    for (cgltf_size v = V0; v < V; ++v)
        for (int r = 0; r < 3; ++r) append<float>(bin, static_cast<float>(rest[v][r]));
    close_view();
    // View 10: prim1 UV.
    record_view(offsets, sizes);
    for (cgltf_size v = V0; v < V; ++v) {
        append<float>(bin, static_cast<float>(v - V0) * 0.5f);
        append<float>(bin, 0.75f);
    }
    close_view();
    // Views 11..14: prim1 morph deltas.
    for (cgltf_size s = 0; s < S; ++s) {
        record_view(offsets, sizes);
        const double angle = 0.2 * static_cast<double>(s);
        for (cgltf_size v = V0; v < V; ++v) {
            double plus[3], minus[3], posed[3];
            rotz(angle, rest[v], plus);
            rotz(-angle, rest[v], minus);
            if (region[v] > 0)
                std::memcpy(posed, plus, sizeof(posed));
            else if (region[v] < 0)
                std::memcpy(posed, minus, sizeof(posed));
            else
                for (int r = 0; r < 3; ++r) posed[r] = 0.5 * (plus[r] + minus[r]);
            for (int r = 0; r < 3; ++r)
                append<float>(bin, static_cast<float>(posed[r] - rest[v][r]));
        }
        close_view();
    }

    const std::string document = build_document(bin, offsets, sizes);
    cgltf_options options{};
    cgltf_data* data = nullptr;
    if (cgltf_parse(&options, document.data(), document.size(), &data) !=
        cgltf_result_success) {
        std::fprintf(stderr, "example: failed to parse demo asset\n");
        return 1;
    }
    if (cgltf_load_buffers(&options, data, nullptr) != cgltf_result_success) {
        std::fprintf(stderr, "example: failed to load demo buffers\n");
        cgltf_free(data);
        return 1;
    }

    std::vector<int> labels(V);
    for (cgltf_size v = 0; v < V; ++v) labels[v] = region[v] < 0 ? 1 : 0;

    DecomposeOptions opts;
    opts.data = data;
    opts.target_node = &data->nodes[1];
    opts.animation_index = 0;
    opts.sample_times = {0.0, 1.0, 2.0, 3.0};
    opts.bone_count = K;
    opts.max_influences = 2;
    opts.labels = labels;
    opts.iterations = 120;
    opts.max_error = 1e-2;

    DecomposeReport report = decompose_morph_skin(opts);
    if (!report.ok) {
        std::fprintf(stderr, "example: decompose failed: %s\n",
                     report.error.c_str());
        cgltf_free(data);
        return 1;
    }
    std::printf("decompose: vertices=%zu frames=%zu energy=%.3g max_local_error=%.3g\n",
                static_cast<std::size_t>(report.vertices),
                static_cast<std::size_t>(report.frames),
                report.solution.final_energy, report.max_local_error);
    for (const auto& r : report.solution.reports)
        std::printf("  report iter=%zu energy=%.10g\n", r.iteration, r.energy);

    std::string export_error;
    const cgltf_result written =
        export_decomposed_gltf(report, argv[1], &export_error);
    cgltf_free(data);
    if (written != cgltf_result_success) {
        std::fprintf(stderr, "example: export failed: %s\n",
                     export_error.c_str());
        return 1;
    }

    // Reload the written file and verify the samples independently.
    cgltf_data* reloaded = nullptr;
    if (cgltf_parse_file(&options, argv[1], &reloaded) != cgltf_result_success ||
        cgltf_load_buffers(&options, reloaded, argv[1]) != cgltf_result_success) {
        std::fprintf(stderr, "example: failed to reload exported file\n");
        return 1;
    }
    if (cgltf_validate(reloaded) != cgltf_result_success ||
        reloaded->meshes_count != 1 || reloaded->skins_count != 1 ||
        reloaded->animations_count != 1) {
        std::fprintf(stderr, "example: exported file failed validation\n");
        cgltf_free(reloaded);
        return 1;
    }
    const cgltf_skin& skin = reloaded->skins[0];
    if (skin.joints_count != K) {
        std::fprintf(stderr, "example: exported bone count mismatch\n");
        cgltf_free(reloaded);
        return 1;
    }
    std::printf("reload: skin joints=%zu primitives=%zu target_count=%zu\n",
                static_cast<std::size_t>(skin.joints_count),
                static_cast<std::size_t>(reloaded->meshes[0].primitives_count),
                static_cast<std::size_t>(reloaded->meshes[0].primitives[0].targets_count));

    // Independently replay the reloaded skin at every sample keyframe and
    // compare against the observed samples, primitive by primitive.
    const cgltf_size bone_count = skin.joints_count;
    cgltf_size global_offset = 0;
    for (cgltf_size pi = 0; pi < reloaded->meshes[0].primitives_count; ++pi) {
        cgltf_primitive& prim = reloaded->meshes[0].primitives[pi];
        const cgltf_accessor *pos_acc = nullptr, *joints_acc = nullptr,
                             *weights_acc = nullptr;
        for (cgltf_size a = 0; a < prim.attributes_count; ++a) {
            if (prim.attributes[a].type == cgltf_attribute_type_position)
                pos_acc = prim.attributes[a].data;
            if (prim.attributes[a].type == cgltf_attribute_type_joints)
                joints_acc = prim.attributes[a].data;
            if (prim.attributes[a].type == cgltf_attribute_type_weights)
                weights_acc = prim.attributes[a].data;
        }
        std::vector<float> pos(cgltf_accessor_unpack_floats(pos_acc, nullptr, 0));
        cgltf_accessor_unpack_floats(pos_acc, pos.data(), pos.size());
        std::vector<float> w4(cgltf_accessor_unpack_floats(weights_acc, nullptr, 0));
        cgltf_accessor_unpack_floats(weights_acc, w4.data(), w4.size());
        std::vector<float> j4(cgltf_accessor_unpack_floats(joints_acc, nullptr, 0));
        cgltf_accessor_unpack_floats(joints_acc, j4.data(), j4.size());

        for (cgltf_size s = 0; s < report.frames; ++s) {
            std::vector<double> R(bone_count * 9, 0.0),
                                t(bone_count * 3, 0.0);
            for (cgltf_size c = 0; c < reloaded->animations[0].channels_count; ++c) {
                cgltf_animation_channel& channel =
                    reloaded->animations[0].channels[c];
                cgltf_size joint = bone_count;
                for (cgltf_size k = 0; k < bone_count; ++k)
                    if (skin.joints[k] == channel.target_node) joint = k;
                if (joint == bone_count) continue;
                std::vector<float> values(
                    cgltf_accessor_unpack_floats(channel.sampler->output,
                                                 nullptr, 0));
                cgltf_accessor_unpack_floats(channel.sampler->output,
                                             values.data(), values.size());
                if (channel.target_path ==
                    cgltf_animation_path_type_translation)
                    for (int r = 0; r < 3; ++r)
                        t[joint * 3 + r] = values[s * 3 + r];
                else if (channel.target_path ==
                         cgltf_animation_path_type_rotation) {
                    const double x = values[s * 4], y = values[s * 4 + 1],
                                 z = values[s * 4 + 2], w = values[s * 4 + 3];
                    double* M = &R[joint * 9];
                    M[0] = 1 - 2 * (y * y + z * z);
                    M[1] = 2 * (x * y - z * w);
                    M[2] = 2 * (x * z + y * w);
                    M[3] = 2 * (x * y + z * w);
                    M[4] = 1 - 2 * (x * x + z * z);
                    M[5] = 2 * (y * z - x * w);
                    M[6] = 2 * (x * z - y * w);
                    M[7] = 2 * (y * z + x * w);
                    M[8] = 1 - 2 * (x * x + y * y);
                }
            }
            for (cgltf_size v = 0; v < pos_acc->count; ++v) {
                double skinned[3] = {0, 0, 0};
                for (int slot = 0; slot < 4; ++slot) {
                    const float weight = w4[v * 4 + slot];
                    if (weight == 0.0f) continue;
                    const cgltf_size k =
                        static_cast<cgltf_size>(j4[v * 4 + slot]);
                    for (int r = 0; r < 3; ++r) {
                        const double y = R[k * 9 + r * 3] * pos[v * 3] +
                                         R[k * 9 + r * 3 + 1] * pos[v * 3 + 1] +
                                         R[k * 9 + r * 3 + 2] * pos[v * 3 + 2] +
                                         t[k * 3 + r];
                        skinned[r] += static_cast<double>(weight) * y;
                    }
                }
                const cgltf_size global_v = global_offset + v;
                for (int r = 0; r < 3; ++r) {
                    const double expected =
                        report.observed_positions[(s * report.vertices +
                                                   global_v) * 3 + r];
                    if (std::fabs(skinned[r] - expected) >
                        report.max_local_error + 1e-6) {
                        std::fprintf(stderr,
                                     "example: reloaded sample mismatch "
                                     "frame=%zu vertex=%zu\n",
                                     static_cast<std::size_t>(s),
                                     static_cast<std::size_t>(global_v));
                        cgltf_free(reloaded);
                        return 1;
                    }
                }
            }
        }
        global_offset += pos_acc->count;
    }
    cgltf_free(reloaded);
    std::printf("example OK: wrote %s\n", argv[1]);
    return 0;
}
