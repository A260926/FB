//=====================================================================
//
// moment_oit.cpp - RenderHelp 的矩透明帧（Moment OIT）离屏预览扩展
//
// 实现要点：
//
// - 不透明/透明两个阶段共用同一条光栅化路径：齐次空间完整裁剪
//   （x/y ∈ [-w, w]，z ∈ [0, w]，交点携带全部 varying）、屏幕 y 向下
//   的半开覆盖规则（先整理成有向面积为正，向上的边和向右的水平边
//   包含边界）、像素中心按透视权重恢复属性。
// - 深度是屏幕上线性插值的 z/w，越小越近，背景深度是正无穷。
//   不透明面只有在严格更近、并且 PS 没丢弃之后才写颜色/深度；
//   透明面只在严格位于封存深度之前时才有贡献，且不写深度。
// - 透明阶段每像素只保留 10 个 double 的矩（另有 10 个 Kahan 补偿
//   累加量）：tau = -log1p(-alpha)，b[k] = sum(tau * z^k)，
//   g[c][k] = sum(tau * RGB[c] * z^k)。不存片元链表。
// - 解析时从 b[0..3] 恢复至多两个有序深度和正的光学质量（正测度的
//   二节点高斯求积），再从 g 恢复层色，由近到远合成，最后加上
//   exp(-b[0]) * 封存颜色。
//
//=====================================================================
#include "moment_oit.h"

#include <cmath>
#include <stdexcept>


