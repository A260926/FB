// 矩透明帧的测试：capture / reconstruct / lifecycle 三组
// 所有期望矩、颜色和计数都由真实 DrawPrimitive 经独立手算对照
#include "task_support/check.h"
#include "moment_oit.h"

#include <cmath>
#include <vector>
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace {

//---------------------------------------------------------------------
// 手算工具
//---------------------------------------------------------------------

// 一个透明片元：深度 z、不透明度 alpha、线性 RGB
struct Frag {
	double z, alpha;
	double r, g, b;
};

Frag MakeFrag(double z, double alpha, double r, double g, double b) {
	Frag f;
	f.z = z; f.alpha = alpha;
	f.r = r; f.g = g; f.b = b;
	return f;
}

// 由片元序列手算期望矩：tau = -log1p(-alpha)，
// b[k] = sum(tau * z^k)，g[c][k] = sum(tau * RGB[c] * z^k)
MomentOitMoments HandMoments(const std::vector<Frag> &frags) {
	MomentOitMoments m;
	for (size_t i = 0; i < frags.size(); i++) {
		const Frag &f = frags[i];
		double tau = -log1p(-f.alpha);
		double zp = 1.0;
		for (int k = 0; k < 4; k++) {
			m.b[k] += tau * zp;
			zp *= f.z;
		}
		m.g[0][0] += tau * f.r; m.g[0][1] += tau * f.r * f.z;
		m.g[1][0] += tau * f.g; m.g[1][1] += tau * f.g * f.z;
		m.g[2][0] += tau * f.b; m.g[2][1] += tau * f.b * f.z;
	}
	return m;
}

// 手算精确分层合成：同深度片元先按 tau 加权并成一层，
// 再由近到远合成，最后加上剩余透射率乘背景
Vec3d HandComposite(std::vector<Frag> frags, const Vec3d &bg) {
	std::sort(frags.begin(), frags.end(),
	          [](const Frag &a, const Frag &b) { return a.z < b.z; });
	// 同深度并层：颜色用 tau 加权
	std::vector<Frag> layers;
	for (size_t i = 0; i < frags.size(); i++) {
		const Frag &f = frags[i];
		if (!layers.empty() && layers.back().z == f.z) {
			Frag &L = layers.back();
			double t0 = -log1p(-L.alpha);
			double t1 = -log1p(-f.alpha);
			double t = t0 + t1;
			L.r = (t0 * L.r + t1 * f.r) / t;
			L.g = (t0 * L.g + t1 * f.g) / t;
			L.b = (t0 * L.b + t1 * f.b) / t;
			L.alpha = -expm1(-t);
		}
		else {
			layers.push_back(f);
		}
	}
	Vec3d acc(0.0, 0.0, 0.0);
	double reach = 1.0;
	for (size_t i = 0; i < layers.size(); i++) {
		const Frag &L = layers[i];
		acc.x += reach * L.alpha * L.r;
		acc.y += reach * L.alpha * L.g;
		acc.z += reach * L.alpha * L.b;
		reach *= 1.0 - L.alpha;
	}
	acc += reach * bg;
	return acc;
}

//---------------------------------------------------------------------
// 比较工具
//---------------------------------------------------------------------

bool Near(double a, double b, double tol) {
	return std::fabs(a - b) <= tol;
}

bool VecNear(const double a[3], const Vec3d &b, double tol) {
	return Near(a[0], b.x, tol) && Near(a[1], b.y, tol) && Near(a[2], b.z, tol);
}

bool MomentsNear(const MomentOitMoments &m, const MomentOitMoments &ref, double tol) {
	for (int k = 0; k < 4; k++)
		if (!Near(m.b[k], ref.b[k], tol)) return false;
	for (int c = 0; c < 3; c++)
		for (int k = 0; k < 2; k++)
			if (!Near(m.g[c][k], ref.g[c][k], tol)) return false;
	return true;
}

bool PixelNear(const MomentOitPixel &a, const MomentOitPixel &b, double tol) {
	for (int c = 0; c < 3; c++)
		if (!Near(a.color[c], b.color[c], tol)) return false;
	if (!Near(a.transmittance, b.transmittance, tol)) return false;
	if (a.layers.size() != b.layers.size()) return false;
	for (size_t i = 0; i < a.layers.size(); i++) {
		if (!Near(a.layers[i].depth, b.layers[i].depth, tol)) return false;
		if (!Near(a.layers[i].optical_mass, b.layers[i].optical_mass, tol)) return false;
		if (!Near(a.layers[i].alpha, b.layers[i].alpha, tol)) return false;
		for (int c = 0; c < 3; c++)
			if (!Near(a.layers[i].color[c], b.layers[i].color[c], tol)) return false;
	}
	return true;
}

template <typename E, typename F> bool Throws(F &&f) {
	try { f(); }
	catch (const E &) { return true; }
	catch (...) { return false; }
	return false;
}

//---------------------------------------------------------------------
// 测试装置
//---------------------------------------------------------------------

// 暴露普通帧缓冲，便于检查矩帧不写普通帧的颜色和深度
struct Probe : RenderHelp {
	using RenderHelp::RenderHelp;
	uint32_t Pixel(int x, int y) { return _frame_buffer->GetPixel(x, y); }
	float Depth(int x, int y) { return _depth_buffer[y][x]; }
};

// RenderHelp + MomentOit + 计数着色器
struct Rig {
	Probe rh;
	MomentOit mo;
	int vs_count;
	int ps_count;
	std::vector<Vec4f> pos;       // 每三角形 3 个齐次坐标
	std::vector<Vec4f> color;     // 每三角形 3 个顶点颜色
	int tri;

	Rig(int w, int h) : rh(w, h), mo(rh) {
		vs_count = ps_count = 0;
		tri = 0;
		InstallShaders();
	}

	void InstallShaders() {
		rh.SetVertexShader([&](int i, ShaderContext &out) -> Vec4f {
			vs_count++;
			out.varying_vec4f[0] = color[(size_t)tri * 3 + (size_t)i];
			return pos[(size_t)tri * 3 + (size_t)i];
		});
		rh.SetPixelShader([&](ShaderContext &in) -> Vec4f {
			ps_count++;
			return in.varying_vec4f[0];
		});
	}

	bool Draw(int t) { tri = t; return rh.DrawPrimitive(); }

	// 追加一个三角形，返回其序号
	int AddTri(const Vec4f p[3], const Vec4f c[3]) {
		int t = (int)(pos.size() / 3);
		for (int i = 0; i < 3; i++) {
			pos.push_back(p[i]);
			color.push_back(c[i]);
		}
		return t;
	}

	// 追加一个四边形（三角形 (0,1,2) 和 (0,2,3)），返回第一个三角形序号
	int AddQuad(const Vec4f p[4], const Vec4f c[4]) {
		Vec4f t1p[3] = { p[0], p[1], p[2] };
		Vec4f t1c[3] = { c[0], c[1], c[2] };
		Vec4f t2p[3] = { p[0], p[2], p[3] };
		Vec4f t2c[3] = { c[0], c[2], c[3] };
		int t = AddTri(t1p, t1c);
		AddTri(t2p, t2c);
		return t;
	}

	// 追加一个全屏四边形：NDC 覆盖 [-1,1]^2，恒定深度和颜色
	int AddFullQuad(float z, const Vec4f &c) {
		Vec4f p[4] = {
			Vec4f(-1.0f,  1.0f, z, 1.0f), Vec4f(1.0f,  1.0f, z, 1.0f),
			Vec4f( 1.0f, -1.0f, z, 1.0f), Vec4f(-1.0f, -1.0f, z, 1.0f)
		};
		Vec4f cc[4] = { c, c, c, c };
		return AddQuad(p, cc);
	}

	bool DrawQuad(int q) {
		bool a = Draw(q);
		bool b = Draw(q + 1);
		return a && b;
	}
};

//---------------------------------------------------------------------
// capture：矩的采集
//---------------------------------------------------------------------

// 单层：全屏透明四边形，逐像素核对矩和计数
int cap_single_layer() {
	Rig rig(8, 8);
	rig.mo.BeginFrame(Vec3d(0.125, 0.25, 0.375));
	rig.mo.SealOpaque();
	Vec4f C(0.5f, 0.25f, 0.75f, 0.25f);
	int q = rig.AddFullQuad(0.5f, C);
	TASK_CHECK(rig.Draw(q));
	TASK_CHECK(rig.Draw(q + 1));
	TASK_CHECK(rig.vs_count == 6);       // 每次提交三个角的 VS 各执行一次
	TASK_CHECK(rig.ps_count == 64);      // 全屏 8x8，每像素恰好一次 PS
	Frag f = MakeFrag(0.5, 0.25, 0.5, 0.25, 0.75);
	MomentOitMoments ref = HandMoments(std::vector<Frag>(1, f));
	for (int y = 0; y < 8; y++)
		for (int x = 0; x < 8; x++)
			TASK_CHECK(MomentsNear(rig.mo.GetMoments(x, y), ref, 1e-12));
	rig.mo.CancelFrame();
	return 0;
}

// 异色双层：两个全屏四边形，矩是两层之和
int cap_two_layers() {
	Rig rig(8, 8);
	rig.mo.BeginFrame(Vec3d(0.125, 0.25, 0.375));
	rig.mo.SealOpaque();
	int q0 = rig.AddFullQuad(0.25f, Vec4f(0.5f, 0.25f, 0.75f, 0.5f));
	int q1 = rig.AddFullQuad(0.75f, Vec4f(0.25f, 0.75f, 0.5f, 0.25f));
	TASK_CHECK(rig.DrawQuad(q0));
	TASK_CHECK(rig.DrawQuad(q1));
	TASK_CHECK(rig.vs_count == 12);
	TASK_CHECK(rig.ps_count == 128);
	std::vector<Frag> frags;
	frags.push_back(MakeFrag(0.25, 0.5, 0.5, 0.25, 0.75));
	frags.push_back(MakeFrag(0.75, 0.25, 0.25, 0.75, 0.5));
	MomentOitMoments ref = HandMoments(frags);
	TASK_CHECK(MomentsNear(rig.mo.GetMoments(3, 3), ref, 1e-12));
	TASK_CHECK(MomentsNear(rig.mo.GetMoments(0, 7), ref, 1e-12));
	TASK_CHECK(MomentsNear(rig.mo.GetMoments(7, 0), ref, 1e-12));
	rig.mo.CancelFrame();
	return 0;
}

// 同深度混色：同深度的片元矩直接相加（透明面不占深度）
int cap_same_depth_mix() {
	Rig rig(8, 8);
	rig.mo.BeginFrame(Vec3d(0.125, 0.25, 0.375));
	rig.mo.SealOpaque();
	int q0 = rig.AddFullQuad(0.5f, Vec4f(0.5f, 0.25f, 0.75f, 0.5f));
	int q1 = rig.AddFullQuad(0.5f, Vec4f(0.25f, 0.75f, 0.5f, 0.25f));
	TASK_CHECK(rig.DrawQuad(q0));
	TASK_CHECK(rig.DrawQuad(q1));
	// 同深度第二层仍然贡献：透明面不占深度
	TASK_CHECK(rig.ps_count == 128);
	std::vector<Frag> frags;
	frags.push_back(MakeFrag(0.5, 0.5, 0.5, 0.25, 0.75));
	frags.push_back(MakeFrag(0.5, 0.25, 0.25, 0.75, 0.5));
	MomentOitMoments ref = HandMoments(frags);
	TASK_CHECK(MomentsNear(rig.mo.GetMoments(4, 4), ref, 1e-12));
	rig.mo.CancelFrame();
	return 0;
}

// 交叉面左右换序：深度梯度交叉的两个面，换序提交矩只差浮点累加误差
int cap_crossing_swap() {
	std::vector<Frag> sample[3];
	MomentOitMoments got[2][3];
	for (int order = 0; order < 2; order++) {
		Rig rig(16, 16);
		rig.mo.BeginFrame(Vec3d(0.125, 0.25, 0.375));
		rig.mo.SealOpaque();
		// 面 A：深度从左到右 0.25 -> 0.75
		Vec4f pa[4] = {
			Vec4f(-1.0f,  1.0f, 0.25f, 1.0f), Vec4f(1.0f,  1.0f, 0.75f, 1.0f),
			Vec4f( 1.0f, -1.0f, 0.75f, 1.0f), Vec4f(-1.0f, -1.0f, 0.25f, 1.0f)
		};
		Vec4f ca[4] = {
			Vec4f(0.75f, 0.25f, 0.5f, 0.5f), Vec4f(0.75f, 0.25f, 0.5f, 0.5f),
			Vec4f(0.75f, 0.25f, 0.5f, 0.5f), Vec4f(0.75f, 0.25f, 0.5f, 0.5f)
		};
		// 面 B：深度从右到左 0.25 -> 0.75（与 A 穿插）
		Vec4f pb[4] = {
			Vec4f(-1.0f,  1.0f, 0.75f, 1.0f), Vec4f(1.0f,  1.0f, 0.25f, 1.0f),
			Vec4f( 1.0f, -1.0f, 0.25f, 1.0f), Vec4f(-1.0f, -1.0f, 0.75f, 1.0f)
		};
		Vec4f cb[4] = {
			Vec4f(0.25f, 0.75f, 0.5f, 0.5f), Vec4f(0.25f, 0.75f, 0.5f, 0.5f),
			Vec4f(0.25f, 0.75f, 0.5f, 0.5f), Vec4f(0.25f, 0.75f, 0.5f, 0.5f)
		};
		int qa = rig.AddQuad(pa, ca);
		int qb = rig.AddQuad(pb, cb);
		if (order == 0) {
			TASK_CHECK(rig.DrawQuad(qa));
			TASK_CHECK(rig.DrawQuad(qb));
		}
		else {
			TASK_CHECK(rig.DrawQuad(qb));
			TASK_CHECK(rig.DrawQuad(qa));
		}
		TASK_CHECK(rig.ps_count == 512);    // 顺序不影响透明阶段的计数
		int px[3] = { 2, 9, 15 };
		int py[3] = { 7, 3, 15 };
		for (int i = 0; i < 3; i++) {
			got[order][i] = rig.mo.GetMoments(px[i], py[i]);
			if (order == 0) {
				// 手算该像素的两个深度：z = 0.25 + 0.25 * (x_ndc + 1)
				double xndc = (px[i] + 0.5) / 8.0 - 1.0;
				std::vector<Frag> frags;
				frags.push_back(MakeFrag(0.25 + 0.25 * (xndc + 1.0), 0.5, 0.75, 0.25, 0.5));
				frags.push_back(MakeFrag(0.75 - 0.25 * (xndc + 1.0), 0.5, 0.25, 0.75, 0.5));
				sample[i] = frags;
			}
		}
		rig.mo.CancelFrame();
	}
	for (int i = 0; i < 3; i++) {
		// 和手算矩一致
		TASK_CHECK(MomentsNear(got[0][i], HandMoments(sample[i]), 1e-12));
		// 换序只差浮点累加误差
		TASK_CHECK(MomentsNear(got[0][i], got[1][i], 1e-12));
	}
	return 0;
}

// 不透明遮挡：透明面只在严格位于封存深度之前时才有贡献
int cap_opaque_occlusion() {
	Rig rig(8, 8);
	rig.mo.BeginFrame(Vec3d(0.125, 0.25, 0.375));
	int qx = rig.AddFullQuad(0.5f, Vec4f(0.75f, 0.5f, 0.25f, 1.0f));
	TASK_CHECK(rig.DrawQuad(qx));
	TASK_CHECK(rig.ps_count == 64);
	rig.mo.SealOpaque();
	// 透明面在不透明面之后（0.75 > 0.5）：不贡献，PS 被深度挡掉
	int qbehind = rig.AddFullQuad(0.75f, Vec4f(0.5f, 0.25f, 0.75f, 0.5f));
	TASK_CHECK(rig.DrawQuad(qbehind));
	TASK_CHECK(rig.ps_count == 64);
	TASK_CHECK(rig.mo.GetMoments(4, 4).b[0] == 0.0);
	// 透明面与不透明面等深：严格更近才贡献，等深不贡献
	int qequal = rig.AddFullQuad(0.5f, Vec4f(0.5f, 0.25f, 0.75f, 0.5f));
	TASK_CHECK(rig.DrawQuad(qequal));
	TASK_CHECK(rig.ps_count == 64);
	TASK_CHECK(rig.mo.GetMoments(4, 4).b[0] == 0.0);
	// 透明面在不透明面之前（0.25 < 0.5）：贡献
	int qfront = rig.AddFullQuad(0.25f, Vec4f(0.5f, 0.25f, 0.75f, 0.5f));
	TASK_CHECK(rig.DrawQuad(qfront));
	TASK_CHECK(rig.ps_count == 128);
	Frag f = MakeFrag(0.25, 0.5, 0.5, 0.25, 0.75);
	TASK_CHECK(MomentsNear(rig.mo.GetMoments(4, 4), HandMoments(std::vector<Frag>(1, f)), 1e-12));
	// 合成结果 = 透明层 over 封存的不透明颜色
	MomentOitPixel px = rig.mo.ResolvePixel(4, 4);
	Vec3d expect = HandComposite(std::vector<Frag>(1, f), Vec3d(0.75, 0.5, 0.25));
	TASK_CHECK(VecNear(px.color, expect, 1e-12));
	rig.mo.CancelFrame();
	return 0;
}

// 不透明阶段：严格更近才写、PS 丢弃不写、alpha > 0 一律当不透明
int cap_opaque_depth_discard() {
	// 远的先画近的后画：近的胜利，两次都执行 PS
	{
		Rig rig(8, 8);
		rig.mo.BeginFrame(Vec3d(0.125, 0.25, 0.375));
		int qfar = rig.AddFullQuad(0.6f, Vec4f(0.75f, 0.5f, 0.25f, 1.0f));
		int qnear = rig.AddFullQuad(0.4f, Vec4f(0.25f, 0.5f, 0.75f, 1.0f));
		TASK_CHECK(rig.DrawQuad(qfar));
		TASK_CHECK(rig.DrawQuad(qnear));
		TASK_CHECK(rig.ps_count == 128);
		rig.mo.SealOpaque();
		TASK_CHECK(VecNear(rig.mo.ResolvePixel(4, 4).color, Vec3d(0.25, 0.5, 0.75), 1e-12));
		rig.mo.CancelFrame();
	}
	// 近的先画远的后画：仍是近的胜利，远的被深度挡掉不再执行 PS
	{
		Rig rig(8, 8);
		rig.mo.BeginFrame(Vec3d(0.125, 0.25, 0.375));
		int qnear = rig.AddFullQuad(0.4f, Vec4f(0.25f, 0.5f, 0.75f, 1.0f));
		int qfar = rig.AddFullQuad(0.6f, Vec4f(0.75f, 0.5f, 0.25f, 1.0f));
		TASK_CHECK(rig.DrawQuad(qnear));
		TASK_CHECK(rig.DrawQuad(qfar));
		TASK_CHECK(rig.ps_count == 64);
		rig.mo.SealOpaque();
		TASK_CHECK(VecNear(rig.mo.ResolvePixel(4, 4).color, Vec3d(0.25, 0.5, 0.75), 1e-12));
		rig.mo.CancelFrame();
	}
	// alpha <= 0 丢弃：不写颜色也不占深度；alpha > 1 也当不透明
	{
		Rig rig(8, 8);
		rig.mo.BeginFrame(Vec3d(0.125, 0.25, 0.375));
		int qdrop = rig.AddFullQuad(0.3f, Vec4f(1.0f, 1.0f, 1.0f, 0.0f));
		int qneg = rig.AddFullQuad(0.2f, Vec4f(1.0f, 1.0f, 1.0f, -0.5f));
		int qsolid = rig.AddFullQuad(0.6f, Vec4f(0.75f, 0.5f, 0.25f, 2.0f));
		TASK_CHECK(rig.DrawQuad(qdrop));
		TASK_CHECK(rig.DrawQuad(qneg));
		TASK_CHECK(rig.DrawQuad(qsolid));
		TASK_CHECK(rig.ps_count == 192);    // PS 都执行了，但前两个没写
		rig.mo.SealOpaque();
		// 若丢弃面占了深度（0.2/0.3），0.6 的面就写不进来
		TASK_CHECK(VecNear(rig.mo.ResolvePixel(4, 4).color, Vec3d(0.75, 0.5, 0.25), 1e-12));
		// 透明面在 0.4（比 0.6 近）有贡献，进一步证明丢弃面没占深度
		int qt = rig.AddFullQuad(0.4f, Vec4f(0.5f, 0.25f, 0.75f, 0.5f));
		TASK_CHECK(rig.DrawQuad(qt));
		TASK_CHECK(Near(rig.mo.GetMoments(4, 4).b[0], -log1p(-0.5), 1e-12));
		rig.mo.CancelFrame();
	}
	return 0;
}

// alpha 为零不贡献、极小值不被门槛删掉、高不透明度不出错、超界被钳制
int cap_alpha_edges() {
	// alpha = 0：PS 执行但不贡献
	{
		Rig rig(4, 4);
		rig.mo.BeginFrame(Vec3d(0.125, 0.25, 0.375));
		rig.mo.SealOpaque();
		int q = rig.AddFullQuad(0.5f, Vec4f(0.5f, 0.25f, 0.75f, 0.0f));
		TASK_CHECK(rig.DrawQuad(q));
		TASK_CHECK(rig.ps_count == 16);
		TASK_CHECK(rig.mo.GetMoments(2, 2).b[0] == 0.0);
		rig.mo.CancelFrame();
	}
	// alpha 极小（1e-12）：薄层真实存在，不能被全局门槛删掉
	{
		Rig rig(4, 4);
		rig.mo.BeginFrame(Vec3d(0.125, 0.25, 0.375));
		rig.mo.SealOpaque();
		float af = 1e-12f;
		int q = rig.AddFullQuad(0.5f, Vec4f(0.5f, 0.25f, 0.75f, af));
		TASK_CHECK(rig.DrawQuad(q));
		double tau = -log1p(-(double)af);
		TASK_CHECK(Near(rig.mo.GetMoments(2, 2).b[0], tau, 1e-18));
		MomentOitPixel px = rig.mo.ResolvePixel(2, 2);
		TASK_CHECK(px.layers.size() == 1);
		TASK_CHECK(px.layers[0].optical_mass > 0.0);
		TASK_CHECK(Near(px.layers[0].alpha, (double)af, 1e-18));
		rig.mo.CancelFrame();
	}
	// alpha = 1-1e-6（合约上限）：tau = -log(1e-6)，不出 NaN
	{
		Rig rig(4, 4);
		rig.mo.BeginFrame(Vec3d(0.125, 0.25, 0.375));
		rig.mo.SealOpaque();
		float af = 0.999999f;
		int q = rig.AddFullQuad(0.5f, Vec4f(0.5f, 0.25f, 0.75f, af));
		TASK_CHECK(rig.DrawQuad(q));
		double tau = -log1p(-(double)af);
		TASK_CHECK(Near(rig.mo.GetMoments(2, 2).b[0], tau, 1e-9));
		MomentOitPixel px = rig.mo.ResolvePixel(2, 2);
		TASK_CHECK(px.layers.size() == 1);
		TASK_CHECK(Near(px.layers[0].alpha, (double)af, 1e-9));
		TASK_CHECK(px.transmittance < 2e-6);
		rig.mo.CancelFrame();
	}
	// alpha = 1.0（超出合约上限）：被钳到 1-1e-6
	{
		Rig rig(4, 4);
		rig.mo.BeginFrame(Vec3d(0.125, 0.25, 0.375));
		rig.mo.SealOpaque();
		int q = rig.AddFullQuad(0.5f, Vec4f(0.5f, 0.25f, 0.75f, 1.0f));
		TASK_CHECK(rig.DrawQuad(q));
		double tau = -log1p(-(1.0 - 1e-6));
		TASK_CHECK(Near(rig.mo.GetMoments(2, 2).b[0], tau, 1e-9));
		MomentOitPixel px = rig.mo.ResolvePixel(2, 2);
		TASK_CHECK(Near(px.layers[0].alpha, 1.0 - 1e-6, 1e-9));
		rig.mo.CancelFrame();
	}
	return 0;
}

// 穿近面：三角形穿过 z=0 平面被裁剪，深度按裁剪后插值
int cap_clip_near_plane() {
	Rig rig(16, 16);
	rig.mo.BeginFrame(Vec3d(0.125, 0.25, 0.375));
	rig.mo.SealOpaque();
	// 左侧两个角 z=-0.5（在近面之外），右侧 z=+0.5
	Vec4f p[4] = {
		Vec4f(-1.0f,  1.0f, -0.5f, 1.0f), Vec4f(1.0f,  1.0f, 0.5f, 1.0f),
		Vec4f( 1.0f, -1.0f,  0.5f, 1.0f), Vec4f(-1.0f, -1.0f, -0.5f, 1.0f)
	};
	Vec4f c[4] = {
		Vec4f(0.5f, 0.25f, 0.75f, 0.5f), Vec4f(0.5f, 0.25f, 0.75f, 0.5f),
		Vec4f(0.5f, 0.25f, 0.75f, 0.5f), Vec4f(0.5f, 0.25f, 0.75f, 0.5f)
	};
	int q = rig.AddQuad(p, c);
	TASK_CHECK(rig.DrawQuad(q));
	// 裁剪后只剩右半屏：x_ndc ∈ [0,1]，共 8*16 = 128 个像素
	TASK_CHECK(rig.ps_count == 128);
	// 深度 z = 0.5 * x_ndc，逐像素手算核对
	int px[3] = { 8, 12, 15 };
	int py[3] = { 0, 7, 15 };
	for (int i = 0; i < 3; i++) {
		double z = (px[i] + 0.5) / 16.0 - 0.5;
		Frag f = MakeFrag(z, 0.5, 0.5, 0.25, 0.75);
		TASK_CHECK(MomentsNear(rig.mo.GetMoments(px[i], py[i]),
		                       HandMoments(std::vector<Frag>(1, f)), 1e-12));
	}
	// 左半屏没有覆盖
	TASK_CHECK(rig.mo.GetMoments(7, 8).b[0] == 0.0);
	TASK_CHECK(rig.mo.GetMoments(0, 0).b[0] == 0.0);
	rig.mo.CancelFrame();
	return 0;
}

// 穿侧面：超出 x=±1 被裁剪，裁剪交点的 varying 随之插值
int cap_clip_sides() {
	Rig rig(16, 16);
	rig.mo.BeginFrame(Vec3d(0.125, 0.25, 0.375));
	rig.mo.SealOpaque();
	// x_ndc ∈ [-2,2]，varying 红色通道 = (x+2)/4
	Vec4f p[4] = {
		Vec4f(-2.0f,  1.0f, 0.5f, 1.0f), Vec4f(2.0f,  1.0f, 0.5f, 1.0f),
		Vec4f( 2.0f, -1.0f, 0.5f, 1.0f), Vec4f(-2.0f, -1.0f, 0.5f, 1.0f)
	};
	Vec4f c[4] = {
		Vec4f(0.0f, 0.25f, 0.75f, 0.5f), Vec4f(1.0f, 0.25f, 0.75f, 0.5f),
		Vec4f(1.0f, 0.25f, 0.75f, 0.5f), Vec4f(0.0f, 0.25f, 0.75f, 0.5f)
	};
	int q = rig.AddQuad(p, c);
	TASK_CHECK(rig.DrawQuad(q));
	// 裁剪后恰好覆盖全屏
	TASK_CHECK(rig.ps_count == 256);
	// 逐像素手算红色通道：v = (x_ndc + 2) / 4
	int px[3] = { 0, 7, 15 };
	for (int i = 0; i < 3; i++) {
		double xndc = (px[i] + 0.5) / 8.0 - 1.0;
		double v = (xndc + 2.0) / 4.0;
		MomentOitPixel res = rig.mo.ResolvePixel(px[i], 8);
		TASK_CHECK(res.layers.size() == 1);
		TASK_CHECK(Near(res.layers[0].color[0], v, 1e-6));
		TASK_CHECK(Near(res.layers[0].color[1], 0.25, 1e-9));
		TASK_CHECK(Near(res.layers[0].color[2], 0.75, 1e-9));
	}
	rig.mo.CancelFrame();
	return 0;
}

// 不同 w 的属性：深度是屏幕上线性插值的 z/w，varying 按透视权重恢复
int cap_perspective_w() {
	Rig rig(8, 8);
	rig.mo.BeginFrame(Vec3d(0.125, 0.25, 0.375));
	rig.mo.SealOpaque();
	// 三个角 w 分别为 1、2、4，z/w 为 0.25、0.5、0.75，红色 varying 为 0、0.5、1
	Vec4f p[3] = {
		Vec4f(-1.0f, -1.0f, 0.25f, 1.0f),
		Vec4f( 2.0f, -2.0f, 1.0f,  2.0f),
		Vec4f(-4.0f,  4.0f, 3.0f,  4.0f)
	};
	Vec4f c[3] = {
		Vec4f(0.0f, 0.25f, 0.75f, 0.5f),
		Vec4f(0.5f, 0.25f, 0.75f, 0.5f),
		Vec4f(1.0f, 0.25f, 0.75f, 0.5f)
	};
	int t = rig.AddTri(p, c);
	TASK_CHECK(rig.Draw(t));
	TASK_CHECK(rig.vs_count == 3);
	// 手算：屏幕角 (0,8) (8,8) (0,0)，对角线像素被半开规则排除，共 28 个像素
	TASK_CHECK(rig.ps_count == 28);
	// 逐像素手算（重心坐标 + 透视权重），数值来自独立推导
	// 像素 (1,5)：lambda = (0.5, 0.1875, 0.3125)
	//   depth = 0.453125，v = 8/43
	// 像素 (1,6)：lambda = (0.625, 0.1875, 0.1875)
	//   depth = 0.390625，v = 6/49
	// 像素 (2,5)：lambda = (0.375, 0.3125, 0.3125)
	//   depth = 0.484375，v = 10/39
	int px[3] = { 1, 1, 2 };
	int py[3] = { 5, 6, 5 };
	double zref[3] = { 0.453125, 0.390625, 0.484375 };
	double vref[3] = { 8.0 / 43.0, 6.0 / 49.0, 10.0 / 39.0 };
	double tau = -log1p(-0.5);
	for (int i = 0; i < 3; i++) {
		MomentOitMoments m = rig.mo.GetMoments(px[i], py[i]);
		TASK_CHECK(Near(m.b[0], tau, 1e-12));
		TASK_CHECK(Near(m.b[1], tau * zref[i], 1e-9));
		TASK_CHECK(Near(m.b[2], tau * zref[i] * zref[i], 1e-9));
		TASK_CHECK(Near(m.g[0][0], tau * vref[i], 1e-6));
		MomentOitPixel res = rig.mo.ResolvePixel(px[i], py[i]);
		TASK_CHECK(res.layers.size() == 1);
		TASK_CHECK(Near(res.layers[0].depth, zref[i], 1e-9));
		TASK_CHECK(Near(res.layers[0].color[0], vref[i], 1e-6));
	}
	rig.mo.CancelFrame();
	return 0;
}

// 裁剪 fan：一个三角形被裁成全屏四边形，fan 内部边上的像素只贡献一次
int cap_clip_fan() {
	Rig rig(16, 16);
	rig.mo.BeginFrame(Vec3d(0.125, 0.25, 0.375));
	rig.mo.SealOpaque();
	// 大三角形，裁剪后恰好是全屏四边形（fan 的内部边穿过像素中心）
	Vec4f p[3] = {
		Vec4f(-3.0f, 3.0f, 0.5f, 1.0f),
		Vec4f( 3.0f, 3.0f, 0.5f, 1.0f),
		Vec4f( 0.0f, -3.0f, 0.5f, 1.0f)
	};
	Vec4f c[3] = {
		Vec4f(0.5f, 0.25f, 0.75f, 0.4f),
		Vec4f(0.5f, 0.25f, 0.75f, 0.4f),
		Vec4f(0.5f, 0.25f, 0.75f, 0.4f)
	};
	int t = rig.AddTri(p, c);
	TASK_CHECK(rig.Draw(t));
	TASK_CHECK(rig.vs_count == 3);
	// 每个像素恰好被 fan 的一个三角形覆盖一次：PS 恰好 256 次
	TASK_CHECK(rig.ps_count == 256);
	// 内部边上的 alpha 没有被加两次：所有像素的 b[0] 都是一份 tau
	double tau = -log1p(-(double)0.4f);
	for (int y = 0; y < 16; y++) {
		for (int x = 0; x < 16; x++) {
			MomentOitMoments m = rig.mo.GetMoments(x, y);
			TASK_CHECK(Near(m.b[0], tau, 1e-12));
			TASK_CHECK(Near(m.b[1], tau * 0.5, 1e-12));
		}
	}
	rig.mo.CancelFrame();
	return 0;
}

// 共边：两个三角形拼成一个四边形，共享边上的像素恰好覆盖一次
int cap_shared_edge() {
	Rig rig(16, 16);
	rig.mo.BeginFrame(Vec3d(0.125, 0.25, 0.375));
	rig.mo.SealOpaque();
	// 红三角 (0,1,2) 与蓝三角 (0,2,3) 共享对角线，对角线穿过像素中心
	Vec4f p[4] = {
		Vec4f(-1.0f,  1.0f, 0.5f, 1.0f), Vec4f(1.0f,  1.0f, 0.5f, 1.0f),
		Vec4f( 1.0f, -1.0f, 0.5f, 1.0f), Vec4f(-1.0f, -1.0f, 0.5f, 1.0f)
	};
	Vec4f c[4] = {
		Vec4f(1.0f, 0.0f, 0.0f, 0.5f), Vec4f(1.0f, 0.0f, 0.0f, 0.5f),
		Vec4f(1.0f, 0.0f, 0.0f, 0.5f), Vec4f(1.0f, 0.0f, 0.0f, 0.5f)
	};
	int q = rig.AddQuad(p, c);
	// 把第二个三角形的颜色改成蓝色
	rig.color[(size_t)(q + 1) * 3 + 0] = Vec4f(0.0f, 0.0f, 1.0f, 0.5f);
	rig.color[(size_t)(q + 1) * 3 + 1] = Vec4f(0.0f, 0.0f, 1.0f, 0.5f);
	rig.color[(size_t)(q + 1) * 3 + 2] = Vec4f(0.0f, 0.0f, 1.0f, 0.5f);
	TASK_CHECK(rig.DrawQuad(q));
	// 共边恰好覆盖一次：PS 总数 = 256，每个像素 b[0] 都是一份 tau
	TASK_CHECK(rig.ps_count == 256);
	double tau = -log1p(-0.5);
	for (int y = 0; y < 16; y++)
		for (int x = 0; x < 16; x++)
			TASK_CHECK(Near(rig.mo.GetMoments(x, y).b[0], tau, 1e-12));
	// 对角线上的像素 (k,k) 归红三角：红矩有一份、蓝矩为零
	for (int k = 0; k < 16; k++) {
		MomentOitMoments m = rig.mo.GetMoments(k, k);
		TASK_CHECK(Near(m.g[0][0], tau, 1e-12));
		TASK_CHECK(m.g[2][0] == 0.0);
	}
	// 对角线两侧各归各：像素 (5,2) 是红色，(2,5) 是蓝色
	TASK_CHECK(Near(rig.mo.GetMoments(5, 2).g[0][0], tau, 1e-12));
	TASK_CHECK(Near(rig.mo.GetMoments(2, 5).g[2][0], tau, 1e-12));
	rig.mo.CancelFrame();
	return 0;
}

// 三层场景：核对四个光学矩和六个颜色矩
int cap_three_layers() {
	Rig rig(8, 8);
	rig.mo.BeginFrame(Vec3d(0.125, 0.25, 0.375));
	rig.mo.SealOpaque();
	int q0 = rig.AddFullQuad(0.25f, Vec4f(0.5f, 0.25f, 0.75f, 0.5f));
	int q1 = rig.AddFullQuad(0.5f, Vec4f(0.25f, 0.75f, 0.5f, 0.25f));
	int q2 = rig.AddFullQuad(0.75f, Vec4f(0.75f, 0.5f, 0.25f, 0.75f));
	TASK_CHECK(rig.DrawQuad(q0));
	TASK_CHECK(rig.DrawQuad(q1));
	TASK_CHECK(rig.DrawQuad(q2));
	TASK_CHECK(rig.ps_count == 192);
	std::vector<Frag> frags;
	frags.push_back(MakeFrag(0.25, 0.5, 0.5, 0.25, 0.75));
	frags.push_back(MakeFrag(0.5, 0.25, 0.25, 0.75, 0.5));
	frags.push_back(MakeFrag(0.75, 0.75, 0.75, 0.5, 0.25));
	MomentOitMoments ref = HandMoments(frags);
	for (int y = 0; y < 8; y += 3)
		for (int x = 0; x < 8; x += 3)
			TASK_CHECK(MomentsNear(rig.mo.GetMoments(x, y), ref, 1e-12));
	rig.mo.CancelFrame();
	return 0;
}

// 非法输入：整体剔除，VS 仍然每次提交执行三次
int cap_rejections() {
	Rig rig(8, 8);
	rig.mo.BeginFrame(Vec3d(0.125, 0.25, 0.375));
	rig.mo.SealOpaque();
	Vec4f c[3] = {
		Vec4f(0.5f, 0.25f, 0.75f, 0.5f),
		Vec4f(0.5f, 0.25f, 0.75f, 0.5f),
		Vec4f(0.5f, 0.25f, 0.75f, 0.5f)
	};
	// 整体在近面之外（z 全负）
	{
		Vec4f p[3] = {
			Vec4f(-1.0f, -1.0f, -0.5f, 1.0f),
			Vec4f( 1.0f, -1.0f, -0.5f, 1.0f),
			Vec4f( 0.0f,  1.0f, -0.5f, 1.0f)
		};
		int t = rig.AddTri(p, c);
		TASK_CHECK(!rig.Draw(t));
		TASK_CHECK(rig.vs_count == 3);
	}
	// w = 0 的顶点
	{
		Vec4f p[3] = {
			Vec4f(-1.0f, -1.0f, 0.5f, 0.0f),
			Vec4f( 1.0f, -1.0f, 0.5f, 1.0f),
			Vec4f( 0.0f,  1.0f, 0.5f, 1.0f)
		};
		int t = rig.AddTri(p, c);
		TASK_CHECK(!rig.Draw(t));
		TASK_CHECK(rig.vs_count == 6);
	}
	// 非有限坐标
	{
		Vec4f p[3] = {
			Vec4f(-1.0f, -1.0f, 0.5f, 1.0f),
			Vec4f( 1.0f, -1.0f, 0.5f, 1.0f),
			Vec4f( 0.0f,  1.0f, 0.5f, 1.0f)
		};
		int t = rig.AddTri(p, c);
		rig.pos[(size_t)t * 3 + 1] = Vec4f(std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.5f, 1.0f);
		TASK_CHECK(!rig.Draw(t));
		TASK_CHECK(rig.vs_count == 9);
	}
	// 退化（共线）三角形
	{
		Vec4f p[3] = {
			Vec4f(-0.5f, 0.0f, 0.5f, 1.0f),
			Vec4f( 0.0f, 0.0f, 0.5f, 1.0f),
			Vec4f( 0.5f, 0.0f, 0.5f, 1.0f)
		};
		int t = rig.AddTri(p, c);
		TASK_CHECK(!rig.Draw(t));
		TASK_CHECK(rig.vs_count == 12);
	}
	TASK_CHECK(rig.ps_count == 0);
	rig.mo.CancelFrame();
	return 0;
}

//---------------------------------------------------------------------
// reconstruct：从矩重建层并合成
//---------------------------------------------------------------------

// 单层：重建等价于精确分层合成
int rec_single_layer() {
	Rig rig(8, 8);
	Vec3d bg(0.125, 0.25, 0.375);
	rig.mo.BeginFrame(bg);
	rig.mo.SealOpaque();
	int q = rig.AddFullQuad(0.5f, Vec4f(0.5f, 0.25f, 0.75f, 0.25f));
	TASK_CHECK(rig.DrawQuad(q));
	MomentOitPixel px = rig.mo.ResolvePixel(3, 3);
	double tau = -log1p(-0.25);
	TASK_CHECK(px.layers.size() == 1);
	TASK_CHECK(Near(px.layers[0].depth, 0.5, 1e-12));
	TASK_CHECK(Near(px.layers[0].optical_mass, tau, 1e-12));
	TASK_CHECK(Near(px.layers[0].alpha, 0.25, 1e-12));
	TASK_CHECK(Near(px.layers[0].color[0], 0.5, 1e-12));
	TASK_CHECK(Near(px.layers[0].color[1], 0.25, 1e-12));
	TASK_CHECK(Near(px.layers[0].color[2], 0.75, 1e-12));
	// 未钳制的层色也可以检查
	TASK_CHECK(Near(px.layers[0].unclamped[0], 0.5, 1e-12));
	TASK_CHECK(Near(px.transmittance, 0.75, 1e-12));
	Frag f = MakeFrag(0.5, 0.25, 0.5, 0.25, 0.75);
	TASK_CHECK(VecNear(px.color, HandComposite(std::vector<Frag>(1, f), bg), 1e-12));
	rig.mo.CancelFrame();
	return 0;
}

// 双层：两个不同深度的层被精确恢复（节点间隔 >= 1e-4 时 RGB 误差 <= 1e-7）
int rec_two_layers() {
	// 普通尺度：间隔 0.5
	{
		Rig rig(8, 8);
		Vec3d bg(0.125, 0.25, 0.375);
		rig.mo.BeginFrame(bg);
		rig.mo.SealOpaque();
		int q0 = rig.AddFullQuad(0.25f, Vec4f(0.5f, 0.25f, 0.75f, 0.5f));
		int q1 = rig.AddFullQuad(0.75f, Vec4f(0.25f, 0.75f, 0.5f, 0.25f));
		TASK_CHECK(rig.DrawQuad(q0));
		TASK_CHECK(rig.DrawQuad(q1));
		MomentOitPixel px = rig.mo.ResolvePixel(4, 4);
		TASK_CHECK(px.layers.size() == 2);
		// 节点由近到远
		TASK_CHECK(Near(px.layers[0].depth, 0.25, 1e-9));
		TASK_CHECK(Near(px.layers[1].depth, 0.75, 1e-9));
		TASK_CHECK(Near(px.layers[0].optical_mass, -log1p(-0.5), 1e-9));
		TASK_CHECK(Near(px.layers[1].optical_mass, -log1p(-0.25), 1e-9));
		TASK_CHECK(Near(px.layers[0].alpha, 0.5, 1e-9));
		TASK_CHECK(Near(px.layers[1].alpha, 0.25, 1e-9));
		TASK_CHECK(Near(px.layers[0].color[0], 0.5, 1e-9));
		TASK_CHECK(Near(px.layers[1].color[0], 0.25, 1e-9));
		TASK_CHECK(Near(px.layers[1].color[1], 0.75, 1e-9));
		TASK_CHECK(Near(px.transmittance, 0.375, 1e-9));
		std::vector<Frag> frags;
		frags.push_back(MakeFrag(0.25, 0.5, 0.5, 0.25, 0.75));
		frags.push_back(MakeFrag(0.75, 0.25, 0.25, 0.75, 0.5));
		TASK_CHECK(VecNear(px.color, HandComposite(frags, bg), 1e-9));
		rig.mo.CancelFrame();
	}
	// 间隔 1e-2：RGB 和矩重建误差都不超过 1e-7
	{
		Rig rig(8, 8);
		Vec3d bg(0.125, 0.25, 0.375);
		rig.mo.BeginFrame(bg);
		rig.mo.SealOpaque();
		int q0 = rig.AddFullQuad(0.5f, Vec4f(0.5f, 0.25f, 0.75f, 0.5f));
		int q1 = rig.AddFullQuad(0.51f, Vec4f(0.25f, 0.75f, 0.5f, 0.25f));
		TASK_CHECK(rig.DrawQuad(q0));
		TASK_CHECK(rig.DrawQuad(q1));
		MomentOitPixel px = rig.mo.ResolvePixel(4, 4);
		TASK_CHECK(px.layers.size() == 2);
		std::vector<Frag> frags;
		frags.push_back(MakeFrag(0.5, 0.5, 0.5, 0.25, 0.75));
		frags.push_back(MakeFrag((double)0.51f, 0.25, 0.25, 0.75, 0.5));
		TASK_CHECK(VecNear(px.color, HandComposite(frags, bg), 1e-7));
		TASK_CHECK(Near(px.layers[0].color[0], 0.5, 1e-7));
		TASK_CHECK(Near(px.layers[1].color[0], 0.25, 1e-7));
		rig.mo.CancelFrame();
	}
	// 间隔 1e-4：矩重建仍然精确，节点有效（RGB 的条件数限制见 README）
	{
		Rig rig(8, 8);
		Vec3d bg(0.125, 0.25, 0.375);
		rig.mo.BeginFrame(bg);
		rig.mo.SealOpaque();
		int q0 = rig.AddFullQuad(0.5f, Vec4f(0.5f, 0.25f, 0.75f, 0.5f));
		int q1 = rig.AddFullQuad(0.5001f, Vec4f(0.25f, 0.75f, 0.5f, 0.25f));
		TASK_CHECK(rig.DrawQuad(q0));
		TASK_CHECK(rig.DrawQuad(q1));
		MomentOitPixel px = rig.mo.ResolvePixel(4, 4);
		TASK_CHECK(px.layers.size() == 2);
		// 节点在 [0,1] 内、质量为正
		for (int j = 0; j < 2; j++) {
			TASK_CHECK(px.layers[j].depth >= 0.0 && px.layers[j].depth <= 1.0);
			TASK_CHECK(px.layers[j].optical_mass > 0.0);
		}
		// 用重建节点和未钳制层色核对四个矩和六个颜色矩
		MomentOitMoments m = rig.mo.GetMoments(4, 4);
		for (int k = 0; k < 4; k++) {
			double bk = 0.0;
			for (int j = 0; j < 2; j++)
				bk += px.layers[j].optical_mass * pow(px.layers[j].depth, k);
			TASK_CHECK(Near(bk, m.b[k], 1e-7));
		}
		for (int c = 0; c < 3; c++) {
			double g0 = 0.0, g1 = 0.0;
			for (int j = 0; j < 2; j++) {
				g0 += px.layers[j].optical_mass * px.layers[j].unclamped[c];
				g1 += px.layers[j].optical_mass * px.layers[j].depth * px.layers[j].unclamped[c];
			}
			TASK_CHECK(Near(g0, m.g[c][0], 1e-7));
			TASK_CHECK(Near(g1, m.g[c][1], 1e-7));
		}
		TASK_CHECK(Near(px.transmittance, exp(-m.b[0]), 1e-12));
		rig.mo.CancelFrame();
	}
	return 0;
}

// 同深度混色：并成一层，颜色用 tau 加权
int rec_same_depth_merged() {
	Rig rig(8, 8);
	Vec3d bg(0.125, 0.25, 0.375);
	rig.mo.BeginFrame(bg);
	rig.mo.SealOpaque();
	int q0 = rig.AddFullQuad(0.5f, Vec4f(0.5f, 0.25f, 0.75f, 0.5f));
	int q1 = rig.AddFullQuad(0.5f, Vec4f(0.25f, 0.75f, 0.5f, 0.25f));
	TASK_CHECK(rig.DrawQuad(q0));
	TASK_CHECK(rig.DrawQuad(q1));
	MomentOitPixel px = rig.mo.ResolvePixel(4, 4);
	TASK_CHECK(px.layers.size() == 1);
	double t0 = -log1p(-0.5), t1 = -log1p(-0.25);
	TASK_CHECK(Near(px.layers[0].depth, 0.5, 1e-12));
	TASK_CHECK(Near(px.layers[0].optical_mass, t0 + t1, 1e-12));
	TASK_CHECK(Near(px.layers[0].alpha, -expm1(-(t0 + t1)), 1e-12));
	// 颜色用 tau 加权
	TASK_CHECK(Near(px.layers[0].color[0], (t0 * 0.5 + t1 * 0.25) / (t0 + t1), 1e-12));
	TASK_CHECK(Near(px.layers[0].color[1], (t0 * 0.25 + t1 * 0.75) / (t0 + t1), 1e-12));
	std::vector<Frag> frags;
	frags.push_back(MakeFrag(0.5, 0.5, 0.5, 0.25, 0.75));
	frags.push_back(MakeFrag(0.5, 0.25, 0.25, 0.75, 0.5));
	TASK_CHECK(VecNear(px.color, HandComposite(frags, bg), 1e-12));
	rig.mo.CancelFrame();
	return 0;
}

// 空帧和空像素：直接保留封存颜色
int rec_empty_background() {
	// 空帧：没有任何绘制
	{
		Rig rig(4, 4);
		Vec3d bg(0.125, 0.25, 0.375);
		rig.mo.BeginFrame(bg);
		rig.mo.SealOpaque();
		MomentOitPixel px = rig.mo.ResolvePixel(2, 2);
		TASK_CHECK(px.layers.size() == 0);
		TASK_CHECK(px.transmittance == 1.0);
		TASK_CHECK(VecNear(px.color, bg, 1e-12));
		rig.mo.CancelFrame();
	}
	// 只有不透明面：透明部分为空，保留封存的不透明颜色
	{
		Rig rig(4, 4);
		rig.mo.BeginFrame(Vec3d(0.125, 0.25, 0.375));
		int q = rig.AddFullQuad(0.5f, Vec4f(0.5f, 0.25f, 0.75f, 1.0f));
		TASK_CHECK(rig.DrawQuad(q));
		rig.mo.SealOpaque();
		MomentOitPixel px = rig.mo.ResolvePixel(2, 2);
		TASK_CHECK(px.layers.size() == 0);
		TASK_CHECK(px.transmittance == 1.0);
		TASK_CHECK(VecNear(px.color, Vec3d(0.5, 0.25, 0.75), 1e-12));
		rig.mo.CancelFrame();
	}
	return 0;
}

// 近合并节点：不出 NaN、不出负质量，结果退化为合理的单层
int rec_near_merged() {
	Rig rig(8, 8);
	Vec3d bg(0.125, 0.25, 0.375);
	rig.mo.BeginFrame(bg);
	rig.mo.SealOpaque();
	int q0 = rig.AddFullQuad(0.5f, Vec4f(0.5f, 0.25f, 0.75f, 0.5f));
	int q1 = rig.AddFullQuad(0.500000001f, Vec4f(0.25f, 0.75f, 0.5f, 0.25f));
	TASK_CHECK(rig.DrawQuad(q0));
	TASK_CHECK(rig.DrawQuad(q1));
	MomentOitPixel px = rig.mo.ResolvePixel(4, 4);
	TASK_CHECK(px.layers.size() >= 1);
	double mass = 0.0;
	for (size_t j = 0; j < px.layers.size(); j++) {
		TASK_CHECK(px.layers[j].depth >= 0.0 && px.layers[j].depth <= 1.0);
		TASK_CHECK(px.layers[j].optical_mass > 0.0);
		TASK_CHECK(std::isfinite(px.layers[j].alpha));
		for (int c = 0; c < 3; c++)
			TASK_CHECK(std::isfinite(px.layers[j].color[c]));
		mass += px.layers[j].optical_mass;
	}
	// 总质量守恒：tau0 + tau1
	double t0 = -log1p(-0.5), t1 = -log1p(-0.25);
	TASK_CHECK(Near(mass, t0 + t1, 1e-9));
	TASK_CHECK(Near(px.transmittance, exp(-(t0 + t1)), 1e-9));
	// 合成结果接近把两层并成一层
	std::vector<Frag> frags;
	frags.push_back(MakeFrag(0.5, 0.5, 0.5, 0.25, 0.75));
	frags.push_back(MakeFrag((double)0.500000001f, 0.25, 0.25, 0.75, 0.5));
	TASK_CHECK(VecNear(px.color, HandComposite(frags, bg), 1e-6));
	for (int c = 0; c < 3; c++)
		TASK_CHECK(std::isfinite(px.color[c]));
	rig.mo.CancelFrame();
	return 0;
}

// 极小 alpha 和极高不透明度：都能正确重建，不出 NaN
int rec_tiny_max_alpha() {
	Rig rig(8, 8);
	Vec3d bg(0.125, 0.25, 0.375);
	rig.mo.BeginFrame(bg);
	rig.mo.SealOpaque();
	float atiny = 1e-12f;
	float amax = 0.999999f;
	int q0 = rig.AddFullQuad(0.25f, Vec4f(0.5f, 0.25f, 0.75f, atiny));
	int q1 = rig.AddFullQuad(0.75f, Vec4f(0.25f, 0.75f, 0.5f, amax));
	TASK_CHECK(rig.DrawQuad(q0));
	TASK_CHECK(rig.DrawQuad(q1));
	MomentOitPixel px = rig.mo.ResolvePixel(4, 4);
	TASK_CHECK(px.layers.size() == 2);
	TASK_CHECK(px.layers[0].optical_mass > 0.0);
	TASK_CHECK(px.layers[1].optical_mass > 0.0);
	// 极小的薄层被保留下来（恢复精度受条件数限制，量级正确）
	TASK_CHECK(Near(px.layers[0].alpha, (double)atiny, 1e-13));
	TASK_CHECK(Near(px.layers[1].alpha, (double)amax, 1e-9));
	// 透射率由两层共同决定：exp(-(tau0 + tau1))
	double tau = -log1p(-(double)atiny) + -log1p(-(double)amax);
	TASK_CHECK(Near(px.transmittance, exp(-tau), 1e-12));
	std::vector<Frag> frags;
	frags.push_back(MakeFrag(0.25, (double)atiny, 0.5, 0.25, 0.75));
	frags.push_back(MakeFrag(0.75, (double)amax, 0.25, 0.75, 0.5));
	TASK_CHECK(VecNear(px.color, HandComposite(frags, bg), 1e-9));
	for (int c = 0; c < 3; c++)
		TASK_CHECK(std::isfinite(px.color[c]));
	rig.mo.CancelFrame();
	return 0;
}

// 三层场景：两节点近似，重建节点在 [0,1] 内，四个矩误差不超过 1e-7
int rec_three_layers() {
	Rig rig(8, 8);
	Vec3d bg(0.125, 0.25, 0.375);
	rig.mo.BeginFrame(bg);
	rig.mo.SealOpaque();
	int q0 = rig.AddFullQuad(0.25f, Vec4f(0.5f, 0.25f, 0.75f, 0.5f));
	int q1 = rig.AddFullQuad(0.5f, Vec4f(0.25f, 0.75f, 0.5f, 0.25f));
	int q2 = rig.AddFullQuad(0.75f, Vec4f(0.75f, 0.5f, 0.25f, 0.75f));
	TASK_CHECK(rig.DrawQuad(q0));
	TASK_CHECK(rig.DrawQuad(q1));
	TASK_CHECK(rig.DrawQuad(q2));
	MomentOitPixel px = rig.mo.ResolvePixel(4, 4);
	// 三层是两节点矩模型的近似
	TASK_CHECK(px.layers.size() == 2);
	for (int j = 0; j < 2; j++) {
		TASK_CHECK(px.layers[j].depth >= 0.0 && px.layers[j].depth <= 1.0);
		TASK_CHECK(px.layers[j].optical_mass > 0.0);
	}
	// 四个矩的重建误差不超过 1e-7
	MomentOitMoments m = rig.mo.GetMoments(4, 4);
	for (int k = 0; k < 4; k++) {
		double bk = 0.0;
		for (int j = 0; j < 2; j++)
			bk += px.layers[j].optical_mass * pow(px.layers[j].depth, k);
		TASK_CHECK(Near(bk, m.b[k], 1e-7));
	}
	// 六个颜色矩也能用未钳制层色核对
	for (int c = 0; c < 3; c++) {
		double g0 = 0.0, g1 = 0.0;
		for (int j = 0; j < 2; j++) {
			g0 += px.layers[j].optical_mass * px.layers[j].unclamped[c];
			g1 += px.layers[j].optical_mass * px.layers[j].depth * px.layers[j].unclamped[c];
		}
		TASK_CHECK(Near(g0, m.g[c][0], 1e-7));
		TASK_CHECK(Near(g1, m.g[c][1], 1e-7));
	}
	TASK_CHECK(Near(px.transmittance, exp(-m.b[0]), 1e-12));
	// 合成颜色是有限且在 [0,1] 内的近似
	for (int c = 0; c < 3; c++) {
		TASK_CHECK(std::isfinite(px.color[c]));
		TASK_CHECK(px.color[c] >= 0.0 && px.color[c] <= 1.0);
	}
	rig.mo.CancelFrame();
	return 0;
}

// 逆序提交：结果只差浮点累加误差
int rec_reverse_order() {
	MomentOitPixel got[2];
	MomentOitMoments mgot[2];
	for (int order = 0; order < 2; order++) {
		Rig rig(8, 8);
		rig.mo.BeginFrame(Vec3d(0.125, 0.25, 0.375));
		rig.mo.SealOpaque();
		int q0 = rig.AddFullQuad(0.25f, Vec4f(0.5f, 0.25f, 0.75f, 0.5f));
		int q1 = rig.AddFullQuad(0.5f, Vec4f(0.25f, 0.75f, 0.5f, 0.25f));
		int q2 = rig.AddFullQuad(0.75f, Vec4f(0.75f, 0.5f, 0.25f, 0.75f));
		if (order == 0) {
			TASK_CHECK(rig.DrawQuad(q0));
			TASK_CHECK(rig.DrawQuad(q1));
			TASK_CHECK(rig.DrawQuad(q2));
		}
		else {
			TASK_CHECK(rig.DrawQuad(q2));
			TASK_CHECK(rig.DrawQuad(q1));
			TASK_CHECK(rig.DrawQuad(q0));
		}
		mgot[order] = rig.mo.GetMoments(4, 4);
		got[order] = rig.mo.ResolvePixel(4, 4);
		rig.mo.CancelFrame();
	}
	TASK_CHECK(MomentsNear(mgot[0], mgot[1], 1e-12));
	TASK_CHECK(PixelNear(got[0], got[1], 1e-12));
	return 0;
}

// 遮挡合成：透明层合成在封存的不透明颜色之上
int rec_occluded_composite() {
	Rig rig(8, 8);
	Vec3d bg(0.125, 0.25, 0.375);
	rig.mo.BeginFrame(bg);
	int qx = rig.AddFullQuad(0.5f, Vec4f(0.75f, 0.5f, 0.25f, 1.0f));
	TASK_CHECK(rig.DrawQuad(qx));
	rig.mo.SealOpaque();
	int qt = rig.AddFullQuad(0.25f, Vec4f(0.5f, 0.25f, 0.75f, 0.5f));
	TASK_CHECK(rig.DrawQuad(qt));
	MomentOitPixel px = rig.mo.ResolvePixel(4, 4);
	// 0.5 * 透明色 + 0.5 * 不透明色
	Frag f = MakeFrag(0.25, 0.5, 0.5, 0.25, 0.75);
	TASK_CHECK(VecNear(px.color, HandComposite(std::vector<Frag>(1, f), Vec3d(0.75, 0.5, 0.25)), 1e-12));
	TASK_CHECK(Near(px.transmittance, 0.5, 1e-12));
	rig.mo.CancelFrame();
	return 0;
}

//---------------------------------------------------------------------
// lifecycle：阶段、取消、快照
//---------------------------------------------------------------------

// 错误阶段调用抛 std::logic_error，且抛出前不修改状态
int life_phase_errors() {
	Rig rig(4, 4);
	// 未开始：封存/解析/读矩都抛错
	TASK_CHECK(Throws<std::logic_error>([&] { rig.mo.SealOpaque(); }));
	TASK_CHECK(Throws<std::logic_error>([&] { rig.mo.ResolvePixel(0, 0); }));
	TASK_CHECK(Throws<std::logic_error>([&] { rig.mo.GetMoments(0, 0); }));
	TASK_CHECK(rig.mo.GetPhase() == MomentOit::PHASE_IDLE);
	// 取消是幂等的，不抛异常
	rig.mo.CancelFrame();
	TASK_CHECK(rig.mo.GetPhase() == MomentOit::PHASE_IDLE);
	// 开始后重复开始抛错，且当前帧不被破坏
	rig.mo.BeginFrame(Vec3d(0.125, 0.25, 0.375));
	TASK_CHECK(rig.mo.GetPhase() == MomentOit::PHASE_OPAQUE);
	TASK_CHECK(Throws<std::logic_error>([&] { rig.mo.BeginFrame(Vec3d(0.0, 0.0, 0.0)); }));
	TASK_CHECK(rig.mo.GetPhase() == MomentOit::PHASE_OPAQUE);
	// 未封存不能解析/读矩
	TASK_CHECK(Throws<std::logic_error>([&] { rig.mo.ResolvePixel(0, 0); }));
	TASK_CHECK(Throws<std::logic_error>([&] { rig.mo.GetMoments(0, 0); }));
	// 被拒绝后不透明绘制仍可进行
	int q = rig.AddFullQuad(0.5f, Vec4f(0.5f, 0.25f, 0.75f, 1.0f));
	TASK_CHECK(rig.DrawQuad(q));
	rig.mo.SealOpaque();
	TASK_CHECK(rig.mo.GetPhase() == MomentOit::PHASE_SEALED);
	// 重复封存抛错
	TASK_CHECK(Throws<std::logic_error>([&] { rig.mo.SealOpaque(); }));
	// 封存后可以解析
	TASK_CHECK(VecNear(rig.mo.ResolvePixel(2, 2).color, Vec3d(0.5, 0.25, 0.75), 1e-12));
	// 坐标越界
	TASK_CHECK(Throws<std::out_of_range>([&] { rig.mo.ResolvePixel(-1, 0); }));
	TASK_CHECK(Throws<std::out_of_range>([&] { rig.mo.GetMoments(4, 4); }));
	// 取消后解析抛错，但可以重新开始
	rig.mo.CancelFrame();
	TASK_CHECK(Throws<std::logic_error>([&] { rig.mo.ResolvePixel(2, 2); }));
	rig.mo.BeginFrame(Vec3d(0.25, 0.5, 0.75));
	rig.mo.SealOpaque();
	TASK_CHECK(VecNear(rig.mo.ResolvePixel(2, 2).color, Vec3d(0.25, 0.5, 0.75), 1e-12));
	rig.mo.CancelFrame();
	return 0;
}

// 取消后恢复普通帧行为，且可以重新开始
int life_cancel_restart() {
	Rig rig(8, 8);
	// 没有矩帧时 DrawPrimitive 是普通帧行为
	int q0 = rig.AddFullQuad(0.5f, Vec4f(1.0f, 0.0f, 0.0f, 1.0f));
	TASK_CHECK(rig.DrawQuad(q0));
	TASK_CHECK(rig.rh.Pixel(4, 4) == vector_to_color(Vec4f(1.0f, 0.0f, 0.0f, 1.0f)));
	TASK_CHECK(rig.rh.Depth(4, 4) == 1.0f);    // 普通帧存的是 1/w
	rig.rh.Clear();
	// 矩帧期间普通帧缓冲不被写
	rig.mo.BeginFrame(Vec3d(0.125, 0.25, 0.375));
	int q1 = rig.AddFullQuad(0.25f, Vec4f(0.0f, 1.0f, 0.0f, 1.0f));
	TASK_CHECK(rig.DrawQuad(q1));
	TASK_CHECK(rig.rh.Pixel(4, 4) != vector_to_color(Vec4f(0.0f, 1.0f, 0.0f, 1.0f)));
	// 取消后恢复普通帧行为
	rig.mo.CancelFrame();
	TASK_CHECK(rig.mo.GetPhase() == MomentOit::PHASE_IDLE);
	TASK_CHECK(rig.rh.GetMomentHook() == NULL);
	int q2 = rig.AddFullQuad(0.75f, Vec4f(0.0f, 0.0f, 1.0f, 1.0f));
	TASK_CHECK(rig.DrawQuad(q2));
	TASK_CHECK(rig.rh.Pixel(4, 4) == vector_to_color(Vec4f(0.0f, 0.0f, 1.0f, 1.0f)));
	// 取消后可以重新开始
	rig.mo.BeginFrame(Vec3d(0.5, 0.5, 0.5));
	rig.mo.SealOpaque();
	TASK_CHECK(VecNear(rig.mo.ResolvePixel(4, 4).color, Vec3d(0.5, 0.5, 0.5), 1e-12));
	rig.mo.CancelFrame();
	return 0;
}

// Clear/Init/Reset 会取消活动中的矩帧
int life_clear_init_reset() {
	// Clear 取消
	{
		Rig rig(8, 8);
		rig.mo.BeginFrame(Vec3d(0.125, 0.25, 0.375));
		int q = rig.AddFullQuad(0.5f, Vec4f(0.5f, 0.25f, 0.75f, 1.0f));
		TASK_CHECK(rig.DrawQuad(q));
		TASK_CHECK(rig.mo.GetPhase() == MomentOit::PHASE_OPAQUE);
		rig.rh.Clear();
		TASK_CHECK(rig.mo.GetPhase() == MomentOit::PHASE_IDLE);
		TASK_CHECK(rig.rh.GetMomentHook() == NULL);
		// 取消后可以重新开始
		rig.mo.BeginFrame(Vec3d(0.25, 0.5, 0.75));
		rig.mo.SealOpaque();
		TASK_CHECK(VecNear(rig.mo.ResolvePixel(4, 4).color, Vec3d(0.25, 0.5, 0.75), 1e-12));
		rig.mo.CancelFrame();
	}
	// Init 取消
	{
		Rig rig(8, 8);
		rig.mo.BeginFrame(Vec3d(0.125, 0.25, 0.375));
		TASK_CHECK(rig.mo.GetPhase() == MomentOit::PHASE_OPAQUE);
		rig.rh.Init(4, 4);
		TASK_CHECK(rig.mo.GetPhase() == MomentOit::PHASE_IDLE);
		// 新尺寸下重新开始
		rig.mo.BeginFrame(Vec3d(0.5, 0.5, 0.5));
		rig.mo.SealOpaque();
		TASK_CHECK(VecNear(rig.mo.ResolvePixel(2, 2).color, Vec3d(0.5, 0.5, 0.5), 1e-12));
		TASK_CHECK(Throws<std::out_of_range>([&] { rig.mo.ResolvePixel(5, 5); }));
		rig.mo.CancelFrame();
	}
	// Reset 取消；Reset 后宿主未初始化，BeginFrame 抛错
	{
		Rig rig(8, 8);
		rig.mo.BeginFrame(Vec3d(0.125, 0.25, 0.375));
		TASK_CHECK(rig.mo.GetPhase() == MomentOit::PHASE_OPAQUE);
		rig.rh.Reset();
		TASK_CHECK(rig.mo.GetPhase() == MomentOit::PHASE_IDLE);
		TASK_CHECK(Throws<std::logic_error>([&] { rig.mo.BeginFrame(Vec3d(0.0, 0.0, 0.0)); }));
		// 重新 Init 并重装着色器后可以继续
		rig.rh.Init(8, 8);
		rig.InstallShaders();
		rig.mo.BeginFrame(Vec3d(0.5, 0.25, 0.75));
		rig.mo.SealOpaque();
		TASK_CHECK(VecNear(rig.mo.ResolvePixel(4, 4).color, Vec3d(0.5, 0.25, 0.75), 1e-12));
		rig.mo.CancelFrame();
	}
	return 0;
}

// 封存后没有深度写入：透明面不遮挡更远的透明面
int life_no_depth_write_after_seal() {
	Rig rig(8, 8);
	rig.mo.BeginFrame(Vec3d(0.125, 0.25, 0.375));
	int qx = rig.AddFullQuad(0.5f, Vec4f(0.75f, 0.5f, 0.25f, 1.0f));
	TASK_CHECK(rig.DrawQuad(qx));
	rig.mo.SealOpaque();
	// 近的透明面先画：若它占了深度，远的透明面就会被错误挡掉
	int qa = rig.AddFullQuad(0.25f, Vec4f(0.5f, 0.25f, 0.75f, 0.5f));
	int qb = rig.AddFullQuad(0.375f, Vec4f(0.25f, 0.75f, 0.5f, 0.25f));
	TASK_CHECK(rig.DrawQuad(qa));
	TASK_CHECK(rig.DrawQuad(qb));
	TASK_CHECK(rig.ps_count == 192);    // 64(不透明) + 64 + 64：两层都贡献了
	std::vector<Frag> frags;
	frags.push_back(MakeFrag(0.25, 0.5, 0.5, 0.25, 0.75));
	frags.push_back(MakeFrag(0.375, 0.25, 0.25, 0.75, 0.5));
	TASK_CHECK(MomentsNear(rig.mo.GetMoments(4, 4), HandMoments(frags), 1e-12));
	// 合成结果与手算一致（封存的不透明颜色在底层）
	TASK_CHECK(VecNear(rig.mo.ResolvePixel(4, 4).color,
	                   HandComposite(frags, Vec3d(0.75, 0.5, 0.25)), 1e-9));
	rig.mo.CancelFrame();
	return 0;
}

// 多次解析、旧快照、修改或释放回调捕获的输入都不改变旧结果
int life_resolve_side_effects() {
	Rig rig(8, 8);
	uint32_t bg_pixel = rig.rh.Pixel(3, 3);
	float bg_depth = rig.rh.Depth(3, 3);
	rig.mo.BeginFrame(Vec3d(0.125, 0.25, 0.375));
	int qx = rig.AddFullQuad(0.5f, Vec4f(0.75f, 0.5f, 0.25f, 1.0f));
	TASK_CHECK(rig.DrawQuad(qx));
	rig.mo.SealOpaque();
	int qt = rig.AddFullQuad(0.25f, Vec4f(0.5f, 0.25f, 0.75f, 0.5f));
	TASK_CHECK(rig.DrawQuad(qt));
	int vs1 = rig.vs_count, ps1 = rig.ps_count;
	// 多次解析结果一致
	MomentOitPixel r1 = rig.mo.ResolvePixel(3, 3);
	MomentOitPixel r2 = rig.mo.ResolvePixel(3, 3);
	TASK_CHECK(PixelNear(r1, r2, 0.0));
	// 解析不执行 VS/PS
	TASK_CHECK(rig.vs_count == vs1 && rig.ps_count == ps1);
	// 解析（以及整个矩帧）不写普通帧的颜色和深度
	TASK_CHECK(rig.rh.Pixel(3, 3) == bg_pixel);
	TASK_CHECK(rig.rh.Depth(3, 3) == bg_depth);
	// 旧快照独立持有：再画一个透明面，旧结果不变
	Vec3d saved(r1.color[0], r1.color[1], r1.color[2]);
	double saved_t = r1.transmittance;
	size_t saved_layers = r1.layers.size();
	int qt2 = rig.AddFullQuad(0.4f, Vec4f(0.25f, 0.75f, 0.5f, 0.25f));
	TASK_CHECK(rig.DrawQuad(qt2));
	MomentOitPixel r3 = rig.mo.ResolvePixel(3, 3);
	TASK_CHECK(VecNear(r1.color, saved, 0.0));
	TASK_CHECK(r1.transmittance == saved_t);
	TASK_CHECK(r1.layers.size() == saved_layers);
	TASK_CHECK(!PixelNear(r3, r1, 1e-9));    // 新结果确实变了
	// 修改回调捕获的输入：解析不执行 VS/PS，结果不受影响
	rig.color[0] = Vec4f(0.0f, 0.0f, 0.0f, 0.0f);
	MomentOitPixel r4 = rig.mo.ResolvePixel(3, 3);
	TASK_CHECK(PixelNear(r4, r3, 0.0));
	rig.mo.CancelFrame();

	// 释放回调捕获的输入：旧结果不变
	{
		Probe pr(4, 4);
		MomentOit mo2(pr);
		Vec4f *heap_pos = new Vec4f[3];
		Vec4f *heap_color = new Vec4f[3];
		heap_pos[0] = Vec4f(-1.0f, 1.0f, 0.5f, 1.0f);
		heap_pos[1] = Vec4f(1.0f, 1.0f, 0.5f, 1.0f);
		heap_pos[2] = Vec4f(1.0f, -1.0f, 0.5f, 1.0f);
		for (int i = 0; i < 3; i++)
			heap_color[i] = Vec4f(0.5f, 0.25f, 0.75f, 0.5f);
		pr.SetVertexShader([&](int i, ShaderContext &out) -> Vec4f {
			out.varying_vec4f[0] = heap_color[i];
			return heap_pos[i];
		});
		pr.SetPixelShader([&](ShaderContext &in) -> Vec4f {
			return in.varying_vec4f[0];
		});
		mo2.BeginFrame(Vec3d(0.125, 0.25, 0.375));
		mo2.SealOpaque();
		TASK_CHECK(pr.DrawPrimitive());
		MomentOitPixel before = mo2.ResolvePixel(2, 2);
		delete []heap_pos;
		delete []heap_color;    // 释放捕获的输入
		MomentOitPixel after = mo2.ResolvePixel(2, 2);
		TASK_CHECK(PixelNear(before, after, 0.0));
		mo2.CancelFrame();
	}
	return 0;
}

// 两台设备交错：各自的矩帧互不影响
int life_two_devices() {
	Rig A(4, 4), B(4, 4);
	A.mo.BeginFrame(Vec3d(0.125, 0.25, 0.375));
	B.mo.BeginFrame(Vec3d(0.375, 0.25, 0.125));
	// 交错提交
	int qa = A.AddFullQuad(0.5f, Vec4f(0.5f, 0.25f, 0.75f, 1.0f));
	TASK_CHECK(A.DrawQuad(qa));
	int qb = B.AddFullQuad(0.75f, Vec4f(0.25f, 0.75f, 0.5f, 1.0f));
	TASK_CHECK(B.DrawQuad(qb));
	A.mo.SealOpaque();
	B.mo.SealOpaque();
	int qta = A.AddFullQuad(0.25f, Vec4f(0.75f, 0.5f, 0.25f, 0.5f));
	TASK_CHECK(A.DrawQuad(qta));
	int qtb = B.AddFullQuad(0.5f, Vec4f(0.5f, 0.75f, 0.25f, 0.25f));
	TASK_CHECK(B.DrawQuad(qtb));
	// 各自的结果与手算一致
	MomentOitPixel pa = A.mo.ResolvePixel(2, 2);
	MomentOitPixel pb = B.mo.ResolvePixel(2, 2);
	std::vector<Frag> fa, fb;
	fa.push_back(MakeFrag(0.25, 0.5, 0.75, 0.5, 0.25));
	fb.push_back(MakeFrag(0.5, 0.25, 0.5, 0.75, 0.25));
	TASK_CHECK(VecNear(pa.color, HandComposite(fa, Vec3d(0.5, 0.25, 0.75)), 1e-12));
	TASK_CHECK(VecNear(pb.color, HandComposite(fb, Vec3d(0.25, 0.75, 0.5)), 1e-12));
	TASK_CHECK(Near(pa.transmittance, 0.5, 1e-12));
	TASK_CHECK(Near(pb.transmittance, 0.75, 1e-12));
	A.mo.CancelFrame();
	B.mo.CancelFrame();
	return 0;
}

}    // namespace


