//=====================================================================
//
// caustics_target.c - 焦散通道确定性数值测试
//
// 通过 task_support/check.h 的 task_cases/task_case_count 注册 capture、
// flux、camera 三组用例；本文件不定义 main，由 task_support/driver.c 驱动。
//
//=====================================================================
#include <math.h>
#include <string.h>
#include <stdlib.h>

#include "caustics.h"
#include "../task_support/check.h"

static int nearf(float a, float b) { return fabsf(a - b) < 1.0e-5f; }
static int nearf_tol(float a, float b, float tol) { return fabsf(a - b) < tol; }

// 标准平面玻璃：z=0 大三角形，法线朝入射侧 -z，恒等光矩阵
static void flat_glass(point_t pos[3], vector_t nrm[3], vector_t fill_nrm[3],
	float z) {
	pos[0] = (point_t){ -8, -8, z, 1 };
	pos[1] = (point_t){  8, -8, z, 1 };
	pos[2] = (point_t){  0,  8, z, 1 };
	nrm[0] = nrm[1] = nrm[2] = (vector_t){ 0, 0, -1, 0 };
	if (fill_nrm) fill_nrm[0] = fill_nrm[1] = fill_nrm[2] = nrm[0];
}

static void flat_input(caustics_capture_input *in, const point_t *pos,
	const vector_t *nrm, const color_t *tr, const color_t *inc, matrix_t *lm,
	int lw, int lh, float apw, float aph) {
	memset(in, 0, sizeof(*in));
	in->triangle_count = 1;
	in->positions = pos;
	in->normals = nrm;
	in->transmit = tr;
	in->light = lm;
	in->aperture_width = apw;
	in->aperture_height = aph;
	in->incident = inc;
	in->light_width = lw;
	in->light_height = lh;
	in->ior_in = 1.0f;
	in->ior_out = 1.5f;
}

static void std_receiver(caustics_receiver *rc, float z, float umin,
	float umax, float vmin, float vmax, int w, int h) {
	memset(rc, 0, sizeof(*rc));
	rc->origin = (point_t){ 0, 0, z, 1 };
	rc->axis_u = (vector_t){ 1, 0, 0, 0 };
	rc->axis_v = (vector_t){ 0, 1, 0, 0 };
	rc->range_u_min = umin; rc->range_u_max = umax;
	rc->range_v_min = vmin; rc->range_v_max = vmax;
	rc->map_width = w; rc->map_height = h;
}

//=====================================================================
// capture 组
//=====================================================================

// 平面玻璃：像素中心样本位置、深度、面 ID 与有效性
static int case_capture_flat(void) {
	point_t pos[3]; vector_t nrm[3]; color_t tr[1] = { {1,1,1} };
	color_t inc = { 1, 1, 1 };
	matrix_t lm; matrix_set_identity(&lm);
	flat_glass(pos, nrm, NULL, 0.0f);
	caustics_capture_input in;
	flat_input(&in, pos, nrm, tr, &inc, &lm, 4, 4, 2.0f, 2.0f);
	caustics_receiver rc;
	std_receiver(&rc, 2.0f, -1, 1, -1, 1, 8, 8);
	caustics_map *m = caustics_map_create(&rc);
	TASK_CHECK(m != NULL);
	TASK_CHECK(caustics_capture(m, &in) == 0);
	int valid = 0;
	for (int y = 0; y < 4; y++) {
		for (int x = 0; x < 4; x++) {
			caustics_sample s = caustics_get_sample(m, x, y);
			float wx = ((x + 0.5f) / 4.0f) * 2.0f - 1.0f;
			float wy = 1.0f - ((y + 0.5f) / 4.0f) * 2.0f;
			TASK_CHECK(s.valid);
			TASK_CHECK(s.face_id == 0);
			TASK_CHECK(nearf(s.depth, 0.0f));
			TASK_CHECK(nearf(s.position.x, wx) && nearf(s.position.y, wy));
			TASK_CHECK(nearf(s.position.z, 0.0f));
			TASK_CHECK(nearf(s.normal.z, -1.0f));
			TASK_CHECK(nearf(s.hit.x, wx) && nearf(s.hit.y, wy) &&
				nearf(s.hit.z, 2.0f));
			valid++;
		}
	}
	TASK_CHECK(valid == 16);
	// 越界查询返回无效样本
	caustics_sample oob = caustics_get_sample(m, 99, 99);
	TASK_CHECK(!oob.valid && oob.face_id == -1);
	caustics_map_release(m);
	return 0;
}

