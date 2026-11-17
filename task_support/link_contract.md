# 链接组接收契约

开关仅增加传输组装，默认关闭。不增加另一个文字解析器。每次 Decode 输入从现有 80/81 ff 头开始；PES_data_packet_header_length 仍适用。后面恰好一个链接片：第一个字节高六位为完整 data_group_id，低两位 version；接着 link_number、last_link_number、大端16位 payload 长度、payload、两字节大端 CRC。CRC-16 使用多项式 0x1021、初值 0xffff、无反射、无末异或，覆盖从 data_group_id/version 字节起至 payload 末。CRC 字节不参与正文拼接。长度必须恰好匹配。

同一完整 data_group_id 至多一个在途组，不同 ID 独立。首片号0，last 可为0；后续必须依次递增且 version/last 一致。最近已接收片若 header/payload 相同，是幂等重发，返回 NoCaption；包括首片在仅有首片时重发，不能改时间。除此之外，合法的片0替换旧组；新版本片0也如此。非零片缺前序、倒序、内容不同的重传、version/last变化、CRC或长度错误均返回 Error 并丢掉该 ID 的在途组，其他组保留。无法读出组 ID 的坏头返回 Error，保留其他组。每组拼接 payload 上限1 MiB，超限同样丢掉该组。已完成组无传输重发缓存；它之后再收到片0是新一组。

中间片返回 NoCaption，无 Caption，且不改变文字解码状态。末片到达才用拼好的连续 payload 调原管理/正文解析；正文解析失败丢组并返回 Error，不要求撤销原解析器本已有的部分副作用。完整管理包的旧 A/B 去重仅在此次调用原解析后生效。语言筛选也在完成后执行。输出 pts 来自首片，包含 PTS_NOPTS；不能做时间加法。link0/last0仍需验证CRC然后立刻解析。已有 Caption 自有数据的约定不变。

Flush、Initialize及 prompt 列出的实际配置变化丢弃所有在途组；相同配置不丢。开关从开到关再到开不能恢复旧组。C 接口已有结果清理约定保持；公开声明、实现、导出可见性及文档一起更新。