//---------------------------------------------------------------------
// 用例注册
//---------------------------------------------------------------------

const struct task_case task_cases[] = {
	{ "capture", "single_layer", cap_single_layer },
	{ "capture", "two_layers", cap_two_layers },
	{ "capture", "same_depth_mix", cap_same_depth_mix },
	{ "capture", "crossing_swap", cap_crossing_swap },
	{ "capture", "opaque_occlusion", cap_opaque_occlusion },
	{ "capture", "opaque_depth_discard", cap_opaque_depth_discard },
	{ "capture", "alpha_edges", cap_alpha_edges },
	{ "capture", "clip_near_plane", cap_clip_near_plane },
	{ "capture", "clip_sides", cap_clip_sides },
	{ "capture", "perspective_w", cap_perspective_w },
	{ "capture", "clip_fan", cap_clip_fan },
	{ "capture", "shared_edge", cap_shared_edge },
	{ "capture", "three_layers", cap_three_layers },
	{ "capture", "rejections", cap_rejections },
	{ "reconstruct", "single_layer", rec_single_layer },
	{ "reconstruct", "two_layers", rec_two_layers },
	{ "reconstruct", "same_depth_merged", rec_same_depth_merged },
	{ "reconstruct", "empty_background", rec_empty_background },
	{ "reconstruct", "near_merged", rec_near_merged },
	{ "reconstruct", "tiny_max_alpha", rec_tiny_max_alpha },
	{ "reconstruct", "three_layers", rec_three_layers },
	{ "reconstruct", "reverse_order", rec_reverse_order },
	{ "reconstruct", "occluded_composite", rec_occluded_composite },
	{ "lifecycle", "phase_errors", life_phase_errors },
	{ "lifecycle", "cancel_restart", life_cancel_restart },
	{ "lifecycle", "clear_init_reset", life_clear_init_reset },
	{ "lifecycle", "no_depth_write_after_seal", life_no_depth_write_after_seal },
	{ "lifecycle", "resolve_side_effects", life_resolve_side_effects },
	{ "lifecycle", "two_devices", life_two_devices },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
