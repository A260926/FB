//=====================================================================
//
// examples/soft_fit.cpp - 用软光栅反向梯度做小范围几何拟合
//
// 生成两块重叠色片与一张目标图，真实调用 SoftRasterContext::Backward
// 对近面三个角的齐次坐标与 RGB 做若干步梯度下降，打印损失并保存拟合
// 前后的 PPM。全程 C++17 标准库，不使用任何自动微分库。
//
//=====================================================================
#include "soft_raster.h"

#include <cstdio>
#include <cmath>
#include <vector>
#include <string>


namespace {

struct Face { Vec4d q[3]; Vec3d c[3]; };

const int kW = 48;
const int kH = 40;
const Vec3d kBg(0.04, 0.05, 0.08);

// 用当前几何跑一帧，返回图像、记录与逐面梯度（损失 = 0.5 Σ (img-target)^2）
struct FrameResult {
	std::vector<double> image;
	SoftFrameRecord record;
	std::vector<SoftFaceGradient> grad;
	double loss;
};

static FrameResult Evaluate(const std::vector<Face>& faces, double sigma,
                            double tau, const std::vector<double>& target) {
	RenderHelp rh(kW, kH);
	size_t cursor = 0;
	const std::vector<Face>* p = &faces;
	rh.SetVertexShader([&](int, ShaderContext& out) -> Vec4f {
		size_t f = cursor / 3; int k = cursor % 3; cursor++;
		out.varying_vec3f[0] = Vec3f((float)(*p)[f].c[k].x,
		                             (float)(*p)[f].c[k].y,
		                             (float)(*p)[f].c[k].z);
		return Vec4f((float)(*p)[f].q[k].x, (float)(*p)[f].q[k].y,
		             (float)(*p)[f].q[k].z, (float)(*p)[f].q[k].w);
	});
	SoftRasterContext soft(rh);
	soft.Begin(kW, kH, Vec3f((float)kBg.x, (float)kBg.y, (float)kBg.z),
	           sigma, tau, 0);
	for (size_t f = 0; f < faces.size(); f++) soft.SubmitTriangle();

	FrameResult r;
	r.record = soft.End(r.image);

	// 逐像素伴随 dL/dI = I - target
	std::vector<double> adj(r.image.size(), 0.0);
	r.loss = 0.0;
	for (size_t i = 0; i < r.image.size(); i++) {
		double diff = r.image[i] - target[i];
		adj[i] = diff;
		r.loss += 0.5 * diff * diff;
	}
	r.grad = SoftRasterContext::Backward(r.record, adj);
	return r;
}

// 保存二进制 PPM（P6），double 线性 RGB 转 8 位
static bool SavePPM(const std::string& path, const std::vector<double>& img) {
	FILE* fp = std::fopen(path.c_str(), "wb");
	if (!fp) return false;
	std::fprintf(fp, "P6\n%d %d\n255\n", kW, kH);
	for (int y = 0; y < kH; y++) {
		for (int x = 0; x < kW; x++) {
			unsigned char rgb[3];
			for (int ch = 0; ch < 3; ch++) {
				double v = img[(y * kW + x) * 3 + ch];
				if (v < 0.0) v = 0.0;
				if (v > 1.0) v = 1.0;
				rgb[ch] = (unsigned char)(v * 255.0 + 0.5);
			}
			std::fwrite(rgb, 1, 3, fp);
		}
	}
	std::fclose(fp);
	return true;
}

} // namespace


int main() {
	const double sigma = 1.0;
	const double tau = 0.35;

	// 目标几何：远面（固定）+ 近面（待拟合，位置/颜色都与初值不同）
	std::vector<Face> targetFaces(2);
	targetFaces[0].q[0] = Vec4d(-0.85, -0.75, 0.70, 1.0);
	targetFaces[0].q[1] = Vec4d( 0.85, -0.75, 0.72, 1.0);
	targetFaces[0].q[2] = Vec4d( 0.00,  0.85, 0.70, 1.0);
	targetFaces[0].c[0] = Vec3d(0.10, 0.45, 0.70);
	targetFaces[0].c[1] = Vec3d(0.10, 0.55, 0.65);
	targetFaces[0].c[2] = Vec3d(0.20, 0.40, 0.80);
	targetFaces[1].q[0] = Vec4d(-0.55, -0.45, 0.30, 1.0);
	targetFaces[1].q[1] = Vec4d( 0.55, -0.40, 0.32, 1.0);
	targetFaces[1].q[2] = Vec4d( 0.05,  0.55, 0.30, 1.0);
	targetFaces[1].c[0] = Vec3d(0.90, 0.55, 0.20);
	targetFaces[1].c[1] = Vec3d(0.95, 0.45, 0.15);
	targetFaces[1].c[2] = Vec3d(0.85, 0.60, 0.25);

	FrameResult target = Evaluate(targetFaces, sigma, tau,
	                              std::vector<double>(kW * kH * 3, 0.0));
	const std::vector<double> targetImg = target.image;
	SavePPM("soft_fit_target.ppm", targetImg);

	// 待拟合几何：近面从偏移的位置与颜色出发
	std::vector<Face> fit = targetFaces;
	fit[1].q[0].x += 0.22; fit[1].q[0].y -= 0.18;
	fit[1].q[1].x -= 0.18; fit[1].q[1].y += 0.15;
	fit[1].q[2].x += 0.15; fit[1].q[2].y += 0.20;
	for (int k = 0; k < 3; k++)
		fit[1].c[k] = Vec3d(0.50, 0.20, 0.55);

	FrameResult before = Evaluate(fit, sigma, tau, targetImg);
	SavePPM("soft_fit_before.ppm", before.image);
	std::printf("initial loss = %.6f\n", before.loss);

	// 对近面（index 1）做梯度下降；远面固定不动
	const double lrPos = 0.005;
	const double lrCol = 0.02;
	const int kSteps = 120;
	for (int step = 0; step < kSteps; step++) {
		FrameResult r = Evaluate(fit, sigma, tau, targetImg);
		const SoftFaceGradient& g = r.grad[1];
		// 学习率指数衰减，避免固定步长在收敛点附近振荡
		double decay = std::pow(0.97, step);
		double lp = lrPos * decay;
		double lc = lrCol * decay;
		for (int k = 0; k < 3; k++) {
			// 只优化 x/y 与 RGB，保留 z/w 以维持深度竞争设定
			fit[1].q[k].x -= lp * g.position[k].x;
			fit[1].q[k].y -= lp * g.position[k].y;
			// 约束在安全 CVV 内，避免越界被裁后失去软覆盖与梯度
			fit[1].q[k].x = std::max(-0.95, std::min(0.95, fit[1].q[k].x));
			fit[1].q[k].y = std::max(-0.95, std::min(0.95, fit[1].q[k].y));
			for (int ch = 0; ch < 3; ch++) {
				double v = fit[1].c[k][ch] - lc * g.color[k][ch];
				fit[1].c[k][ch] = std::max(0.0, std::min(1.0, v));
			}
		}
		if (step % 10 == 0 || step == kSteps - 1)
			std::printf("step %2d  loss = %.6f\n", step, r.loss);
	}

	FrameResult after = Evaluate(fit, sigma, tau, targetImg);
	SavePPM("soft_fit_after.ppm", after.image);
	std::printf("final   loss = %.6f\n", after.loss);
	std::printf("improved = %s\n", after.loss < before.loss ? "yes" : "NO");

	if (!(after.loss < before.loss)) return 1;
	return 0;
}
