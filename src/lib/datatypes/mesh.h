//
//  mesh.h
//  c-ray
//
//  Created by Valtteri Koskivuori on 27/04/2017.
//  Copyright © 2017-2025 Valtteri Koskivuori. All rights reserved.
//

#pragma once

#include <v.h>
#include <c-ray/c-ray.h>
#include <datatypes/poly.h>
#include <datatypes/pn_triangle.h>
#include <common/vector.h>

struct mesh {
	struct vertex_buffer vbuf;
	struct poly *polygons;
	struct bvh *bvh;
	struct pn_mesh_data *pn; // NULL unless PN triangle mode is enabled
	float surface_area;
	char *name;
	float rayOffset;
	bool pn_enabled;   // Requested through cr_mesh_set_pn_triangles before finalize
	bool pn_finalized; // PN control net has been built for this mesh
};

void mesh_free(struct mesh *mesh);