namespace moment_oit_detail {

//---------------------------------------------------------------------
// 工具
//---------------------------------------------------------------------

inline double Clamp01(double x) {
	if (!(x > 0.0)) return 0.0;    // 同时把 NaN 归 0
	if (x > 1.0) return 1.0;
	return x;
}

inline bool IsFinite(const Vec4f &v) {
	return std::isfinite(v.x) && std::isfinite(v.y) &&
	       std::isfinite(v.z) && std::isfinite(v.w);
}

// Kahan 补偿累加：固定数量的补偿量，换取接近无误差的顺序无关求和
inline void KahanAdd(double &sum, double &comp, double value) {
	double y = value - comp;
	double t = sum + y;
	comp = (t - sum) - y;
	sum = t;
}

//---------------------------------------------------------------------
// varying 读写：输入属性槽在三个角上一致（缺失的键按 0 处理，
// 与 RenderHelp 普通帧的插值语义一致）
//---------------------------------------------------------------------

inline float GetVarying(const ShaderContext &ctx, int key, float) {
	auto it = ctx.varying_float.find(key);
	return (it == ctx.varying_float.end())? 0.0f : it->second;
}

inline Vec2f GetVarying(const ShaderContext &ctx, int key, Vec2f) {
	auto it = ctx.varying_vec2f.find(key);
	return (it == ctx.varying_vec2f.end())? Vec2f(0.0f, 0.0f) : it->second;
}

inline Vec3f GetVarying(const ShaderContext &ctx, int key, Vec3f) {
	auto it = ctx.varying_vec3f.find(key);
	return (it == ctx.varying_vec3f.end())? Vec3f(0.0f, 0.0f, 0.0f) : it->second;
}

inline Vec4f GetVarying(const ShaderContext &ctx, int key, Vec4f) {
	auto it = ctx.varying_vec4f.find(key);
	return (it == ctx.varying_vec4f.end())? Vec4f(0.0f, 0.0f, 0.0f, 0.0f) : it->second;
}

// 裁剪交点的 varying 线性插值（在齐次裁剪空间随位置一起插值）
inline ShaderContext LerpContext(const ShaderContext &a, const ShaderContext &b, double t) {
	ShaderContext out;
	double s = 1.0 - t;
	for (auto const &it : a.varying_float) {
		int key = it.first;
		out.varying_float[key] = (float)(s * it.second + t * GetVarying(b, key, float()));
	}
	for (auto const &it : a.varying_vec2f) {
		int key = it.first;
		Vec2f vb = GetVarying(b, key, Vec2f());
		out.varying_vec2f[key] = Vec2f((float)(s * it.second.x + t * vb.x),
		                               (float)(s * it.second.y + t * vb.y));
	}
	for (auto const &it : a.varying_vec3f) {
		int key = it.first;
		Vec3f vb = GetVarying(b, key, Vec3f());
		out.varying_vec3f[key] = Vec3f((float)(s * it.second.x + t * vb.x),
		                               (float)(s * it.second.y + t * vb.y),
		                               (float)(s * it.second.z + t * vb.z));
	}
	for (auto const &it : a.varying_vec4f) {
		int key = it.first;
		Vec4f vb = GetVarying(b, key, Vec4f());
		out.varying_vec4f[key] = Vec4f((float)(s * it.second.x + t * vb.x),
		                               (float)(s * it.second.y + t * vb.y),
		                               (float)(s * it.second.z + t * vb.z),
		                               (float)(s * it.second.w + t * vb.w));
	}
	return out;
}

// 像素中心的 varying 透视插值：c0/c1/c2 是带透视权重的系数
inline void InterpContext(ShaderContext &out,
                          const ShaderContext &i0, const ShaderContext &i1, const ShaderContext &i2,
                          double c0, double c1, double c2) {
	for (auto const &it : i0.varying_float) {
		int key = it.first;
		out.varying_float[key] = (float)(c0 * it.second + c1 * GetVarying(i1, key, float())
		                                 + c2 * GetVarying(i2, key, float()));
	}
	for (auto const &it : i0.varying_vec2f) {
		int key = it.first;
		Vec2f v1 = GetVarying(i1, key, Vec2f()), v2 = GetVarying(i2, key, Vec2f());
		out.varying_vec2f[key] = Vec2f((float)(c0 * it.second.x + c1 * v1.x + c2 * v2.x),
		                               (float)(c0 * it.second.y + c1 * v1.y + c2 * v2.y));
	}
	for (auto const &it : i0.varying_vec3f) {
		int key = it.first;
		Vec3f v1 = GetVarying(i1, key, Vec3f()), v2 = GetVarying(i2, key, Vec3f());
		out.varying_vec3f[key] = Vec3f((float)(c0 * it.second.x + c1 * v1.x + c2 * v2.x),
		                               (float)(c0 * it.second.y + c1 * v1.y + c2 * v2.y),
		                               (float)(c0 * it.second.z + c1 * v1.z + c2 * v2.z));
	}
	for (auto const &it : i0.varying_vec4f) {
		int key = it.first;
		Vec4f v1 = GetVarying(i1, key, Vec4f()), v2 = GetVarying(i2, key, Vec4f());
		out.varying_vec4f[key] = Vec4f((float)(c0 * it.second.x + c1 * v1.x + c2 * v2.x),
		                               (float)(c0 * it.second.y + c1 * v1.y + c2 * v2.y),
		                               (float)(c0 * it.second.z + c1 * v1.z + c2 * v2.z),
		                               (float)(c0 * it.second.w + c1 * v1.w + c2 * v2.w));
	}
}

//---------------------------------------------------------------------
// 齐次空间裁剪
//---------------------------------------------------------------------

// 裁剪多边形顶点：双精度齐次坐标 + varying
struct ClipVertex {
	Vec4d clip;              // 齐次裁剪空间坐标
	ShaderContext ctx;       // 顶点 varying
};

// 六个半空间：x >= -w, x <= w, y >= -w, y <= w, z >= 0, z <= w
inline double PlaneValue(const Vec4d &c, int plane) {
	switch (plane) {
	case 0: return c.x + c.w;
	case 1: return c.w - c.x;
	case 2: return c.y + c.w;
	case 3: return c.w - c.y;
	case 4: return c.z;
	default: return c.w - c.z;
	}
}

inline ClipVertex LerpVertex(const ClipVertex &a, const ClipVertex &b, double t) {
	ClipVertex out;
	out.clip = a.clip + (b.clip - a.clip) * t;
	out.ctx = LerpContext(a.ctx, b.ctx, t);
	return out;
}

// Sutherland-Hodgman：用一个半空间裁剪多边形，交点携带全部 varying
inline void ClipPolygon(std::vector<ClipVertex> &poly, int plane) {
	std::vector<ClipVertex> out;
	size_t n = poly.size();
	for (size_t i = 0; i < n; i++) {
		const ClipVertex &cur = poly[i];
		const ClipVertex &prv = poly[(i + n - 1) % n];
		double fc = PlaneValue(cur.clip, plane);
		double fp = PlaneValue(prv.clip, plane);
		bool inc = (fc >= 0.0), inp = (fp >= 0.0);
		if (inc) {
			if (!inp) {    // 由外入内：先补交点
				double t = fp / (fp - fc);
				out.push_back(LerpVertex(prv, cur, t));
			}
			out.push_back(cur);
		}
		else if (inp) {    // 由内出外：补交点
			double t = fp / (fp - fc);
			out.push_back(LerpVertex(prv, cur, t));
		}
	}
	poly.swap(out);
}

//---------------------------------------------------------------------
// 光栅化
//---------------------------------------------------------------------

// 投影后的多边形顶点
struct RastVertex {
	ShaderContext ctx;    // varying
	double rhw;           // 1/w
	double z;             // z/w，深度，越小越近
	double sx, sy;        // 屏幕坐标（y 向下）
};

// 一条屏幕边：半开覆盖规则用
// 为了共边的一致性，边函数一律按字典序规范方向求值，再按三角形自身
// 的边方向取符号：共享同一条边的两个三角形用同一组端点、同一个表达式
// 计算，结果逐位一致，符号相反，配合半开规则保证共边恰好覆盖一次。
struct Edge {
	double ux, uy, vx, vy;    // 规范方向端点（字典序小的在前）
	bool flip;                // 三角形边方向与规范方向相反
	bool inclusive;           // 三角形方向是向上的边或向右的水平边
};

inline Edge MakeEdge(double ax, double ay, double bx, double by) {
	Edge e;
	if (ax < bx || (ax == bx && ay <= by)) {
		e.ux = ax; e.uy = ay; e.vx = bx; e.vy = by; e.flip = false;
	}
	else {
		e.ux = bx; e.uy = by; e.vx = ax; e.vy = ay; e.flip = true;
	}
	// 屏幕 y 向下：向上的边（dy < 0）和向右的水平边（dy == 0 且 dx > 0）包含边界
	e.inclusive = (by < ay) || (by == ay && bx > ax);
	return e;
}

inline double EvalEdge(const Edge &e, double px, double py) {
	double v = (e.vx - e.ux) * (py - e.uy) - (e.vy - e.uy) * (px - e.ux);
	return e.flip? -v : v;
}

}    // namespace moment_oit_detail


