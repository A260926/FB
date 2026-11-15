//=====================================================================
//
// caustics.c - 离线确定性光束通量焦散通道实现
//
// 管线：
//   1. 正交光视图光栅化输入三角形（六个齐次边界裁剪、像素中心采样、
//      最小 z/w 深度，平深取面 ID 小者），记录最近折射面世界位置、
//      归一法线与原面 ID；
//   2. 逐样本按 Snell 折射一次，求与接收平面的正向交点，无效样本剔除；
//   3. 每四个相邻光样本沿固定对角线拆成两束，三样本均有效且同面才成束，
//      束通量 = 入射辐照度 * 半网格世界面积 * 面透过率；
//   4. 束在接收面的三角足迹按真实相交面积把通量沉积进浮点辐照度图。
//
//=====================================================================
#include "caustics.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef CAUSTICS_EPS
#define CAUSTICS_EPS 1.0e-9f
#define BARY_EPS 1.0e-6f
#endif

struct caustics_map {
	caustics_receiver recv;
	int light_w, light_h;             // 最近一次捕获的光图尺寸
	caustics_sample *samples;         // light_w*light_h 光图样本
	float *irradiance;                // 3*rw*rh 接收图浮点辐照度
	float beam_area;                  // 半光网格在垂直光线平面上的世界面积
	vector_t light_dir;               // 光传播方向（世界空间单位矢量）
};

//=====================================================================
// 小工具
//=====================================================================
static float vec3_dot(float ax, float ay, float az, float bx, float by, float bz) {
	return ax * bx + ay * by + az * bz;
}

// 有限标量检查：NaN/Inf 一律视为非法输入
static int fin(float x) { return isfinite(x); }
static int fin3(float x, float y, float z) {
	return isfinite(x) && isfinite(y) && isfinite(z);
}

// 通用 4x4 求逆（行主序，与 matrix_t 一致）；奇异返回 0
static int matrix_invert4(const matrix_t *m, matrix_t *out) {
	float a[4][8];
	int i, j, k;
	for (i = 0; i < 4; i++) {
		for (j = 0; j < 4; j++) a[i][j] = m->m[i][j];
		for (j = 0; j < 4; j++) a[i][4 + j] = (i == j)? 1.0f : 0.0f;
	}
	for (k = 0; k < 4; k++) {
		int pivot = k;
		float best = (float)fabs(a[k][k]);
		for (i = k + 1; i < 4; i++) {
			float v = (float)fabs(a[i][k]);
			if (v > best) { best = v; pivot = i; }
		}
		if (best < 1.0e-12f) return 0;
		if (pivot != k) {
			for (j = 0; j < 8; j++) {
				float t = a[k][j]; a[k][j] = a[pivot][j]; a[pivot][j] = t;
			}
		}
		{
			float d = a[k][k];
			for (j = 0; j < 8; j++) a[k][j] /= d;
			for (i = 0; i < 4; i++) {
				if (i == k) continue;
				d = a[i][k];
				for (j = 0; j < 8; j++) a[i][j] -= d * a[k][j];
			}
		}
	}
	for (i = 0; i < 4; i++)
		for (j = 0; j < 4; j++)
			out->m[i][j] = a[i][4 + j];
	return 1;
}

//=====================================================================
// 创建 / 释放 / 查询
//=====================================================================
caustics_map *caustics_map_create(const caustics_receiver *receiver) {
	caustics_map *map;
	int n;
	if (receiver == NULL) return NULL;
	if (receiver->map_width < 1 || receiver->map_height < 1) return NULL;
	if (!(fin(receiver->range_u_max) && fin(receiver->range_u_min) &&
		fin(receiver->range_v_max) && fin(receiver->range_v_min))) return NULL;
	if (!(receiver->range_u_max > receiver->range_u_min)) return NULL;
	if (!(receiver->range_v_max > receiver->range_v_min)) return NULL;
	if (!fin3(receiver->origin.x, receiver->origin.y, receiver->origin.z))
		return NULL;
	if (!fin3(receiver->axis_u.x, receiver->axis_u.y, receiver->axis_u.z) ||
		!fin3(receiver->axis_v.x, receiver->axis_v.y, receiver->axis_v.z))
		return NULL;
	if (fabsf(vector_length(&receiver->axis_u) - 1.0f) > 1.0e-4f) return NULL;
	if (fabsf(vector_length(&receiver->axis_v) - 1.0f) > 1.0e-4f) return NULL;
	if (fabsf(vector_dotproduct(&receiver->axis_u, &receiver->axis_v)) > 1.0e-4f)
		return NULL;
	map = (caustics_map*)calloc(1, sizeof(caustics_map));
	if (map == NULL) return NULL;
	map->recv = *receiver;
	n = receiver->map_width * receiver->map_height;
	map->irradiance = (float*)calloc((size_t)n * 3, sizeof(float));
	if (map->irradiance == NULL) {
		free(map);
		return NULL;
	}
	map->light_w = map->light_h = 0;
	map->samples = NULL;
	map->beam_area = 0.0f;
	map->light_dir.x = map->light_dir.y = map->light_dir.z = 0.0f;
	map->light_dir.w = 0.0f;
	return map;
}

