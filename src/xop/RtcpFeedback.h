// PHZ
// 2026-11-15
// RFC 4585 Generic NACK (RTPFB, FMT=1) 反馈解析

#ifndef XOP_RTCP_FEEDBACK_H
#define XOP_RTCP_FEEDBACK_H

#include <cstdint>
#include <vector>

namespace xop
{

/* 一条 Generic NACK 请求: 某个媒体 SSRC 下需要重传的 RTP 序号集合 */
struct RtcpNackRequest
{
	RtcpNackRequest()
		: media_ssrc(0)
	{
	}

	uint32_t media_ssrc;          /* 主机字节序 */
	std::vector<uint16_t> seq_list; /* PID/BLP 按 16 位序号展开(可跨 65535/0), 同一份复合包内已去重 */
};

/*
 * 解析一份完整的 RTCP 复合包(UDP 下是一个数据报, TCP 下是一个交错帧的负载)。
 * 复合包结构合法时返回 true, nacks 列出其中的 Generic NACK (PT=205, FMT=1)
 * 请求; 合法但不支持的反馈类型(RR/SR/PSFB 等)直接跳过。
 * 版本、子包长度、FCI 对齐或最后一个子包的 padding 不合法时返回 false,
 * 调用方不得根据这份包触发任何重传。
 */
bool ParseRtcpFeedback(const uint8_t* data, uint32_t size, std::vector<RtcpNackRequest>& nacks);

}

#endif
