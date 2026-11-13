# zlib 可重放性质模糊测试

这套测试针对 madler/zlib 的流式 `deflate*` / `inflate*` 接口做**性质测试
（property-based）**，核心命题是：正常输入在经历状态或表示上的变换
（分片喂入、flush、`deflateCopy`/`inflateCopy`、缓冲重绑）之后，结果仍然
能被一个**独立于被测 copy 函数的模型**解释。所有动作都走真实库接口，不写
只测自身的演示程序。

## 构建与运行

```sh
cmake -S . -B .task-build -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
      -DCMAKE_C_FLAGS=-Dfdopen=fdopen -DBUILD_SHARED_LIBS=OFF
cmake --build .task-build -j 4
sh task_support/baseline.sh   # 既有基线，只演示原库调用
sh task_support/check.sh      # 本性质测试 + 故障模拟器
```

`check.sh` 只依赖 C99 编译器、`sh`、`grep` 和静态库 `.task-build/libz.a`，
不安装任何包、不联网。

## 文件布局

| 文件 | 职责 |
| --- | --- |
| `task_model.h/.c` | 独立模型：确定性 RNG（splitmix64）、可增长字节缓冲、带计数的 zlib 分配器、以及**只使用非 copy 流式 API** 的参考编解码器 |
| `task_harness.h/.c` | 有界操作程序的生成器、在真实库上的解释器、`inflateCopy` 专项场景 |
| `target.c` | 入口 `int task_cases(const char* group)`，`model`/`metamorphic`/`replay` 三组断言，以及故障模拟器的独立 `main` |
| `check.h` / `driver.c` | 既有断言宏与组驱动（每组至少 5 个有效断言） |
| `check.sh` | 编译四个翻译单元并运行；另编译运行故障模拟器 |

## 生命周期语法（动作程序）

一个程序是满足真实 API 前置条件的有界动作序列，动作集合：

- `INIT`：`deflateInit2`，窗口位 `15`（zlib 包装）或 `-15`（raw deflate）；
  字典仅在首个 `deflate()` 之前用 `deflateSetDictionary` 设置。
- `FEED` / `PARTFEED`：把模型生成的原文字节追加进调用方输入存储；后者故意
  保留一部分不提供给 `avail_in`，制造复制时刻的部分输入。
- `FLUSH`：`Z_SYNC_FLUSH` 或 `Z_FULL_FLUSH`。连续 flush 无新输入时库返回
  `Z_BUF_ERROR`，解释器按文档将其视为无操作而非失败。
- `DCOPY`：`deflateCopy` 到空闲槽位，随后**重绑**全部调用方存储。
- `FINISH`：`Z_FINISH` 直到 `Z_STREAM_END`，先排空暂存输入，再捕获整条流。
- `END`：`deflateEnd` 并释放槽位。

硬性边界（生成器保证，测试再断言）：

- 每个程序最多 64 个动作、最多 4 个活跃流、原文总量 ≤ 64 KiB；
- copy 只能落到空闲槽位；每个流恰好 finish/end 一次；
- 原始库只收到测试自身正常编码出来的压缩流，不构造损坏报文或漏洞触发器。

### copy 的所有权划分

库拥有 window 与内部状态；`next_in` / `next_out` 归调用方。解释器在 copy
前后严格遵守这一点：

- 输入存储、未取走的输出、已保存的压缩前缀、期望原文都放在外部模型
  （`task_branch`）里；
- copy 后两个分支拿到**各自独立分配**的输入/输出缓冲，断言指针互不相同，
  两个副本绝不写同一块输出；
- 复制时刻区分三段输入：已消费（模型游标）、已提供未消费（`avail_in`）、
  调用方暂存（游标之后、`avail_in` 之外），副本只重绑前两段的状态；
- 期望状态的复制用普通内存拷贝完成，**从不调用 `deflateCopy`/`inflateCopy`
  来构造期望值**（`task_model.c` 中没有任何 copy 符号）。

## 三组测试