void caustics_map_release(caustics_map *map) {
	if (map == NULL) return;
	free(map->samples);
	free(map->irradiance);
	free(map);
}

int caustics_map_width(const caustics_map *map) {
	return (map != NULL)? map->recv.map_width : 0;
}

int caustics_map_height(const caustics_map *map) {
	return (map != NULL)? map->recv.map_height : 0;
}

void caustics_get_irradiance(const caustics_map *map, int x, int y,
	float *r, float *g, float *b) {
	int idx;
	if (r) *r = 0.0f;
	if (g) *g = 0.0f;
	if (b) *b = 0.0f;
	if (map == NULL) return;
	if ((unsigned)x >= (unsigned)map->recv.map_width) return;
	if ((unsigned)y >= (unsigned)map->recv.map_height) return;
	idx = (y * map->recv.map_width + x) * 3;
	if (r) *r = map->irradiance[idx + 0];
	if (g) *g = map->irradiance[idx + 1];
	if (b) *b = map->irradiance[idx + 2];
}

caustics_sample caustics_get_sample(const caustics_map *map, int x, int y) {
	caustics_sample empty;
	memset(&empty, 0, sizeof(empty));
	empty.valid = 0;
	empty.depth = (float)INFINITY;
	empty.face_id = -1;
	if (map == NULL || map->samples == NULL) return empty;
	if ((unsigned)x >= (unsigned)map->light_w) return empty;
	if ((unsigned)y >= (unsigned)map->light_h) return empty;
	return map->samples[y * map->light_w + x];
}

//=====================================================================
// 光图光栅化：六平面齐次裁剪 + 最近深度
//=====================================================================
typedef struct {
	float lx, ly, lz, lw;      // 光空间齐次坐标
	float wx, wy, wz;          // 世界位置
	float nx, ny, nz;          // 世界法线（未归一，插值后归一）
	int face;                  // 原面 ID，整三角形恒定
} cvertex;

// 六个裁剪平面的有符号内侧值，>= 0 表示在内
static float clip_value(const cvertex *v, int plane) {
	switch (plane) {
	case 0: return v->lz;            // z >= 0
	case 1: return v->lw - v->lz;    // z <= w
	case 2: return v->lx + v->lw;    // x >= -w
	case 3: return v->lw - v->lx;    // x <= w
	case 4: return v->ly + v->lw;    // y >= -w
	default:return v->lw - v->ly;    // y <= w
	}
}

static void cvertex_lerp(cvertex *o, const cvertex *a, const cvertex *b, float t) {
	o->lx = a->lx + (b->lx - a->lx) * t;
	o->ly = a->ly + (b->ly - a->ly) * t;
	o->lz = a->lz + (b->lz - a->lz) * t;
	o->lw = a->lw + (b->lw - a->lw) * t;
	o->wx = a->wx + (b->wx - a->wx) * t;
	o->wy = a->wy + (b->wy - a->wy) * t;
	o->wz = a->wz + (b->wz - a->wz) * t;
	o->nx = a->nx + (b->nx - a->nx) * t;
	o->ny = a->ny + (b->ny - a->ny) * t;
	o->nz = a->nz + (b->nz - a->nz) * t;
	o->face = a->face;
}

