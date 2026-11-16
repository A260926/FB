#pragma once
//
// Offline FBX -> glTF 2.0 GLB export.
//
// The exporter reduces every animated FBX node transform into a chain of
// plain glTF TRS nodes (translation / rotation / scale only, no matrices,
// no extensions) so players that only understand TRS animation reproduce the
// FBX pivot/offset/Pre/PostRotation/euler semantics exactly, including
// multi-turn rotations and desynchronized euler channels.
//
// The transform factor definitions are shared with Object::evalLocal() in
// ofbx.cpp (see ofbx::localTransformFactors); the exporter never re-derives
// the pivot formula on its own.
//
#include "ofbx.h"

#include <cstdint>
#include <map>
#include <vector>

namespace ofbx
{

// Error codes returned by exportGltf(). Every failure leaves the previous
// GltfResult contents untouched and never modifies the source scene.
enum class GltfError : int
{
	OK = 0,
	INVALID_ARGUMENT = 1,        // null layer, begin >= end, negative interval
	NON_FINITE_TIME = 2,         // key time or interval is not a finite float second
	UNSORTED_KEYS = 3,           // an animation curve's key times are not strictly increasing
	UNSUPPORTED_INPUT = 4,       // blend shapes, non-triangle geometry, ...
	INVALID_SKIN = 5,            // bad/duplicate clusters, >4 influences, negative weights, ...
	SINGULAR_TRANSFORM = 6,      // a factor matrix cannot be expressed as TRS (zero/flip scale)
	OUT_OF_RANGE = 7,            // cluster vertex index out of geometry range
	NUMERIC_OVERFLOW = 8,        // value does not survive a float round trip, NaN/Inf
	SIZE_OVERFLOW = 9,           // generated GLB would exceed the representable size
	INTERNAL_ERROR = 10,
};

struct GltfResult
{
	GltfError code = GltfError::OK;
	// Human readable, always set when `code != OK`. Points to static storage
	// or to a message owned by this object (see error_message).
	const char* message = "";

	// Self contained binary glTF (GLB), populated only on success.
	std::vector<unsigned char> glb;

	// Maps every source Object::id that became a node chain to the index of
	// the chain's terminal node in the output GLB, i.e. the node carrying the
	// complete local transform of the source object. Source children and skin
	// joints all attach to / reference this terminal node.
	std::map<u64, u32> node_map;

	bool ok() const { return code == GltfError::OK; }

	std::vector<char> error_message;
};

// Exports one animation layer over the half-open-ish interval [begin, end]
// (both endpoints are always emitted; begin must be finite, non-negative and
// strictly smaller than end, both expressible as finite float seconds).
// Output animation time starts at zero, i.e. output t = source t - begin.
//
// On success fills `out` (replacing its previous contents) and returns
// GltfError::OK. On failure returns the error code, fills out->code/message,
// leaves out->glb and out->node_map from the previous successful call intact,
// and never mutates the source scene.
GltfError exportGltf(const IScene& scene, const AnimationLayer& layer,
	double begin, double end, GltfResult& out);

} // namespace ofbx
