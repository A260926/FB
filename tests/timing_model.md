# Intel PT 时间模型独立回归说明

本文件配套 `tests/task_cases.c`，解释独立晶振 tick 模型如何工作、两条可
手算的包序列、每个包的取整规则，以及数字取证场景下事件时间记录的证据
含义。所有数值均可用整数算术手工复核，不依赖 `pt_time.c` 的位掩码实现。

## 1. 模型与记号

独立模型按完整晶振 tick 推进，再把状态投影到 Intel PT 包字段：

- 一个 MTC 周期含 `P = 2^mtc_freq` 个晶振 tick。
- MTC 包只携带周期计数器的低 8 位；可观测 CTC 宽度
  `W = mtc_freq + 8`，即一个可观测绕回跨度为 `2^W` 个 tick。
- CPUID 0x15 给出晶振到 TSC（快计数器 FC）的比例 `ebx/eax`。
- FCR 是 8 位小数定点值：`fcr = (FC 每周期) << 8`。例如 2 个 FC/周期
  对应 `fcr = 512`，4 个 FC/周期对应 `fcr = 1024`。
- TSC 包是 56 位负载，时间模型在收到 TSC 时把 `base` 与 `fc` 重置到
  该锚点。

### 取整规则（全部为整数运算，向负无穷截断）

1. 晶振距离转 FC：
   `fc = (ctc_delta * ebx) / eax`，整数除法向下取整。
2. 单个 CYC 包的量化贡献（按生产约定每个 CYC 独立量化后累加）：
   `fc += (cyc * fcr) >> 8`。
3. 跨 MTC 周期的 CYC，先估算旧周期应扣除的周期数：
   `old_cyc = (fc_old << 8) / fcr`，同样向下取整；
   当 `old_cyc > payload` 时饱和处理——把整个 payload 归给旧周期，
   当前周期贡献为 0（宁慢勿快）。
4. MTC 校准：`fcr = (fc_period << 8) / cyc_since_last_mtc`。
5. 头部 TSC 校准：`fcr = (tsc_delta << 8) / cyc_since_last_tsc`，
   单次 56 位绕回按 `tsc += 2^56` 修正。
6. 标称频率 CBR 校准：`fcr = (nom_freq << 8) / cbr`。

这些除法的余数被库有意保留为估计误差，不代表真实时钟精度；文档与测试
都不把估计值当作精确墙钟。

## 2. 手算序列 A：单 MTC 周期内的两个审计点（anchors 组）

配置：`mtc_freq=4`（P=16）、`eax=2, ebx=1`、`nom_freq=8`、CBR=2，
故标称 `fcr = (8<<8)/2 = 1024`，即每周期 4 个 FC。

| 包序号 | 包 | 模型动作 | 估计 TSC |
|---:|---|---|---:|
| 0 | PSB | 进入 PSB+ | 无时间 |
| 1 | CBR 2 | 安装 fcr=1024 | 无时间 |
| 2 | TSC 10000 | 锚点 base=10000, fc=0 | 10000 |
| 3 | TMA ctc=0 fc=0 | CTC 位置 0 | 10000 |
| 4 | CYC 36（事件 AUDIT-1） | `(36*1024)>>8 = 144` | 10144 |
| 5 | CYC 10（事件 AUDIT-2） | 累加 `(10*1024)>>8 = 40` | 10184 |
| 6 | MTC 周期 1 | fc 清零，base 推进一个周期 | 10184 |
| 7 | PSBEND | 离开 PSB+ | 10184 |

因为两个 CYC 都落在同一 MTC 周期，`ctc == ctc_cyc`，不发生跨周期扣除。
事件记录：

- `AUDIT-1`：包序号 4，锚点 TSC=10000（包 2），`lost_mtc=0,
  lost_cyc=0`，时间可确定，TSC=10144。
- `AUDIT-2`：包序号 5，同一锚点，lost 计数均为 0，TSC=10184。

## 3. 手算序列 B：跨 8 位 MTC 绕回的事件（wrap 组）

配置同上（P=16，可观测宽度 W=12，绕回跨度 4096 tick），CBR=2，
fcr=1024。

