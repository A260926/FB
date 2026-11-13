# c-ares 可重放性质模糊测试（property fuzzing）

这组测试用一个**独立于 c-ares 的逻辑模型**生成 DNS 响应，再通过 c-ares
的真实公开接口解析，核对解析结果能否始终由该模型解释。生成器、压缩编码器
和期望值都在 `model.c` / `model.h` 中，**不调用 c-ares 产生预期，也不从
被测的展开结果反推模型**；c-ares 只出现在核对（oracle）一侧。

## 文件

| 文件 | 作用 |
| --- | --- |
| `model.h` / `model.c` | 逻辑模型、确定性生成器、三种合法报文布局的压缩编码器、oracle、逻辑差异模拟器 |
| `target.c` | `int task_cases(const char* group)`，驱动 500 个固定种子 case，分 model / metamorphic / replay 三组，断言均为 `TASK_CHECK` |
| `driver.c` | 组驱动，依次运行三个组，每组要求至少 5 个有效断言 |
| `check.h` | `TASK_CHECK` 断言宏 |
| `baseline.c` / `baseline.sh` | 只调用原库的最小冒烟基线 |
| `check.sh` | 先跑基线，再编译并运行性质测试 |

## 语法范围（grammar scope）

只生成**符合协议**的报文，不需要网络 resolver 或真实 DNS 服务：

- 名称是 ASCII 域名的标签数组，标签字符集为 `[A-Za-z0-9-]`（连字符只出现在
  标签内部）；根名（root，线上为单个 0 字节）也会出现。
- 一条响应包含 1 个 question、**1 到 4 跳 CNAME 链**，以及末端的
  **A（IPv4，4 字节）或 AAAA（IPv6，16 字节）**记录集（1 到 4 条）。
- 同一 case 的名称共享一个两标签基域，CNAME 目标逐级在前面叠加一个新标签，
  从而在合法前提下产生真实的多级后缀压缩。
- 三种合法布局：
  - `layout=0` 不压缩（所有名称完整写出，指针层数为 0）；
  - `layout=1` 只压缩后缀（指针只指向更早的**整条名称**起点）；
  - `layout=2` 跨记录多级后缀压缩（指针可指向更早的任意**名称标签边界**）。
- 所有压缩指针只能指向**此前已经写入**的标签边界，禁止前向指针；报文在构造
  时再次校验这一点。
- 名称线上长度与完整逻辑名称都满足 RFC 1035 的 255 字节限制；整份报文远小于
  65535 字节上限（本批最大约 747 字节）。

## 核对内容

- `ares_parse_a_reply` / `ares_parse_aaaa_reply` 解析出的 `hostent`：
  规范名、别名（按 CNAME 链顺序）、地址族、地址宽度、地址身份与**地址顺序**，
  名称比较按 DNS ASCII 大小写不敏感规则（`strcasecmp`）。
- 对报文中记录的每个名称，调用 `ares_expand_name`，把它返回的 `enclen`
  与编码器按字节布局记下的实际线上占用字节数逐一比对，同时核对展开拼写；
  根名展开为空字符串 `""` 且 `enclen` 正确。
- 变形（metamorphic）性质：合法的末端记录顺序调整（交换前两条 / 整体逆序，
  均为地址集合上的双射）、query 名大小写变化、以及不相关附加 TXT 记录
  （置于 answer 前部 / answer 后部 / additional 区）都**不得改变问答身份与
  最终地址集合**。

## 种子重放（seed replay）

- 随机源是自带的 xorshift32，不使用 `rand()`，完全确定。
- 主种子固定为 `MASTER_SEED`，case 号 `n`（0..499）与种子混合后重新播种，
  因此同一 `(seed, n)` 永远生成同一模型、同一报文字节。
- replay 组会重新生成并重新构建，逐字节比较两次报文，并在序列末尾重跑固定
  case 比较长度，验证可重放性。

## 覆盖数字（一次完整运行的实际统计）

```
cases=500 layouts=1500 builds=1500 skipped=0
chain_len_1=123 chain_len_2=133 chain_len_3=128 chain_len_4=116
A=260 AAAA=240 root_names=225
ptr_depth_max=5 compressed_names=6892 multilevel_names=4167
uncompressed_layout_pointers=0 extra_records=241
```

- 500 个 case × 3 种布局 = 1500 次构建，**0 跳过**（不靠跳过凑覆盖）。
- 四种 CNAME 链长度、A 与 AAAA、根名、多级指针（最深 5 层间接）均有覆盖；
  不压缩布局的指针数严格为 0。
- 变形测试运行 2723 个变体，身份差异、地址集合差异、query 名差异均为 0。

## 结果限制（result limits）

- 只覆盖上述 RFC 1035 子集：单 question、CNAME 链加末端 A/AAAA，以及一个
  不相关 TXT 记录；不覆盖其它记录类型、EDNS、TCP、截断标志等。
- 只构造**协议合法**的输入；非法报文仅在 replay 组用“末端截掉一个字节”的
  方式验证库会拒绝（返回非 `ARES_SUCCESS`），不做畸形模糊。
- 完全离线：不联网、不启动 resolver、不依赖真实 DNS 服务。
- 逻辑差异模拟器（`em_diff_simulator`）只在**期望值**上翻转一个地址字节，
  用于证明 oracle 确实会报错并保留首个 case；它不修改、也不需要修改任何
  上游库代码来制造漏洞。

## 构建与运行

```sh
cmake -S . -B .task-build -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DCARES_STATIC=ON -DCARES_SHARED=OFF \
  -DCARES_BUILD_TESTS=OFF -DCARES_BUILD_TOOLS=OFF
cmake --build .task-build -j 4
sh task_support/baseline.sh   # 仅原库调用的冒烟基线
sh task_support/check.sh      # 基线 + 全部性质测试
```

成功时三个组各打印 `GROUP ... assertions=N complete`，最后输出
`TARGET_COMPLETE`。运行中每个 case 打印种子与逻辑链，每种布局打印报文长度、
指针层数与名称数；若存在偏差，会在汇总处打印 `FIRST DEVIATION`（种子、
case、布局、逻辑链与首个偏差详情）。