// Sutherland-Hodgman 单平面裁剪，返回输出顶点数
static int clip_plane(const cvertex *in, int n, cvertex *out, int plane) {
	int i, count = 0;
	for (i = 0; i < n; i++) {
		const cvertex *s = &in[(i + n - 1) % n];
		const cvertex *e = &in[i];
		float ds = clip_value(s, plane);
		float de = clip_value(e, plane);
		int sin = ds >= 0.0f, ein = de >= 0.0f;
		if (ein) {
			if (!sin) {
				float t = ds / (ds - de);
				cvertex_lerp(&out[count++], s, e, t);
			}
			out[count++] = *e;
		} else if (sin) {
			float t = ds / (ds - de);
			cvertex_lerp(&out[count++], s, e, t);
		}
	}
	return count;
}

// 把一个世界三角形（含逐顶点法线与面 ID）光栅化进光图
static void rasterize_triangle(caustics_sample *samples, int lw, int lh,
	const cvertex *src) {
	cvertex stage[2][16];
	int n = 3, p, i, k;
	cvertex poly[16];
	for (i = 0; i < 3; i++) stage[0][i] = src[i];
	for (p = 0; p < 6; p++) {
		int in_buf = p & 1, out_buf = in_buf ^ 1;
		n = clip_plane(stage[in_buf], n, stage[out_buf], p);
		if (n < 3) return;
	}
	memcpy(poly, stage[6 & 1], (size_t)n * sizeof(cvertex));

	// 扇形拆三角形后逐像素中心做重心覆盖
	for (k = 1; k + 1 < n; k++) {
		const cvertex *a = &poly[0];
		const cvertex *b = &poly[k];
		const cvertex *c = &poly[k + 1];
		float sx[3], sy[3];
		float minx, maxx, miny, maxy;
		int x0, x1, y0, y1, px, py;
		sx[0] = (a->lx / a->lw + 1.0f) * 0.5f * (float)lw;
		sy[0] = (1.0f - a->ly / a->lw) * 0.5f * (float)lh;
		sx[1] = (b->lx / b->lw + 1.0f) * 0.5f * (float)lw;
		sy[1] = (1.0f - b->ly / b->lw) * 0.5f * (float)lh;
		sx[2] = (c->lx / c->lw + 1.0f) * 0.5f * (float)lw;
		sy[2] = (1.0f - c->ly / c->lw) * 0.5f * (float)lh;
		minx = sx[0]; maxx = sx[0]; miny = sy[0]; maxy = sy[0];
		for (i = 1; i < 3; i++) {
			if (sx[i] < minx) minx = sx[i];
			if (sx[i] > maxx) maxx = sx[i];
			if (sy[i] < miny) miny = sy[i];
			if (sy[i] > maxy) maxy = sy[i];
		}
		x0 = (int)floorf(minx); if (x0 < 0) x0 = 0;
		x1 = (int)floorf(maxx); if (x1 > lw - 1) x1 = lw - 1;
		y0 = (int)floorf(miny); if (y0 < 0) y0 = 0;
		y1 = (int)floorf(maxy); if (y1 > lh - 1) y1 = lh - 1;
		for (py = y0; py <= y1; py++) {
			for (px = x0; px <= x1; px++) {
				float cx = (float)px + 0.5f;
				float cy = (float)py + 0.5f;
				float den = (sy[1] - sy[2]) * (sx[0] - sx[2]) +
					(sx[2] - sx[1]) * (sy[0] - sy[2]);
				float l1, l2, l3;
				caustics_sample *dst;
				if (fabsf(den) < CAUSTICS_EPS) continue;
				l1 = ((sy[1] - sy[2]) * (cx - sx[2]) +
					(sx[2] - sx[1]) * (cy - sy[2])) / den;
				l2 = ((sy[2] - sy[0]) * (cx - sx[2]) +
					(sx[0] - sx[2]) * (cy - sy[2])) / den;
				l3 = 1.0f - l1 - l2;
				// 重心坐标是无量纲量，按浮点误差量级给统一容差，
				// 保证落在扇形共享边上的像素中心被其中一个三角形收下。
				if (l1 < -BARY_EPS || l2 < -BARY_EPS || l3 < -BARY_EPS)
					continue;
				if (l1 < 0.0f) l1 = 0.0f;
				if (l2 < 0.0f) l2 = 0.0f;
				if (l3 < 0.0f) l3 = 0.0f;
				dst = &samples[py * lw + px];
				{
					cvertex hit;
					float nlen, depth;
					hit.lx = l1 * a->lx + l2 * b->lx + l3 * c->lx;
					hit.ly = l1 * a->ly + l2 * b->ly + l3 * c->ly;
					hit.lz = l1 * a->lz + l2 * b->lz + l3 * c->lz;
					hit.lw = l1 * a->lw + l2 * b->lw + l3 * c->lw;
					hit.wx = l1 * a->wx + l2 * b->wx + l3 * c->wx;
					hit.wy = l1 * a->wy + l2 * b->wy + l3 * c->wy;
					hit.wz = l1 * a->wz + l2 * b->wz + l3 * c->wz;
					hit.nx = l1 * a->nx + l2 * b->nx + l3 * c->nx;
					hit.ny = l1 * a->ny + l2 * b->ny + l3 * c->ny;
					hit.nz = l1 * a->nz + l2 * b->nz + l3 * c->nz;
					hit.face = a->face;
					nlen = sqrtf(hit.nx * hit.nx + hit.ny * hit.ny +
						hit.nz * hit.nz);
					if (nlen < CAUSTICS_EPS) continue;
					hit.nx /= nlen; hit.ny /= nlen; hit.nz /= nlen;
					if (!(hit.lw > 0.0f)) continue;
					depth = hit.lz / hit.lw;
					// 最小 z/w，平深取面 ID 小者；深度比较使用相对容差
					if (dst->valid) {
						float dref = fabsf(dst->depth);
						float dtol = 1.0e-7f * (dref > 1.0f ? dref : 1.0f);
						if (depth > dst->depth + dtol ||
							(fabsf(depth - dst->depth) <= dtol &&
								hit.face >= dst->face_id))
							continue;
					}
					dst->valid = 1;
					dst->depth = depth;
					dst->face_id = hit.face;
					dst->position.x = hit.wx;
					dst->position.y = hit.wy;
					dst->position.z = hit.wz;
					dst->position.w = 1.0f;
					dst->normal.x = hit.nx;
					dst->normal.y = hit.ny;
					dst->normal.z = hit.nz;
					dst->normal.w = 0.0f;
				}
			}
		}
	}
}

