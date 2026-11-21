// 矩透明帧示例：相互穿插的透明色片 + 不透明遮挡物
//
// 用法：moment_oit <输出目录>
// 在输出目录写出 moments.ppm（解析后的合成图），并向 stdout
// 打印一行透射率统计。
//
// 场景：一个不透明的竖条遮挡物（z = 0.45），后面两片深度梯度交叉的
// 透明色片（左近右远与左远右近，相互穿插），再加一片部分被遮挡物
// 挡住的黄色透明片。全部通过 RenderHelp 的 VS/PS 接口绘制。
#include "moment_oit.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

// 一个三角形：三个角的齐次坐标 + 三个角的颜色（作为 varying）
struct Tri {
	Vec4f pos[3];
	Vec4f color[3];
};

// 场景数据：三角形列表 + 当前提交序号（由着色器闭包捕获）
struct Scene {
	std::vector<Tri> tris;
	int cursor = 0;
};

// 添加一个四边形（三角形 (0,1,2) 和 (0,2,3)），四角给 (x, y, z)，w = 1
void AddQuad(Scene &scene, const Vec3f ndc[4], const Vec4f &color) {
	const int idx[6] = { 0, 1, 2, 0, 2, 3 };
	for (int k = 0; k < 6; k += 3) {
		Tri t;
		for (int i = 0; i < 3; i++) {
			const Vec3f &p = ndc[idx[k + i]];
			t.pos[i] = Vec4f(p.x, p.y, p.z, 1.0f);
			t.color[i] = color;
		}
		scene.tris.push_back(t);
	}
}

}    // namespace

int main(int argc, char **argv) {
	if (argc < 2) {
		fprintf(stderr, "usage: %s <output_dir>\n", argv[0]);
		return 1;
	}
	std::string out_dir = argv[1];

	const int W = 256, H = 256;
	RenderHelp rh(W, H);
	MomentOit mo(rh);
	Scene scene;

	// 沿用现有的 VS/PS/ShaderContext 接口
	rh.SetVertexShader([&](int index, ShaderContext &output) -> Vec4f {
		const Tri &t = scene.tris[(size_t)scene.cursor];
		output.varying_vec4f[0] = t.color[index];
		return t.pos[index];
	});
	rh.SetPixelShader([&](ShaderContext &input) -> Vec4f {
		return input.varying_vec4f[0];
	});

	// 线性 RGB 背景
	const Vec3d background(0.05, 0.06, 0.10);
	mo.BeginFrame(background);

	// ---- 不透明阶段：竖条遮挡物（z = 0.45）----
	{
		Vec3f bar[4] = {
			Vec3f(-0.15f,  0.9f, 0.45f), Vec3f(0.25f,  0.9f, 0.45f),
			Vec3f( 0.25f, -0.9f, 0.45f), Vec3f(-0.15f, -0.9f, 0.45f)
		};
		AddQuad(scene, bar, Vec4f(0.72f, 0.68f, 0.60f, 1.0f));
	}
	for (size_t i = 0; i < scene.tris.size(); i++) {
		scene.cursor = (int)i;
		rh.DrawPrimitive();
	}

	// 封存不透明颜色和窗口深度
	mo.SealOpaque();

	// ---- 透明阶段：两片相互穿插的色片 + 一片被部分遮挡的黄片 ----
	size_t transparent_begin = scene.tris.size();
	// 色片 A（红）：深度从左到右 0.25 -> 0.70
	{
		Vec3f q[4] = {
			Vec3f(-0.95f,  0.80f, 0.25f), Vec3f(0.95f,  0.80f, 0.70f),
			Vec3f( 0.95f, -0.80f, 0.70f), Vec3f(-0.95f, -0.80f, 0.25f)
		};
		AddQuad(scene, q, Vec4f(0.90f, 0.15f, 0.20f, 0.55f));
	}
	// 色片 B（青）：深度从右到左 0.25 -> 0.70（与 A 穿插）
	{
		Vec3f q[4] = {
			Vec3f(-0.95f,  0.60f, 0.70f), Vec3f(0.95f,  0.60f, 0.25f),
			Vec3f( 0.95f, -0.60f, 0.25f), Vec3f(-0.95f, -0.60f, 0.70f)
		};
		AddQuad(scene, q, Vec4f(0.10f, 0.80f, 0.85f, 0.55f));
	}
	// 色片 C（黄）：固定深度 0.35，横在遮挡物前面，部分被挡住
	{
		Vec3f q[4] = {
			Vec3f(-0.60f,  0.30f, 0.35f), Vec3f(0.60f,  0.30f, 0.35f),
			Vec3f( 0.60f, -0.30f, 0.35f), Vec3f(-0.60f, -0.30f, 0.35f)
		};
		AddQuad(scene, q, Vec4f(0.95f, 0.85f, 0.20f, 0.70f));
	}
	for (size_t i = transparent_begin; i < scene.tris.size(); i++) {
		scene.cursor = (int)i;
		rh.DrawPrimitive();
	}

	// 只读解析整帧（不执行 VS/PS，不写普通帧缓冲）
	std::vector<MomentOitPixel> frame = mo.ResolveFrame();

	// 写出 moments.ppm（P6），合成结果做简单的伽马提亮
	std::string path = out_dir + "/moments.ppm";
	FILE *fp = fopen(path.c_str(), "wb");
	if (fp == NULL) {
		fprintf(stderr, "cannot open %s\n", path.c_str());
		return 2;
	}
	fprintf(fp, "P6\n%d %d\n255\n", W, H);
	double tmin = 1.0, tmax = 0.0, tsum = 0.0;
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			const MomentOitPixel &px = frame[(size_t)y * W + (size_t)x];
			for (int c = 0; c < 3; c++) {
				double v = px.color[c];
				v = (v < 0.0)? 0.0 : ((v > 1.0)? 1.0 : v);
				v = sqrt(v);    // 伽马提亮，便于查看
				fputc((int)(v * 255.0 + 0.5), fp);
			}
			double t = px.transmittance;
			tsum += t;
			if (t < tmin) tmin = t;
			if (t > tmax) tmax = t;
		}
	}
	fclose(fp);

	// 一行透射率统计
	printf("MOMENT_EXAMPLE %dx%d transmittance min=%.6f avg=%.6f max=%.6f\n",
	       W, H, tmin, tsum / ((double)W * (double)H), tmax);

	mo.CancelFrame();
	return 0;
}
