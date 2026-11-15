/*
 * skin_shadow_demo.h
 *
 * Self-contained demonstration entry point (no main()) for the
 * skin_shadow example. It writes a small closed tetrahedral mesh split
 * across two materials with duplicated seam vertices (different normals
 * and UVs), parses and loads it through the real cgltf pipeline, builds an
 * Occluder and generates two point-light shadow volumes in different
 * poses. It only needs a writable temp directory and the C++17 standard
 * library - no window, GPU, external model file or third-party library.
 */
#ifndef SKIN_SHADOW_DEMO_H
#define SKIN_SHADOW_DEMO_H

#include <cstddef>

#include "skin_shadow.h"

namespace skin_shadow_demo {

struct DemoReport {
	bool		ok = false;
	cgltf_size	welded_vertices = 0;
	cgltf_size	welded_faces = 0;
	cgltf_size	components = 0;
	cgltf_size	source_vertices = 0; /* before welding, across both primitives */
};

/*
 * Runs the full demonstration. On success first/second hold independent
 * shadow volumes for two poses of the same node and the report describes
 * the recovered closed topology. Returns false (with reset outputs) on any
 * unexpected failure.
 */
bool run(skin_shadow::ShadowVolume& first,
		skin_shadow::ShadowVolume& second, DemoReport& report);

} /* namespace skin_shadow_demo */

#endif /* SKIN_SHADOW_DEMO_H */
