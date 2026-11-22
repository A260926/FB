// cgltf_skin_decompose.h
//
// Offline conversion of morph-target animation on a single mesh node into a
// conventional glTF rigid skin.
//
// The pipeline is split in two:
//
//   1. decompose_morph_skin() samples POSITION morph animation of one mesh
//      node at caller supplied times, builds a SkinProblem, runs the joint
//      solver from skin_decompose_solver.h, and returns a self-contained
//      DecomposeReport that owns every result by value.
//   2. export_decomposed_gltf() turns a successful report into a standalone
//      .gltf file: the selected mesh (morph targets removed, JOINTS_0 /
//      WEIGHTS_0 added, identity inverse-bind matrices), K independent bones
//      with translation/rotation keys at every sample time, and the target
//      node's static world matrix moved onto a common root so the transform
//      is not applied twice. The file is written through cgltf_write_file to
//      a temporary path, re-parsed, buffer-loaded, validated and verified
//      against real skinning before the destination is replaced.
//
// The source cgltf_data is never modified. A failed call never mutates a
// previously returned report and never replaces an existing output file.
//
// Supported inputs: TRIANGLES primitives, indexed (U8/U16/U32) or not, any
// POSITION/target accessor layout (stride, offset, sparse, flattened
// multi-target weights, node/mesh default weights), STEP/LINEAR weight
// channels with end clamping. Explicitly rejected: compression extensions,
// textured materials, non-POSITION morph targets, unsupported weight
// interpolation, and TRS animation on the target node or any ancestor.
//
// C++17 standard library plus the in-repository cgltf reader/writer only.
#ifndef CGLTF_SKIN_DECOMPOSE_H
#define CGLTF_SKIN_DECOMPOSE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "cgltf.h"

#include "skin_decompose_solver.h"

namespace skin_decompose {

// All inputs needed to sample one animated mesh node and solve its skin.
// Every pointer must outlive the call; the pointed-to data is read only.
struct DecomposeOptions {
    cgltf_data* data = nullptr;          // must have its buffers loaded
    cgltf_node* target_node = nullptr;   // mesh node without a skin
    cgltf_size animation_index = 0;      // index into data->animations
    // 2..64 strictly increasing, finite, non-negative sample times in seconds.
    std::vector<double> sample_times;
    cgltf_size bone_count = 0;           // K, 1..8
    cgltf_size max_influences = 0;       // P, 1..min(4, K)
    // One label in [0, K) per connected vertex (primitives in order); every
    // label must occur at least once.
    std::vector<int> labels;
    cgltf_size iterations = 0;           // non-negative alternating rounds
    double max_error = 0.0;              // finite, non-negative, same units as positions
};

// Archived, material-independent copy of one static vertex attribute.
struct ArchivedAttribute {
    cgltf_attribute_type type = cgltf_attribute_type_invalid;
    int index = 0;
    std::string name;                    // populated for custom attributes
    cgltf_type accessor_type = cgltf_type_invalid;
    std::vector<float> values;           // unpacked, normalization already applied
};

// Scalar-only material description; textures are rejected up front.
struct ArchivedMaterial {
    std::string name;
    bool has_pbr = false;
    float base_color_factor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float metallic_factor = 1.0f;
    float roughness_factor = 1.0f;
    float emissive_factor[3] = {0.0f, 0.0f, 0.0f};
    bool double_sided = false;
    bool unlit = false;
    cgltf_alpha_mode alpha_mode = cgltf_alpha_mode_opaque;
    float alpha_cutoff = 0.5f;
};

struct ArchivedPrimitive {
    cgltf_size vertex_count = 0;
    bool indexed = false;
    cgltf_component_type index_component_type = cgltf_component_type_invalid;
    std::vector<uint32_t> indices;       // empty when the primitive is non-indexed
    std::vector<ArchivedAttribute> attributes; // includes POSITION, UV, the rest
    int material_index = -1;             // index into DecomposeReport::materials
};

// Self-contained result: sampling data, solver output and export inputs.
struct DecomposeReport {
    bool ok = false;
    std::string error;                   // human-readable, empty when ok == true

    // Echoed request parameters.
    std::vector<double> sample_times;
    cgltf_size frames = 0;
    cgltf_size vertices = 0;
    cgltf_size bones = 0;
    cgltf_size influences = 0;
    double max_error = 0.0;

    // Sampled data (doubles, owned here): rest p_v and observed x_sv.
    std::vector<double> rest_positions;       // V * 3
    std::vector<double> observed_positions;   // S * V * 3
    // Per-frame flattened morph weights actually used (S * target_count).
    std::vector<double> morph_weights;

    // Solver output and its full energy history.
    SkinSolution solution;

    // Error measured after rounding weights/translations/rotations to the
    // float values that will be written, plus the worst (frame, vertex),
    // ties broken by smallest frame then smallest vertex.
    double max_local_error = 0.0;
    cgltf_size worst_frame = 0;
    cgltf_size worst_vertex = 0;

    // Everything needed to rebuild the glTF without the source cgltf_data.
    double world_matrix[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    std::vector<ArchivedPrimitive> primitives;
    std::vector<ArchivedMaterial> materials;
};

// Sample the asset, validate every contract and run the alternating solver.
// On any failure DecomposeReport::ok is false and `error` explains why; the
// report still owns no aliases into the source asset.
DecomposeReport decompose_morph_skin(const DecomposeOptions& options);

// Build a standalone .gltf from a successful report and atomically replace
// output_path only after the written file reloads, validates and reproduces
// the skinning within the report's tolerance. Returns a cgltf result code;
// on failure *error_out (when non-null) receives the reason and any existing
// output file is left untouched.
cgltf_result export_decomposed_gltf(const DecomposeReport& report,
                                    const char* output_path,
                                    std::string* error_out);

} // namespace skin_decompose

#endif // CGLTF_SKIN_DECOMPOSE_H