using namespace moment_oit_detail;


//---------------------------------------------------------------------
// 生命周期
//---------------------------------------------------------------------

MomentOit::MomentOit(RenderHelp &host) {
	_host = &host;
	_phase = PHASE_IDLE;
	_w = _h = 0;
}

MomentOit::~MomentOit() {
	// 卸载钩子，避免宿主之后把绘制转发到已析构的对象
	if (_host != NULL && _host->GetMomentHook() == this)
		_host->SetMomentHook(NULL);
	_host = NULL;
}

void MomentOit::BeginFrame(const Vec3d &background) {
	// 先检查，后修改：错误阶段一律抛 std::logic_error
	if (_phase != PHASE_IDLE)
		throw std::logic_error("MomentOit::BeginFrame: frame already active");
	if (_host == NULL || _host->_frame_buffer == NULL)
		throw std::logic_error("MomentOit::BeginFrame: host not initialized");
	if (_host->GetMomentHook() != NULL && _host->GetMomentHook() != this)
		throw std::logic_error("MomentOit::BeginFrame: host used by another moment frame");

	_w = _host->_fb_width;
	_h = _host->_fb_height;
	size_t n = (size_t)_w * (size_t)_h;

	// 封存颜色初始化为线性 RGB 背景，窗口深度初始化为正无穷，矩清零
	_color.assign(n * 3, 0.0);
	for (size_t i = 0; i < n; i++) {
		_color[i * 3 + 0] = background.x;
		_color[i * 3 + 1] = background.y;
		_color[i * 3 + 2] = background.z;
	}
	_depth.assign(n, HUGE_VAL);
	_mom.assign(n, PixelMoments());

	_host->SetMomentHook(this);
	_phase = PHASE_OPAQUE;
}

void MomentOit::SealOpaque() {
	if (_phase != PHASE_OPAQUE)
		throw std::logic_error("MomentOit::SealOpaque: not in opaque phase");
	// 封存是概念性的：透明阶段只读这些缓冲，不再写入
	_phase = PHASE_SEALED;
}

void MomentOit::CancelFrame() {
	_phase = PHASE_IDLE;
	_w = _h = 0;
	_color.clear();
	_depth.clear();
	_mom.clear();
	if (_host != NULL && _host->GetMomentHook() == this)
		_host->SetMomentHook(NULL);
}

void MomentOit::OnFrameReset() {
	// 宿主的 Clear/Init/Reset 会取消活动中的矩帧
	CancelFrame();
}

void MomentOit::RequireSealed() const {
	if (_phase != PHASE_SEALED)
		throw std::logic_error("MomentOit: frame not sealed");
}

