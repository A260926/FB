# zlib 可重放性质模糊测试（task_support）

这一套测试在**真实的 zlib 库接口**上做性质（property-based）模糊测试，而不是另写一个
只测自己的演示程序。核心思路是：正常输入经过状态或表示上的变换后，结果必须仍然能由一个
**独立于 zlib 的模型**解释。

## 文件清单

| 文件 | 角色 |
| --- | --- |
| `model.h` / `model.c` | 独立参考模型：确定性 PRNG、字节缓冲、独立 Adler-32/FNV、动作语法、生命周期状态机、生成器与合法性校验、覆盖统计。**该编译单元不包含 `zlib.h`**（源码内有 `#error` 强制），因此不可能调用被测的 copy 函数。 |
| `target.c` | 唯一接触 zlib 的编译单元：把一个 `Program` 通过真实的 `deflateInit2/deflate/deflateCopy/inflateInit2/inflate/inflateCopy/...End` 跑完，并用模型做 oracle 比对。提供 `int task_cases(const char* group)`，使用 `TASK_CHECK`。 |
| `driver.c` / `check.h` | 任务自带驱动与断言宏，未改动。 |
| `check.sh` | 先跑基线，再编译 `driver.c + target.c + model.c + libz.a` 并运行。 |
| `mutate.sh` | 预先确定的逻辑差异模拟器：用 `TASK_MUTATE=1` 只污染 oracle 的期望值，验证 oracle 真的会报错并停在首个 case。**不改上游库代码。** |
| `baseline.c` / `baseline.sh` | 任务自带的原始库调用基线，未改动。 |

## 语法范围（有界生命周期）

一个程序是至多 `TASK_MAX_ACTIONS = 64` 个动作的序列，至多
`TASK_MAX_STREAMS = 4` 个活跃流，原语总量硬截断在 `TASK_MAX_INPUT = 64 KiB`。

动作集合（`model.h` 中的 `OP_*`）：

- `DINIT`：`deflateInit2`，可配 level(0..9)/memLevel(1..9)/strategy(0..4)，
  `windowBits` 取 9..15，正数为 zlib 包装、负数为 raw deflate；可在 init 之后、
  任何 `deflate` 之前立即 `deflateSetDictionary`。
- `FEED`：喂入模型生成的原文字节（调用方分块、刻意保留一部分未消费输入）。
- `FLUSH`：仅 `Z_SYNC_FLUSH` 或 `Z_FULL_FLUSH`。
- `DCOPY`：`deflateCopy` 分叉出新的输出身份；模型自行复制原语前缀，不调用 zlib copy。
- `FINISH` / `DEND`：先 `Z_FINISH` 排空到 `Z_STREAM_END`，再 `deflateEnd`（顺序强制）。
- `IINIT` / `IFEED` / `ICOPY` / `IFINISH` / `IEND`：对应的 inflate 生命周期，
  输入/输出缓冲都有界；`IEND` 强制在 `IFINISH` 之后。

状态机（`program_legal`）只允许满足原 API 前置条件的迁移，例如：槽位不能重复初始化、
只能对未结束的流喂入/flush/copy、copy 目标槽必须空闲、`inflateInit2` 只能绑定已经
`FINISH` 的压缩输出、字典只在允许处设置、程序结束时所有流必须关闭。每个生成出的程序在
执行前都会先过一遍这个独立校验。

### raw 与 zlib 两条路径、字典规则

- raw（负 `windowBits`）与 zlib 包装（正 `windowBits`）各有独立的生成路径。
- 压缩端字典只在 init 之后立即设置。
- zlib 包装解压时，由库返回 `Z_NEED_DICT`，引擎用模型保存的字典、并核对库给出的
  Adler-32 与**独立实现的** `model_adler32` 一致后再 `inflateSetDictionary`。
- raw inflate 允许在任意时刻设字典，引擎在 init 后立即预置（`dict_raw` 路径）。

### copy 的所有权划分

- 滑动窗口与内部压缩/解压状态归**库**所有，由 `deflateCopy/inflateCopy` 复制。
- `next_in`、`next_out` 归**调用方**所有。引擎为每个流维护独立的调用方存储：
  待消费输入 `in`、已收集输出 `out`（inflate 侧为私有压缩输入 `inbuf` 与游标 `pos`）。