// 斜面：每个有效样本的落点都必须符合独立 Snell 推导
static int case_capture_snell(void) {
	const float theta = 0.3f, sn = sinf(theta), cs = cosf(theta);
	vector_t N = { -sn, 0, -cs, 0 };
	vector_t e1 = { cs, 0, -sn, 0 }, e2 = { 0, 1, 0, 0 };
	point_t ctr = { 0, 0, 0.5f, 1 };
	const float a = 0.85f;
	point_t pos[3] = {
		{ ctr.x-a*e1.x-a*e2.x, ctr.y-a*e1.y-a*e2.y, ctr.z-a*e1.z-a*e2.z, 1 },
		{ ctr.x+a*e1.x-a*e2.x, ctr.y+a*e1.y-a*e2.y, ctr.z+a*e1.z-a*e2.z, 1 },
		{ ctr.x       +a*e2.x, ctr.y       +a*e2.y, ctr.z       +a*e2.z, 1 }
	};
	vector_t nrm[3] = { N, N, N };
	color_t tr[1] = { {1,1,1} }, inc = { 1,1,1 };
	matrix_t lm; matrix_set_identity(&lm);
	caustics_capture_input in;
	flat_input(&in, pos, nrm, tr, &inc, &lm, 4, 4, 2.0f, 2.0f);
	caustics_receiver rc;
	std_receiver(&rc, 2.0f, -2, 2, -2, 2, 16, 16);
	caustics_map *m = caustics_map_create(&rc);
	TASK_CHECK(caustics_capture(m, &in) == 0);
	int nvalid = 0;
	for (int y = 0; y < 4; y++) {
		for (int x = 0; x < 4; x++) {
			caustics_sample s = caustics_get_sample(m, x, y);
			if (!s.valid) continue;
			nvalid++;
			float L[3] = { 0, 0, 1 };
			float Nk[3] = { s.normal.x, s.normal.y, s.normal.z };
			float cosi = -(Nk[0]*L[0] + Nk[1]*L[1] + Nk[2]*L[2]);
			TASK_CHECK(cosi > 0.0f);
			float eta = 1.0f / 1.5f;
			float sin2 = eta * eta * (1.0f - cosi * cosi);
			TASK_CHECK(sin2 <= 1.0f);
			float cost = sqrtf(1.0f - sin2), T[3];
			for (int k = 0; k < 3; k++)
				T[k] = eta * L[k] + (eta * cosi - cost) * Nk[k];
			float tl = sqrtf(T[0]*T[0] + T[1]*T[1] + T[2]*T[2]);
			for (int k = 0; k < 3; k++) T[k] /= tl;
			float P[3] = { s.position.x, s.position.y, s.position.z };
			float sd = (2.0f - P[2]) / T[2];
			TASK_CHECK(sd >= 0.0f);
			TASK_CHECK(nearf(s.hit.x, P[0] + sd * T[0]));
			TASK_CHECK(nearf(s.hit.y, P[1] + sd * T[1]));
			TASK_CHECK(nearf(s.hit.z, 2.0f));
			// 样本位置必须落在斜面方程上：N.(P - ctr) = 0
			float plane = N.x*(P[0]-ctr.x) + N.y*(P[1]-ctr.y) +
				N.z*(P[2]-ctr.z);
			TASK_CHECK(nearf_tol(plane, 0.0f, 1.0e-5f));
		}
	}
	TASK_CHECK(nvalid >= 3);
	caustics_map_release(m);
	return 0;
}

// 齐次边界裁剪 + 非法线归一化：跨 NDC 边界的三角形仍连续可取
static int case_capture_clip_and_normalize(void) {
	point_t pos[3] = { {-4,-4,0,1},{4,-4,0,1},{0,4,0,1} };
	// 逐顶点法线故意给非单位长度，捕获后必须归一化
	vector_t nrm[3] = { {0,0,-2,0},{0,0,-7.5f,0},{0,0,-3.0f,0} };
	color_t tr[1] = { {1,1,1} }, inc = { 1,1,1 };
	matrix_t lm; matrix_set_identity(&lm);
	caustics_capture_input in;
	flat_input(&in, pos, nrm, tr, &inc, &lm, 4, 4, 2.0f, 2.0f);
	caustics_receiver rc;
	std_receiver(&rc, 2.0f, -1, 1, -1, 1, 8, 8);
	caustics_map *m = caustics_map_create(&rc);
	TASK_CHECK(caustics_capture(m, &in) == 0);
	int valid = 0;
	for (int y = 0; y < 4; y++) {
		for (int x = 0; x < 4; x++) {
			caustics_sample s = caustics_get_sample(m, x, y);
			TASK_CHECK(s.valid);
			float len = sqrtf(s.normal.x*s.normal.x + s.normal.y*s.normal.y +
				s.normal.z*s.normal.z);
			TASK_CHECK(nearf(len, 1.0f));
			TASK_CHECK(nearf(s.normal.z, -1.0f));
			TASK_CHECK(nearf(s.position.z, 0.0f));
			valid++;
		}
	}
	TASK_CHECK(valid == 16);
	caustics_map_release(m);
	return 0;
}

