# mini3d

3D软件渲染教程，并没有任何性能优化，主要向人说明如何写一个固定管线的软件渲染器。虽然主体代码只有 700行，但是麻雀虽小，五脏俱全。

【[README in English](README.en.md)】

## 特性

* 单个文件：源代码只有一个 mini3d.c，单个文件实现所有内容，容易阅读。
* 独立编译：没有任何第三方库依赖，没有复杂的工程目录。
* 模型标准：标准 D3D 坐标模型，左手系加 WORLD / VIEW / PROJECTION 三矩阵
* 实现裁剪：简单 CVV 裁剪
* 纹理支持：最大支持 1024 x 1024 的纹理
* 深度缓存：使用深度缓存判断图像前后
* 透视贴图：透视纹理映射以及透视色彩填充
* 边缘计算：精确的多边形边缘覆盖计算
* 实现精简：渲染引擎只有 700行，模块清晰，主干突出。
* 详细注释：主要代码详细注释

## 编译

* mingw: 
		gcc -O3 mini3d.c -o mini3d.exe -lgdi32
* msvc:
		cl -O2 -nologo mini3d.c 
* 已编译版本：
[https://github.com/skywind3000/mini3d/releases](https://github.com/skywind3000/mini3d/releases)

## 演示

纹理填充：RENDER_STATE_TEXTURE
![](https://raw.githubusercontent.com/skywind3000/mini3d/master/images/mini_1.png)

色彩填充：RENDER_STATE_COLOR
![](https://raw.githubusercontent.com/skywind3000/mini3d/master/images/mini_0.png)

线框绘制：RENDER_STATE_WIREFRAME
![](https://raw.githubusercontent.com/skywind3000/mini3d/master/images/mini_2.png)

增加光照和二次线性插值（朋友给 Mini3D 增加的光照效果截图）
![](https://raw.githubusercontent.com/skywind3000/mini3d/master/images/mini_3.png)

## 基础作业

* 增加背面剔除
* 增加简单光照
* 提供更多渲染模式
* 实现二次线性差值的纹理读取

## 进阶作业

* 推导并证明程序中用到的所有几何知识
* 优化顶点计算性能
* 优化 draw_scanline 性能
* 从 BMP/TGA 文件加载纹理
* 载入 BSP 场景并实现漫游

## 原理讲解

- [韦易笑：OpenGL 和 DirectX 是如何在只知道顶点的情况下得出像素位置的？](https://skywind.me/blog/archives/2594)
- [韦易笑：计算机底层是如何访问显卡的？](https://skywind.me/blog/archives/1774)

## 相关项目

- [RenderHelp](https://github.com/skywind3000/RenderHelp)：另外一个支持可编程渲染管线的精简软渲染器实现，详细见 [介绍](https://skywind.me/blog/archives/2589)。

## 离线玻璃焦散通道（caustics）

在原矩阵 / 梯形 / 扫描线管线上增加了一条确定性的光束通量焦散通道，全部为
C99 代码，不引入窗口、GPU 或任何第三方库：

* `mini3d.h`：原渲染器的公共接口（`vertex_t` 新增 `world` 字段用于透视
  插值世界坐标，`device_t` 新增焦散绑定字段，未绑定时行为与原实现一致）。
* `caustics.h` / `caustics.c`：单界面平行光折射焦散。
  1. 用正交光视图光栅化世界空间三角形（六个齐次边界裁剪、像素中心采样、
     保存最小 z/w，平深取面 ID 小者），记录最近折射面的世界位置、归一法线
     与原面 ID；
  2. 对每个样本按 Snell 定律折射一次并求接收平面的正向交点，全反射 / 无覆盖 /
     不相交的样本置为无效；
  3. 每四个相邻光样本沿固定对角线拆成两束，三样本均有效且同面才成束，束通量
     = 入射辐照度 × 半光网格世界面积 × RGB 透过率；
  4. 每束的三角足迹按与接收纹素的真实相交面积均匀沉积通量，重叠与翻转足迹
     累加，出图部分丢失且不重新归一，图中存每纹素面积上的平均辐照度；
  5. 通过 `caustics_bind_device` 绑定到 `device_t` 后，相机片元的世界位置
     （经原透视插值）在接收平面容差内时，按
     `底色 × (ambient + gain × 双线性采样辐照度)` 一次量化。绑定不改变原
     rhw 遮挡与纹理覆盖顶点色的优先级，图不被绘制改写，多设备互不污染。

### 调用方法

```c
#include "caustics.h"

caustics_receiver recv = {
    origin, axis_u, axis_v,          // 平面原点与正交单位轴
    u_min, u_max, v_min, v_max,      // 世界范围
    map_width, map_height            // 接收图尺寸
};
caustics_map *map = caustics_map_create(&recv);

caustics_capture_input in = {
    triangle_count, positions, normals, transmit,   // 只读几何输入
    &light_matrix, aperture_width, aperture_height, // w 恒为 1 的世界到光图矩阵
    &incident_rgb, light_width, light_height,       // 光图至少 2×2
    ior_in, ior_out                                 // 两侧正折射率
};
caustics_capture(map, &in);              // 每次捕获从空图开始，失败不发布半幅

caustics_bind_device(map, &device, tolerance, ambient, gain);
device_draw_primitive(&device, &v1, &v2, &v3);   // 原绘制入口，顶点填 world
caustics_unbind_device(&device);
caustics_map_release(map);
```

样本与辐照度只读查询：`caustics_get_sample`、`caustics_get_irradiance`、
`caustics_sample_irradiance`（双线性，图外为 0）。完整参数与所有权说明见
`caustics.h` 注释。

### 示例与测试

```sh
# 离线示例：生成 caustics_map.ppm 与 caustics_camera.ppm
cc -std=c99 -DMINI3D_HEADLESS -I. examples/caustics.c \
   mini3d.c caustics.c -lm -o caustics_demo && ./caustics_demo

# 三组数值测试（capture / flux / camera）+ 原颜色/纹理基线
bash task_support/run.sh target
```


## 欢迎捐赠

![](https://raw.githubusercontent.com/skywind3000/mini3d/master/images/donation.png)

您的捐助是对该教程的最大肯定，欢迎使用支付宝手扫描上面的二维码，进行捐赠。捐赠款项将用于完善教程文档和图例，以及帮助我写出更多有意思的教程来。



欢迎关注

blog: https://skywind.me/blog

zhihu: https://www.zhihu.com/people/skywind3000 
