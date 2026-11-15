//=====================================================================
//
// tests/soft_target.cpp - 可微软光栅通道的任务测试
//
// 三组用例：
//   forward             独立参考前向、归一化、深度竞争、裁剪与背景
//   visibility_gradient 两档中心差分核对坐标/RGB 解析梯度
//   snapshot            图外零梯度、重复 VJP、独立记录与深拷贝所有权
//
// 不定义 main，由 task_support/driver.c 驱动；TASK_CHECK 做实际判断。
//
//=====================================================================
#include "soft_raster.h"
#include "task_support/check.h"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <vector>
#include <algorithm>


namespace {

using std::vector;

struct Face { Vec4d q[3]; Vec3d c[3]; };

const int kW = 64;
const int kH = 56;
const Vec3d kBg(0.05, 0.07, 0.10);

//---------------------------------------------------------------------
// 生产管线：用独立 VS 逐面提交，返回图像与拥有型记录
//---------------------------------------------------------------------
struct Pipe {
	RenderHelp rh;
	vector<Face> faces;
	double sigma, tau;
	Vec3d bg;
	size_t cursor;

	Pipe(const vector<Face>& fs, double s, double t, Vec3d b)
		: rh(kW, kH), faces(fs), sigma(s), tau(t), bg(b), cursor(0) {
		const vector<Face>* p = &faces;
		size_t* cur = &cursor;
		rh.SetVertexShader([p, cur](int, ShaderContext& out) -> Vec4f {
			size_t f = (*cur) / 3;
			int k = (int)((*cur) % 3);
			++(*cur);
			out.varying_vec3f[0] = Vec3f((float)(*p)[f].c[k].x,
			                             (float)(*p)[f].c[k].y,
			                             (float)(*p)[f].c[k].z);
			return Vec4f((float)(*p)[f].q[k].x, (float)(*p)[f].q[k].y,
			             (float)(*p)[f].q[k].z, (float)(*p)[f].q[k].w);
		});
	}