// 全反射无效化；共面平深取面 ID 小者；真深度差时最近者胜
static int case_capture_tir_and_tie(void) {
	point_t pos[3]; vector_t nrm[3]; color_t tr[2] = { {1,1,1},{1,1,1} };
	color_t inc = { 1,1,1 };
	matrix_t lm; matrix_set_identity(&lm);
	flat_glass(pos, nrm, NULL, 0.0f);

	// 全反射：玻璃 -> 空气，法线几乎横置
	caustics_capture_input tin;
	flat_input(&tin, pos, nrm, tr, &inc, &lm, 4, 4, 2.0f, 2.0f);
	vector_t side[3] = { {-1,0,0,0},{-1,0,0,0},{-1,0,0,0} };
	tin.normals = side;
	tin.ior_in = 1.5f; tin.ior_out = 1.0f;
	caustics_receiver rc;
	std_receiver(&rc, 2.0f, -1, 1, -1, 1, 8, 8);
	caustics_map *mt = caustics_map_create(&rc);
	TASK_CHECK(caustics_capture(mt, &tin) == 0);
	for (int i = 0; i < 16; i++)
		TASK_CHECK(!caustics_get_sample(mt, i % 4, i / 4).valid);
	caustics_map_release(mt);

	// 平深：两个重合共面三角形，面 0 必须胜出
	point_t p2[6] = {
		{ -8,-8,0,1 },{ 8,-8,0,1 },{ 0,8,0,1 },
		{ -8,-8,0,1 },{ 8,-8,0,1 },{ 0,8,0,1 }
	};
	vector_t n2[6] = {
		{0,0,-1,0},{0,0,-1,0},{0,0,-1,0},
		{0,0,-1,0},{0,0,-1,0},{0,0,-1,0}
	};
	caustics_capture_input in2;
	memset(&in2, 0, sizeof(in2));
	in2.triangle_count = 2; in2.positions = p2; in2.normals = n2;
	in2.transmit = tr; in2.light = &lm;
	in2.aperture_width = 2.0f; in2.aperture_height = 2.0f;
	in2.incident = &inc; in2.light_width = 4; in2.light_height = 4;
	in2.ior_in = 1.0f; in2.ior_out = 1.5f;
	caustics_map *mz = caustics_map_create(&rc);
	TASK_CHECK(caustics_capture(mz, &in2) == 0);
	for (int i = 0; i < 16; i++) {
		caustics_sample s = caustics_get_sample(mz, i % 4, i / 4);
		TASK_CHECK(s.valid && s.face_id == 0);
	}
	caustics_map_release(mz);

	// 真深度差：面 1 更靠近光（z 更小），两面都在光裁剪体内，面 1 胜出
	point_t p3[6] = {
		{ -8,-8,0.5f,1 },{ 8,-8,0.5f,1 },{ 0,8,0.5f,1 },
		{ -8,-8,0.25f,1 },{ 8,-8,0.25f,1 },{ 0,8,0.25f,1 }
	};
	caustics_capture_input in3 = in2;
	in3.positions = p3;
	caustics_map *md = caustics_map_create(&rc);
	TASK_CHECK(caustics_capture(md, &in3) == 0);
	for (int i = 0; i < 16; i++) {
		caustics_sample s = caustics_get_sample(md, i % 4, i / 4);
		TASK_CHECK(s.valid && s.face_id == 1);
		TASK_CHECK(nearf(s.position.z, 0.25f));
	}
	caustics_map_release(md);
	return 0;
}

//=====================================================================
// flux 组
//=====================================================================