void MomentOit::RequireCoord(int x, int y) const {
	if (x < 0 || x >= _w || y < 0 || y >= _h)
		throw std::out_of_range("MomentOit: pixel coordinate out of range");
}


//---------------------------------------------------------------------
// 矩帧自己的光栅化路径（DrawPrimitive 在矩帧激活期间转发到这里）
//---------------------------------------------------------------------

bool MomentOit::OnDrawPrimitive(RenderHelp &rh) {
	if (_phase == PHASE_IDLE) return false;    // 防御：正常不会到这里
	return Rasterize(rh, _phase == PHASE_SEALED);
}

bool MomentOit::Rasterize(RenderHelp &rh, bool transparent) {
	if (rh._vertex_shader == NULL) return false;

	// 每次提交，三个角的 VS 各执行一次（先全部执行，再统一校验）
	ClipVertex tri[3];
	Vec4f pos[3];
	for (int k = 0; k < 3; k++)
		pos[k] = rh._vertex_shader(k, tri[k].ctx);
	for (int k = 0; k < 3; k++) {
		// 只接受有限且 w > 0 的三角形
		if (!IsFinite(pos[k]) || !(pos[k].w > 0.0f)) return false;
		tri[k].clip = Vec4d(pos[k].x, pos[k].y, pos[k].z, pos[k].w);
	}

	// 齐次空间完整裁剪：x/y ∈ [-w, w]，z ∈ [0, w]，交点携带全部 varying
	std::vector<ClipVertex> poly(tri, tri + 3);
	for (int plane = 0; plane < 6 && poly.size() >= 3; plane++)
		ClipPolygon(poly, plane);
	if (poly.size() < 3) return false;

	// 投影到屏幕：深度取 z/w，1/w 供透视插值用
	size_t n = poly.size();
	std::vector<RastVertex> rv(n);
	for (size_t i = 0; i < n; i++) {
		const Vec4d &c = poly[i].clip;
		if (!(c.w > 0.0)) return false;    // 防御：裁剪后 w 恒正
		double rhw = 1.0 / c.w;
		rv[i].ctx = poly[i].ctx;
		rv[i].rhw = rhw;
		rv[i].z = c.z * rhw;
		rv[i].sx = (c.x * rhw + 1.0) * _w * 0.5;
		rv[i].sy = (1.0 - c.y * rhw) * _h * 0.5;
	}

	// 屏幕 y 向下，先整理成有向面积为正（双面填充）
	double area2 = 0.0;
	for (size_t i = 0; i < n; i++) {
		size_t j = (i + 1) % n;
		area2 += rv[i].sx * rv[j].sy - rv[j].sx * rv[i].sy;
	}
	if (area2 == 0.0) return false;    // 退化多边形
	if (area2 < 0.0) {
		for (size_t i = 1, j = n - 1; i < j; i++, j--) {
			RastVertex tmp = rv[i];
			rv[i] = rv[j];
			rv[j] = tmp;
		}
	}

	// 裁剪后的多边形是凸的，按扇形拆成三角形逐个光栅化；
	// 扇形的内部边和相邻面的共边都按半开覆盖处理，恰好覆盖一次
	for (size_t i = 1; i + 1 < n; i++) {
		const RastVertex &v0 = rv[0];
		const RastVertex &v1 = rv[i];
		const RastVertex &v2 = rv[i + 1];

		// 三角形的有向面积（扇形三角形与多边形同向，面积为正）
		double tri_area2 = (v1.sx - v0.sx) * (v2.sy - v0.sy)
		                 - (v2.sx - v0.sx) * (v1.sy - v0.sy);
		if (!(tri_area2 > 0.0)) continue;    // 跳过退化扇形三角形

		Edge e01 = MakeEdge(v0.sx, v0.sy, v1.sx, v1.sy);
		Edge e12 = MakeEdge(v1.sx, v1.sy, v2.sx, v2.sy);
		Edge e20 = MakeEdge(v2.sx, v2.sy, v0.sx, v0.sy);

		// 外接矩形（取宽松边界，覆盖判断以下面的边函数为准）
		double minx = Min(Min(v0.sx, v1.sx), v2.sx);
		double maxx = Max(Max(v0.sx, v1.sx), v2.sx);
		double miny = Min(Min(v0.sy, v1.sy), v2.sy);
		double maxy = Max(Max(v0.sy, v1.sy), v2.sy);
		int x0 = Max(0, (int)floor(minx - 0.5));
		int x1 = Min(_w - 1, (int)ceil(maxx - 0.5));
		int y0 = Max(0, (int)floor(miny - 0.5));
		int y1 = Min(_h - 1, (int)ceil(maxy - 0.5));

		for (int cy = y0; cy <= y1; cy++) {
			for (int cx = x0; cx <= x1; cx++) {
				double px = cx + 0.5, py = cy + 0.5;

				// 半开覆盖：E > 0 在内部；E == 0 时由包含标志决定
				double E01 = EvalEdge(e01, px, py);
				if (E01 < 0.0 || (E01 == 0.0 && !e01.inclusive)) continue;
				double E12 = EvalEdge(e12, px, py);
				if (E12 < 0.0 || (E12 == 0.0 && !e12.inclusive)) continue;
				double E20 = EvalEdge(e20, px, py);
				if (E20 < 0.0 || (E20 == 0.0 && !e20.inclusive)) continue;

				// 重心坐标
				double l0 = E12 / tri_area2;
				double l1 = E20 / tri_area2;
				double l2 = E01 / tri_area2;

				// 1/w 和 z/w 在屏幕上都是线性的，直接重心插值
				double rhw = v0.rhw * l0 + v1.rhw * l1 + v2.rhw * l2;
				double z = v0.z * l0 + v1.z * l1 + v2.z * l2;
				double w = 1.0 / rhw;

				// 透视权重，恢复像素中心的属性
				double c0 = v0.rhw * l0 * w;
				double c1 = v1.rhw * l1 * w;
				double c2 = v2.rhw * l2 * w;

				size_t idx = (size_t)cy * (size_t)_w + (size_t)cx;

				if (!transparent) {
					// 不透明阶段：严格更近才考虑，PS 没丢弃才写颜色/深度
					if (!(z < _depth[idx])) continue;
					ShaderContext input;
					InterpContext(input, v0.ctx, v1.ctx, v2.ctx, c0, c1, c2);
					Vec4f color(0.0f, 0.0f, 0.0f, 0.0f);
					if (rh._pixel_shader != NULL)
						color = rh._pixel_shader(input);
					if (!(color.a > 0.0f)) continue;    // alpha <= 0 表示丢弃
					_depth[idx] = z;
					_color[idx * 3 + 0] = Clamp01(color.r);
					_color[idx * 3 + 1] = Clamp01(color.g);
					_color[idx * 3 + 2] = Clamp01(color.b);
				}
				else {
					// 透明阶段：只在严格位于封存深度之前时才有贡献，不占深度
					if (!(z < _depth[idx])) continue;
					ShaderContext input;
					InterpContext(input, v0.ctx, v1.ctx, v2.ctx, c0, c1, c2);
					Vec4f color(0.0f, 0.0f, 0.0f, 0.0f);
					if (rh._pixel_shader != NULL)
						color = rh._pixel_shader(input);
					double alpha = color.a;
					if (!(alpha > 0.0)) continue;    // alpha = 0（或非法值）不贡献
					if (alpha > 1.0 - 1e-6) alpha = 1.0 - 1e-6;
					double cr = Clamp01(color.r);
					double cg = Clamp01(color.g);
					double cb = Clamp01(color.b);

					// 累加光学矩和颜色矩（Kahan 补偿，固定 10+10 个 double）
					double tau = -log1p(-alpha);
					PixelMoments &m = _mom[idx];
					double zp = 1.0;
					for (int k = 0; k < 4; k++) {
						KahanAdd(m.b[k], m.cb[k], tau * zp);
						zp *= z;
					}
					double tr = tau * cr, tg = tau * cg, tb = tau * cb;
					KahanAdd(m.g[0][0], m.cg[0][0], tr);
					KahanAdd(m.g[0][1], m.cg[0][1], tr * z);
					KahanAdd(m.g[1][0], m.cg[1][0], tg);
					KahanAdd(m.g[1][1], m.cg[1][1], tg * z);
					KahanAdd(m.g[2][0], m.cg[2][0], tb);
					KahanAdd(m.g[2][1], m.cg[2][1], tb * z);
				}
			}
		}
	}

	return true;
}