1. **model（独立模型）**：RNG 的确定性与种子敏感性；模型原文经真实流式 API
   在 raw/zlib 两种包装下往返；两种字典路径（zlib 的 `Z_NEED_DICT` 与 raw
   的流前字典）；参考编码器对相同输入产出字节级一致的流；模型缓冲为深拷贝、
   分支间不共享内存。
2. **metamorphic（变换）**：跑满 500 个固定种子程序；同一前缀 copy 后给两个
   分支不同后缀，各自独立完成并由参考解码器还原出本分支原文；另含
   `inflateCopy` 专项——在输入只消费一部分、输出槽饱和时复制，重绑缓冲后
   **先结束原流再继续副本**，核对已产出前缀与后续解码都与模型原文一致。
3. **replay（资源/重放边界）**：每个程序先捕获再重复重放，压缩字节逐字节
   相同；全新生成（新堆布局、重置全局）后捕获仍稳定；计数分配器在每个程序
   结束后 live 数归零，重置/重放周期后账目正确；动作与原文边界始终成立。

## 种子重放

- 前 8 个程序是手工钉住的（pinned），保证两条 copy、部分输入/输出、先结束
  原流、两类 flush、raw/zlib、字典等路径无论 RNG 如何都被覆盖；
- 其余程序由程序下标经 splitmix64 确定性生成，无时间、地址或全局状态参与；
- 同一下标任意次重放产生相同动作序列与相同压缩字节（已用两次独立进程
  `diff` 验证），因此失败可直接用程序编号稳定复现。

## 覆盖数字（固定种子下的实测值）

`check.sh` 运行时由 `COVERAGE ...` 行打印。当前实现的一次参考运行：

```
programs=500 raw=233 zlib=267 dict=181 sync=273 full=253
dcopy=1032 icopy=8 part_in=143 part_out=247 forks=1032
finish_first=625 idict=4 peak=4
```

含义：500 个程序中 raw 233 / zlib 267；字典路径 181；SYNC/FULL flush 分别
273/253；`deflateCopy` 1032 次、`inflateCopy` 8 次；复制时刻带部分输入 143
次、带未取走输出 247 次；分支分叉 1032 次；先结束原流 625 次；inflate 字典
路径 4 次；活跃流峰值 4。三次组运行的有效断言数为 model 187、metamorphic
66007、replay 261696（合计约 32.8 万）。这些数字由固定种子决定，重新运行
应逐字复现；若调整生成器权重或钉住程序，数字会随之变化，以实际打印为准。

## 逻辑差异模拟器（oracle 自检）

为证明 oracle 真的会报错，`target.c` 在 `-DTASK_FAULT_MAIN` 下提供独立
`main`，把一个**测试侧**的预定差异注入 oracle 输出（翻转首字节），上游库
代码不做任何改动。`check.sh` 会编译并运行它，要求：

- 进程以非零状态退出，且停在**第一个** case（program 0）；
- 日志同时包含 `FAULT_SIM first case: program 0` 与 `FAIL`。

手动复现：

```sh
cc -std=c99 -O2 -Itask_support -DTASK_FAULT_MAIN \
   task_support/target.c task_support/task_model.c \
   task_support/task_harness.c -I. -I.task-build \
   .task-build/libz.a -o .task-build/task_fault
.task-build/task_fault ; echo "exit=$?"   # 期望 exit=1
```

## 结果限制

- 本测试是**行为性质**测试：独立模型负责生成原文并以非 copy 流式 API 充当
  解码 oracle，它不是 deflate 压缩算法本身的形式化模型或证明。
- 驱动只喂自身正常编码的流，刻意不覆盖损坏报文、内存破坏或漏洞触发路径；
  发现的是状态/表示变换与资源生命周期层面的回归，而非输入解析健壮性。
- 语料固定为 500 个有界种子程序，不是无界随机 fuzz；边界（64 动作 / 4 流 /
  64 KiB）之外的行为不在覆盖范围内。
- 资源释放通过注入 zlib 的计数分配器（每程序 live 归零）与独立的
  AddressSanitizer/UBSan 运行共同保证；macOS 的 ASan 不带 LeakSanitizer，
  泄漏结论以计数分配器断言为准。
- 语言与依赖限定为 C99 与 POSIX `sh`，无第三方包、无网络访问。
