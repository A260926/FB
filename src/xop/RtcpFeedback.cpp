// PHZ
// 2026-11-15

#include "RtcpFeedback.h"

using namespace xop;

namespace
{

const uint8_t RTCP_VERSION = 2;
const uint8_t RTCP_RTPFB = 205;     /* RFC 4585 传输层反馈 */
const uint8_t RTPFB_FMT_NACK = 1;   /* Generic NACK */

uint16_t ReadUint16BE(const uint8_t* p)
{
	return (uint16_t)((p[0] << 8) | p[1]);
}

uint32_t ReadUint32BE(const uint8_t* p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
			((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* 同一份复合包里重复请求的序号只保留一次 */
void AppendNackSeq(RtcpNackRequest& request, uint16_t seq)
{
	for (size_t n = 0; n < request.seq_list.size(); n++) {
		if (request.seq_list[n] == seq) {
			return;
		}
	}
	request.seq_list.push_back(seq);
}

}

bool xop::ParseRtcpFeedback(const uint8_t* data, uint32_t size, std::vector<RtcpNackRequest>& nacks)
{
	nacks.clear();

	uint32_t offset = 0;
	while (offset < size) {
		uint32_t remaining = size - offset;
		if (remaining < 4) {
			return false;
		}

		uint8_t version = data[offset] >> 6;
		if (version != RTCP_VERSION) {
			return false;
		}

		bool has_padding = (data[offset] & 0x20) != 0;
		uint8_t fmt = data[offset] & 0x1f;
		uint8_t payload_type = data[offset + 1];
		uint32_t pkt_size = ((uint32_t)ReadUint16BE(data + offset + 2) + 1) * 4;
		if (pkt_size > remaining) {
			return false; /* 子包长度越界 */
		}

		bool is_last = (offset + pkt_size == size);
		uint32_t padding = 0;
		if (has_padding) {
			if (!is_last) {
				return false; /* padding 只允许出现在最后一个子包 */
			}
			padding = data[offset + pkt_size - 1];
			if (padding == 0 || padding > pkt_size - 4) {
				return false; /* padding 长度不合法 */
			}
		}

		uint32_t content_size = pkt_size - padding;
		if (payload_type == RTCP_RTPFB && fmt == RTPFB_FMT_NACK) {
			/* 固定的反馈头之外, FCI 必须按 32 位对齐 */
			if (content_size < 12 || ((content_size - 12) & 0x3) != 0) {
				return false;
			}

			uint32_t media_ssrc = ReadUint32BE(data + offset + 8);
			RtcpNackRequest* request = nullptr;
			for (size_t n = 0; n < nacks.size(); n++) {
				if (nacks[n].media_ssrc == media_ssrc) {
					request = &nacks[n];
					break;
				}
			}
			if (request == nullptr) {
				nacks.push_back(RtcpNackRequest());
				request = &nacks.back();
				request->media_ssrc = media_ssrc;
			}

			for (uint32_t fci = offset + 12; fci + 4 <= offset + content_size; fci += 4) {
				uint16_t pid = ReadUint16BE(data + fci);
				uint16_t blp = ReadUint16BE(data + fci + 2);
				AppendNackSeq(*request, pid);
				for (int bit = 0; bit < 16; bit++) {
					if (blp & (1 << bit)) {
						/* 16 位序号自然回绕, 跨 65535/0 也能展开 */
						AppendNackSeq(*request, (uint16_t)(pid + bit + 1));
					}
				}
			}
		}
		/* 其余合法子包(RR/SR/SDES/PSFB 或不支持的 FMT)跳过即可 */

		offset += pkt_size;
	}

	return true;
}
