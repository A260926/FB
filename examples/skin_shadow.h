/*
 * skin_shadow.h
 *
 * Occluder resources and per-instance point-light shadow volumes for
 * glTF meshes whose geometric edges are split across material primitives,
 * UV islands or hard-normal seams.
 *
 * The resource is built once from a validated, buffer-loaded cgltf_data and
 * one of its meshes. It keeps every TRIANGLES face of every primitive with
 * its original primitive/triangle provenance and rebuilds a closed,
 * manifold occluder topology by welding only vertices that are identical for
 * every quantity that can move the occluder: static position, every POSITION
 * morph delta and the normalized skin influence. UVs, normals, tangents,
 * colors and material assignments are deliberately ignored.
 *
 * A shadow volume is generated per node instance from the caller supplied
 * world matrices, an optional morph weight override and a world-space point
 * light. Each generated object owns its own vertices, indices and provenance
 * and is independent of every other generation; failed generations never
 * touch previously returned results and never mutate the source data.
 *
 * The implementation only uses the C++17 standard library and cgltf.
 */
#ifndef SKIN_SHADOW_H
#define SKIN_SHADOW_H

#include <cstddef>
#include <cstdint>

#include "cgltf.h"

namespace skin_shadow {

/*
 * Reason codes returned together with the build/generation error so callers
 * can distinguish topology rejection from invalid frame input.
 */
enum class Status {
	ok = 0,
	invalid_argument,       /* null pointers, mesh not owned by data, ...    */
	unsupported_geometry,   /* compression, non-TRIANGLES primitives, ...    */
	bad_accessor,           /* required accessor missing or wrong format     */
	non_manifold,           /* open edge, same-winding edge, bad fan, ...    */
	degenerate_face,        /* repeated or zero-area source triangle         */
	bad_matrix,             /* missing/non-finite world or joint matrix      */
	bad_weights,            /* wrong override count or non-finite weights    */
	bad_light,              /* non-finite light position                     */
	degenerate_frame,       /* non-finite deformation, zero volume/area      */
	out_of_memory
};

/* Location of the source triangle or edge that caused a topology failure. */
struct ErrorLocation {
	cgltf_size primitive = 0; /* primitive index within the mesh             */
	cgltf_size triangle = 0;  /* triangle index within that primitive        */
	cgltf_size vertex = 0;    /* local corner/vertex index when relevant      */
};

struct Error {
	Status status = Status::ok;
	/* Human readable, null terminated. Always valid, even on success. */
	const char* message = "";
	/* Filled for topology/degenerate-face rejections. */
	ErrorLocation first;
	ErrorLocation second;
};

/* Source face that every occluder/output triangle can be traced back to. */
struct FaceSource {
	cgltf_size primitive = 0;
	cgltf_size triangle = 0;
};

/* Source face whose one-sided adjacency produced a silhouette edge. */
struct EdgeSource {
	cgltf_size primitive = 0;
	cgltf_size triangle = 0;
	/* Corner (0..2) of that triangle: directed edge corner -> corner+1. */
	int corner = 0;
};

/*
 * Immutable occluder resource. Reference the same object from as many nodes
 * as required; generation never mutates its shared topology.
 */
class ShadowVolume;

class Occluder {
public:
				Occluder() = default;
				~Occluder();
				Occluder(const Occluder&) = delete;
	Occluder&	operator=(const Occluder&) = delete;
				Occluder(Occluder&&) noexcept;
	Occluder&	operator=(Occluder&&) noexcept;

	cgltf_size	vertex_count() const;
	cgltf_size	face_count() const;
	/* Number of POSITION morph targets shared by all primitives. */
	cgltf_size	target_count() const;
	/* Number of connected closed components found in the mesh. */
	cgltf_size	component_count() const;
	const FaceSource* face_sources() const;

private:
	friend class ShadowVolume;
	friend Status build(const cgltf_data*, const cgltf_mesh*, Occluder&, Error&);
	friend Status build_occluder_impl(const cgltf_data*, const cgltf_mesh*,
			Occluder&, Error&);
	friend Status generate_shadow_impl(const Occluder&, const cgltf_data*,
			const cgltf_node*, const float*, const float*, cgltf_size,
			const float*, ShadowVolume&, Error&);