//=====================================================================
// Snell 折射 + 接收平面求交
//=====================================================================
static void refract_samples(caustics_sample *samples, int total,
	const caustics_capture_input *in, const caustics_receiver *recv,
	const vector_t *light_dir, const vector_t *plane_n) {
	const float eta = in->ior_in / in->ior_out;
	const float eta2 = eta * eta;
	int i;
	for (i = 0; i < total; i++) {
		caustics_sample *s = &samples[i];
		float cosi, sin2t, cost, tx, ty, tz, tlen;
		float denom, dist, hitx, hity, hitz;
		if (!s->valid) continue;
		// 法线朝向入射侧：cosi = -N.L 必须为正
		cosi = -vec3_dot(s->normal.x, s->normal.y, s->normal.z,
			light_dir->x, light_dir->y, light_dir->z);
		if (!(cosi > 0.0f)) { s->valid = 0; continue; }
		sin2t = eta2 * (1.0f - cosi * cosi);
		if (sin2t > 1.0f) { s->valid = 0; continue; }   // 全反射
		cost = sqrtf(1.0f - sin2t);
		// T = eta*L + (eta*cosi - cost)*N
		tx = eta * light_dir->x + (eta * cosi - cost) * s->normal.x;
		ty = eta * light_dir->y + (eta * cosi - cost) * s->normal.y;
		tz = eta * light_dir->z + (eta * cosi - cost) * s->normal.z;
		tlen = sqrtf(tx * tx + ty * ty + tz * tz);
		if (tlen < CAUSTICS_EPS) { s->valid = 0; continue; }
		tx /= tlen; ty /= tlen; tz /= tlen;
		// 接收平面：n.(X - O) = 0，射线 P + s*T，要求正向相交 s >= 0
		denom = vec3_dot(plane_n->x, plane_n->y, plane_n->z, tx, ty, tz);
		if (!(denom > 0.0f)) { s->valid = 0; continue; }
		dist = vec3_dot(plane_n->x, plane_n->y, plane_n->z,
			recv->origin.x - s->position.x,
			recv->origin.y - s->position.y,
			recv->origin.z - s->position.z) / denom;
		if (!(dist >= 0.0f)) { s->valid = 0; continue; }
		hitx = s->position.x + tx * dist;
		hity = s->position.y + ty * dist;
		hitz = s->position.z + tz * dist;
		s->hit.x = hitx; s->hit.y = hity; s->hit.z = hitz; s->hit.w = 1.0f;
	}
}