- copy 之后两份副本重新绑定到**各自独立**的输入/输出缓冲，断言两份副本不会写同一块
  输出缓冲；已保存的输出前缀按字节比对一致。

两种被显式覆盖的形态：

1. 同一前缀 `deflateCopy` 后给两个分支不同后缀，各自独立 `FINISH` 并各自解码，
   每个完整压缩输出都还原出**自己那条分支**的原文（模型校验共享前缀一致、后缀独立）。
2. inflate 已消耗部分正常压缩输入、且输出缓冲受限时做 `inflateCopy`，重新绑定缓冲，
   先把**原流**驱动到 `Z_STREAM_END`，再继续副本，核对已产生前缀与后续解码一致
   （`end_original_first` 统计）。

驱动只喂自己正常编码出来的压缩流，从不构造损坏报文或漏洞触发输入。

## 种子重放

- PRNG 是无全局状态的 splitmix64，`program_generate(pg, seed)` 对同一 seed 必然生成
  字节一致的程序（`program_hash` 用独立 FNV-1a 做结构指纹并断言两次生成相同）。
- 固定跑 `TASK_SEED_COUNT = 500` 个种子。
- 每个程序在 replay 组里用**全新的引擎状态**跑两遍，对每条压缩输出计算 FNV 指纹并要求
  两次完全相同；因此重放不依赖上一次的地址或任何全局状态。
- 引擎给 zlib 装了自定义 `zalloc/zfree` 计数分配器，断言程序结束时存活分配为 0。

## 三组断言

`driver.c` 依次调用 `task_cases("model" | "metamorphic" | "replay")`，每组都远超
驱动要求的至少 5 个有效断言：

- **model**：独立 Adler-32/FNV 的固定向量；每个程序合法、有界；同种子再生成一致；
  真实库跑通且 zlib 包装流的 `adler` 等于独立模型的 Adler-32。
- **metamorphic**：语法层与引擎层同时验证两类 copy、部分输入/部分输出、先结束原流、
  两类 flush、raw/zlib、字典路径，以及分叉后共享前缀一致/后缀独立的形变关系。
- **replay**：双跑指纹一致、500 次重放、分配全部释放、关闭检查数量达标，并打印覆盖。

## 一次代表性运行的覆盖数字

数值随代码确定、同环境稳定，以 `check.sh` 实际输出为准：

```
coverage: programs=1000 replays=500
  deflate streams=2290 inflate streams=2420
  deflateCopy=720 (partial_in=478 partial_out=268)
  inflateCopy=744 (partial_in=678 partial_out=144)
  inflate original-ended-before-copy-completion=582
  SYNC_FLUSH=928 FULL_FLUSH=916
  dict zlib NEED_DICT path=668 raw dict path=598
  raw programs=1096 zlib programs=1194 branch-suffix runs=720
  clean end checks=6174 live allocations peak=28
```

语法边界实测：动作数 12..64（命中 64 上限），单程序输出身份最多 10 个，原文总量最大约
44 KiB，硬上限始终为 64 KiB。

## 结果限制（边界与诚实声明）

- 这是**正常输入的性质测试**：只验证合法生命周期与表示变换的可解释性，不做负面/崩溃
  测试，也不向 inflate 喂损坏数据。
- oracle 的期望值由独立模型给出，模型本身不含压缩代码、不调用 `deflateCopy/inflateCopy`；
  它验证的是“库的行为符合模型”，而非替代库的压缩算法。
- 覆盖数字是生成器在 500 个固定种子上的**真实运行统计**，不是静态估计；空原文流（仅
  zlib 头/尾）也属于合法用例并被接受。
- 差异模拟器只翻转 oracle 期望的首字节，`mutate.sh` 断言进程在首个失配处退出；它
  **不修改任何上游 zlib 源码**。

## 运行方式

```sh
cmake -S . -B .task-build -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
      -DCMAKE_C_FLAGS=-Dfdopen=fdopen -DBUILD_SHARED_LIBS=OFF
cmake --build .task-build -j 4
sh task_support/baseline.sh   # 原始库调用基线
sh task_support/check.sh      # 性质测试（model / metamorphic / replay）
sh task_support/mutate.sh     # 逻辑差异模拟器，确认 oracle 能抓住首个错误
```

实现与测试只使用 C99 与 POSIX shell，不引入第三方包，也不联网。