//---------------------------------------------------------------------
// 只读解析
//---------------------------------------------------------------------

MomentOitMoments MomentOit::GetMoments(int x, int y) const {
	RequireSealed();
	RequireCoord(x, y);
	MomentOitMoments out;
	const PixelMoments &m = _mom[(size_t)y * (size_t)_w + (size_t)x];
	for (int k = 0; k < 4; k++) out.b[k] = m.b[k];
	for (int c = 0; c < 3; c++) {
		out.g[c][0] = m.g[c][0];
		out.g[c][1] = m.g[c][1];
	}
	return out;
}

namespace moment_oit_detail {

// 从四个光学矩恢复至多两个有序深度 d0 <= d1 和正的光学质量 w0, w1，
// 使 sum(wj * dj^k) = b[k]（k = 0..3），即正测度的二节点高斯求积。
// 用中心矩求解避免相消：q^2 = 4*var + (c3/var)^2 恒正，权重恒正。
// 返回节点个数（0 = 空像素，1 = 单深度/近合并，2 = 双深度）。
inline int SolveNodes(const double b[4], double d[2], double w[2]) {
	double B = b[0];
	if (!(B > 0.0)) return 0;    // 空像素
	double m1 = b[1] / B;
	double m2 = b[2] / B;
	double m3 = b[3] / B;
	double var = m2 - m1 * m1;    // 归一化方差 = w0' * w1' * (d1 - d0)^2
	// 单深度或近合并：退化成一个节点。阈值跟着矩的尺度走（约为 var 的
	// 浮点噪声下限的百倍），只合并噪声不可分的节点，不会删掉质量很小
	// 但真实存在的薄层。
	double var_tol = 1e-14 * m2;
	if (!(var > var_tol)) {
		d[0] = Clamp01(m1);
		w[0] = B;
		return 1;
	}
	// 三阶中心矩给出权重不对称度：c3/var = -(w1' - w0') * (d1 - d0)
	double c3 = m3 - 3.0 * m2 * m1 + 2.0 * m1 * m1 * m1;
	double r = c3 / var;
	double q = sqrt(4.0 * var + r * r);    // 节点间距，无相消
	double s = -r / q;                     // w1' - w0'，|s| < 1 恒成立
	double w0 = 0.5 * (1.0 - s);
	double w1 = 0.5 * (1.0 + s);
	double d0 = m1 - w1 * q;
	double d1 = m1 + w0 * q;
	if (!(d1 > d0)) {             // 数值兜底：退化成单节点
		d[0] = Clamp01(m1);
		w[0] = B;
		return 1;
	}
	d[0] = Clamp01(d0);
	d[1] = Clamp01(d1);
	w[0] = w0 * B;
	w[1] = w1 * B;
	return 2;
}

}    // namespace moment_oit_detail