//=====================================================================
// 二维多边形对矩形裁剪（用于足迹与接收纹素求真实相交面积）
//=====================================================================
typedef struct { float x, y; } p2d;

static int clip2d_plane(const p2d *in, int n, p2d *out, int plane, float v) {
	int i, count = 0;
	for (i = 0; i < n; i++) {
		const p2d *s = &in[(i + n - 1) % n];
		const p2d *e = &in[i];
		float ds, de, t;
		switch (plane) {
		case 0: ds = s->x - v; de = e->x - v; break;   // x >= v
		case 1: ds = v - s->x; de = v - e->x; break;   // x <= v
		case 2: ds = s->y - v; de = e->y - v; break;   // y >= v
		default:ds = v - s->y; de = v - e->y; break;   // y <= v
		}
		if (de >= 0.0f) {
			if (ds < 0.0f) {
				t = ds / (ds - de);
				out[count].x = s->x + (e->x - s->x) * t;
				out[count].y = s->y + (e->y - s->y) * t;
				count++;
			}
			out[count++] = *e;
		} else if (ds >= 0.0f) {
			t = ds / (ds - de);
			out[count].x = s->x + (e->x - s->x) * t;
			out[count].y = s->y + (e->y - s->y) * t;
			count++;
		}
	}
	return count;
}

static float polygon_area(const p2d *p, int n) {
	float a = 0.0f;
	int i;
	for (i = 0; i < n; i++) {
		const p2d *q = &p[(i + 1) % n];
		a += p[i].x * q->y - q->x * p[i].y;
	}
	return 0.5f * fabsf(a);
}

// 三角形与单个接收纹素矩形的相交面积
static float triangle_pixel_overlap(const p2d tri[3], float x0, float x1,
	float y0, float y1) {
	p2d buf[2][16];
	int n = 3;
	memcpy(buf[0], tri, 3 * sizeof(p2d));
	n = clip2d_plane(buf[0], n, buf[1], 0, x0);
	if (n < 3) return 0.0f;
	n = clip2d_plane(buf[1], n, buf[0], 1, x1);
	if (n < 3) return 0.0f;
	n = clip2d_plane(buf[0], n, buf[1], 2, y0);
	if (n < 3) return 0.0f;
	n = clip2d_plane(buf[1], n, buf[0], 3, y1);
	if (n < 3) return 0.0f;
	return polygon_area(buf[0], n);
}