// 平面守恒：18 束 * 半网格面积 0.125 = 2.25
static int case_flux_conservation(void) {
	point_t pos[3]; vector_t nrm[3]; color_t tr[1] = { {1,1,1} };
	color_t inc = { 1,1,1 };
	matrix_t lm; matrix_set_identity(&lm);
	flat_glass(pos, nrm, NULL, 0.0f);
	caustics_capture_input in;
	flat_input(&in, pos, nrm, tr, &inc, &lm, 4, 4, 2.0f, 2.0f);
	caustics_receiver rc;
	std_receiver(&rc, 2.0f, -1, 1, -1, 1, 10, 10);
	caustics_map *m = caustics_map_create(&rc);
	TASK_CHECK(caustics_capture(m, &in) == 0);
	float total[3] = { 0, 0, 0 };
	const float texel = 0.2f * 0.2f;
	for (int y = 0; y < 10; y++) for (int x = 0; x < 10; x++) {
		float r, g, b;
		caustics_get_irradiance(m, x, y, &r, &g, &b);
		total[0] += r * texel; total[1] += g * texel; total[2] += b * texel;
	}
	// 18 束，每束通量 1 * (0.5*0.5) * 0.5 = 0.125
	TASK_CHECK(nearf_tol(total[0], 2.25f, 2.0e-5f));
	TASK_CHECK(nearf(total[0], total[1]) && nearf(total[1], total[2]));
	caustics_map_release(m);
	return 0;
}

// 折叠相加：2x2 粗接收图，每纹素按四向对称收 2.25/4 的通量
static int case_flux_fold_add(void) {
	point_t pos[3]; vector_t nrm[3]; color_t tr[1] = { {1,1,1} };
	color_t inc = { 1,1,1 };
	matrix_t lm; matrix_set_identity(&lm);
	flat_glass(pos, nrm, NULL, 0.0f);
	caustics_capture_input in;
	flat_input(&in, pos, nrm, tr, &inc, &lm, 4, 4, 2.0f, 2.0f);
	caustics_receiver rc;
	std_receiver(&rc, 2.0f, -1, 1, -1, 1, 2, 2);
	caustics_map *m = caustics_map_create(&rc);
	TASK_CHECK(caustics_capture(m, &in) == 0);
	float sum = 0.0f;
	for (int y = 0; y < 2; y++) for (int x = 0; x < 2; x++) {
		float r, g, b;
		caustics_get_irradiance(m, x, y, &r, &g, &b);
		// 纹素面积 1，通量 2.25/4 = 0.5625，必为多个足迹累加的结果
		TASK_CHECK(nearf_tol(r, 0.5625f, 2.0e-5f));
		TASK_CHECK(nearf(r, g) && nearf(g, b));
		sum += r;
	}
	TASK_CHECK(nearf_tol(sum, 2.25f, 4.0e-5f));
	caustics_map_release(m);
	return 0;
}

// 缩小/扩张足迹：单个共面三角形 + 逐顶点变化法线，折射后只改变落点。
// 同一束在两种法向梯度下分别得到缩小或扩张足迹；内部纹素辐照度 = 通量/面积，
// 18 束总通量严格守恒。
static int case_flux_shrink_expand(void) {
	const float beam_flux = 0.125f;   // cell=0.5x0.5，半网格面积 0.125，I=T=1
	for (int pass = 0; pass < 2; pass++) {
		float k = pass == 0 ? 0.25f : -0.25f;
		point_t pos[3] = {
			{ -8,-8,0.5f,1 },{ 8,-8,0.5f,1 },{ 0,8,0.5f,1 }
		};
		vector_t nrm[3] = {
			{ -k,0,-1,0 },{ k,0,-1,0 },{ 0,k,-1,0 }
		};
		for (int i = 0; i < 3; i++) {
			float l = sqrtf(nrm[i].x*nrm[i].x + nrm[i].z*nrm[i].z);
			nrm[i].x /= l; nrm[i].z /= l;
		}
		color_t tr[1] = { {1,1,1} }, inc = { 1,1,1 };
		matrix_t lm; matrix_set_identity(&lm);
		caustics_capture_input in;
		flat_input(&in, pos, nrm, tr, &inc, &lm, 4, 4, 2.0f, 2.0f);
		caustics_receiver rc;
		std_receiver(&rc, 3.0f, -3, 3, -3, 3, 120, 120);
		caustics_map *m = caustics_map_create(&rc);
		TASK_CHECK(caustics_capture(m, &in) == 0);
		// 单个大面覆盖全部 16 个样本，全部有效
		for (int i = 0; i < 16; i++)
			TASK_CHECK(caustics_get_sample(m, i % 4, i / 4).valid);
		// 用公开落点独立计算束 (A,B,C) 的足迹面积
		caustics_sample A = caustics_get_sample(m, 0, 0);
		caustics_sample B = caustics_get_sample(m, 1, 0);
		caustics_sample C = caustics_get_sample(m, 1, 1);
		float foot[3][2] = {
			{ A.hit.x, A.hit.y },{ B.hit.x, B.hit.y },{ C.hit.x, C.hit.y }
		};
		float area = 0.5f * fabsf(
			(foot[1][0]-foot[0][0])*(foot[2][1]-foot[0][1]) -
			(foot[2][0]-foot[0][0])*(foot[1][1]-foot[0][1]));
		TASK_CHECK(area > 0.0f);
		if (pass == 0) TASK_CHECK(area < beam_flux);   // 缩小
		else TASK_CHECK(area > beam_flux);             // 扩张
		// 重心纹素被该束完整覆盖：辐照度恰为 通量/面积
		float gx = (foot[0][0]+foot[1][0]+foot[2][0]) / 3.0f;
		float gy = (foot[0][1]+foot[1][1]+foot[2][1]) / 3.0f;
		const float dv = 6.0f / 120.0f;
		int ix = (int)floorf((gx + 3.0f) / dv);
		int iy = (int)floorf((gy + 3.0f) / dv);
		float r, g, b;
		caustics_get_irradiance(m, ix, iy, &r, &g, &b);
		TASK_CHECK(nearf_tol(r, beam_flux / area, 2.0e-4f));
		TASK_CHECK(nearf(r, g) && nearf(g, b));
		// 18 束总通量守恒
		float total = 0.0f;
		for (int y = 0; y < 120; y++) for (int x = 0; x < 120; x++) {
			caustics_get_irradiance(m, x, y, &r, &g, &b);
			total += r * dv * dv;
		}
		TASK_CHECK(nearf_tol(total, 18.0f * beam_flux, 2.0e-4f));
		caustics_map_release(m);
	}
	return 0;
}

