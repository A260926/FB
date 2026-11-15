//=====================================================================
//
// soft_raster.cpp - 可微软光栅通道实现
//
// 前向：齐次 Sutherland-Hodgman 裁剪 -> 外边 smoothstep 覆盖 ->
//       透视校正 RGB / 线性 window-z -> log 空间稳定 softmax 深度竞争
// 反向：对 VS 输出的齐次 xyzw 与线性 RGB 做完整解析微分。裁剪交点、
//       屏幕重心、softmax 分母均不冻结；分支（空多边形、零长边、
//       clamp 区间）沿用前向选择，clamp 两端导数为 0。
//
//=====================================================================
#include "soft_raster.h"

#include <cmath>
#include <algorithm>
#include <limits>
#include <string>


namespace {

using std::vector;

static constexpr double kPlaneEps = 1e-12;   // 裁剪面 inside 判断余量
static constexpr double kZeroEdge = 1e-12;   // 零长边阈值
static constexpr double kZeroArea  = 1e-15;  // 零面积多边形阈值

inline double Cross2(const Vec2d& a, const Vec2d& b) {
	return vector_cross(a, b);
}

inline bool FiniteVec4(const Vec4d& v) {
	return std::isfinite(v.x) && std::isfinite(v.y) &&
	       std::isfinite(v.z) && std::isfinite(v.w);
}

inline bool FiniteVec3(const Vec3d& v) {
	return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// 齐次坐标 -> 屏幕坐标，约定与 RenderHelp::DrawPrimitive 一致：
// sx = (x/w + 1) * W / 2，sy = (1 - y/w) * H / 2
inline Vec2d ProjectScreen(const Vec4d& q, int width, int height) {
	double invw = 1.0 / q.w;
	return Vec2d((q.x * invw + 1.0) * 0.5 * width,
	             (1.0 - q.y * invw) * 0.5 * height);
}

// 屏幕坐标对 (x, y, z, w) 的雅可比（z 不影响屏幕位置）
inline void ProjectJacobian(Vec4d& jx, Vec4d& jy,
                            const Vec4d& q, int width, int height) {
	double invw = 1.0 / q.w;
	double invw2 = invw * invw;
	jx = Vec4d(0.5 * width * invw, 0.0, 0.0,
	           -0.5 * width * q.x * invw2);
	jy = Vec4d(0.0, -0.5 * height * invw, 0.0,
	            0.5 * height * q.y * invw2);
}

// g_screen 累积回齐次坐标
inline void AccumulateScreenToQuad(Vec4d& gq, const Vec2d& gs,
                                   const Vec4d& jx, const Vec4d& jy) {
	gq += jx * gs.x;
	gq += jy * gs.y;
}

// 六个裁剪面的有符号 inside 值，inside 条件 f >= 0：
// x/y ∈ [-w,w]，z ∈ [0,w]
inline double PlaneValue(int plane, const Vec4d& q) {
	switch (plane) {
	case 0: return q.w + q.x;   // x >= -w
	case 1: return q.w - q.x;   // x <=  w
	case 2: return q.w + q.y;   // y >= -w
	case 3: return q.w - q.y;   // y <=  w
	case 4: return q.z;         // z >=  0
	case 5: return q.w - q.z;   // z <=  w
	}
	return 0.0;
}

// 裁剪节点：下标 0..2 为原始三角角点，其余为线性插值交点
struct ClipNode {
	Vec2d s;
	Vec4d q;
	Vec3d c;
	int ia;          // 父节点下标，原始点为 -1
	int ib;
	int plane;       // 交点所在裁剪面，原始点为 -1
	double t;        // 交点参数，node = a + (b-a) * t
};

struct SampleEdge {
	int ia;          // 外边端点（裁剪节点下标）
	int ib;
	double d;        // 前向有符号朝内垂距
	double h;        // 前向 smoothstep 值
	bool active;     // 0 < d/sigma < 1，处于 clamp 中段才有非零导数
};

struct Sample {
	int face;
	int pixel;
	Vec3d rgb;        // 该面在该像素的透视校正 RGB
	double logw;      // log(p) - z/tau
	double p;
	double b[3];      // 原三角屏幕重心
	double k[3];      // 透视校正权重 k_i
	double r;         // Σ b_i / w_i
	double zwin;      // 屏幕线性插值 z/w
	vector<SampleEdge> edges;
};

struct FaceData {
	Vec4d q[3];
	Vec3d c[3];
	vector<ClipNode> nodes;
	vector<int> poly;
	double orient;    // 裁剪多边形绕序符号
	bool valid;
};

// 原三角屏幕重心：P = b0*s0 + b1*s1 + b2*s2
inline void ScreenBary(const Vec2d& P, const Vec2d& s0, const Vec2d& s1,
                       const Vec2d& s2, double b[3]) {
	Vec2d A = s1 - s0;
	Vec2d B = s2 - s0;
	double den = Cross2(A, B);
	double u = Cross2(P - s0, B) / den;
	double v = Cross2(A, P - s0) / den;
	b[1] = u;
	b[2] = v;
	b[0] = 1.0 - u - v;
}

// 重心坐标反向：给定 gb，回传屏幕点梯度（P 为固定采样点，gP 仅占位）
// u = cross(U, B)/D，v = cross(A, U)/D，D = cross(A, B)
// U = P - s0，A = s1 - s0，B = s2 - s0，b0 = 1 - u - v
void BaryBackprop(const Vec2d& P, const Vec2d& s0, const Vec2d& s1,
                  const Vec2d& s2, const double gb[3],
                  Vec2d& gP, Vec2d& g0, Vec2d& g1, Vec2d& g2) {
	double gu = gb[1] - gb[0];
	double gv = gb[2] - gb[0];
	Vec2d A = s1 - s0;
	Vec2d B = s2 - s0;
	Vec2d U = P - s0;
	double D = Cross2(A, B);

	Vec2d gU(0.0, 0.0), gA(0.0, 0.0), gB(0.0, 0.0);
	// u = (U.x*B.y - U.y*B.x) / D
	gU += Vec2d(B.y, -B.x) * (gu / D);
	gB += Vec2d(-U.y, U.x) * (gu / D);
	// v = (A.x*U.y - A.y*U.x) / D
	gA += Vec2d(U.y, -U.x) * (gv / D);
	gU += Vec2d(-A.y, A.x) * (gv / D);
	// 公共分母 D = A.x*B.y - A.y*B.x
	double gD = -gu * Cross2(U, B) / (D * D)
	          - gv * Cross2(A, U) / (D * D);
	gA += Vec2d(B.y, -B.x) * gD;
	gB += Vec2d(-A.y, A.x) * gD;

	gP += gU;
	g1 += gA;
	g2 += gB;
	g0 -= gU + gA + gB;
}

// 有符号朝内垂距 d = n . (P - a) 的反向（P 为固定采样点，不回传 gP）
// n = orient * (-ey, ex) / len，e = b - a
void DistanceBackprop(double gd, const Vec2d& P, const Vec2d& a,
                      const Vec2d& b, double orient,
                      Vec2d& gP, Vec2d& ga, Vec2d& gb) {
	Vec2d e = b - a;
	double len = vector_length(e);
	Vec2d dxy = P - a;
	double q = -e.y * dxy.x + e.x * dxy.y;
	double gq = gd * orient / len;
	double glen = -gd * orient * q / (len * len);

	double gdx = -e.y * gq;
	double gdy = e.x * gq;
	Vec2d ge(dxy.y * gq + e.x / len * glen,
	         -dxy.x * gq + e.y / len * glen);
	gP.x += gdx; gP.y += gdy;
	ga.x -= gdx; ga.y -= gdy;
	ga -= ge;
	gb += ge;
}

inline double SmoothStep(double s) {
	return s * s * (3.0 - 2.0 * s);
}

// 对单个面执行齐次裁剪，生成裁剪节点池与多边形顶点下标序列
bool ClipFace(FaceData& fd, int width, int height) {
	fd.nodes.clear();
	fd.poly.clear();
	fd.orient = 1.0;
	fd.valid = false;

	for (int k = 0; k < 3; k++) {
		ClipNode node;
		node.q = fd.q[k];
		node.c = fd.c[k];
		node.s = ProjectScreen(node.q, width, height);
		node.ia = node.ib = -1;
		node.plane = -1;
		node.t = 0.0;
		fd.nodes.push_back(node);
	}
	vector<int> in(3);
	in[0] = 0; in[1] = 1; in[2] = 2;

	for (int plane = 0; plane < 6; plane++) {
		vector<int> out;
		if (in.empty()) break;
		size_t n = in.size();
		for (size_t idx = 0; idx < n; idx++) {
			int ia = in[idx];
			int ib = in[(idx + 1) % n];
			const ClipNode& na = fd.nodes[ia];
			const ClipNode& nb = fd.nodes[ib];
			double fa = PlaneValue(plane, na.q);
			double fb = PlaneValue(plane, nb.q);
			double scale = 1.0 + std::max(std::fabs(fa), std::fabs(fb));
			bool ain = fa >= -kPlaneEps * scale;
			bool bin = fb >= -kPlaneEps * scale;

			if (ain) {
				out.push_back(ia);
				if (!bin) {
					double t = fa / (fa - fb);
					t = std::max(0.0, std::min(1.0, t));
					ClipNode m;
					m.ia = ia; m.ib = ib; m.plane = plane; m.t = t;
					m.q = na.q + (nb.q - na.q) * t;
					m.c = na.c + (nb.c - na.c) * t;
					// 屏幕位置由齐次插值点重新投影：透视非线性，不能在屏幕空间 lerp
					m.s = ProjectScreen(m.q, width, height);
					fd.nodes.push_back(m);
					out.push_back((int)fd.nodes.size() - 1);
				}
			} else if (bin) {
				double t = fa / (fa - fb);
				t = std::max(0.0, std::min(1.0, t));
				ClipNode m;
				m.ia = ia; m.ib = ib; m.plane = plane; m.t = t;
				m.q = na.q + (nb.q - na.q) * t;
				m.c = na.c + (nb.c - na.c) * t;
				// 屏幕位置由齐次插值点重新投影：透视非线性，不能在屏幕空间 lerp
				m.s = ProjectScreen(m.q, width, height);
				fd.nodes.push_back(m);
				out.push_back((int)fd.nodes.size() - 1);
			}
		}
		in.swap(out);
	}

	if (in.size() < 3) return false;
	fd.poly = in;

	// fan 面积（绕序保留自输入三角形），内部 fan 对角线不参与覆盖
	double area = 0.0;
	const Vec2d& s0 = fd.nodes[fd.poly[0]].s;
	for (size_t i = 1; i + 1 < fd.poly.size(); i++) {
		const Vec2d& s1 = fd.nodes[fd.poly[i]].s;
		const Vec2d& s2 = fd.nodes[fd.poly[i + 1]].s;
		area += Cross2(s1 - s0, s2 - s0);
	}
	area *= 0.5;
	if (std::fabs(area) <= kZeroArea) return false;
	fd.orient = (area > 0.0) ? 1.0 : -1.0;
	fd.valid = true;
	return true;
}

} // namespace


//---------------------------------------------------------------------
// SoftFrameRecord::Impl：深拷贝全部前向值
//---------------------------------------------------------------------
struct SoftFrameRecord::Impl {
	int width = 0;
	int height = 0;
	double sigma = 1.0;
	double tau = 1.0;
	Vec3d background;
	vector<FaceData> faces;
	vector<Sample> samples;
	vector<vector<int> > pixelSamples;   // 每像素对应的 sample 下标
	vector<double> image;                // 3*W*H double RGB
	vector<double> logZ;                 // 每像素 softmax 配分函数对数
	double bgLog = 0.0;                  // 背景 log 权重 -1/tau