//=====================================================================
// 光束构造与足迹沉积
//=====================================================================
static void deposit_beams(caustics_map *map, const caustics_capture_input *in,
	const caustics_sample *samples, int lw, int lh, float beam_area) {
	const caustics_receiver *r = &map->recv;
	const float du = (r->range_u_max - r->range_u_min) / (float)r->map_width;
	const float dv = (r->range_v_max - r->range_v_min) / (float)r->map_height;
	const float texel_area = du * dv;
	int cx, cy, beam;

	for (cy = 0; cy < lh - 1; cy++) {
		for (cx = 0; cx < lw - 1; cx++) {
			// 固定对角线 A-C：(A,B,C) 与 (A,C,D)，全局确定且与输入无关
			const caustics_sample *q[4] = {
				&samples[cy * lw + cx],
				&samples[cy * lw + cx + 1],
				&samples[(cy + 1) * lw + cx + 1],
				&samples[(cy + 1) * lw + cx]
			};
			static const int tri[2][3] = { {0,1,2}, {0,2,3} };
			for (beam = 0; beam < 2; beam++) {
				const caustics_sample *s0 = q[tri[beam][0]];
				const caustics_sample *s1 = q[tri[beam][1]];
				const caustics_sample *s2 = q[tri[beam][2]];
				p2d foot[3];
				float area, inv_area;
				float flux[3];
				int face, i, ix0, ix1, iy0, iy1, ix, iy;
				if (!s0->valid || !s1->valid || !s2->valid) continue;
				if (s0->face_id != s1->face_id || s1->face_id != s2->face_id)
					continue;
				face = s0->face_id;
				// 落点转接收面 uv
				for (i = 0; i < 3; i++) {
					const caustics_sample *s = (i == 0)? s0 : (i == 1)? s1 : s2;
					float rx = s->hit.x - r->origin.x;
					float ry = s->hit.y - r->origin.y;
					float rz = s->hit.z - r->origin.z;
					foot[i].x = vec3_dot(rx, ry, rz,
						r->axis_u.x, r->axis_u.y, r->axis_u.z);
					foot[i].y = vec3_dot(rx, ry, rz,
						r->axis_v.x, r->axis_v.y, r->axis_v.z);
				}
				area = polygon_area(foot, 3);      // 翻转足迹同样取正面积
				if (!(area > 0.0f)) continue;       // 零面积丢弃
				inv_area = 1.0f / area;
				// 束通量 = 入射辐照度 * 半网格世界面积 * RGB 透过率
				flux[0] = in->incident->r * beam_area * in->transmit[face].r;
				flux[1] = in->incident->g * beam_area * in->transmit[face].g;
				flux[2] = in->incident->b * beam_area * in->transmit[face].b;
				// 包围的纹素范围，出图部分自然丢失且不重新归一
				ix0 = (int)floorf((fminf(fminf(foot[0].x, foot[1].x), foot[2].x)
					- r->range_u_min) / du);
				ix1 = (int)floorf((fmaxf(fmaxf(foot[0].x, foot[1].x), foot[2].x)
					- r->range_u_min) / du);
				iy0 = (int)floorf((fminf(fminf(foot[0].y, foot[1].y), foot[2].y)
					- r->range_v_min) / dv);
				iy1 = (int)floorf((fmaxf(fmaxf(foot[0].y, foot[1].y), foot[2].y)
					- r->range_v_min) / dv);
				if (ix0 < 0) ix0 = 0;
				if (iy0 < 0) iy0 = 0;
				if (ix1 > r->map_width - 1) ix1 = r->map_width - 1;
				if (iy1 > r->map_height - 1) iy1 = r->map_height - 1;
				for (iy = iy0; iy <= iy1; iy++) {
					for (ix = ix0; ix <= ix1; ix++) {
						float x0 = r->range_u_min + ix * du;
						float y0 = r->range_v_min + iy * dv;
						float overlap = triangle_pixel_overlap(foot, x0, x0 + du,
							y0, y0 + dv);
						float fraction;
						int idx;
						if (!(overlap > 0.0f)) continue;
						fraction = overlap * inv_area;
						idx = (iy * r->map_width + ix) * 3;
						// 平均辐照度 = 沉积通量 / 纹素面积；重叠足迹累加
						map->irradiance[idx + 0] += flux[0] * fraction / texel_area;
						map->irradiance[idx + 1] += flux[1] * fraction / texel_area;
						map->irradiance[idx + 2] += flux[2] * fraction / texel_area;
					}
				}
			}
		}
	}
}