// 图边损失：出图部分丢失且不重新归一，共享纹素与全图一致
static int case_flux_edge_loss(void) {
	point_t pos[3]; vector_t nrm[3]; color_t tr[1] = { {1,1,1} };
	color_t inc = { 1,1,1 };
	matrix_t lm; matrix_set_identity(&lm);
	flat_glass(pos, nrm, NULL, 0.0f);
	caustics_capture_input in;
	flat_input(&in, pos, nrm, tr, &inc, &lm, 4, 4, 2.0f, 2.0f);
	caustics_receiver full_rc, crop_rc;
	std_receiver(&full_rc, 2.0f, -1, 1, -1, 1, 10, 10);
	std_receiver(&crop_rc, 2.0f,  0, 1,  0, 1,  5,  5);
	caustics_map *mf = caustics_map_create(&full_rc);
	caustics_map *mc = caustics_map_create(&crop_rc);
	TASK_CHECK(caustics_capture(mf, &in) == 0);
	TASK_CHECK(caustics_capture(mc, &in) == 0);
	float kept = 0.0f;
	for (int y = 0; y < 5; y++) for (int x = 0; x < 5; x++) {
		float rf, gf, bf, rc2, gc, bc;
		caustics_get_irradiance(mf, x + 5, y + 5, &rf, &gf, &bf);
		caustics_get_irradiance(mc, x, y, &rc2, &gc, &bc);
		TASK_CHECK(nearf(rf, rc2) && nearf(gf, gc) && nearf(bf, bc));
		kept += rc2 * 0.04f;
	}
	TASK_CHECK(kept > 0.0f && kept < 2.25f);
	// 双线性图外为 0
	float r, g, b;
	caustics_sample_irradiance(mc, 5.0f, 5.0f, &r, &g, &b);
	TASK_CHECK(nearf(r, 0.0f) && nearf(g, 0.0f) && nearf(b, 0.0f));
	caustics_map_release(mf);
	caustics_map_release(mc);
	return 0;
}

// 无效样本断束：相邻不同面的束全部丢弃，同面束保留
static int case_flux_invalid_break(void) {
	// 左右两块玻璃，x<0 属面 0，x>=0 属面 1
	point_t pos[6] = {
		{ -8,-8,0,1 },{ 0,-8,0,1 },{ 0,8,0,1 },
		{ 0,-8,0,1 },{ 8,-8,0,1 },{ 0,8,0,1 }
	};
	vector_t nrm[6];
	for (int i = 0; i < 6; i++) nrm[i] = (vector_t){ 0,0,-1,0 };
	color_t tr[2] = { {1,1,1},{1,1,1} }, inc = { 1,1,1 };
	matrix_t lm; matrix_set_identity(&lm);
	caustics_capture_input in;
	memset(&in, 0, sizeof(in));
	in.triangle_count = 2; in.positions = pos; in.normals = nrm;
	in.transmit = tr; in.light = &lm;
	in.aperture_width = 2.0f; in.aperture_height = 2.0f;
	in.incident = &inc; in.light_width = 4; in.light_height = 4;
	in.ior_in = 1.0f; in.ior_out = 1.5f;
	caustics_receiver rc;
	std_receiver(&rc, 2.0f, -1, 1, -1, 1, 10, 10);
	caustics_map *m = caustics_map_create(&rc);
	TASK_CHECK(caustics_capture(m, &in) == 0);
	// 3 行跨界四边形各丢 2 束 = 丢 6 束；保留 12 束 * 0.125 = 1.5
	float total = 0.0f;
	const float texel = 0.04f;
	for (int y = 0; y < 10; y++) for (int x = 0; x < 10; x++) {
		float r, g, b;
		caustics_get_irradiance(m, x, y, &r, &g, &b);
		total += r * texel;
	}
	TASK_CHECK(nearf_tol(total, 1.5f, 2.0e-5f));
	// 跨界样本本身仍有效，只是不能与对侧成束
	TASK_CHECK(caustics_get_sample(m, 1, 0).valid);
	TASK_CHECK(caustics_get_sample(m, 2, 0).valid);
	caustics_map_release(m);
	return 0;
}

