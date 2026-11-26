# 声道森林（Channel Forest）

声道森林模式利用多声道录音之间的相关性，为每个声道选择独立编码，或以任意其他声道为父进行差分编码。所有声道的父边组成一个无环森林，并使整帧总成本全局最小。

## 接口

两个构造函数都新增了末尾参数 `bool channelForest`，默认值为 `false`，因此原有源码和旧输出保持兼容：

```cpp
#include "src/include/frame.hpp"

// 默认 false：走原来的编码路径
frame::FrameEncoder legacyEncoder(wavFrame);

// 显式开启声道森林
frame::FrameEncoder forestEncoder(wavFrame, true);
data::SelaFrame selaFrame = forestEncoder.process();
```

文件入口同样把开关传到每一个编码工作线程：

```cpp
#include "src/include/sela/encoder.hpp"

std::ifstream input("input.wav", std::ios::binary);
sela::Encoder encoder(input, /*channelForest=*/true);
file::SelaFile output = encoder.process();
```

完整的文件编码/解码示例见 [`examples/channel_forest.cpp`](channel_forest.cpp)。先运行 `bash task_support/build.sh`，然后可这样编译：

```bash
c++ -std=c++11 -O2 -pthread -I. \
  examples/channel_forest.cpp .task-build/libsela.a \
  -o .task-build/channel_forest_example

.task-build/channel_forest_example input.wav output.sela decoded.wav
```

## 当前约束

开启 `channelForest` 后：

- 支持 1 至 8 个声道；
- 仅支持 16 位有符号 PCM；
- 每帧每声道必须为 2048 个采样；
- WAV 中可以包含多个完整帧；
- 不扩展位深，也不增加短尾帧支持。

关闭开关时仍使用原有编码逻辑，旧文件字节不变。

## 成本与并列规则

每个子帧都使用生产代码中的 LPC 和 Rice 编码器真实生成。候选成本为实际子帧固定字段和两个 32 位数组所占字节数：

```text
cost = 12 + 4 * (reflection_coefficient_word_count
                 + residue_word_count)
```

差分子帧的信号仍定义为：

```text
difference[sample] = parent[sample] - child[sample]
```

独立根在并列比较向量中记作声道数 `N`。最终方案满足：

1. 每个声道恰好是独立根，或选择一个其他声道作为父；
2. 父依赖图无环；
3. 所有子帧成本之和最小；
4. 成本相同时，从声道 0 开始比较父编号向量，取字典序最小者。

独立子帧写入文件时，`parentChannelNumber` 仍沿用旧含义并写回声道自身；只有并列选择向量使用 `N` 表示根。

## 解码与校验

`FrameDecoder` 不依赖 `subFrames` 的存储顺序。它先建立“声道编号 → 子帧”的映射并完成全部结构校验，再沿父依赖恢复任意合法的多层森林，输出仍按声道编号排列。

恢复前会验证：

- 声道编号恰好覆盖 `0 ... N-1`；
- 子帧类型只能是 `0`（独立）或 `1`（依赖）；
- 依赖父节点存在且不能是自身；
- 父子两端的采样数一致；
- 依赖图无环。

不合法结构抛出 `data::Exception`。文件解码工作线程会保存异常并在 `join()` 后交回调用线程，不会让异常逃出线程而终止进程。新解码器仍可读取旧的全独立文件和旧双声道差分文件。

## 回归测试

回归位于 `tests/channel_forest.cpp`，通过 `task_support/check.h` 注册 `optimal`、`dependencies`、`compatibility` 三组用例：

```bash
bash task_support/run.sh target
```

测试包含独立穷举成本/并列规则、相关与反相关材料、互相最便宜但会成环的材料、逐样本文件往返、乱序多层依赖、非法结构、文件线程异常传播，以及关闭开关时与 legacy FNV 字节指纹的比较。