	struct Impl;
	Impl*		impl_ = nullptr;
};

/*
 * Per-instance point light shadow volume.
 *
 * Vertices are stored as homogeneous positions so the far cap can keep its
 * w = 0 identity instead of faking infinity with a large distance:
 *   finite vertex: (x, y, z, 1)
 *   far vertex   : (x - light.x, y - light.y, z - light.z, 0)
 * finite_count() == far_count(); finite vertex i and far vertex i are the
 * same topological point seen at the light and at infinity.
 */
class ShadowVolume {
public:
				ShadowVolume() = default;
				~ShadowVolume();
				ShadowVolume(const ShadowVolume&) = delete;
	ShadowVolume& operator=(const ShadowVolume&) = delete;
				ShadowVolume(ShadowVolume&&) noexcept;
	ShadowVolume& operator=(ShadowVolume&&) noexcept;

	cgltf_size	finite_count() const;
	cgltf_size	far_count() const;
	/* xyzw packed, finite vertices first followed by the w=0 copies. */
	const float* vertices() const;
	/* Triangle indices into vertices(). */
	cgltf_size	index_count() const;
	const cgltf_uint* indices() const;

	/* Provenance of every output triangle (caps and side walls). */
	cgltf_size	triangle_source_count() const;
	const FaceSource* triangle_sources() const;
	/* One entry per generated side wall quad (two output triangles). */
	cgltf_size	silhouette_edge_count() const;
	const EdgeSource* silhouette_edges() const;

private:
	friend Status generate_shadow_impl(const Occluder&, const cgltf_data*,
			const cgltf_node*, const float*, const float*, cgltf_size,
			const float*, ShadowVolume&, Error&);

	struct Impl;
	Impl*		impl_ = nullptr;
};

/*
 * Build the occluder resource for one mesh.
 *
 * Requirements on data/mesh:
 *   - data comes from cgltf_parse + cgltf_load_buffers + cgltf_validate;
 *   - only TRIANGLES primitives are accepted (other primitive types and any
 *     Draco/meshopt compression are explicitly rejected);
 *   - POSITION is float VEC3, with arbitrary stride/offset and sparse data;
 *   - indices are U8/U16/U32 or the primitive is non-indexed;
 *   - POSITION morph targets are optional, but every primitive must expose
 *     the same target count; a target missing a POSITION contributes zero;
 *   - JOINTS_0/WEIGHTS_0 and the optional second set are merged per vertex;
 *     joints are non-sparse U8/U16, weights are float or normalized
 *     U8/U16; duplicate joints are merged, zero weights dropped, the rest
 *     sorted by joint and re-normalized; zero-weight slots never read joint.
 *
 * Vertices are welded only when position, every morph delta and the
 * normalized skin influence all match. The welded undirected edges must each
 * have exactly two oppositely oriented faces and every vertex neighborhood
 * must be a single face fan, otherwise the first offending location is
 * reported and the build fails.
 */
Status build(const cgltf_data* data, const cgltf_mesh* mesh,
		Occluder& out, Error& error);

/*
 * Generate one shadow volume.
 *
 *   node           : node referencing the resource's mesh.
 *   world_matrices : data->nodes_count column-major affine world matrices,
 *                    ordered exactly like data->nodes (all 16 finite).
 *   override       : optional morph weights, override_count entries. An
 *                    explicit non-null pointer with count 0 is a valid
 *                    "all zero" override. Without an override the weights
 *                    come from node, then mesh, then all zero.
 *   light          : finite world-space point light position.
 *
 * Deformation applies morphs first, then weights jointWorld * inverseBind;
 * a missing inverse-bind matrix is treated as identity. Skinned results are
 * not multiplied by the node world matrix, only non-skinned vertices are.
 * The skin's joint nodes are resolved through the current instance every
 * time, so two nodes may share one resource with independent skins.
 *
 * Each connected component is re-oriented outward from the sign of its
 * current signed volume (reflection/shear/non-uniform scale allowed). Faces
 * are lit when dot(outward cross, light - p) > 0; an exact zero is back
 * lit. Side walls are only emitted on edges lit from one side; each quad is
 * split into two triangles whose directed edges cancel both caps.
 */
Status generate(const Occluder& resource, const cgltf_data* data,
		const cgltf_node* node, const float* world_matrices,
		const float* override, cgltf_size override_count,
		const float light[3], ShadowVolume& out, Error& error);

/* Convenience overload without a morph override. */
Status generate(const Occluder& resource, const cgltf_data* data,
		const cgltf_node* node, const float* world_matrices,
		const float light[3], ShadowVolume& out, Error& error);

const char* status_message(Status status);

} /* namespace skin_shadow */

#endif /* SKIN_SHADOW_H */