	Impl() = default;
	Impl(const Impl&) = default;
	Impl(Impl&&) = default;
	Impl& operator = (const Impl&) = default;
	Impl& operator = (Impl&&) = default;
};


SoftFrameRecord::SoftFrameRecord() : _impl(new Impl()) {}
SoftFrameRecord::~SoftFrameRecord() = default;

SoftFrameRecord::SoftFrameRecord(const SoftFrameRecord& other)
	: _impl(new Impl(*other._impl)) {}

SoftFrameRecord::SoftFrameRecord(SoftFrameRecord&& other) noexcept = default;

SoftFrameRecord& SoftFrameRecord::operator = (const SoftFrameRecord& other) {
	if (this != &other) _impl.reset(new Impl(*other._impl));
	return *this;
}

SoftFrameRecord& SoftFrameRecord::operator = (SoftFrameRecord&& other) noexcept = default;

int SoftFrameRecord::Width() const { return _impl->width; }
int SoftFrameRecord::Height() const { return _impl->height; }
size_t SoftFrameRecord::FaceCount() const { return _impl->faces.size(); }


//---------------------------------------------------------------------
// SoftRasterContext 提交期缓存
//---------------------------------------------------------------------
struct SoftRasterContext::RawFace {
	Vec4d q[3];
	Vec3d c[3];
};


SoftRasterContext::SoftRasterContext(RenderHelp& rh)
	: _rh(rh), _active(false), _width(0), _height(0),
	  _color_slot(0), _sigma(1.0), _tau(1.0) {
}

SoftRasterContext::~SoftRasterContext() = default;

void SoftRasterContext::Begin(int width, int height, const Vec3f& background,
                              double sigma, double tau, int color_slot) {
	if (_active)
		throw std::logic_error("SoftRasterContext::Begin: frame already active");
	if (width <= 0 || height <= 0)
		throw std::invalid_argument("SoftRasterContext::Begin: bad image size");
	if (!(sigma > 0.0) || !std::isfinite(sigma))
		throw std::invalid_argument("SoftRasterContext::Begin: sigma must be > 0");
	if (!(tau > 0.0) || !std::isfinite(tau))
		throw std::invalid_argument("SoftRasterContext::Begin: tau must be > 0");
	if (!_rh.HasVertexShader())
		throw std::invalid_argument("SoftRasterContext::Begin: RenderHelp has no vertex shader");

	_active = true;
	_width = width;
	_height = height;
	_sigma = sigma;
	_tau = tau;
	_color_slot = color_slot;
	_background = Vec3d(background.x, background.y, background.z);
	_faces.clear();
}

void SoftRasterContext::SubmitTriangle() {
	if (!_active)
		throw std::logic_error("SoftRasterContext::SubmitTriangle: no active soft frame");

	RawFace face;
	for (int k = 0; k < 3; k++) {
		ShaderContext output;
		Vec4f pos = _rh.InvokeVertexShader(k, output);  // 每角恰好一次 VS
		auto it = output.varying_vec3f.find(_color_slot);
		if (it == output.varying_vec3f.end()) {
			_active = false;  // 回滚半开帧
			throw std::invalid_argument(
			    "SoftRasterContext::SubmitTriangle: missing varying_vec3f color slot");
		}
		Vec4d q(pos.x, pos.y, pos.z, pos.w);
		Vec3d c(it->second.x, it->second.y, it->second.z);
		if (!FiniteVec4(q) || !FiniteVec3(c)) {
			_active = false;
			throw std::invalid_argument(
			    "SoftRasterContext::SubmitTriangle: non-finite VS output");
		}
		if (!(q.w > 0.0)) {
			_active = false;
			throw std::invalid_argument(
			    "SoftRasterContext::SubmitTriangle: homogeneous w must be > 0");
		}
		face.q[k] = q;
		face.c[k] = c;
	}
	_faces.push_back(face);
}


SoftFrameRecord SoftRasterContext::End(std::vector<double>& rgb_image) {
	if (!_active)
		throw std::logic_error("SoftRasterContext::End: no active soft frame");

	std::unique_ptr<SoftFrameRecord::Impl> impl(new SoftFrameRecord::Impl());
	impl->width = _width;
	impl->height = _height;
	impl->sigma = _sigma;
	impl->tau = _tau;
	impl->background = _background;
	impl->bgLog = -1.0 / _tau;

	int npix = _width * _height;
	impl->image.assign((size_t)npix * 3, 0.0);
	impl->logZ.assign(npix, 0.0);
	impl->pixelSamples.assign(npix, vector<int>());

	// 逐面：裁剪、覆盖、透视 RGB、深度 log 权重
	for (size_t fi = 0; fi < _faces.size(); fi++) {
		FaceData fd;
		for (int k = 0; k < 3; k++) {
			fd.q[k] = _faces[fi].q[k];
			fd.c[k] = _faces[fi].c[k];
		}
		if (!ClipFace(fd, _width, _height)) {
			impl->faces.push_back(std::move(fd));  // 零贡献面，反传梯度为 0
			continue;
		}

		double loX = std::numeric_limits<double>::infinity();
		double hiX = -std::numeric_limits<double>::infinity();
		double loY = loX, hiY = hiX;
		for (int ni : fd.poly) {
			const Vec2d& s = fd.nodes[ni].s;
			loX = std::min(loX, s.x); hiX = std::max(hiX, s.x);
			loY = std::min(loY, s.y); hiY = std::max(hiY, s.y);
		}
		// 软边带在多边形外 sigma 像素内仍有非零覆盖，外扩按 sigma 取整
		int margin = (int)std::ceil(_sigma) + 1;
		int x0 = std::max(0, (int)std::floor(loX) - margin);
		int x1 = std::min(_width - 1, (int)std::ceil(hiX) + margin);
		int y0 = std::max(0, (int)std::floor(loY) - margin);
		int y1 = std::min(_height - 1, (int)std::ceil(hiY) + margin);

		const Vec2d& s0 = fd.nodes[0].s;
		const Vec2d& s1 = fd.nodes[1].s;
		const Vec2d& s2 = fd.nodes[2].s;

		for (int cy = y0; cy <= y1; cy++) {
			for (int cx = x0; cx <= x1; cx++) {
				Vec2d P((double)cx + 0.5, (double)cy + 0.5);

				// 外边覆盖：内部 fan 对角线不参与
				double p = 1.0;
				vector<SampleEdge> sedges;
				size_t m = fd.poly.size();
				for (size_t i = 0; i < m; i++) {
					int ia = fd.poly[i];
					int ib = fd.poly[(i + 1) % m];
					Vec2d e = fd.nodes[ib].s - fd.nodes[ia].s;
					double len = vector_length(e);
					if (len <= kZeroEdge) continue;  // 零长边忽略
					Vec2d n(-e.y / len, e.x / len);
					n = n * fd.orient;               // 朝内法向
					Vec2d a = fd.nodes[ia].s;
					double d = vector_dot(n, P - a); // 朝内为正
					double sv = d / _sigma;
					bool active = (sv > 0.0 && sv < 1.0);
					sv = std::max(0.0, std::min(1.0, sv));
					double h = SmoothStep(sv);
					p *= h;
					SampleEdge se;
					se.ia = ia; se.ib = ib;
					se.d = d; se.h = h; se.active = active;
					sedges.push_back(se);
					if (p == 0.0) break;
				}
				if (!(p > 0.0)) continue;

				// 原三角屏幕重心 + 透视校正（与 DrawPrimitive 同约定）
				double b[3];
				ScreenBary(P, s0, s1, s2, b);
				double r = b[0] / fd.q[0].w + b[1] / fd.q[1].w + b[2] / fd.q[2].w;
				if (!(r > 0.0) || !std::isfinite(r)) continue;
				double k[3];
				Vec3d rgb(0.0, 0.0, 0.0);
				for (int i = 0; i < 3; i++) {
					k[i] = b[i] / (fd.q[i].w * r);
					rgb += fd.c[i] * k[i];
				}
				// window-z：屏幕空间线性插值 z/w
				double zwin = b[0] * (fd.q[0].z / fd.q[0].w)
				            + b[1] * (fd.q[1].z / fd.q[1].w)
				            + b[2] * (fd.q[2].z / fd.q[2].w);

				double logw = std::log(p) - zwin / _tau;
				if (!std::isfinite(logw)) continue;

				Sample sm;
				sm.face = (int)fi;
				sm.pixel = cy * _width + cx;
				sm.rgb = rgb;
				sm.logw = logw;
				sm.p = p;
				sm.r = r;
				sm.zwin = zwin;
				for (int i = 0; i < 3; i++) { sm.b[i] = b[i]; sm.k[i] = k[i]; }
				sm.edges.swap(sedges);
				int sid = (int)impl->samples.size();
				impl->samples.push_back(std::move(sm));
				impl->pixelSamples[sm.pixel].push_back(sid);
			}
		}
		impl->faces.push_back(std::move(fd));
	}

	// 逐像素 log-sum-exp 稳定归一化（含背景），全程 double，不转字节
	for (int px = 0; px < npix; px++) {
		const vector<int>& ids = impl->pixelSamples[px];
		double maxLog = impl->bgLog;
		for (int id : ids)
			maxLog = std::max(maxLog, impl->samples[id].logw);
		double sumExp = std::exp(impl->bgLog - maxLog);
		for (int id : ids)
			sumExp += std::exp(impl->samples[id].logw - maxLog);
		double logZ = maxLog + std::log(sumExp);
		impl->logZ[px] = logZ;

		double qBg = std::exp(impl->bgLog - logZ);
		Vec3d col = impl->background * qBg;
		for (int id : ids) {
			double qj = std::exp(impl->samples[id].logw - logZ);
			col += impl->samples[id].rgb * qj;
		}
		impl->image[px * 3 + 0] = col.x;
		impl->image[px * 3 + 1] = col.y;
		impl->image[px * 3 + 2] = col.z;
	}

	rgb_image = impl->image;

	_active = false;
	SoftFrameRecord record;
	*record._impl = std::move(*impl);
	_faces.clear();
	return record;
}


std::vector<SoftFaceGradient> SoftRasterContext::Backward(
    const SoftFrameRecord& record, const std::vector<double>& adj_rgb) {
	const SoftFrameRecord::Impl& impl = *record._impl;
	int npix = impl.width * impl.height;
	if ((int)adj_rgb.size() != npix * 3)
		throw std::invalid_argument("SoftRasterContext::Backward: adj size mismatch");

	size_t nf = impl.faces.size();
	std::vector<SoftFaceGradient> grads(nf);
	for (size_t fi = 0; fi < nf; fi++) {
		for (int k = 0; k < 3; k++) {
			grads[fi].position[k] = Vec4d(0.0, 0.0, 0.0, 0.0);
			grads[fi].color[k] = Vec3d(0.0, 0.0, 0.0);
		}
	}

	// 每面独立的节点梯度工作区
	std::vector<std::vector<Vec4d> > gqNode(nf);
	std::vector<std::vector<Vec2d> > gsNode(nf);
	for (size_t fi = 0; fi < nf; fi++) {
		gqNode[fi].assign(impl.faces[fi].nodes.size(), Vec4d(0, 0, 0, 0));
		gsNode[fi].assign(impl.faces[fi].nodes.size(), Vec2d(0.0, 0.0));
	}

	for (int px = 0; px < npix; px++) {
		const std::vector<int>& ids = impl.pixelSamples[px];
		if (ids.empty()) continue;
		Vec3d gI(adj_rgb[px * 3 + 0], adj_rgb[px * 3 + 1], adj_rgb[px * 3 + 2]);
		if (gI.x == 0.0 && gI.y == 0.0 && gI.z == 0.0) continue;

		// softmax 反向：I = qBg*bg + Σ qj rgbj，qj = exp(logw_j - logZ)
		// dL/dlogw_j = qj * gI . (rgbj - I)，dL/drgbj = qj * gI
		double logZ = impl.logZ[px];
		Vec3d col(impl.image[px * 3 + 0],
		          impl.image[px * 3 + 1],
		          impl.image[px * 3 + 2]);

		for (int id : ids) {
			const Sample& sm = impl.samples[id];
			const FaceData& fd = impl.faces[sm.face];
			std::vector<Vec2d>& sn = gsNode[sm.face];

			double qj = std::exp(sm.logw - logZ);
			double gLogW = qj * vector_dot(gI, sm.rgb - col);
			Vec3d gRgb = gI * qj;
			if (gLogW == 0.0 &&
			    gRgb.x == 0.0 && gRgb.y == 0.0 && gRgb.z == 0.0) continue;

			// ---- RGB 透视链：rgb = Σ k_i c_i，k_i = b_i/(w_i r) ----
			double gk[3] = {0.0, 0.0, 0.0};
			for (int i = 0; i < 3; i++)
				gk[i] = vector_dot(gRgb, fd.c[i]);
			grads[sm.face].color[0] += gRgb * sm.k[0];
			grads[sm.face].color[1] += gRgb * sm.k[1];
			grads[sm.face].color[2] += gRgb * sm.k[2];

			double gR = 0.0;
			double gb[3] = {0.0, 0.0, 0.0};
			double gwc[3] = {0.0, 0.0, 0.0};
			for (int i = 0; i < 3; i++) {
				double wi = fd.q[i].w;
				gR += gk[i] * (-sm.b[i] / (wi * sm.r * sm.r));
				gb[i] += gk[i] / (wi * sm.r);
				gwc[i] += -gk[i] * sm.b[i] / (wi * wi * sm.r);
			}
			for (int i = 0; i < 3; i++) {
				double wi = fd.q[i].w;
				gb[i] += gR / wi;
				gwc[i] += gR * (-sm.b[i] / (wi * wi));
			}

			// ---- window-z 链：zwin = Σ b_i z_i/w_i ----
			double gZwin = -gLogW / impl.tau;
			for (int i = 0; i < 3; i++) {
				double wi = fd.q[i].w;
				double zi = fd.q[i].z;
				gb[i] += gZwin * (zi / wi);
				gwc[i] += gZwin * sm.b[i] * (-zi / (wi * wi));
				grads[sm.face].position[i].z += gZwin * sm.b[i] / wi;
			}

			// ---- 重心 -> 原三角屏幕点（P 为固定采样点，丢弃 gP）----
			Vec2d P(((sm.pixel % impl.width)) + 0.5,
			        ((sm.pixel / impl.width)) + 0.5);
			Vec2d gP(0.0, 0.0), g0(0.0, 0.0), g1(0.0, 0.0), g2(0.0, 0.0);
			BaryBackprop(P, fd.nodes[0].s, fd.nodes[1].s, fd.nodes[2].s,
			             gb, gP, g0, g1, g2);
			sn[0] += g0; sn[1] += g1; sn[2] += g2;

			// ---- 覆盖积 -> 外边距离，clamp 两端导数为 0 ----
			// logw 含 log(p)：d log(p)/dd_e = h'_e/(sigma h_e)
			for (size_t ei = 0; ei < sm.edges.size(); ei++) {
				const SampleEdge& se = sm.edges[ei];
				if (!se.active) continue;
				double sv = se.d / impl.sigma;
				double hs = 6.0 * sv * (1.0 - sv);   // smoothstep 导数
				double gd = gLogW * hs / (impl.sigma * se.h);
				Vec2d gPe(0.0, 0.0), ga(0.0, 0.0), gbe(0.0, 0.0);
				DistanceBackprop(gd, P, fd.nodes[se.ia].s,
				                 fd.nodes[se.ib].s, fd.orient,
				                 gPe, ga, gbe);
				sn[se.ia] += ga;
				sn[se.ib] += gbe;
			}

			// ---- 齐次 w 的直接项（透视分母与 window-z）----
			for (int i = 0; i < 3; i++)
				grads[sm.face].position[i].w += gwc[i];
		}
	}

	// 节点屏幕梯度 -> 齐次梯度，再沿裁剪交点拓扑反向到原始角点
	for (size_t fi = 0; fi < nf; fi++) {
		const FaceData& fd = impl.faces[fi];
		std::vector<Vec4d>& qn = gqNode[fi];
		std::vector<Vec2d>& sn = gsNode[fi];
		for (int ni = 0; ni < (int)fd.nodes.size(); ni++) {
			Vec4d jx, jy;
			ProjectJacobian(jx, jy, fd.nodes[ni].q, impl.width, impl.height);
			AccumulateScreenToQuad(qn[ni], sn[ni], jx, jy);
		}
		// 节点按下标递增创建，交点只引用更早节点：逆序传播。
		// m = a + (b-a) t，t = fa/(fa-fb) 本身也对父齐次坐标求导。
		for (int ni = (int)fd.nodes.size() - 1; ni >= 3; ni--) {
			const ClipNode& node = fd.nodes[ni];
			const Vec4d& gm = qn[ni];
			Vec4d qa = fd.nodes[node.ia].q;
			Vec4d qb = fd.nodes[node.ib].q;
			double fa = PlaneValue(node.plane, qa);
			double fb = PlaneValue(node.plane, qb);
			double denom = fa - fb;
			double t = node.t;

			Vec4d gaConst(0, 0, 0, 0), gbConst(0, 0, 0, 0), gt(0, 0, 0, 0);
			for (int c = 0; c < 4; c++) {
				double delta = qb[c] - qa[c];
				gaConst[c] = gm[c] * (1.0 - t);
				gbConst[c] = gm[c] * t;
				gt[c] = gm[c] * delta;
			}
			// t = fa / (fa - fb)：dfa/dqa 等由具体裁剪面决定
			Vec4d nrm(0, 0, 0, 0);
			switch (node.plane) {
			case 0: nrm = Vec4d(1, 0, 0, 1); break;  // w + x
			case 1: nrm = Vec4d(-1, 0, 0, 1); break; // w - x
			case 2: nrm = Vec4d(0, 1, 0, 1); break;  // w + y
			case 3: nrm = Vec4d(0, -1, 0, 1); break; // w - y
			case 4: nrm = Vec4d(0, 0, 1, 0); break;  // z
			case 5: nrm = Vec4d(0, 0, -1, 1); break; // w - z
			}
			double gtScalar = gt.x + gt.y + gt.z + gt.w;
			// d t / d fa = -fb / denom^2，d t / d fb = fa / denom^2
			double dtDfa = -fb / (denom * denom);
			double dtDfb =  fa / (denom * denom);
			Vec4d gaPlane = nrm * (gtScalar * dtDfa);
			Vec4d gbPlane = nrm * (gtScalar * dtDfb);

			qn[node.ia] += gaConst + gaPlane;
			qn[node.ib] += gbConst + gbPlane;
		}
		for (int k = 0; k < 3; k++) {
			grads[fi].position[k].x += qn[k].x;
			grads[fi].position[k].y += qn[k].y;
			grads[fi].position[k].z += qn[k].z;
			grads[fi].position[k].w += qn[k].w;
		}
	}

	return grads;
}