// RGB 透过率逐通道缩放束通量
static int case_flux_transmission(void) {
	point_t pos[3]; vector_t nrm[3];
	color_t tr[1] = { {0.5f, 0.25f, 0.0f} };
	color_t inc = { 1,1,1 };
	matrix_t lm; matrix_set_identity(&lm);
	flat_glass(pos, nrm, NULL, 0.0f);
	caustics_capture_input in;
	flat_input(&in, pos, nrm, tr, &inc, &lm, 2, 2, 2.0f, 2.0f);
	caustics_receiver rc;
	std_receiver(&rc, 2.0f, -1, 1, -1, 1, 20, 20);
	caustics_map *m = caustics_map_create(&rc);
	TASK_CHECK(caustics_capture(m, &in) == 0);
	// 束 1 独享区域内的纹素：通量 0.5 / 足迹面积 0.5 = 1，再乘透过率
	float r, g, b;
	caustics_get_irradiance(m, 13, 6, &r, &g, &b);    // 中心 (0.35,-0.35)
	TASK_CHECK(nearf_tol(r, 0.5f, 2.0e-5f));
	TASK_CHECK(nearf_tol(g, 0.25f, 2.0e-5f));
	TASK_CHECK(nearf(b, 0.0f));
	caustics_map_release(m);
	return 0;
}

//=====================================================================
// camera 组
//=====================================================================
static caustics_map *build_camera_map(caustics_receiver *rc) {
	point_t pos[3]; vector_t nrm[3]; color_t tr[1] = { {1,1,1} };
	color_t inc = { 1,1,1 };
	matrix_t lm; matrix_set_identity(&lm);
	flat_glass(pos, nrm, NULL, 0.0f);
	caustics_capture_input in;
	flat_input(&in, pos, nrm, tr, &inc, &lm, 8, 8, 2.0f, 2.0f);
	std_receiver(rc, 2.0f, -1, 1, -1, 1, 5, 5);
	caustics_map *m = caustics_map_create(rc);
	if (m == NULL) return NULL;
	if (caustics_capture(m, &in) != 0) {
		caustics_map_release(m);
		return NULL;
	}
	return m;
}

// 相机片元世界位置对应接收面 (0,0)，取该点双线性辐照度作为期望
static float center_irradiance(const caustics_map *m) {
	float r, g, b;
	caustics_sample_irradiance(m, 0.0f, 0.0f, &r, &g, &b);
	return r;
}