MomentOitPixel MomentOit::ResolvePixel(int x, int y) const {
	RequireSealed();
	RequireCoord(x, y);
	size_t idx = (size_t)y * (size_t)_w + (size_t)x;
	const PixelMoments &m = _mom[idx];

	MomentOitPixel out;
	out.transmittance = exp(-m.b[0]);    // 空像素 b[0] = 0，透射率为 1

	double d[2], w[2];
	int nlayers = SolveNodes(m.b, d, w);

	double acc[3] = { 0.0, 0.0, 0.0 };
	double reach = 1.0;    // 到达当前层的透射率
	for (int j = 0; j < nlayers; j++) {
		MomentOitLayer layer;
		layer.depth = d[j];
		layer.optical_mass = w[j];
		layer.alpha = -expm1(-w[j]);
		for (int c = 0; c < 3; c++) {
			double G0 = m.g[c][0];
			double G1 = m.g[c][1];
			double uc;
			if (nlayers == 1) {
				uc = G0 / w[0];
			}
			else if (j == 0) {
				uc = (G0 * d[1] - G1) / (w[0] * (d[1] - d[0]));
			}
			else {
				uc = (G1 - G0 * d[0]) / (w[1] * (d[1] - d[0]));
			}
			layer.unclamped[c] = uc;          // 未钳制，便于核对矩
			layer.color[c] = Clamp01(uc);     // 合成用钳制后的层色
		}
		// 由近到远合成
		for (int c = 0; c < 3; c++)
			acc[c] += reach * layer.alpha * layer.color[c];
		reach *= exp(-w[j]);
		out.layers.push_back(layer);
	}

	// 最后加上 exp(-b[0]) * 封存颜色
	for (int c = 0; c < 3; c++)
		out.color[c] = acc[c] + out.transmittance * _color[idx * 3 + c];

	return out;
}

std::vector<MomentOitPixel> MomentOit::ResolveFrame() const {
	RequireSealed();
	std::vector<MomentOitPixel> out;
	out.reserve((size_t)_w * (size_t)_h);
	for (int y = 0; y < _h; y++)
		for (int x = 0; x < _w; x++)
			out.push_back(ResolvePixel(x, y));
	return out;
}
