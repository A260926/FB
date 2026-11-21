//
//  pn_preview.c
//
//  PN triangle preview: builds one flat triangle (red) and one curved PN
//  triangle (green) side by side against a blue background, waits for the
//  background BVH tasks to finish, renders and writes a 48x32 P3 PPM.
//
//  No window, GPU, external model or account is required.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <c-ray/c-ray.h>
#include <v.h>

static struct cr_shader_node *emissive(struct cr_color c) {
	struct cr_value_node *r = calloc(1, sizeof(*r));
	struct cr_value_node *g = calloc(1, sizeof(*g));
	struct cr_value_node *b = calloc(1, sizeof(*b));
	r->type = g->type = b->type = cr_vn_constant;
	r->arg.constant = c.r;
	g->arg.constant = c.g;
	b->arg.constant = c.b;
	struct cr_color_node *col = calloc(1, sizeof(*col));
	col->type = cr_cn_rgb;
	col->arg.rgb.red = r;
	col->arg.rgb.green = g;
	col->arg.rgb.blue = b;
	struct cr_value_node *strength = calloc(1, sizeof(*strength));
	strength->type = cr_vn_constant;
	strength->arg.constant = 1.0;
	struct cr_shader_node *node = calloc(1, sizeof(*node));
	node->type = cr_bsdf_emissive;
	node->arg.emissive.color = col;
	node->arg.emissive.strength = strength;
	return node;
}

static struct cr_shader_node *background(struct cr_color c) {
	struct cr_value_node *r = calloc(1, sizeof(*r));
	struct cr_value_node *g = calloc(1, sizeof(*g));
	struct cr_value_node *b = calloc(1, sizeof(*b));
	r->type = g->type = b->type = cr_vn_constant;
	r->arg.constant = c.r;
	g->arg.constant = c.g;
	b->arg.constant = c.b;
	struct cr_color_node *col = calloc(1, sizeof(*col));
	col->type = cr_cn_rgb;
	col->arg.rgb.red = r;
	col->arg.rgb.green = g;
	col->arg.rgb.blue = b;
	struct cr_value_node *strength = calloc(1, sizeof(*strength));
	strength->type = cr_vn_constant;
	strength->arg.constant = 1.0;
	struct cr_vector_node *pose = calloc(1, sizeof(*pose));
	pose->type = cr_vec_constant;
	pose->arg.constant = (struct cr_vector){ 0, 0, 0 };
	struct cr_shader_node *node = calloc(1, sizeof(*node));
	node->type = cr_bsdf_background;
	node->arg.background.color = col;
	node->arg.background.strength = strength;
	node->arg.background.pose = pose;
	return node;
}

static cr_mesh add_triangle(struct cr_scene *s, const char *name,
                            struct cr_vector verts[3], struct cr_vector *normals,
                            struct cr_color color, bool pn,
                            cr_material_set *set_out) {
	cr_mesh mesh = cr_scene_mesh_new(s, name);
	struct cr_vertex_buf_param buf = {
		.vertices = verts, .vertex_count = 3,
		.normals = normals, .normal_count = normals ? 3 : 0,
	};
	cr_mesh_bind_vertex_buf(s, mesh, buf);
	int flat_idx[3] = { -1, -1, -1 };
	int normal_idx[3] = { 0, 1, 2 };
	struct cr_face face = {
		.vertex_idx = { 0, 1, 2 },
		.texture_idx = { -1, -1, -1 },
		.mat_idx = 0,
		.has_normals = normals != NULL,
	};
	if (normals)
		memcpy(face.normal_idx, normal_idx, sizeof(normal_idx));
	else
		memcpy(face.normal_idx, flat_idx, sizeof(flat_idx));
	cr_mesh_bind_faces(s, mesh, &face, 1);
	cr_material_set set = cr_scene_new_material_set(s);
	struct cr_shader_node *mat = emissive(color);
	cr_material_set_add(s, set, mat);
	// The API deep-copies the description, so this program does not keep it.
	*set_out = set;
	if (pn)
		cr_mesh_set_pn_triangles(s, mesh, true);
	cr_mesh_finalize(s, mesh);
	return mesh;
}

