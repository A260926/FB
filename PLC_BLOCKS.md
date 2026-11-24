# `gsm_decode_plc` 块接口与丢失策略

`gsm_decode_plc()` 以完整压缩块为单位推进 GSM 06.10 音频时间轴：

```c
int gsm_decode_plc(gsm s,
                   const gsm_byte *data,
                   unsigned int bytes,
                   unsigned int good_mask,
                   gsm_signal *pcm,
                   unsigned int capacity);
```

返回写入的样本数；参数或块状态不合法时返回 `-1`。错误调用不会修改解码器状态，也不会写入 `pcm`。

## 块单位

| 上下文 | 压缩块长度 | 输出样本 | `good_mask` |
|---|---:|---:|---|
| 普通 GSM | 33 字节 | 160 | 只有 bit0 有效 |
| WAV49 | 65 字节 | 320 | bit0/bit1 分别表示前、后 160 样本半帧是否完好 |

`capacity` 以 `gsm_signal` 为单位，至少要容纳对应块的 160 或 320 个样本。

只要 mask 中至少有一个好半帧，调用时就必须提供完整的 33 或 65 字节块，因为好半帧的参数可能需要整块中的位置和 WAV49 共享字节。全部丢失时有两种合法输入：

- 提供完整块，但相应数据被忽略；
- 传 `data == NULL` 且 `bytes == 0`。

普通模式中，若 bit0 表示帧好，首字节高 4 位必须仍是 `GSM_MAGIC` (`0xD`)。mask 含越界位、`bytes` 不符合上述规则、容量不足或 `pcm == NULL` 时直接拒绝。

## WAV49 边界与共享半字节

旧的 `gsm_decode()` WAV49 入口按半帧推进，第一次调用后 `frame_index` 停在半块中间。`gsm_decode_plc()` 只接受整块边界上的 WAV49 上下文；如果旧入口刚解完前半帧、尚未解后半帧，块入口会返回 `-1`，且不改变这个中间状态。

成功的块调用结束后，WAV49 状态一定回到整块边界，因此旧入口和新入口可以在整块边界交替使用。

65 字节 WAV49 块的第 33 个字节（下标 32）由两个半帧共享。后半帧参数累加器的初始 4 bit 是该字节的高半字节：

```c
second_half_low_bits = (data[32] >> 4) & 0x0f;
```

当前半帧丢失、后半帧完好（`good_mask == 2`）时，PLC 先合成前 160 个样本，然后直接从共享字节取这 4 bit 并运行原库的后半帧解析/解码路径。不会先把坏的前半帧送入正常解码器，因而不会让坏参数推进长期预测、短时合成或去加重状态。

## 最近好帧缓存

每个由 `gsm_decode()` 或 `gsm_decode_plc()` 成功解码的 160 样本帧都会记录：

- 8 个解码后的 `LARc`；
- 4 个子帧实际使用的有效 `Nc`；
- 4 个子帧的 `bc`；
- 4 组、每组 40 点的 RPE 反量化激励。

原始 `Nc` 超出 40..120 时，沿用原库规则回退到 `s->nrp`，并像 `Gsm_Long_Term_Synthesis_Filtering()` 一样更新 `s->nrp`。因此缓存中保存的是真正参与预测的 lag。

缓存是 `struct gsm_state` 内的固定数组，不按流长分配或增长。隐藏帧只读取最近好帧缓存，不覆盖它；任何好的 160 样本帧到达后都会把连续丢失计数清零。WAV49 的前、后两个 160 样本半帧按音频时间轴分别计数：整块丢失时，前半帧和后半帧分别使用第 `n` 和第 `n+1` 个衰减等级。

## 隐藏帧递推

这是播放器自己的确定性策略，不声明符合某个外部 PLC 标准。

连续第 `n` 个丢帧的 Q15 衰减系数为：

| `n` | gain |
|---:|---:|
| 1 | 29491 |
| 2 | 26214 |
| 3 | 19661 |
| 4 | 13107 |
| 5 | 6554 |
| 6 及以后 | 0 |

定义：

```text
q15(x, g) = floor((x * g + 16384) / 32768)
```

每个子帧使用最近好帧对应子帧的有效 `Nc`、`bc`，以及当前长期历史 `dp0`：

```text
p         = q15(history[k - Nc], QLB[bc])   # 原库舍入/饱和
new_drp[k] = sat16(q15(old_excitation[k], g) + q15(p, g))
```

`QLB` 仍是原库的 `{3277, 11469, 21299, 32767}`。40 个新残差生成后，执行与原 LTP 合成相同的 120 点历史移位；后续子帧和后续帧会使用这些真实生成的残差。

之后与正常帧完全相同：

1. `Gsm_Short_Term_Synthesis_Filter()` 使用复用的 `LARc` 推进短时合成状态；
2. `Gsm_Postprocessing()` 推进去加重状态并执行原来的截断/放大。

当 `g == 0` 时，上式产生全零新残差，但不会强行把整帧 PCM 置零：短时合成和去加重滤波器中此前留下的状态仍会产生尾音。

如果解码器从创建后还没有见过好帧，则使用全零 `LARc`、零旧激励以及初始零历史开始合成。

## 状态连续性与等价性

无丢失时：

- 普通模式 `gsm_decode_plc(s, data, 33, 1, pcm, capacity)` 与原 `gsm_decode(s, data, pcm)` 逐样本相同；
- WAV49 `good_mask == 3` 时，等价于依次对块起点和 `data + 33` 调用两次旧 `gsm_decode()`。

PLC 隐藏帧直接更新同一个 `dp0`、`nrp`、`LARpp/j`、`v` 和 `msr`。因此丢帧后恢复的好帧会沿用隐藏段留下的长期历史、短时滤波和去加重状态。

## 测试与示例

运行任务要求的目标：

```sh
bash task_support/run.sh target
```

它会编译 `src/*.c` 下的实际生产实现、`task_support/driver.c` 和 `tests/plc_blocks.c`，检查 `Excitation`、`Packing`、`Recovery` 三组均有用例和断言，然后运行正常音频基线。

编译并运行独立示例：

```sh
make lib/libgsm.a
cc -std=c99 -DSASR -DWAV49 -Iinc examples/plc_blocks.c \
   lib/libgsm.a -o /tmp/plc_blocks_example
/tmp/plc_blocks_example /tmp/plc_blocks.pcm
```
