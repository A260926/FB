# 蒙版子图问题：根因与影响范围

## 根因

`SetMask` 借用的 `*image.Alpha` 蒙版如果是从大图 `SubImage` 切出来的，其
`Bounds().Min` 不为零，且 `Pix` 只是父图像素的一个窗口（含行内 padding 和
相邻像素）。旧代码在两处隐含地假设蒙版原点为 `(0,0)` 且 `Pix` 紧密排列：

1. **采样坐标系错位**。所有绘制入口都直接用设备坐标 `(x,y)` 去索引蒙版，
   而不是 `(x+Bounds.Min.X, y+Bounds.Min.Y)`：
   - `pattern.go` 的 `patternPainter.Paint`（Fill/FillPreserve、
     Stroke/StrokePreserve 的实际像素写入处）调用 `mask.AlphaAt(x, y)`；
   - `context.go` 的 `DrawImageAnchored` 传 `DstMaskP: image.ZP`；
   - `context.go` 的 `DrawStringAnchored` 与 `ClipPreserve` 里的
     `draw.DrawMask` 都传 `maskp = image.ZP`。

   对 `Bounds.Min` 非零的子图，这些采样全部落在子图有效区域之外，
   `AlphaAt` 返回 0，于是整块内容看起来都被裁掉了。

2. **`InvertMask` 遍历整个 `Pix` 切片**。子图的 `Pix` 与父图共享内存，
   按 `for i := range Pix` 反转会把行 padding、右侧父图列、以及最后一行
   之后的父图字节一起改掉（复现中邻接字节 77 被改成 178）。

## 修复

- 统一约定：蒙版左上角（`Bounds().Min`）映射到设备像素 `(0,0)`，尺寸匹配
  只看逻辑尺寸（`Bounds().Size()`），不看绝对原点。该映射对几何变换不变
  ——变换只影响路径光栅化，不移动蒙版。
- `patternPainter.Paint` 改为 `mask.AlphaAt(x+mp.X, y+mp.Y)`；
  `DrawImageAnchored`、`DrawStringAnchored`、`ClipPreserve` 的
  `DstMaskP`/`maskp` 改为 `dc.mask.Bounds().Min`。
- `InvertMask` 改为只遍历 `Bounds()` 覆盖的有效区域（按 `PixOffset` 逐行

  处理），padding 与父图其他像素不再被触碰。
- 保持不变的行为：`SetMask` 仍然借用而不复制（调用方改子图立即生效）；
  尺寸不匹配返回错误并保留原蒙版；`Clip` 交集结果仍归 context 所有；
  `ResetClip`、零原点紧密蒙版、nil 蒙版的 `InvertMask` 行为兼容。

## 影响范围

- 受影响：所有经过蒙版的绘制入口——Fill/FillPreserve、
  Stroke/StrokePreserve（经 `patternPainter`）、DrawImage/DrawImageAnchored、
  DrawString 系列，以及 Clip/ClipPreserve 的交集计算；还有对子图蒙版调用
  `InvertMask` 的调用方（父图数据曾被越界改写）。
- 不受影响：蒙版为 nil 或零原点紧密蒙版的既有用法（坐标映射退化为原行为）；
  Push/Pop 状态保存语义；`AsMask` 的返回值。
