公开接口放在 OpenSubdiv 版本命名空间的 Bfr 中，全部值类型为拥有型。
```cpp
struct ClipPlane { std::array<double,3> normal; double offset; };
struct ClipOptions { double geometricTolerance, planeTolerance; int maxDepth; };
struct ClipCorner { std::array<double,2> parameter, uv; std::array<double,3> normal; };
struct ClippedMesh {
  std::vector<std::array<double,3>> positions;
  std::vector<std::array<unsigned,3>> triangles;
  std::vector<int> sourceFaces;
  std::vector<ClipCorner> corners; // triangle-major, 3 per triangle
};
bool ClipRegularSurfaceMesh(const Far::TopologyRefiner &,
 const std::vector<double>& positions, const std::vector<double>& uv,
 const std::vector<int>& selectedFaces, const ClipPlane&, const ClipOptions&,
 ClippedMesh& output, std::string& error);
```
positions 长度为粗点数乘3，uv 长度为 channel 0 的粗值数乘2；不得用几何索引解释 UV。选区面号唯一；无选中面时返回空成功。只支持 Catmark、四边面、Bfr 规则双三次位置片、无洞无锐边；选区可以断开。面变片可在接缝处不连续。不支持部分、坏长度/面号、非有限值、零平面法向、非正误差、maxDepth不在1..24均返回 false 并给非空 error。成功 error 清空，失败 output 保持逐字段原值。

坐标绝对值<=16；切向夹角正弦>=1e-4且长度在[1e-3,100]。平面先归一化。普通横截情况下，零线的参数梯度模>=1e-3；分离的零线/边界事件参数距离>=1e-3，粗顶点不在平面上。可明确拒绝真相切、零线穿粗顶点、整条粗边或整片共面；不得把普通内部闭环和相距1e-3的多分支当作退化。若给定深度不足以满足界，必须失败，不能返回不完整网格。三角形顶点来自真实 Bfr 求值，输出角参数位于原面[0,1]^2。

geometricTolerance 是每个输出三角形对应参数三角域上，极限曲面与线性三角插值的最大欧氏距离上限。planeTolerance 是截线顶点到归一化平面的距离上限；保留域与真实正域的差异只允许位于距离平面 <= geometricTolerance+planeTolerance 的带内。必须以保守可细分界决定丢弃和局部终止；稠密采样仅可作为测试参照，不能充当生产保证。至少处理 geometricTolerance=1e-3、planeTolerance=1e-8 的正常用例。保留域需同胚于真实横截正域：闭环不能遗漏，薄分支不能连错。

几何共边按原拓扑与边参数身份共享，不按空间距离焊接不同分量；共享边子段完全一致，内部叶也须无T接。法线是原面解析 du×dv 单位化，UV来自原面 FVar Surface 在该参数的求值；跨缝允许同一位置拥有多个 corners。只输出被引用的位置；三角绕序与原面参数一致，面积>0。求解策略及内部类不固定，trimTessellation.h/.cpp 应承担实际曲面界细分或裁后域装配逻辑，而不是空占位。

tests/clip_contracts.cpp 按 check.h 注册三组；每个具名用例需要 TASK_CHECK。examples/clip_surface.cpp 必须产生可读取的带 v/vt/vn/f 的 OBJ，不依赖窗口/外部资产。run.sh target 删除它自己的旧产物再运行；缺声明、缺符号、缺文件、示例失败均须非零。task_support 中的固定消费者只证明公开 API 和全保留平面片最小正常路径，不能替代完整测试。
