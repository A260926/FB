# 图集重建接口

新增类型都在 fauxgl 包中。三角形共享位置索引，UV逐角独立；所有输入视为线性浮点颜色，A忽略。只使用标准库及本地 simplify。

```go
type AtlasFace struct { Position [3]int; UV [3]Vector }
type AtlasMesh struct { Positions []Vector; Faces []AtlasFace }
type AtlasView struct { Matrix Matrix; Width,Height int; RGB []Color }
type AtlasSource struct { /* 拥有型快照，内部自由 */ }
type AtlasOptions struct { Width,Height int; SeamWeight,Tolerance float64; MaxIterations int }
type AtlasResult struct {
 Width,Height int
 RGB []Color
 Covered []bool
 RelativeResidual float64
 Iterations int
 Nonzeros int // 正规矩阵实际非零元数，含对称项
}
func CaptureAtlasSource(mesh AtlasMesh, view AtlasView) (*AtlasSource,error)
func ReconstructAtlas(mesh AtlasMesh, sources []*AtlasSource, options AtlasOptions) (*AtlasResult,error)
```

Capture复制几何、矩阵、RGB并用真实Context.DrawTriangles、ClipTriangle、rasterize捕获全部面的双面深度；旧绘制不改。输入无实例变换。原顶点投影w必须>=0.1，允许跨六个裁剪面；不接受零世界面积面、非法索引、非有限输入、非正尺寸、错误RGB长度。UV限[0,1]，Z忽略。空mesh/空source列表合法。Reconstruct的mesh必须与source捕获的几何/UV逐值相同，非法或不匹配返回nil,error。求解耗尽迭代也返回nil,error；不改输入、旧结果或快照。

图集行序v向下。texel中心UV=((x+0.5)/W,(y+0.5)/H)。按UV三角形的top-left半开规则（屏幕y向下，正面积后水平向右或非水平向上为包含边）确定归属，UV零面积面忽略；正面积重叠报错。精确相等的共享位置边及相同UV端点连接成同一个chart；几何边至多邻接两面，不允许重复面或非流形边。不相邻面可以同chart。邻接关系依位置索引而非浮点位置焊接。

对覆盖texel p，用UV仿射重心求世界P。第s个source投影P，只有在六面视锥内且w>0才取观测。源像素中心为(i+0.5,j+0.5)，投影光栅坐标为sx=(ndc.x+1)*width/2,sy=(1-ndc.y)*height/2，对(sx-0.5,sy-0.5)双线性取四tap。图外、空深度及零系数tap不贡献。由接收面的投影平面求每个tap中心的z/w（允许外推），映到depth=(z/w+1)/2；该depth<=捕获depth+1e-8才接纳。不能比较均值深度或用P的深度替代各tap。接收面在某source投影的屏幕面积为零时，该source对它不产生观测。令w_sp为接纳系数之和，c_sp为接纳RGB的加权平均；w=0代表无观测。无需视角权重。源顺序不影响方程。

未知量x_p为每个覆盖texel的三通道颜色。数据项为sum(s,p) w_sp * |x_p-c_sp|²。对同chart且四邻接的覆盖p,q，每条无向边只计一次；在两端均有观测的每个source，加入min(w_sp,w_sq)*|(x_p-x_q)-(c_sp-c_sq)|²。这是保细节梯度项，不跨chart按图集坐标误连。

接缝是共享几何边但两端UV不完全对应的边，按较小位置索引到较大索引定向。n=max(1,ceil(max(两侧UV边在texel坐标中的欧氏长度)))；k=0..n-1在t=(k+0.5)/n采样。两侧分别把UV换算为(x=u*W-0.5,y=v*H-0.5)的双线性行，只保留图内且归属该侧chart的覆盖tap并重新归一；任一侧无tap则省略该站点。把两行之差记a，加入SeamWeight/n*|a*x|²。两侧chart各自保留独立未知量，不把缝边像素强行焊成同一个texel。

除上述数据、梯度和接缝项，还对每个覆盖texel加入固定1e-4*|x_p|²的零先验正则。把数据/梯度/接缝行组成A,b，求H*x=f，其中H=AᵀA+1e-4*I，f=Aᵀb。相同行的重复未知量先合并。正则作用于每个覆盖texel，不按连通分量钉点，保证无观测岛及退化接缝支撑仍有唯一解；无任何观测的整个分量结果为零。Nonzeros统计加入正则后的H。非覆盖RGB为0，Covered=false；覆盖RGB的A固定为1。求解不能创建N²稠密矩阵，存储O(N+R)，R为实际有效稀疏行的项数，允许迭代法；256²图集能在240秒内完成。Tolerance在(0,1)，SeamWeight有限正数，MaxIterations>0。RelativeResidual用最终输出对全部覆盖texel正规方程重新计算||H*x-f||₂/max(1,||f||₂)，三通道一起计；必须<=Tolerance。空系统残差/轮数/非零元为0。Iterations是各通道实际轮数最大值，不能把内部残差估计当最终结果。

新文件 atlas_capture.go、atlas_reconstruct.go 必须在当前平台的GoFiles中并真实参与生产包；测试不能用build tag跳过。示例仅接输出目录，创建 average.png、reconstructed.png、average_model.png、reconstructed_model.png，至少16×16有效PNG，后两幅必须经原TextureShader真实绘制。测试按prompt三组接入taskcheck，完整入口bash task_support/run.sh target。