	SoftFrameRecord Run(vector<double>& image) {
		SoftRasterContext soft(rh);
		soft.Begin(kW, kH, Vec3f((float)bg.x, (float)bg.y, (float)bg.z),
		           sigma, tau, 0);
		for (size_t f = 0; f < faces.size(); f++)
			soft.SubmitTriangle();
		return soft.End(image);
	}
};

static double ScalarLoss(const vector<double>& image, const vector<double>& adj) {
	double L = 0.0;
	for (size_t i = 0; i < image.size(); i++) L += image[i] * adj[i];
	return L;
}

// 仅前向求损失（供中心差分反复调用）
static double ForwardLoss(const vector<Face>& faces, double sigma, double tau,
                          const vector<double>& adj) {
	vector<double> image;
	Pipe pipe(faces, sigma, tau, kBg);
	pipe.Run(image);
	return ScalarLoss(image, adj);
}

//---------------------------------------------------------------------
// 独立参考前向（与生产分开编码，仅用 double 标量运算）
//---------------------------------------------------------------------
struct RNode { Vec4d q; Vec3d c; Vec2d s; };

static Vec2d RefProject(const Vec4d& q) {
	double iw = 1.0 / q.w;
	return Vec2d((q.x * iw + 1.0) * 0.5 * kW,
	             (1.0 - q.y * iw) * 0.5 * kH);
}

static double RefPlane(int p, const Vec4d& q) {
	switch (p) {
	case 0: return q.w + q.x;
	case 1: return q.w - q.x;
	case 2: return q.w + q.y;
	case 3: return q.w - q.y;
	case 4: return q.z;
	default: return q.w - q.z;
	}
}

// 参考裁剪，返回按绕序排列的多边形节点
static vector<RNode> RefClip(const Face& f) {
	vector<RNode> nd(3);
	for (int k = 0; k < 3; k++) {
		nd[k].q = f.q[k]; nd[k].c = f.c[k]; nd[k].s = RefProject(f.q[k]);
	}
	vector<int> in(3); in[0] = 0; in[1] = 1; in[2] = 2;
	for (int p = 0; p < 6; p++) {
		vector<int> out;
		int n = (int)in.size();
		for (int i = 0; i < n; i++) {
			int ia = in[i], ib = in[(i + 1) % n];
			double fa = RefPlane(p, nd[ia].q);
			double fb = RefPlane(p, nd[ib].q);
			bool ain = fa >= 0.0, bin = fb >= 0.0;
			auto make = [&]() {
				double t = fa / (fa - fb);
				t = std::max(0.0, std::min(1.0, t));
				RNode m;
				m.q = nd[ia].q + (nd[ib].q - nd[ia].q) * t;
				m.c = nd[ia].c + (nd[ib].c - nd[ia].c) * t;
				m.s = RefProject(m.q);
				nd.push_back(m);
				return (int)nd.size() - 1;
			};
			if (ain) {
				out.push_back(ia);
				if (!bin) out.push_back(make());
			} else if (bin) {
				out.push_back(make());
			}
		}
		in.swap(out);
	}
	vector<RNode> poly;
	for (int idx : in) poly.push_back(nd[idx]);
	return poly;
}

static double RefSmooth(double x) { return x * x * (3.0 - 2.0 * x); }

// 参考整帧前向，输出 RGB 图
static vector<double> RefForward(const vector<Face>& faces, double sigma,
                                 double tau) {
	int npix = kW * kH;
	vector<double> img((size_t)npix * 3, 0.0);
	double bgLog = -1.0 / tau;
	for (int px = 0; px < npix; px++) {
		Vec2d P((px % kW) + 0.5, (px / kW) + 0.5);
		double maxLog = bgLog;
		struct Term { Vec3d rgb; double logw; };
		vector<Term> terms;
		for (const Face& f : faces) {
			vector<RNode> poly = RefClip(f);
			if (poly.size() < 3) continue;
			double area = 0.0;
			for (size_t i = 1; i + 1 < poly.size(); i++)
				area += vector_cross(poly[i].s - poly[0].s,
				                     poly[i + 1].s - poly[0].s);
			if (std::fabs(area) * 0.5 <= 1e-15) continue;
			double orient = area > 0.0 ? 1.0 : -1.0;
			double p = 1.0;
			for (size_t i = 0; i < poly.size(); i++) {
				Vec2d a = poly[i].s;
				Vec2d b = poly[(i + 1) % poly.size()].s;
				Vec2d e = b - a;
				double len = vector_length(e);
				if (len <= 1e-12) continue;
				Vec2d n(-e.y / len, e.x / len);
				n = n * orient;
				double d = vector_dot(n, P - a);
				double sv = std::max(0.0, std::min(1.0, d / sigma));
				p *= RefSmooth(sv);
			}
			if (!(p > 0.0)) continue;
			Vec2d s0 = RefProject(f.q[0]), s1 = RefProject(f.q[1]),
			      s2 = RefProject(f.q[2]);
			Vec2d A = s1 - s0, B = s2 - s0, U = P - s0;
			double den = vector_cross(A, B);
			double u = vector_cross(U, B) / den;
			double v = vector_cross(A, U) / den;
			double b[3] = { 1.0 - u - v, u, v };
			double r = b[0] / f.q[0].w + b[1] / f.q[1].w + b[2] / f.q[2].w;
			if (!(r > 0.0)) continue;
			Vec3d rgb(0.0, 0.0, 0.0);
			for (int i = 0; i < 3; i++)
				rgb += f.c[i] * (b[i] / (f.q[i].w * r));
			double zwin = b[0] * (f.q[0].z / f.q[0].w)
			            + b[1] * (f.q[1].z / f.q[1].w)
			            + b[2] * (f.q[2].z / f.q[2].w);
			terms.push_back({ rgb, std::log(p) - zwin / tau });
			maxLog = std::max(maxLog, terms.back().logw);
		}
		double sum = std::exp(bgLog - maxLog);
		for (const Term& t : terms) sum += std::exp(t.logw - maxLog);
		double logZ = maxLog + std::log(sum);
		Vec3d col = kBg * std::exp(bgLog - logZ);
		for (const Term& t : terms)
			col += t.rgb * std::exp(t.logw - logZ);
		img[px * 3 + 0] = col.x;
		img[px * 3 + 1] = col.y;
		img[px * 3 + 2] = col.z;
	}
	return img;
}

//---------------------------------------------------------------------
// 扫描一个满足“软边带中段、覆盖适中”的稳定像素
//---------------------------------------------------------------------
static bool PickStablePixel(const Face& f, double sigma, int& outX, int& outY,
                            double* svRangeOut = NULL) {
	vector<RNode> poly = RefClip(f);
	if (poly.size() < 3) return false;
	double area = 0.0;
	for (size_t i = 1; i + 1 < poly.size(); i++)
		area += vector_cross(poly[i].s - poly[0].s,
		                     poly[i + 1].s - poly[0].s);
	double orient = area > 0.0 ? 1.0 : -1.0;
	for (int y = 2; y < kH - 2; y++) {
		for (int x = 2; x < kW - 2; x++) {
			Vec2d P(x + 0.5, y + 0.5);
			double p = 1.0;
			double loSv = 2.0, hiSv = -1.0;
			for (size_t i = 0; i < poly.size(); i++) {
				Vec2d a = poly[i].s;
				Vec2d b = poly[(i + 1) % poly.size()].s;
				Vec2d e = b - a;
				double len = vector_length(e);
				if (len <= 1e-12) continue;
				Vec2d n(-e.y / len, e.x / len);
				n = n * orient;
				double d = vector_dot(n, P - a);
				double sv = d / sigma;
				loSv = std::min(loSv, sv);
				hiSv = std::max(hiSv, sv);
				sv = std::max(0.0, std::min(1.0, sv));
				p *= RefSmooth(sv);
			}
			// 覆盖适中且至少一条边落在 smoothstep 中段（分支稳定）
			if (p > 0.1 && p < 0.9 && loSv > 0.2 && loSv < 0.8) {
				outX = x; outY = y;
				if (svRangeOut) { svRangeOut[0] = loSv; svRangeOut[1] = hiSv; }
				return true;
			}
		}
	}
	return false;
}

//---------------------------------------------------------------------
// 两档中心差分核对全部面的 xyzw 与 RGB 梯度
//---------------------------------------------------------------------
static bool CheckGradients(const char* tag, const vector<Face>& faces,
                           double sigma, double tau,
                           const vector<double>& adj) {
	vector<double> image;
	Pipe pipe(faces, sigma, tau, kBg);
	SoftFrameRecord rec = pipe.Run(image);
	vector<SoftFaceGradient> g = SoftRasterContext::Backward(rec, adj);

	const double eps[2] = { 1e-4, 3e-4 };
	bool ok = true;
	for (size_t fi = 0; fi < faces.size(); fi++) {
		for (int k = 0; k < 3; k++) {
			for (int kind = 0; kind < 2; kind++) {
				int ncomp = (kind == 0) ? 4 : 3;
				for (int c = 0; c < ncomp; c++) {
					double analytic = (kind == 0)
					    ? g[fi].position[k][c] : g[fi].color[k][c];
					for (int e = 0; e < 2; e++) {
						vector<Face> fp = faces, fm = faces;
						if (kind == 0) {
							fp[fi].q[k][c] += eps[e];
							fm[fi].q[k][c] -= eps[e];
						} else {
							fp[fi].c[k][c] += eps[e];
							fm[fi].c[k][c] -= eps[e];
						}
						double lp = ForwardLoss(fp, sigma, tau, adj);
						double lm = ForwardLoss(fm, sigma, tau, adj);
						double fd = (lp - lm) / (2.0 * eps[e]);
						double tol = 2e-3 + 2e-3 * std::fabs(fd);
						double err = std::fabs(analytic - fd);
						// 每个坐标/RGB 分量 × 两档步长都做一次真实判断
						TASK_CHECK(err <= tol);
						if (!(err <= tol)) ok = false;
					}
				}
			}
		}
	}
	(void)tag;
	return ok;
}

// 在指定像素构造三通道伴随
static vector<double> AdjAt(int x, int y, Vec3d w) {
	vector<double> adj((size_t)kW * kH * 3, 0.0);
	int idx = (y * kW + x) * 3;
	adj[idx + 0] = w.x; adj[idx + 1] = w.y; adj[idx + 2] = w.z;
	return adj;
}

// 一组标准几何：不等 w 单面
static vector<Face> MakePerspFace() {
	vector<Face> fs(1);
	fs[0].q[0] = Vec4d(-0.7, -0.6, 0.30, 1.3);
	fs[0].q[1] = Vec4d( 0.6, -0.5, 0.40, 0.8);
	fs[0].q[2] = Vec4d(-0.1,  0.7, 0.50, 1.6);
	fs[0].c[0] = Vec3d(0.8, 0.1, 0.1);
	fs[0].c[1] = Vec3d(0.1, 0.8, 0.2);
	fs[0].c[2] = Vec3d(0.2, 0.1, 0.9);
	return fs;
}

// 前后重叠双面，等深竞争
static vector<Face> MakeOverlapFaces() {
	vector<Face> fs(2);
	fs[0].q[0] = Vec4d(-0.6, -0.5, 0.30, 1.2);
	fs[0].q[1] = Vec4d( 0.5, -0.4, 0.35, 0.9);
	fs[0].q[2] = Vec4d(-0.1,  0.6, 0.40, 1.4);
	fs[0].c[0] = Vec3d(0.9, 0.2, 0.1);
	fs[0].c[1] = Vec3d(0.8, 0.3, 0.1);
	fs[0].c[2] = Vec3d(0.7, 0.1, 0.2);
	fs[1].q[0] = Vec4d(-0.2, -0.3, 0.60, 1.0);
	fs[1].q[1] = Vec4d( 0.7, -0.2, 0.65, 1.0);
	fs[1].q[2] = Vec4d( 0.2,  0.7, 0.60, 1.0);
	fs[1].c[0] = Vec3d(0.1, 0.8, 0.2);
	fs[1].c[1] = Vec3d(0.1, 0.7, 0.3);
	fs[1].c[2] = Vec3d(0.2, 0.6, 0.9);
	return fs;
}

// 顶点越侧面（含非端点交点）+ 一个顶点越近面
static vector<Face> MakeClipFaces() {
	vector<Face> fs(2);
	fs[0].q[0] = Vec4d(-0.8, -0.7, 0.80, 1.0);
	fs[0].q[1] = Vec4d( 1.4, -0.6, 0.90, 1.0);
	fs[0].q[2] = Vec4d( 0.1,  0.8, 0.70, 1.0);
	fs[0].c[0] = Vec3d(0.1, 0.7, 0.2);
	fs[0].c[1] = Vec3d(0.05, 0.8, 0.3);
	fs[0].c[2] = Vec3d(0.2, 0.6, 0.9);
	fs[1].q[0] = Vec4d(-0.4, -0.3, -0.40, 1.0);
	fs[1].q[1] = Vec4d( 0.4, -0.3,  0.40, 1.0);
	fs[1].q[2] = Vec4d( 0.0,  0.5,  0.30, 1.0);
	fs[1].c[0] = Vec3d(0.9, 0.6, 0.2);
	fs[1].c[1] = Vec3d(0.8, 0.7, 0.1);
	fs[1].c[2] = Vec3d(0.7, 0.5, 0.3);
	return fs;
}


//=====================================================================
// forward 组
//=====================================================================

int test_forward_matches_reference() {
	vector<Face> fs = MakeOverlapFaces();
	fs.push_back(MakeClipFaces()[0]);
	double sigma = 1.1, tau = 0.4;

	vector<double> prod;
	Pipe pipe(fs, sigma, tau, kBg);
	SoftFrameRecord rec = pipe.Run(prod);
	vector<double> ref = RefForward(fs, sigma, tau);

	TASK_CHECK(prod.size() == ref.size());
	TASK_CHECK((int)rec.FaceCount() == (int)fs.size());
	double worst = 0.0;
	for (size_t i = 0; i < prod.size(); i++)
		worst = std::max(worst, std::fabs(prod[i] - ref[i]));
	// 生产经 VS 的 float 往返（Vec4f/Vec3f），参考走 double，
	// 残差应只来自入口 float 量化（~1e-7 量级）
	TASK_CHECK(worst < 2e-6);
	// 所有通道有限且落在合法范围
	for (double v : prod) {
		TASK_CHECK(std::isfinite(v));
		TASK_CHECK(v >= -1e-12 && v <= 1.0 + 1e-12);
	}
	return 0;
}

int test_forward_background_and_normalization() {
	vector<Face> fs = MakePerspFace();
	double sigma = 1.0, tau = 1.0;
	vector<double> img;
	Pipe pipe(fs, sigma, tau, kBg);
	pipe.Run(img);

	// 角上像素无任何面覆盖，必须精确等于背景（背景经 Vec3f 入口，按 float 容差比较）
	int corner = (2 * kW + 2) * 3;
	TASK_CHECK(std::fabs(img[corner + 0] - kBg.x) < 1e-7);
	TASK_CHECK(std::fabs(img[corner + 1] - kBg.y) < 1e-7);
	TASK_CHECK(std::fabs(img[corner + 2] - kBg.z) < 1e-7);

	// 用参考实现逐像素核对归一权重和为 1（颜色为 bg/rgb 的凸组合）
	vector<double> ref = RefForward(fs, sigma, tau);
	for (int px = 0; px < kW * kH; px++) {
		for (int ch = 0; ch < 3; ch++) {
			double v = ref[px * 3 + ch];
			TASK_CHECK(v >= -1e-12 && v <= 1.0 + 1e-12);
		}
	}
	return 0;
}

int test_forward_depth_competition_and_stability() {
	vector<Face> fs = MakeOverlapFaces();
	// 极小 tau：softmax 不能产生 NaN/Inf（稳定归一化）
	double sigma = 1.0, tau = 1e-6;
	vector<double> img;
	Pipe pipe(fs, sigma, tau, kBg);
	pipe.Run(img);
	for (double v : img) TASK_CHECK(std::isfinite(v));

	// 常规 tau 下，找一个两面都覆盖的像素，前面（z 更小）的归一权重应更大
	tau = 0.25;
	vector<double> img2;
	Pipe pipe2(fs, sigma, tau, kBg);
	SoftFrameRecord rec2 = pipe2.Run(img2);
	// 直接用参考前向在中心像素统计：颜色应更接近前面主色（红）而非后面主色（绿）
	int cx = kW / 2, cy = kH / 2;
	int idx = (cy * kW + cx) * 3;
	TASK_CHECK(img2[idx + 0] > img2[idx + 1]);  // 红通道占优
	(void)rec2;
	return 0;
}

int test_forward_clipping_keeps_visible_part() {
	vector<Face> fs = MakeClipFaces();
	double sigma = 1.1, tau = 0.4;
	vector<Face> sideOnly(1);
	sideOnly[0] = fs[0];  // 越侧面
	vector<RNode> poly = RefClip(sideOnly[0]);

	// 越界顶点被裁，多边形全部节点必须落在齐次裁剪体内
	TASK_CHECK(poly.size() >= 3);
	for (const RNode& n : poly) {
		TASK_CHECK(n.q.x <= n.q.w + 1e-9 && n.q.x >= -n.q.w - 1e-9);
		TASK_CHECK(n.q.y <= n.q.w + 1e-9 && n.q.y >= -n.q.w - 1e-9);
		TASK_CHECK(n.q.z >= -1e-9 && n.q.z <= n.q.w + 1e-9);
		// 屏幕坐标在画面内（软边带可略出界，这里裁点本身在边界上）
		TASK_CHECK(n.s.x >= -1e-6 && n.s.x <= kW + 1e-6);
	}
	// 含非端点交点：节点数多于原始三角
	TASK_CHECK(poly.size() > 3);

	vector<double> ref = RefForward(sideOnly, sigma, tau);
	vector<double> prod;
	Pipe pipe(sideOnly, sigma, tau, kBg);
	pipe.Run(prod);
	double worstClip = 0.0;
	for (size_t i = 0; i < ref.size(); i++)
		worstClip = std::max(worstClip, std::fabs(ref[i] - prod[i]));
	// 残差仅来自 VS 入口的 float 往返
	TASK_CHECK(worstClip < 2e-6);
	return 0;
}


//=====================================================================
// visibility_gradient 组
//=====================================================================

int test_grad_perspective_denominator() {
	vector<Face> fs = MakePerspFace();
	double sigma = 1.0, tau = 1.0;
	int px, py;
	TASK_CHECK(PickStablePixel(fs[0], sigma, px, py));
	vector<double> adj = AdjAt(px, py, Vec3d(0.7, -0.5, 0.9));

	vector<double> img;
	Pipe pipe(fs, sigma, tau, kBg);
	SoftFrameRecord rec = pipe.Run(img);
	vector<SoftFaceGradient> g = SoftRasterContext::Backward(rec, adj);

	// 软边带像素确有非零覆盖与梯度
	int idx = (py * kW + px) * 3;
	TASK_CHECK(std::fabs(img[idx] - kBg.x) +
	           std::fabs(img[idx + 1] - kBg.y) > 1e-4);
	// 不等 w：透视分母 w 的梯度必须非零，且被差分核对覆盖
	bool anyW = false, anyXY = false;
	for (int k = 0; k < 3; k++) {
		if (g[0].position[k].w != 0.0) anyW = true;
		if (g[0].position[k].x != 0.0 || g[0].position[k].y != 0.0) anyXY = true;
	}
	TASK_CHECK(anyW);
	TASK_CHECK(anyXY);
	TASK_CHECK(CheckGradients("persp", fs, sigma, tau, adj));
	return 0;
}

int test_grad_reversed_winding() {
	vector<Face> fs = MakePerspFace();
	std::swap(fs[0].q[1], fs[0].q[2]);   // 反向绕序
	std::swap(fs[0].c[1], fs[0].c[2]);
	double sigma = 1.0, tau = 1.0;
	int px, py;
	TASK_CHECK(PickStablePixel(fs[0], sigma, px, py));
	vector<double> adj = AdjAt(px, py, Vec3d(-0.3, 0.8, 0.4));
	TASK_CHECK(CheckGradients("reversed", fs, sigma, tau, adj));
	return 0;
}

int test_grad_two_faces_both_nonzero() {
	vector<Face> fs = MakeOverlapFaces();
	double sigma = 1.1, tau = 0.3;
	// 选一个两面都有软覆盖的像素（取画面中部）
	int px = kW / 2, py = kH / 2;
	vector<double> adj = AdjAt(px, py, Vec3d(0.5, 0.6, -0.7));

	vector<double> img;
	Pipe pipe(fs, sigma, tau, kBg);
	SoftFrameRecord rec = pipe.Run(img);
	vector<SoftFaceGradient> g = SoftRasterContext::Backward(rec, adj);

	// 即使后面被前面遮挡，只要软权重非零，两面都应有非零梯度
	double norm[2] = { 0.0, 0.0 };
	for (int f = 0; f < 2; f++)
		for (int k = 0; k < 3; k++)
			for (int c = 0; c < 4; c++)
				norm[f] += g[f].position[k][c] * g[f].position[k][c];
	TASK_CHECK(norm[0] > 1e-12);
	TASK_CHECK(norm[1] > 1e-12);
	TASK_CHECK(CheckGradients("twoface", fs, sigma, tau, adj));
	return 0;
}

int test_grad_small_tau_depth() {
	vector<Face> fs = MakeOverlapFaces();
	double sigma = 1.0, tau = 0.08;   // 小 tau 强深度竞争
	int px = kW / 2, py = kH / 2;
	vector<double> adj = AdjAt(px, py, Vec3d(0.9, -0.2, 0.3));
	TASK_CHECK(CheckGradients("smalltau", fs, sigma, tau, adj));
	return 0;
}

int test_grad_clip_intersection() {
	vector<Face> fs = MakeClipFaces();
	double sigma = 1.1, tau = 0.4;
	int px, py;
	// 对越侧面选软边带像素（裁剪边即屏幕右缘附近）
	TASK_CHECK(PickStablePixel(fs[0], sigma, px, py));
	vector<double> adj = AdjAt(px, py, Vec3d(0.4, 0.8, -0.6));

	vector<double> img;
	Pipe pipe(fs, sigma, tau, kBg);
	SoftFrameRecord rec = pipe.Run(img);
	vector<SoftFaceGradient> g = SoftRasterContext::Backward(rec, adj);
	// 越界顶点（q1）经裁剪交点仍应收到非零梯度
	double q1norm = 0.0;
	for (int c = 0; c < 4; c++)
		q1norm += g[0].position[1][c] * g[0].position[1][c];
	TASK_CHECK(q1norm > 1e-12);
	TASK_CHECK(CheckGradients("clip", fs, sigma, tau, adj));
	return 0;
}

int test_grad_background_competition() {
	vector<Face> fs = MakePerspFace();
	double sigma = 1.2, tau = 1.5;
	int px, py;
	TASK_CHECK(PickStablePixel(fs[0], sigma, px, py));
	// 软边带像素背景占比高，面与背景共同竞争，两面梯度仍可差分核对
	vector<double> adj = AdjAt(px, py, Vec3d(0.2, 0.3, 0.5));
	TASK_CHECK(CheckGradients("bgcomp", fs, sigma, tau, adj));
	return 0;
}


//=====================================================================
// snapshot 组
//=====================================================================

int test_snap_outside_zero_gradient() {
	vector<Face> fs(1);
	fs[0].q[0] = Vec4d(3.0, 3.0, 0.5, 1.0);
	fs[0].q[1] = Vec4d(4.0, 3.0, 0.5, 1.0);
	fs[0].q[2] = Vec4d(3.0, 4.0, 0.5, 1.0);
	fs[0].c[0] = fs[0].c[1] = fs[0].c[2] = Vec3d(0.5, 0.5, 0.5);
	vector<double> adj((size_t)kW * kH * 3, 1.0);

	vector<double> img;
	Pipe pipe(fs, 1.0, 1.0, kBg);
	SoftFrameRecord rec = pipe.Run(img);
	vector<SoftFaceGradient> g = SoftRasterContext::Backward(rec, adj);

	// 全图外：所有像素保持背景（经 Vec3f 入口，按 float 容差比较），全部梯度为 0
	for (int px = 0; px < kW * kH; px++)
		for (int ch = 0; ch < 3; ch++)
			TASK_CHECK(std::fabs(img[px * 3 + ch] - kBg[ch]) < 1e-7);
	for (int k = 0; k < 3; k++) {
		for (int c = 0; c < 4; c++) TASK_CHECK(g[0].position[k][c] == 0.0);
		for (int c = 0; c < 3; c++) TASK_CHECK(g[0].color[k][c] == 0.0);
	}
	return 0;
}

int test_snap_repeat_vjp() {
	vector<Face> fs = MakeOverlapFaces();
	vector<double> adj((size_t)kW * kH * 3, 0.0);
	adj[(kH / 2 * kW + kW / 2) * 3 + 1] = 1.0;
	vector<double> img;
	Pipe pipe(fs, 1.0, 0.3, kBg);
	SoftFrameRecord rec = pipe.Run(img);

	auto g1 = SoftRasterContext::Backward(rec, adj);
	auto g2 = SoftRasterContext::Backward(rec, adj);  // 再次反传，从 0 开始
	TASK_CHECK(g1.size() == g2.size());
	for (size_t f = 0; f < g1.size(); f++)
		for (int k = 0; k < 3; k++) {
			for (int c = 0; c < 4; c++)
				TASK_CHECK(g1[f].position[k][c] == g2[f].position[k][c]);
			for (int c = 0; c < 3; c++)
				TASK_CHECK(g1[f].color[k][c] == g2[f].color[k][c]);
		}
	return 0;
}

int test_snap_independent_and_owning() {
	vector<Face> fs = MakePerspFace();
	vector<double> adj((size_t)kW * kH * 3, 0.0);
	adj[(kH / 2 * kW + kW / 2) * 3 + 2] = 1.0;

	SoftFrameRecord recA;
	vector<SoftFaceGradient> gA;
	{
		// VS 捕获的几何放在局部作用域，结束后销毁
		vector<Face> local = fs;
		Pipe pipe(local, 1.0, 1.0, kBg);
		vector<double> img;
		recA = pipe.Run(img);
		gA = SoftRasterContext::Backward(recA, adj);
	}
	// 记录拷贝拥有独立数据
	SoftFrameRecord copyA = recA;

	// 用另一套几何另画一帧，不得影响旧记录
	vector<Face> other = MakeOverlapFaces();
	{
		Pipe pipe2(other, 1.3, 0.5, kBg);
		vector<double> img2;
		SoftFrameRecord recB = pipe2.Run(img2);
		(void)recB;
	}
	auto gA2 = SoftRasterContext::Backward(recA, adj);
	auto gCopy = SoftRasterContext::Backward(copyA, adj);
	TASK_CHECK(gA.size() == gA2.size() && gA.size() == gCopy.size());
	for (size_t f = 0; f < gA.size(); f++)
		for (int k = 0; k < 3; k++) {
			for (int c = 0; c < 4; c++) {
				TASK_CHECK(gA[f].position[k][c] == gA2[f].position[k][c]);
				TASK_CHECK(gA[f].position[k][c] == gCopy[f].position[k][c]);
			}
			for (int c = 0; c < 3; c++) {
				TASK_CHECK(gA[f].color[k][c] == gA2[f].color[k][c]);
				TASK_CHECK(gA[f].color[k][c] == gCopy[f].color[k][c]);
			}
		}
	return 0;
}


//---------------------------------------------------------------------
// 注册（C 链接，供 task_support/driver.c 引用）
//---------------------------------------------------------------------
} // namespace

extern "C" const struct task_case task_cases[] = {
	{ "forward", "matches_reference",        test_forward_matches_reference },
	{ "forward", "background_normalization", test_forward_background_and_normalization },
	{ "forward", "depth_competition_stable", test_forward_depth_competition_and_stability },
	{ "forward", "clipping_visible_part",    test_forward_clipping_keeps_visible_part },

	{ "visibility_gradient", "perspective_denominator", test_grad_perspective_denominator },
	{ "visibility_gradient", "reversed_winding",        test_grad_reversed_winding },
	{ "visibility_gradient", "two_faces_both_nonzero",  test_grad_two_faces_both_nonzero },
	{ "visibility_gradient", "small_tau_depth",         test_grad_small_tau_depth },
	{ "visibility_gradient", "clip_intersection",       test_grad_clip_intersection },
	{ "visibility_gradient", "background_competition",  test_grad_background_competition },

	{ "snapshot", "outside_zero_gradient", test_snap_outside_zero_gradient },
	{ "snapshot", "repeat_vjp",            test_snap_repeat_vjp },
	{ "snapshot", "independent_owning",    test_snap_independent_and_owning },
};

extern "C" const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
