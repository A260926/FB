# 点光源深度透射缓存（Deep Shadow Map）使用说明

Fujiyama 的 `PointLight` 现在支持一条可复用的深度透射缓存路径：在灯周围
捕获六个方向的立方体贴图，每个纹素保存一条随径向距离变化的累计透射率
阶梯函数。`SlIlluminance` 在缓存命中时直接用可见度调制直接光，不再发射
阴影射线；缓存不命中时自动回退到原有的影线逻辑，光强与采样方式不变。

## 构建

```cpp
PointLight light;
light.SetIntensity(1);
// ... 设置灯的变换、加速器等 ...

// 组的加速器必须先构建好（与 SlTrace 的要求相同）
group.ComputeBounds();
const_cast<Accelerator *>(group.GetSurfaceAccelerator())->Build();

TraceContext cxt = SlCameraContext(&group);   // cxt.trace_target 决定目标组
cxt.time = 0.5;                               // cxt.time 决定采样时刻

if (!light.BuildDeepShadowMap(cxt, 256, 100.0)) {
  // 构建被拒绝：参数非正、目标含非三角 Mesh 几何、或着色器 Os 非有限值
}
```

- `resolution`：每个面的边长纹素数，必须为正。总纹素数 = 6 × N × N。
- `maxDistance`：覆盖的最大径向距离（世界单位），必须为正。构建射线
  覆盖区间 `(0, maxDistance]`。
- 光源位置沿用现有 `GetSamples` 的结果；本次不扩展灯的运动采样。
- 实例变换与物体运动（点速度）都按 `cxt.time` 求值。

### 构建约束

- 目标组里每个对象必须是三角 `Mesh` 薄片；曲线、点云或体积目标会被
  明确拒绝（返回 `false`）。
- 着色器在 `CXT_SHADOW_RAY` 下求值，必须给出有限的 `Os`，且不得递归
  追踪（求值上下文的各级最大深度均为 0）。`Os` 非有限时构建失败。
- 没有着色器的命中按不透明处理（`Os = 1`）。
- 每一层把透射率乘上 `1 - clamp(Os, 0, 1)`；同距离的不同实例、不同
  原三角都是独立层，合并为一个右连续断点；Grid 中同一原三角被多个
  格子引用时只计一次。
- 缓存自己拥有数据，不保留对 `Intersection` 或着色器的引用；场景变化
  后由调用方负责重建。

## 查询

```cpp
Real visibility = 0;
if (light.LookupShadowVisibility(cxt, receiver, 0.0001, &visibility)) {
  // visibility ∈ [0,1]，cxt 的组与时刻都和构建时一致，且接收点在覆盖距离内
} else {
  // 不匹配：未构建、组不同、时刻不同、或接收距离超出 maxDistance
}
```

- `Light` 基类提供默认返回 `false` 的虚接口
  `LookupShadowVisibility(const TraceContext &, const Vector &receiver,
  Real bias, Real *visibility) const`，由 `PointLight` 实现。
- 查询时把接收方向映射到立方体面，取四个双线性邻居（跨界邻居按
  `task_support/cube_contract.md` 的规则重映射到相邻面），先在每个邻居
  自己的阶梯曲线上取 `接收距离 - bias` 处的透射率，再加权平均；不会
  先插值深度。
- 查询是只读的：缓存建好后可以多线程并发查询。重建和灯的变换必须
  发生在渲染之外。

## 与 SlIlluminance 的集成

`SlIlluminance` 在 `cast_shadow` 打开时先用世界单位 `bias = 0.0001`
查询缓存：

- 命中：可见度直接乘进直接光，不再调用几何求交或着色器。
- 未命中（组、时刻或覆盖距离对不上）：走原来的阴影射线，行为与之前
  完全一致。
- `cast_shadow` 关闭时不查询缓存。

## 缓存失效

- 通过 `Light` 基类调用任一变换设置入口（`SetTranslate`、`SetRotate`、
  `SetScale`、`SetTransformOrder`、`SetRotateOrder`）都会使这盏点灯的
  缓存失效。
- `ClearDeepShadowMap()` 显式清除；`HasDeepShadowMap()` 可查询是否有效。
- 场景几何、着色器或对象成员变化后，调用方需要重新
  `BuildDeepShadowMap`。

## 坐标约定

立方体面的顺序、uv 与方向的换算、并列时的轴向优先级、四邻域跨面
过滤规则见 `task_support/cube_contract.md`。

## 完整示例

```cpp
Mesh mesh;                       // 两层半透明薄片
BVHAccelerator acc;
ObjectInstance sheet;
OpacityShader shader;            // 在 CXT_SHADOW_RAY 下给出有限 Os
ObjectGroup group;

acc.SetPrimitiveSet(&mesh);
acc.Build();
sheet.SetSurface(&acc);
sheet.SetShader(&shader, 0);
sheet.ComputeBounds();
group.AddObject(&sheet);
group.ComputeBounds();
const_cast<Accelerator *>(group.GetSurfaceAccelerator())->Build();

PointLight light;
TraceContext cxt = SlCameraContext(&group);
if (light.BuildDeepShadowMap(cxt, 256, 100.0)) {
  // 之后的 SlIlluminance 会自动使用缓存
}
```

## 性能说明

- 构建沿每条捕获射线走既有的顶层 BVH 与网格级 BVH/Grid 候选遍历，
  一次性收集全部交点；不做全场景穷举，也不用 epsilon 重发最近命中
  查询。1024 个空间分散三角形、384 条射线的实测三角求交次数约为
  穷举的 0.03%（BVH）/ 0.9%（Grid）。
- 查询每个纹素只做一次二分查找（断点数级），外加常数次邻居重映射，
  不触碰任何几何或着色器。