//=====================================================================
// 捕获入口
//=====================================================================
int caustics_capture(caustics_map *map, const caustics_capture_input *input) {
	caustics_sample *new_samples = NULL;
	float *new_irr = NULL;
	vector_t plane_n;
	vector_t light_dir;
	matrix_t inv_light;
	vector_t light_z;
	int lw, lh, total, i, tri;
	float cell_area, beam_area;

	if (map == NULL || input == NULL) return -1;
	if (input->triangle_count < 0) return -1;
	if (input->triangle_count > 0 &&
		(input->positions == NULL || input->normals == NULL ||
			input->transmit == NULL)) return -1;
	if (input->light == NULL || input->incident == NULL) return -1;
	if (!(fin(input->aperture_width) && input->aperture_width > 0.0f)) return -1;
	if (!(fin(input->aperture_height) && input->aperture_height > 0.0f)) return -1;
	if (input->light_width < 2 || input->light_height < 2) return -1;
	if (!(fin(input->ior_in) && input->ior_in > 0.0f)) return -1;
	if (!(fin(input->ior_out) && input->ior_out > 0.0f)) return -1;
	if (!fin3(input->incident->r, input->incident->g, input->incident->b)) return -1;
	if (input->incident->r < 0.0f || input->incident->g < 0.0f ||
		input->incident->b < 0.0f) return -1;
	for (i = 0; i < 16; i++) {
		if (!fin(input->light->m[i / 4][i % 4])) return -1;
	}
	for (i = 0; i < input->triangle_count; i++) {
		const color_t *t = &input->transmit[i];
		int k;
		if (!fin3(t->r, t->g, t->b)) return -1;
		if (t->r < 0.0f || t->g < 0.0f || t->b < 0.0f) return -1;
		if (t->r > 1.0f || t->g > 1.0f || t->b > 1.0f) return -1;
		for (k = 0; k < 3; k++) {
			const point_t *p = &input->positions[i * 3 + k];
			const vector_t *nv = &input->normals[i * 3 + k];
			if (!fin3(p->x, p->y, p->z) || !fin(p->w)) return -1;
			if (!fin3(nv->x, nv->y, nv->z)) return -1;
		}
	}
	lw = input->light_width;
	lh = input->light_height;
	total = lw * lh;
	new_samples = (caustics_sample*)calloc((size_t)total, sizeof(caustics_sample));
	new_irr = (float*)calloc((size_t)map->recv.map_width * map->recv.map_height * 3,
		sizeof(float));
	if (new_samples == NULL || new_irr == NULL) {
		free(new_samples);
		free(new_irr);
		return -1;
	}
	for (i = 0; i < total; i++) {
		new_samples[i].valid = 0;
		new_samples[i].depth = (float)INFINITY;
		new_samples[i].face_id = -1;
	}

	// 光传播方向：光空间 +z 轴在世界空间的方向
	if (!matrix_invert4(input->light, &inv_light)) {
		free(new_samples);
		free(new_irr);
		return -1;
	}
	light_z.x = 0.0f; light_z.y = 0.0f; light_z.z = 1.0f; light_z.w = 0.0f;
	matrix_apply(&light_dir, &light_z, &inv_light);
	if (vector_length(&light_dir) < CAUSTICS_EPS) {
		free(new_samples);
		free(new_irr);
		return -1;
	}
	vector_normalize(&light_dir);

	// 接收平面法线 n = u x v
	vector_crossproduct(&plane_n, &map->recv.axis_u, &map->recv.axis_v);
	if (vector_length(&plane_n) < CAUSTICS_EPS) {
		free(new_samples);
		free(new_irr);
		return -1;
	}
	vector_normalize(&plane_n);

	// 1) 光图光栅化
	for (tri = 0; tri < input->triangle_count; tri++) {
		cvertex cv[3];
		int k;
		for (k = 0; k < 3; k++) {
			const point_t *p = &input->positions[tri * 3 + k];
			const vector_t *n = &input->normals[tri * 3 + k];
			vector_t lv;
			matrix_apply(&lv, p, input->light);
			cv[k].lx = lv.x; cv[k].ly = lv.y; cv[k].lz = lv.z; cv[k].lw = lv.w;
			cv[k].wx = p->x; cv[k].wy = p->y; cv[k].wz = p->z;
			cv[k].nx = n->x; cv[k].ny = n->y; cv[k].nz = n->z;
			cv[k].face = tri;
		}
		rasterize_triangle(new_samples, lw, lh, cv);
	}

	// 2) 折射与平面求交（结果写回 new_samples 的 hit/valid）
	refract_samples(new_samples, total, input, &map->recv, &light_dir, &plane_n);

	// 3) 束通量足迹沉积到临时接收图
	cell_area = (input->aperture_width / (float)lw) *
		(input->aperture_height / (float)lh);
	beam_area = 0.5f * cell_area;
	{
		float *old_irr = map->irradiance;
		map->irradiance = new_irr;
		deposit_beams(map, input, new_samples, lw, lh, beam_area);
		map->irradiance = old_irr;
	}

	// 4) 成功后一次性发布：替换旧缓冲前对象状态保持不变，
	//    因此任何失败都不会发布半幅结果。
	free(map->samples);
	map->samples = new_samples;
	free(map->irradiance);
	map->irradiance = new_irr;
	map->light_w = lw;
	map->light_h = lh;
	map->beam_area = beam_area;
	map->light_dir = light_dir;
	return 0;
}