| 包序号 | 包 | 模型动作 | 估计 TSC |
|---:|---|---|---:|
| 0 | PSB | 进入 PSB+ | 无时间 |
| 1 | CBR 2 | fcr=1024 | 无时间 |
| 2 | TSC 50000 | 锚点 | 50000 |
| 3 | TMA ctc=4080 fc=0 | 记录 CTC 位置 4080 | 50000 |
| 4 | MTC 周期 255（事件 WRAP-PRE） | ctc=4080，base 不变 | 50000 |
| 5 | MTC 周期 256（负载 0，事件 WRAP-BOUNDARY） | 距离修正：`0+4096-4080=16` tick | 50000 |
| 6 | CYC 20（事件 WRAP-POST） | 先扣旧周期再量化 | 50080 |
| 7 | PSBEND | | 50080 |

绕回修正：MTC 负载从 255 跳到 0，模型在无界周期域先加回一个可观测跨度
4096，再求差。CYC 到达时 `fc==0` 且 `ctc(0) != ctc_cyc(4080)`：

- 晶振距离 16 tick 对应 `fc_old = 16*1/2 = 8`；
- 旧周期估计 `old_cyc = (8<<8)/1024 = 2`；
- 有效 `cyc = 20 - 2 = 18`，贡献 `(18*1024)>>8 = 72`；
- TSC = 50000 + 0（MTC 已并入 base）+ 72 = 50080。

事件记录：

- `WRAP-PRE`：包序号 4，锚点 50000，lost 均 0，可确定，TSC=50000。
- `WRAP-POST`：包序号 6，锚点 50000，lost 均 0，可确定，TSC=50080。

若删掉 `pt_time_ctc_delta()` 的单次绕回修正，本序列包 5 的 MTC 会直接
返回 `-pte_bad_packet`（数值 -5）；运行器在最早出错的序列处即中止并报
`TIMING_MISMATCH field=update_errcode actual=0xfffffffb`（wrap 变异）。

## 4. 校准丢失与恢复的取证解释（recalibrate 组）

`lost_mtc`/`lost_cyc` 计数的是**未能用于时间更新的包数**，不是硬件实际
丢弃的包总数。序列 recal-5：

| 包 | 动作 | lost_cyc | 证据有效性 |
|---|---|---:|---|
| TSC 6100 | 锚点 | 0 | 可确定 |
| TMA 0,0 | 建立 CTC | 0 | 可确定 |
| CYC 7（事件 RECAL-LOST） | 无 fcr，无法更新 | 1 | **无法确定时间** |
| CYC 9 | 仍无 fcr | 2 | 无法确定时间 |
| TSC 6400（事件 RECAL-TSC-REC） | 重新锚定，清零 lost | 0 | 恢复可确定 |

`RECAL-LOST` 记录保留包位置、锚点 6100 与 `lost_cyc=1`，并显式标记
`determined=0`：该窗口只能引用上一个锚点，不能把估计值当成精确墙钟。
下一个 TSC 到达后证据有效性才恢复。若删掉 MTC 恢复路径里的
`time->lost_cyc = 0`，wrap-9 在包 4 报
`TIMING_MISMATCH field=lost_cyc actual=1 expect=0`（loss_reset 变异）。

### 跨流排序限制

两个不同 PT 流（例如两台设备或两个不共享锚点的会话）之间，只要任一方
处于 `determined=0` 窗口，就**不能**断言全局先后顺序。回归中的
`evidence_orderable()` 仅在两侧都可确定时才允许排序；估计插值或丢包
窗口都不构成跨流墙钟证据。

## 5. 仍然不覆盖的范围

- 不采集任何硬件 trace，也不运行被分析机器码；全部输入是离线合成包。
- 不扩展 CPU errata（SKL168、SKD007、SKZ84 等），`errata` 保持全零。
- 不处理 TSC 缩放/偏移（VMX）、双 MTC 绕回——库本身只能修正单次绕回，
  双绕回在生产代码中不可检测，测试不假装能识别。
- 同一 MTC 周期内多个 CYC 的重叠误差是库文档承认的固有不精确，回归
  逐位复现该行为，但不把它宣称为时钟精度。
- 不覆盖 TSC/TMA/MTC/CYC/CBR/PSB/PSBEND 之外的包，也不覆盖 IP 压缩、
  PIP、MODE、电源包等与时间模型无关的路径。
- 不替代 `ptunit-time`：旧单测仍由 `task_support/run.sh` 并联执行。
- 校准的绝对墙钟精度依赖真实晶振频率；本回归只验证包序列与量化/回绕/
  恢复逻辑的确定性，不验证硬件频率准确度。
