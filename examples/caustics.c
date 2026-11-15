//=====================================================================
//
// examples/caustics.c - 离线焦散通道示例
//
// 生成一块带变化法线的玻璃（单界面，平行光 +z 入射），捕获折射光束在
// 接收平面上的焦散，输出两张 PPM（P6）：
//   caustics_map.ppm    接收图浮点辐照度（简单色调映射后）
//   caustics_camera.ppm 原 mini3d 相机绑定焦散图后的绘制结果
//
// 构建（C99，无窗口/GPU/第三方库）：
//   cc -std=c99 -DMINI3D_HEADLESS -I. examples/caustics.c \
//      mini3d.c caustics.c -lm -o caustics_demo
//
//=====================================================================
#include <stdio.h>
#include <math.h>
#include <string.h>

#include "caustics.h"

#define MAP_W 200
#define MAP_H 200
#define FB_W  256
#define FB_H  256

static int write_ppm(const char *path, int w, int h, const unsigned char *rgb) {
	FILE *fp = fopen(path, "wb");
	if (fp == NULL) {
		fprintf(stderr, "cannot open %s\n", path);
		return -1;
	}
	fprintf(fp, "P6\n%d %d\n255\n", w, h);
	if (fwrite(rgb, 3, (size_t)w * h, fp) != (size_t)w * h) {
		fclose(fp);
		return -1;
	}
	fclose(fp);
	return 0;
}

// 辐照度 -> 8bit，简单曝光后截断
static unsigned char tone(float v) {
	float c = v * 0.35f;
	if (c < 0.0f) c = 0.0f;
	if (c > 1.0f) c = 1.0f;
	return (unsigned char)(c * 255.0f);
}

int main(void) {
	// ---- 玻璃：单个大三角形，位置共面 z=0.5，逐顶点法线变化形成会聚 ----
	point_t glass_pos[3] = {
		{ -8,-8,0.5f,1 },{ 8,-8,0.5f,1 },{ 0,8,0.5f,1 }
	};
	const float k = 0.35f;
	vector_t glass_nrm[3] = {
		{ -k,0,-1,0 },{ k,0,-1,0 },{ 0,k,-1,0 }
	};
	for (int i = 0; i < 3; i++) {
		float l = sqrtf(glass_nrm[i].x * glass_nrm[i].x +
			glass_nrm[i].z * glass_nrm[i].z);
		glass_nrm[i].x /= l; glass_nrm[i].z /= l;
	}
	color_t transmit[1] = { { 0.95f, 1.0f, 0.9f } };
	color_t incident = { 1.0f, 1.0f, 1.0f };

	matrix_t light;
	matrix_set_identity(&light);

	caustics_capture_input input;
	memset(&input, 0, sizeof(input));
	input.triangle_count = 1;
	input.positions = glass_pos;
	input.normals = glass_nrm;
	input.transmit = transmit;
	input.light = &light;
	input.aperture_width = 2.0f;
	input.aperture_height = 2.0f;
	input.incident = &incident;
	input.light_width = 48;
	input.light_height = 48;
	input.ior_in = 1.0f;      // 空气
	input.ior_out = 1.5f;     // 玻璃

	// ---- 接收平面：z=3，u=x, v=y，范围 [-2,2] ----
	caustics_receiver recv;
	memset(&recv, 0, sizeof(recv));
	recv.origin = (point_t){ 0, 0, 3, 1 };
	recv.axis_u = (vector_t){ 1, 0, 0, 0 };
	recv.axis_v = (vector_t){ 0, 1, 0, 0 };
	recv.range_u_min = -2.0f; recv.range_u_max = 2.0f;
	recv.range_v_min = -2.0f; recv.range_v_max = 2.0f;
	recv.map_width = MAP_W; recv.map_height = MAP_H;

	caustics_map *map = caustics_map_create(&recv);
	if (map == NULL) {
		fprintf(stderr, "caustics_map_create failed\n");
		return 1;
	}
	if (caustics_capture(map, &input) != 0) {
		fprintf(stderr, "caustics_capture failed\n");
		caustics_map_release(map);
		return 1;
	}

	// ---- 输出接收图 PPM ----
	static unsigned char img[MAP_W * MAP_H * 3];
	for (int y = 0; y < MAP_H; y++) {
		for (int x = 0; x < MAP_W; x++) {
			float r, g, b;
			caustics_get_irradiance(map, x, y, &r, &g, &b);
			int idx = (y * MAP_W + x) * 3;
			img[idx + 0] = tone(r);
			img[idx + 1] = tone(g);
			img[idx + 2] = tone(b);
		}
	}
	if (write_ppm("caustics_map.ppm", MAP_W, MAP_H, img) != 0) return 1;
	printf("wrote caustics_map.ppm (%dx%d)\n", MAP_W, MAP_H);

	// ---- 原 mini3d 相机：透视绘制接收平面，绑定同一张焦散图 ----
	static IUINT32 framebuffer[FB_W * FB_H];
	device_t device;
	device_init(&device, FB_W, FB_H, framebuffer);
	device.background = 0x101018;
	device_clear(&device, 0);

	point_t eye = { 0, -5.0f, 3.6f, 1 };
	point_t at  = { 0,  0.0f, 3.0f, 1 };
	point_t up  = { 0,  0.0f, 1.0f, 1 };
	matrix_set_lookat(&device.transform.view, &eye, &at, &up);
	transform_update(&device.transform);
	device.render_state = RENDER_STATE_COLOR;

	// 接收平面四边形（u/v 轴即 x/y），白色底色
	vertex_t quad[4];
	memset(quad, 0, sizeof(quad));
	float corners[4][2] = { {-2,-2},{2,-2},{2,2},{-2,2} };
	for (int i = 0; i < 4; i++) {
		quad[i].pos = (point_t){ corners[i][0], corners[i][1], 3.0f, 1 };
		quad[i].world = quad[i].pos;
		quad[i].color = (color_t){ 1, 1, 1 };
	}
	caustics_bind_device(map, &device, 0.05f, 0.15f, 0.6f);
	device_draw_primitive(&device, &quad[0], &quad[1], &quad[2]);
	device_draw_primitive(&device, &quad[2], &quad[3], &quad[0]);

	static unsigned char cam[FB_W * FB_H * 3];
	for (int y = 0; y < FB_H; y++) {
		for (int x = 0; x < FB_W; x++) {
			IUINT32 px = framebuffer[y * FB_W + x];
			int idx = (y * FB_W + x) * 3;
			cam[idx + 0] = (px >> 16) & 0xff;
			cam[idx + 1] = (px >> 8) & 0xff;
			cam[idx + 2] = px & 0xff;
		}
	}
	if (write_ppm("caustics_camera.ppm", FB_W, FB_H, cam) != 0) return 1;
	printf("wrote caustics_camera.ppm (%dx%d)\n", FB_W, FB_H);

	caustics_unbind_device(&device);
	device_destroy(&device);
	caustics_map_release(map);
	return 0;
}
