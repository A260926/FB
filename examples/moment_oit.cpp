//=====================================================================
//
// examples/moment_oit.cpp - 矩透明离屏帧示例
//
// 走真实接口：BeginFrame -> 不透明 DrawPrimitive -> SealOpaque ->
// 透明 DrawPrimitive -> Resolve，构造两片相互穿插的半透明色片和一个
// 不透明遮挡物，把解析结果写成 PPM，并打印一行透射率统计。
//
// 用法：moment_example <输出目录>
//
//=====================================================================
#include "moment_oit.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <fstream>
#include <cmath>
#include <vector>


namespace {

constexpr int WIDTH = 160;
constexpr int HEIGHT = 120;

// 一个三角形的三个角：齐次坐标 + 线性 RGBA（属性槽三角一致）
struct Tri {
    Vec4f pos[3];
    Vec4f color[3];
};

// 运行期由 VS 读取的当前三角形
Tri g_current;

Vec4f exampleVS(int index, ShaderContext& output)
{
    output.varying_vec4f[0] = g_current.color[index];
    return g_current.pos[index];
}

Vec4f examplePS(ShaderContext& input)
{
    return input.varying_vec4f[0];
}

// 写二进制 P6 PPM（对线性 RGB 做一次 gamma 1/2.2 便于查看）
bool writePPM(const std::string& path, const MomentOITResult& res)
{
    std::ofstream fp(path, std::ios::binary);
    if (!fp) return false;
    fp << "P6\n" << res.GetWidth() << " " << res.GetHeight()
       << "\n255\n";
    std::vector<unsigned char> row(res.GetWidth() * 3);
    for (int y = 0; y < res.GetHeight(); y++) {
        for (int x = 0; x < res.GetWidth(); x++) {
            const double* rgb = res.GetRGB(x, y);
            for (int c = 0; c < 3; c++) {
                double v = rgb[c];
                if (!(v > 0.0)) v = 0.0;
                if (v > 1.0) v = 1.0;
                double srgb = std::pow(v, 1.0 / 2.2);
                row[x * 3 + c] = (unsigned char)(srgb * 255.0 + 0.5);
            }
        }
        fp.write(reinterpret_cast<const char*>(row.data()), row.size());
    }
    return static_cast<bool>(fp);
}

} // namespace


int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <output-dir>\n", argv[0]);
        return 2;
    }
    std::string outDir = argv[1];

    RenderHelp rh(WIDTH, HEIGHT);
    rh.SetVertexShader(exampleVS);
    rh.SetPixelShader(examplePS);

    MomentOITFrame frame(rh);

    // 线性 RGB 背景：深灰蓝
    frame.BeginFrame(0.08, 0.10, 0.14);

    // ---- 不透明阶段：中央偏下的遮挡物（两个三角形拼成一个矩形）----
    Tri occluderA;
    occluderA.pos[0] = Vec4f(-0.55f, -0.85f, 0.80f, 1.0f);
    occluderA.pos[1] = Vec4f( 0.55f, -0.85f, 0.80f, 1.0f);
    occluderA.pos[2] = Vec4f(-0.55f, -0.10f, 0.80f, 1.0f);
    Tri occluderB;
    occluderB.pos[0] = Vec4f( 0.55f, -0.85f, 0.80f, 1.0f);
    occluderB.pos[1] = Vec4f( 0.55f, -0.10f, 0.80f, 1.0f);
    occluderB.pos[2] = Vec4f(-0.55f, -0.10f, 0.80f, 1.0f);
    for (int i = 0; i < 3; i++) {
        occluderA.color[i] = Vec4f(0.85f, 0.65f, 0.25f, 1.0f);
        occluderB.color[i] = Vec4f(0.85f, 0.65f, 0.25f, 1.0f);
    }
    g_current = occluderA;
    if (!rh.DrawPrimitive()) return 3;
    g_current = occluderB;
    if (!rh.DrawPrimitive()) return 3;

    // 封存不透明颜色与窗口深度
    frame.SealOpaque();

    // ---- 透明阶段：两片左右互换远近、相互穿插的色片 ----
    // 红片：左侧近（浅深度）右侧远
    Tri red;
    red.pos[0] = Vec4f(-1.0f, -1.0f, 0.15f, 1.0f);
    red.pos[1] = Vec4f( 1.0f, -1.0f, 0.70f, 1.0f);
    red.pos[2] = Vec4f(-1.0f,  1.0f, 0.15f, 1.0f);
    for (int i = 0; i < 3; i++)
        red.color[i] = Vec4f(0.90f, 0.20f, 0.18f, 0.55f);

    // 绿片：左侧远右侧近，与红片在屏幕中部交叉
    Tri green;
    green.pos[0] = Vec4f(-1.0f, -1.0f, 0.70f, 1.0f);
    green.pos[1] = Vec4f( 1.0f, -1.0f, 0.15f, 1.0f);
    green.pos[2] = Vec4f( 1.0f,  1.0f, 0.15f, 1.0f);
    for (int i = 0; i < 3; i++)
        green.color[i] = Vec4f(0.20f, 0.80f, 0.35f, 0.45f);

    // 蓝片：穿出裁剪面的大三角形，演示完整齐次裁剪与三层近似
    Tri blue;
    blue.pos[0] = Vec4f(-2.0f, -2.0f, 0.35f, 1.0f);
    blue.pos[1] = Vec4f( 2.5f, -2.0f, 0.55f, 1.0f);
    blue.pos[2] = Vec4f(-2.0f,  2.5f, 0.45f, 1.0f);
    for (int i = 0; i < 3; i++)
        blue.color[i] = Vec4f(0.25f, 0.40f, 0.95f, 0.35f);

    g_current = red;
    if (!rh.DrawPrimitive()) return 4;
    g_current = green;
    if (!rh.DrawPrimitive()) return 4;
    g_current = blue;
    if (!rh.DrawPrimitive()) return 4;

    // 只读解析：不执行 VS/PS，不写普通帧缓存
    MomentOITResult result = frame.Resolve();

    std::string ppmPath = outDir + "/moments.ppm";
    if (!writePPM(ppmPath, result)) {
        std::fprintf(stderr, "failed to write %s\n", ppmPath.c_str());
        return 5;
    }

    // 一行透射率统计：最小值、平均值、最大值
    double tmin = 1.0, tmax = 0.0, tsum = 0.0;
    size_t pixels = (size_t)WIDTH * HEIGHT;
    for (size_t i = 0; i < pixels; i++) {
        double t = result.transmittance[i];
        if (t < tmin) tmin = t;
        if (t > tmax) tmax = t;
        tsum += t;
    }
    std::printf("transmittance min=%.6f mean=%.6f max=%.6f image=%s\n",
                tmin, tsum / (double)pixels, tmax, ppmPath.c_str());

    frame.Cancel();
    return 0;
}