// 透视 w 不等的接收三角形：世界位置经透视插值，容差内上焦散
static int case_camera_perspective(void) {
	caustics_receiver rc;
	caustics_map *m = build_camera_map(&rc);
	TASK_CHECK(m != NULL);
	float ir = center_irradiance(m);
	TASK_CHECK(ir > 0.0f);

	device_t d;
	static IUINT32 fb[32 * 32];
	device_init(&d, 32, 32, fb);
	d.background = 0;
	device_clear(&d, 0);
	matrix_set_perspective(&d.transform.projection, 3.1415926f * 0.5f,
		1.0f, 1.0f, 500.0f);
	matrix_set_identity(&d.transform.world);
	matrix_set_identity(&d.transform.view);
	transform_update(&d.transform);
	d.render_state = RENDER_STATE_COLOR;

	// 世界点在 z=2 平面，齐次权重故意取 1/2/3 -> 裁剪 w 不等
	float ws[3] = { 1, 2, 3 };
	point_t world[3] = {
		{ -0.75f, -0.75f, 2, 1 },
		{  0.75f, -0.75f, 2, 1 },
		{  0.0f,   0.75f, 2, 1 }
	};
	vertex_t v[3];
	memset(v, 0, sizeof(v));
	for (int i = 0; i < 3; i++) {
		v[i].pos = (point_t){ world[i].x * ws[i], world[i].y * ws[i],
			world[i].z * ws[i], ws[i] };
		v[i].world = world[i];
		v[i].color = (color_t){ 1, 1, 1 };
	}
	TASK_CHECK(caustics_bind_device(m, &d, 0.02f, 0.2f, 0.1f) == 0);
	device_draw_primitive(&d, &v[0], &v[1], &v[2]);
	IUINT32 lit = d.framebuffer[16][16];
	float factor = 0.2f + 0.1f * ir;
	int expect = (int)fminf(255.0f, factor * 255.0f);
	TASK_CHECK((int)((lit >> 16) & 0xff) == expect);
	TASK_CHECK((int)((lit >> 8) & 0xff) == expect);

	// 容差为 0 时平面片元仍然点亮（透视插值误差远小于几何偏差）
	caustics_unbind_device(&d);
	TASK_CHECK(caustics_bind_device(m, &d, 0.0f, 0.2f, 0.1f) == 0);
	device_clear(&d, 0);
	device_draw_primitive(&d, &v[0], &v[1], &v[2]);
	TASK_CHECK(d.framebuffer[16][16] == lit);

	// 平面外的三角形（z=3）在同一容差下不上焦散，保持底色
	vertex_t w[3];
	memset(w, 0, sizeof(w));
	for (int i = 0; i < 3; i++) {
		w[i] = v[i];
		w[i].pos = (point_t){ world[i].x, world[i].y, 3.0f, 1 };
		w[i].world = (point_t){ world[i].x, world[i].y, 3.0f, 1 };
	}
	caustics_unbind_device(&d);
	TASK_CHECK(caustics_bind_device(m, &d, 0.02f, 0.2f, 0.1f) == 0);
	device_clear(&d, 0);
	device_draw_primitive(&d, &w[0], &w[1], &w[2]);
	TASK_CHECK(d.framebuffer[16][16] == 0xffffff);

	device_destroy(&d);
	caustics_map_release(m);
	return 0;
}

// 纹理优先级 + 遮挡物 rhw 测试
static int case_camera_texture_occluder(void) {
	caustics_receiver rc;
	caustics_map *m = build_camera_map(&rc);
	TASK_CHECK(m != NULL);
	float ir = center_irradiance(m);

	device_t d;
	static IUINT32 fb[32 * 32];
	device_init(&d, 32, 32, fb);
	d.background = 0;
	device_clear(&d, 0);
	matrix_set_perspective(&d.transform.projection, 3.1415926f * 0.5f,
		1.0f, 1.0f, 500.0f);
	matrix_set_identity(&d.transform.world);
	matrix_set_identity(&d.transform.view);
	transform_update(&d.transform);
	TASK_CHECK(caustics_bind_device(m, &d, 0.05f, 0.2f, 0.1f) == 0);

	vertex_t plane[3], occ[3];
	memset(plane, 0, sizeof(plane));
	memset(occ, 0, sizeof(occ));
	point_t pw[3] = {
		{ -0.75f,-0.75f,2,1 },{ 0.75f,-0.75f,2,1 },{ 0,0.75f,2,1 }
	};
	for (int i = 0; i < 3; i++) {
		plane[i].pos = pw[i]; plane[i].world = pw[i];
		plane[i].color = (color_t){ 1,1,1 };
		plane[i].tc = (texcoord_t){ 0, 0 };
		occ[i] = plane[i];
		occ[i].pos = (point_t){ pw[i].x, pw[i].y, 1.5f, 1 };
		occ[i].world = occ[i].pos;
	}

	// 纹理 + 顶点色：纹理覆盖颜色，平面内仍乘焦散照明，一次量化
	static IUINT32 tex[4] = { 0x808080,0x808080,0x808080,0x808080 };
	device_set_texture(&d, tex, 8, 2, 2);
	d.render_state = RENDER_STATE_COLOR | RENDER_STATE_TEXTURE;
	device_draw_primitive(&d, &plane[0], &plane[1], &plane[2]);
	IUINT32 tp = d.framebuffer[16][16];
	float base = 0x80 / 255.0f;
	int expect = (int)fminf(255.0f, base * (0.2f + 0.1f * ir) * 255.0f);
	TASK_CHECK((int)((tp >> 16) & 0xff) == expect);

	// 遮挡物更近，赢得 rhw；其世界点离平面 0.5 > 容差，保持纹理原色
	d.render_state = RENDER_STATE_COLOR | RENDER_STATE_TEXTURE;
	device_draw_primitive(&d, &occ[0], &occ[1], &occ[2]);
	TASK_CHECK(d.framebuffer[16][16] == 0x808080);

	// 反序绘制：先遮挡物后平面，平面 rhw 更小不能改写
	device_clear(&d, 0);
	device_draw_primitive(&d, &occ[0], &occ[1], &occ[2]);
	device_draw_primitive(&d, &plane[0], &plane[1], &plane[2]);
	TASK_CHECK(d.framebuffer[16][16] == 0x808080);

	// 解绑后恢复原始纹理输出
	caustics_unbind_device(&d);
	device_clear(&d, 0);
	device_draw_primitive(&d, &plane[0], &plane[1], &plane[2]);
	TASK_CHECK(d.framebuffer[16][16] == 0x808080);

	device_destroy(&d);
	caustics_map_release(m);
	return 0;
}

