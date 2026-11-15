// C interface probe for the multi-pose MeshDecl fields. Compiled as C11 and
// called from the C++ test entry point.
#include "source/xatlas/xatlas_c.h"
#include <stddef.h>

int multipose_c_probe(void)
{
	xatlasMeshDecl decl;
	xatlasMeshDeclInit(&decl);
	int ok = 1;
	if (decl.poses != NULL)
		ok = 0;
	if (decl.poseCount != 0)
		ok = 0;
	// Layout: the pose fields are appended after epsilon, in this order.
	if (offsetof(xatlasMeshDecl, poses) < offsetof(xatlasMeshDecl, epsilon))
		ok = 0;
	if (offsetof(xatlasMeshDecl, poseCount) < offsetof(xatlasMeshDecl, poses))
		ok = 0;
	xatlasMeshPose pose;
	pose.vertexPositionData = NULL;
	pose.vertexPositionStride = 12;
	pose.weight = 1.0f;
	decl.poses = &pose;
	decl.poseCount = 1;
	if (decl.poses[0].vertexPositionStride != 12)
		ok = 0;
	if (decl.poses[0].weight != 1.0f)
		ok = 0;
	decl.poses = NULL;
	decl.poseCount = 0;
	// The C struct must remain usable through the API.
	xatlasAtlas *atlas = xatlasCreate();
	if (!atlas)
		return 0;
	xatlasDestroy(atlas);
	return ok ? 0 : 1;
}
