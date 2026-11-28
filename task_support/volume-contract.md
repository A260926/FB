公开接口约定（脚手架，不含实现）

新头 `cal3d/volumeskin.h` 包含必要标准头和 Cal3D 类型，声明以下公开类型；允许私有字段和内部辅助类型。

- `CalVolumeCage`：`std::vector<CalVector> rest`；`std::vector<CalCoreSubmesh::InfluenceVector> weights`；`std::vector<std::array<size_t,4>> cells`；`std::vector<size_t> surface_cells`；`std::vector<std::array<double,4>> barycentric`；`std::vector<size_t> pinned`。weights 与 rest 同长，每个列表是一点的完整影响，最后一条标志要正确。surface_cells 与 barycentric 按核心表面顶点编号排列。cells 的体积为 det(b-a,c-a,d-a)/6；pinned 不重复。绑定须非空，有向静止体积严格为正；嵌入重建误差至多 `1e-5*L`，重心/权重和误差至多 `1e-6`。不自动修复非法数据。
- `CalVolumeOptions`：`double stiffness=10, tolerance=1e-7`；`size_t max_iterations=200`。tolerance 严格正，max_iterations 至少1。固定点按 cage.pinned 指定，每帧固定到当前 LBS y。
- `CalVolumeReport`：`double initial_energy, final_energy, max_relative_volume_error`；`size_t iterations`；`std::vector<CalVector> cage_positions`；`std::vector<double> accepted_energies`。位置按笼点顺序；能量记录含初值，长度=iterations+1；最终报告以实际发布的 float 笼点重算，容差允许 float 舍入造成的 `1e-5` 相对偏差，收敛判据仍对 double 解计算。
- 可默认构造的 `CalVolumeBinding`，公开 `bool bind(const std::shared_ptr<CalCoreSubmesh>& mesh, size_t bone_count, const CalVolumeCage& cage, std::string& error)`。失败返回 false，error 描述原因；失败不得覆盖旧绑定。成功复制 cage，保留 mesh 共享所有权。核心顶点、拓扑和骨权在绑定后不编辑，运行时 morph 权重可变。
- 在 `CalPhysique` 命名空间声明 `bool calculateVolumeVerticesAndNormals(const CalSkeleton&, const CalSubmesh&, const CalVolumeBinding&, const CalVolumeOptions&, float* output, size_t output_floats, CalVolumeReport&, std::string& error)`。输出沿用原生布局：每点 position xyzw、normal xyzw 共8 floats，输出容量至少8倍顶点数，和输入不别名；失败时报告与整个输出缓冲不改，error 可改。调用者已完成 Mixer 更新。单次运行串行，不承诺原有 morph 临时缓存的多线程能力。

普通正体积小变形须收敛。没有全局最优承诺；报告诚实反映驻点与预算。λ=0 时不迭代，但仍核对输入与初始正体积。返回成功的表面必须实际来自生产蒙皮及共享笼求解，不能只提供孤立数值函数。示例与测试应调用这个生产入口。原有资产文件格式不扩展。
