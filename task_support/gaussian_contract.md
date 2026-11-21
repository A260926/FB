# 连续场和结果约定

GaussianKernel 含 Point3 mean、std::array<double,6> covariance（xx,xy,xz,yy,yz,zz）、double extinction 和 Radiance source。covariance 是世界空间对称正定矩阵 C。全部输入有限，extinction 和 source 各分量非负；求逆、包围范围或后续必要中间量不能有限表示时也拒绝。set_kernels(vector<GaussianKernel>) 复制输入，失败抛 invalid_argument 并保留旧快照。旧输入释放或修改不影响已建对象。核顺序不影响物理结果，比较浮点值允许1e-9。

对 x，q=(x-mean)^T C^-1(x-mean)。q<=9 时 g=exp(-q/2)，其他位置 g=0。sigma(x)=Σ extinction_i*g_i；j(x)=Σ extinction_i*g_i*source_i，source 是每单位消光的RGB源函数，不是单独的每单位体积发光。有限支撑不是透明表面，不参与 Object::intersect 的最近命中。

Ray 表示 O+tD，D 不必单位化，ds=|D|dt。interval 为 [a,b]，0<=a<=b，a有限，b可以为正无穷。O/D有限、D非零；abs_error 有限正数，max_work 为正整数。非法查询抛 invalid_argument，已有 result 不变。对零长段、空集合或全零消光：T=1、E=0、error=0。查无穷尾段时只积分有界核支撑。

精确定义是 tau(a,t)=|D|*integral_a^t sigma(O+uD)du，T=exp(-tau(a,b))，E=|D|*integral_a^b exp(-tau(a,t))*j(O+tD)dt。光学厚度用截断Gaussian线积分求出，不用固定步长近似；返回 T 的绝对误差不超过1e-10。发射结果逐通道满足 |returned_E-exact_E|<=returned_error<=abs_error，允许1e-10浮点裕量。需要根据连续函数的有效包围形成误差界；固定小步长、相邻两次近似之差、核按中心排序合成都不满足该约定。不要求把IEEE舍入误差也做成严格机器区间证明。

max_work 限制实际发射区间评估次数；达到预算仍不能保证误差时 segment 返回false，不交付半截结果。sigma 的精确线积分和BVH候选测试不计入这个预算，但必须如实计入相应stats。render_gaussian 每像素共用该计数到全部镜面分支，输出中 error 包含所有这些分支的加权发射误差。正的累积光程不能通过射线方向归一化或epsilon推进被吞掉。

新相机入口使用原 PHONG 的像素中心和递归深度、ambient、材质反射权重、阴影shadow_frac及局部照明；源参数限定这些RGB与强度有限非负、reflectivity在[0,1]、无折射或BRDF。LightList 可嵌套，但叶灯只支持 PointLight。输入没有生命周期并发修改。反射起点沿用原来的0.001偏移，每个介质段从实际创建的射线原点起积到其下一交点；不补回该既有偏移。有限影线从其实际偏移原点积到灯的位置，遇到不透明遮挡时保留原几何shadow_frac，并乘这一完整直线灯段的T，完全不加E；没有几何折射影线路径。主射线最近曲面之后的核不影响首段T，反射回来的段另算。输出 error 是线性RGB绝对界，不是显示颜色。