//=====================================================================
// 双线性采样与设备绑定
//=====================================================================
void caustics_sample_irradiance(const caustics_map *map, float u, float v,
	float *r, float *g, float *b) {
	const caustics_receiver *rc;
	float du, dv, gx, gy, fx, fy;
	int ix, iy;
	float w00, w10, w01, w11;
	if (r) *r = 0.0f;
	if (g) *g = 0.0f;
	if (b) *b = 0.0f;
	if (map == NULL) return;
	rc = &map->recv;
	// 图外为 0
	if (u < rc->range_u_min || u > rc->range_u_max ||
		v < rc->range_v_min || v > rc->range_v_max) return;
	du = (rc->range_u_max - rc->range_u_min) / (float)rc->map_width;
	dv = (rc->range_v_max - rc->range_v_min) / (float)rc->map_height;
	gx = (u - rc->range_u_min) / du - 0.5f;
	gy = (v - rc->range_v_min) / dv - 0.5f;
	ix = (int)floorf(gx);
	iy = (int)floorf(gy);
	fx = gx - (float)ix;
	fy = gy - (float)iy;
	w00 = (1.0f - fx) * (1.0f - fy);
	w10 = fx * (1.0f - fy);
	w01 = (1.0f - fx) * fy;
	w11 = fx * fy;
	// 越界邻域权重按 0 处理
	#define IRR_AT(xx, yy, ww, ch) do { \
		if ((unsigned)(xx) < (unsigned)rc->map_width && \
			(unsigned)(yy) < (unsigned)rc->map_height) { \
			int idx = ((yy) * rc->map_width + (xx)) * 3 + (ch); \
			if (r && (ch) == 0) *r += map->irradiance[idx] * (ww); \
			if (g && (ch) == 1) *g += map->irradiance[idx] * (ww); \
			if (b && (ch) == 2) *b += map->irradiance[idx] * (ww); \
		} \
	} while (0)
	IRR_AT(ix,     iy,     w00, 0); IRR_AT(ix + 1, iy,     w10, 0);
	IRR_AT(ix,     iy + 1, w01, 0); IRR_AT(ix + 1, iy + 1, w11, 0);
	IRR_AT(ix,     iy,     w00, 1); IRR_AT(ix + 1, iy,     w10, 1);
	IRR_AT(ix,     iy + 1, w01, 1); IRR_AT(ix + 1, iy + 1, w11, 1);
	IRR_AT(ix,     iy,     w00, 2); IRR_AT(ix + 1, iy,     w10, 2);
	IRR_AT(ix,     iy + 1, w01, 2); IRR_AT(ix + 1, iy + 1, w11, 2);
	#undef IRR_AT
}

// mini3d 设备的着色钩子：在接收平面容差内做一次照明后量化
static int caustics_device_shade(const caustics_map *map, const point_t *world,
	float tolerance, float ambient, float gain,
	const color_t *base, color_t *out) {
	const caustics_receiver *rc = &map->recv;
	vector_t plane_n, d;
	float dist, uu, vv;
	float ir = 0.0f, ig = 0.0f, ib = 0.0f;
	vector_crossproduct(&plane_n, &rc->axis_u, &rc->axis_v);
	vector_normalize(&plane_n);
	vector_sub(&d, world, &rc->origin);
	dist = vector_dotproduct(&plane_n, &d);
	if (fabsf(dist) > tolerance) return 0;
	uu = vector_dotproduct(&d, &rc->axis_u);
	vv = vector_dotproduct(&d, &rc->axis_v);
	caustics_sample_irradiance(map, uu, vv, &ir, &ig, &ib);
	out->r = base->r * (ambient + gain * ir);
	out->g = base->g * (ambient + gain * ig);
	out->b = base->b * (ambient + gain * ib);
	return 1;
}

int caustics_bind_device(caustics_map *map, device_t *device, float tolerance,
	float ambient, float gain) {
	if (map == NULL || device == NULL) return -1;
	if (!fin(tolerance) || !fin(ambient) || !fin(gain)) return -1;
	if (!(tolerance >= 0.0f) || !(ambient >= 0.0f) || !(gain >= 0.0f)) return -1;
	device->caustics = map;
	device->caustics_tolerance = tolerance;
	device->caustics_ambient = ambient;
	device->caustics_gain = gain;
	device->caustics_shade = caustics_device_shade;
	return 0;
}

void caustics_unbind_device(device_t *device) {
	if (device == NULL) return;
	device->caustics = NULL;
	device->caustics_shade = NULL;
}