// 两台设备与重复绘制互不污染，图不被绘制改写
static int case_camera_isolation(void) {
	caustics_receiver rc;
	caustics_map *m = build_camera_map(&rc);
	TASK_CHECK(m != NULL);
	float before = center_irradiance(m);

	device_t d1, d2;
	static IUINT32 fb1[32 * 32], fb2[32 * 32];
	device_init(&d1, 32, 32, fb1);
	device_init(&d2, 32, 32, fb2);
	d1.background = 0; d2.background = 0;
	device_clear(&d1, 0); device_clear(&d2, 0);
	matrix_set_perspective(&d1.transform.projection, 3.1415926f * 0.5f,
		1.0f, 1.0f, 500.0f);
	matrix_set_identity(&d1.transform.world);
	matrix_set_identity(&d1.transform.view);
	transform_update(&d1.transform);
	d2.transform = d1.transform;
	d1.render_state = RENDER_STATE_COLOR;
	d2.render_state = RENDER_STATE_COLOR;

	vertex_t v[3];
	memset(v, 0, sizeof(v));
	point_t pw[3] = {
		{ -0.75f,-0.75f,2,1 },{ 0.75f,-0.75f,2,1 },{ 0,0.75f,2,1 }
	};
	for (int i = 0; i < 3; i++) {
		v[i].pos = pw[i]; v[i].world = pw[i];
		v[i].color = (color_t){ 1,1,1 };
	}
	TASK_CHECK(caustics_bind_device(m, &d1, 0.05f, 0.2f, 0.1f) == 0);
	TASK_CHECK(caustics_bind_device(m, &d2, 0.05f, 0.2f, 0.1f) == 0);
	device_draw_primitive(&d1, &v[0], &v[1], &v[2]);
	device_draw_primitive(&d2, &v[0], &v[1], &v[2]);
	device_draw_primitive(&d1, &v[0], &v[1], &v[2]);
	TASK_CHECK(d1.framebuffer[16][16] != 0);
	TASK_CHECK(d2.framebuffer[16][16] == d1.framebuffer[16][16]);

	float after = center_irradiance(m);
	TASK_CHECK(nearf(before, after));

	// 未绑定设备保持原输出
	device_t d3;
	static IUINT32 fb3[32 * 32];
	device_init(&d3, 32, 32, fb3);
	d3.background = 0; device_clear(&d3, 0);
	d3.transform = d1.transform;
	d3.render_state = RENDER_STATE_COLOR;
	device_draw_primitive(&d3, &v[0], &v[1], &v[2]);
	TASK_CHECK(d3.framebuffer[16][16] == 0xffffff);

	device_destroy(&d1); device_destroy(&d2); device_destroy(&d3);
	caustics_map_release(m);
	return 0;
}

//=====================================================================
// 注册
//=====================================================================
const struct task_case task_cases[] = {
	{ "capture", "flat",          case_capture_flat },
	{ "capture", "snell",         case_capture_snell },
	{ "capture", "clip_normalize", case_capture_clip_and_normalize },
	{ "capture", "tir_and_tie",   case_capture_tir_and_tie },
	{ "flux",    "conservation",  case_flux_conservation },
	{ "flux",    "fold_add",      case_flux_fold_add },
	{ "flux",    "shrink_expand", case_flux_shrink_expand },
	{ "flux",    "edge_loss",     case_flux_edge_loss },
	{ "flux",    "invalid_break", case_flux_invalid_break },
	{ "flux",    "transmission",  case_flux_transmission },
	{ "camera",  "perspective",   case_camera_perspective },
	{ "camera",  "texture_occluder", case_camera_texture_occluder },
	{ "camera",  "isolation",     case_camera_isolation },
};
const size_t task_case_count =
	sizeof(task_cases) / sizeof(task_cases[0]);
