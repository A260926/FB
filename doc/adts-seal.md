# ADTS 短组封口（seal）

`aacEncSealAdts()` 会把传输层已经攒住、但尚未按
`AACENC_TPSUBFRAMES` 整组交付的 raw-data-block 小组立即封成一个 ADTS 帧。
它不会结束编码器、不会补静音，也不会重新编码音频。

## 支持边界

只有同时满足以下条件的“已初始化且没有待重初始化”实例才支持：

- AAC-LC，帧长 1024；
- 单声道或双声道（`MODE_1` / `MODE_2`）；
- 32 / 44.1 / 48 kHz；
- 96 / 128 / 192 kbps CBR；
- `TT_MP4_ADTS`；
- `AACENC_TPSUBFRAMES` 为 2、3 或 4；
- `AACENC_PROTECTION` 为 0 或 1；
- 不含 PCE、SBR、PS、MPS/metadata，也没有按 AU 附加的 ancillary。

其他已初始化配置返回 `AACENC_UNSUPPORTED_PARAMETER`。尚未完成初始化或有参数
变更等待重初始化时返回 `AACENC_INVALID_CONFIG`；空句柄返回
`AACENC_INVALID_HANDLE`；`info` 为空也返回 `AACENC_INVALID_CONFIG`。

封口对象仅限已经真实编码完、仍在等待 ADTS 聚合交付的 `k` 个 AU，其中
`1 <= k < N`，`N` 是 `AACENC_TPSUBFRAMES`。还没攒满一个 AAC 帧的 PCM 仍保留在
编码器内部输入台账中，之后可继续调用 `aacEncEncode()`。

## 调用方式

```c
AACENC_AdtsSealInfo info;

/* out=NULL：只读查询所需大小。 */
err = aacEncSealAdts(enc, NULL, 0, &info);

/* 调用方按 info.numBytes 分配空间。 */
err = aacEncSealAdts(enc, buffer, capacity, &info);
```

只读查询时，`numAUs`、`numBytes` 报告当前暂存组；`bitResBefore` 和
`bitResAfter` 都填当前生产 QC 储备值，`creditedBits` 为 0。暂存组为空时返回
成功，且所有计数为 0。

如果 `out` 非空但 `capacity < info.numBytes`，返回新增错误码
`AACENC_OUTPUT_BUFFER_TOO_SMALL`（`0x0061`），同时在 `info` 中报告所需大小。
此时不会写输出，ADTS 组装状态、PCM 台账和 QC 状态均保持不变；可以用更大的
缓冲区安全重试。

成功时，完整短 ADTS 帧被复制到 `out`，暂存组被提交，传输聚合器的块编号回到
0，并继续按原来的 `N` 攒下一组。成功后再次查询为空。原来的满组交付路径保持
不变；短组之后的完整组会重建自己的位置表和块编号，不会继承旧位置。

## ADTS 帧重建

输出帧按实际块数 `k` 重建：

- `aac_frame_length`；
- `number_of_raw_data_blocks_in_frame`；
- 多块时的 raw-data-block 位置表；
- 头部 CRC 和各 raw block CRC。

各 AU 中已经编码出的 AAC raw payload 位保持原样，不重新量化、不移动语法位。

保护开启且 `k == 1` 时，不能继续使用原多块布局中每个块独立的 raw CRC。新帧
是单 raw-data-block 布局，其 CRC 是 AAC/ADTS 规定的共享 CRC：覆盖 56 位
ADTS 头以及 AAC 语法中的 CRC 保护域；不是简单搬运旧 CRC，也不是对整个
payload 求 CRC。`k >= 2` 时仍使用多块规则：头部/位置表有一个头部 CRC，每个
raw data block 后保留独立 raw CRC。保护关闭时没有位置表，也没有 CRC。

## 位储备

`removedBits` 是“原来的 `N` 块暂存表示”和“最终 `k` 块短帧”之间真实的总位差，
包含位置表和 CRC 布局变化。

成功封口非空组时，恰好把

```text
min(removedBits, bitResTotMax - bitResBefore)
```

位返还给生产 QC 储备（`QC_STATE::bitResTot`）。`creditedBits` 报告实际返还
位数，并且：

```text
bitResAfter = bitResBefore + creditedBits
```

超过 `bitResTotMax` 上限的位直接舍弃，不会变成额外的帧预算。只读查询、容量
失败、不支持的调用以及空组都不会修改储备。返还后的值会进入后续真实的量化和
码率分配流程，而不只是反映在诊断结构中；该接口也不会清空储备。

## 生命周期与 flush

封口只是提前结束一个传输层聚合边界，不是 EOF。心理声学、变换、量化器和 PCM
历史都会延续。原有 flush 路径
（`aacEncEncode(..., .numInSamples = -1, ...)`）仍用于填充/排空编码延迟并
最终得到 `AACENC_ENCODE_EOF`。本功能不能由“关闭再重开编码器”实现，也不代替
flush。

完整命令行示例见 `examples/adts_seal_example.cpp`；独立 ADTS 解析、标量 CRC
参照、事务/重试、QC 储备和生产解码器验证见
`tests/adts_seal_cases.cpp`。