int main(int argc, char **argv) {
	const char *out_path = argc > 1 ? argv[1] : "pn.ppm";
	cr_log_level_set(Silent);

	struct cr_renderer *r = cr_new_renderer();
	cr_renderer_set_num_pref(r, cr_renderer_samples, 1);
	cr_renderer_set_num_pref(r, cr_renderer_bounces, 0);
	cr_renderer_set_num_pref(r, cr_renderer_threads, 2);
	struct cr_scene *s = cr_renderer_scene_get(r);

	struct cr_shader_node *bg = background((struct cr_color){ 0.1f, 0.2f, 0.9f, 1.0f });
	cr_scene_set_background(s, bg); // Deep-copied; no public free for the desc.

	// Flat red triangle on the left.
	struct cr_vector flat_verts[3] = {
		{ -3.2f, -1.5f, 4.0f }, { -1.2f, -1.5f, 4.0f }, { -2.2f, 1.5f, 4.0f },
	};
	cr_material_set flat_set;
	cr_mesh flat = add_triangle(s, "flat", flat_verts, NULL,
	                            (struct cr_color){ 0.95f, 0.15f, 0.1f, 1.0f }, false, &flat_set);
	cr_instance flat_inst = cr_instance_new(s, flat, cr_object_mesh);
	cr_instance_bind_material_set(s, flat_inst, flat_set);

	// Curved green PN triangle on the right; normals bulge toward the camera.
	struct cr_vector curved_verts[3] = {
		{ 1.2f, -1.5f, 4.0f }, { 3.2f, -1.5f, 4.0f }, { 2.2f, 1.5f, 4.0f },
	};
	struct cr_vector curved_normals[3] = {
		{ 0.5f, 0.0f, 1.0f }, { -0.5f, 0.0f, 1.0f }, { 0.0f, 0.5f, 1.0f },
	};
	cr_material_set curved_set;
	cr_mesh curved = add_triangle(s, "curved", curved_verts, curved_normals,
	                              (struct cr_color){ 0.15f, 0.9f, 0.2f, 1.0f }, true, &curved_set);
	cr_instance curved_inst = cr_instance_new(s, curved, cr_object_mesh);
	cr_instance_bind_material_set(s, curved_inst, curved_set);

	cr_camera cam = cr_camera_new(s);
	cr_camera_set_num_pref(s, cam, cr_camera_res_x, 48);
	cr_camera_set_num_pref(s, cam, cr_camera_res_y, 32);
	cr_camera_update(s, cam);

	// Block until the asynchronous mesh BVH build tasks (including the PN
	// control net) have completed.
	// cr_renderer_render() also waits on the pool before preparing the scene.
	cr_renderer_render(r);

	struct cr_bitmap *bmp = cr_renderer_get_result(r);
	if (!bmp || bmp->precision != cr_bm_float) {
		fprintf(stderr, "pn_preview: missing float result buffer\n");
		return 1;
	}

	FILE *out = fopen(out_path, "w");
	if (!out) {
		fprintf(stderr, "pn_preview: cannot open %s\n", out_path);
		return 1;
	}
	fprintf(out, "P3\n%zu %zu\n255\n", bmp->width, bmp->height);
	const float *px = bmp->data.float_ptr;
	for (size_t y = 0; y < bmp->height; ++y) {
		// The internal buffer stores rows bottom-up; PPM row 0 is the top.
		size_t row = bmp->height - 1 - y;
		for (size_t x = 0; x < bmp->width; ++x) {
			const float *p = &px[(row * bmp->width + x) * 4];
			int rr = (int)(p[0] * 255.0f + 0.5f);
			int gg = (int)(p[1] * 255.0f + 0.5f);
			int bb = (int)(p[2] * 255.0f + 0.5f);
			if (rr < 0) rr = 0; if (rr > 255) rr = 255;
			if (gg < 0) gg = 0; if (gg > 255) gg = 255;
			if (bb < 0) bb = 0; if (bb > 255) bb = 255;
			fprintf(out, "%d %d %d\n", rr, gg, bb);
		}
	}
	fclose(out);
	printf("PN_PREVIEW wrote %zux%zu P3 PPM to %s\n", bmp->width, bmp->height, out_path);

	cr_destroy_renderer(r);
	return 0;
}
