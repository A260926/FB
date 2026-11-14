// PHZ
// 2018-9-30

#include "RtpConnection.h"
#include "RtspConnection.h"
#include "net/SocketUtil.h"
#include <atomic>
#include <cstring>

using namespace std;
using namespace xop;

namespace
{
/* <0 表示随机起始序号(默认), 否则为受控的起始序号 */
std::atomic_int g_initial_packet_seq(-1);
}

void RtpConnection::SetInitialPacketSeq(int seq)
{
	if (seq >= 0 && seq <= 0xffff) {
		g_initial_packet_seq.store(seq);
	}
	else {
		g_initial_packet_seq.store(-1);
	}
}

RtpConnection::RtpConnection(std::weak_ptr<TcpConnection> rtsp_connection)
    : rtsp_connection_(rtsp_connection)
{
	std::random_device rd;
	int initial_seq = g_initial_packet_seq.load();

	for(int chn=0; chn<MAX_MEDIA_CHANNEL; chn++) {
		rtpfd_[chn] = 0;
		rtcpfd_[chn] = 0;
		memset(&media_channel_info_[chn], 0, sizeof(media_channel_info_[chn]));
		media_channel_info_[chn].rtp_header.version = RTP_VERSION;
		media_channel_info_[chn].packet_seq = (initial_seq >= 0) ? (uint16_t)initial_seq : (rd()&0xffff);
		media_channel_info_[chn].rtp_header.seq = 0; //htons(1);
		media_channel_info_[chn].rtp_header.ts = htonl(rd());
		media_channel_info_[chn].rtp_header.ssrc = htonl(rd());
	}

	auto conn = rtsp_connection_.lock();
	rtsp_ip_ = conn->GetIp();
	rtsp_port_ = conn->GetPort();
}

RtpConnection::~RtpConnection()
{
	for(int chn=0; chn<MAX_MEDIA_CHANNEL; chn++) {
		if(rtpfd_[chn] > 0) {
			SocketUtil::Close(rtpfd_[chn]);
		}

		if(rtcpfd_[chn] > 0) {
			SocketUtil::Close(rtcpfd_[chn]);
		}
	}
}

int RtpConnection::GetId() const
{
	auto conn = rtsp_connection_.lock();
	if (!conn) {
		return -1;
	}
	RtspConnection *rtspConn = (RtspConnection *)conn.get();
	return rtspConn->GetId();
}

bool RtpConnection::SetupRtpOverTcp(MediaChannelId channel_id, uint16_t rtp_channel, uint16_t rtcp_channel)
{
	auto conn = rtsp_connection_.lock();
	if (!conn) {
		return false;
	}

	media_channel_info_[channel_id].rtp_channel = rtp_channel;
	media_channel_info_[channel_id].rtcp_channel = rtcp_channel;
	rtpfd_[channel_id] = conn->GetSocket();
	rtcpfd_[channel_id] = conn->GetSocket();
	media_channel_info_[channel_id].is_setup = true;
	transport_mode_ = RTP_OVER_TCP;

	return true;
}

bool RtpConnection::SetupRtpOverUdp(MediaChannelId channel_id, uint16_t rtp_port, uint16_t rtcp_port)
{
	auto conn = rtsp_connection_.lock();
	if (!conn) {
		return false;
	}

	if(SocketUtil::GetPeerAddr(conn->GetSocket(), &peer_addr_) < 0) {
		return false;
	}

	media_channel_info_[channel_id].rtp_port = rtp_port;
	media_channel_info_[channel_id].rtcp_port = rtcp_port;

	std::random_device rd;
	for (int n = 0; n <= 10; n++) {
		if (n == 10) {
			return false;
		}
        
		local_rtp_port_[channel_id] = rd() & 0xfffe;
		local_rtcp_port_[channel_id] =local_rtp_port_[channel_id] + 1;

		rtpfd_[channel_id] = ::socket(AF_INET, SOCK_DGRAM, 0);
		if(!SocketUtil::Bind(rtpfd_[channel_id], "0.0.0.0",  local_rtp_port_[channel_id])) {
			SocketUtil::Close(rtpfd_[channel_id]);
			continue;
		}

		rtcpfd_[channel_id] = ::socket(AF_INET, SOCK_DGRAM, 0);
		if(!SocketUtil::Bind(rtcpfd_[channel_id], "0.0.0.0", local_rtcp_port_[channel_id])) {
			SocketUtil::Close(rtpfd_[channel_id]);
			SocketUtil::Close(rtcpfd_[channel_id]);
			continue;
		}

		break;
	}

	SocketUtil::SetSendBufSize(rtpfd_[channel_id], 50*1024);

	peer_rtp_addr_[channel_id].sin_family = AF_INET;
	peer_rtp_addr_[channel_id].sin_addr.s_addr = peer_addr_.sin_addr.s_addr;
	peer_rtp_addr_[channel_id].sin_port = htons(media_channel_info_[channel_id].rtp_port);

	peer_rtcp_sddr_[channel_id].sin_family = AF_INET;
	peer_rtcp_sddr_[channel_id].sin_addr.s_addr = peer_addr_.sin_addr.s_addr;
	peer_rtcp_sddr_[channel_id].sin_port = htons(media_channel_info_[channel_id].rtcp_port);

	media_channel_info_[channel_id].is_setup = true;
	transport_mode_ = RTP_OVER_UDP;

	return true;
}

bool RtpConnection::SetupRtpOverMulticast(MediaChannelId channel_id, std::string ip, uint16_t port)
{
    std::random_device rd;
    for (int n = 0; n <= 10; n++) {
		if (n == 10) {
			return false;
		}
       
		local_rtp_port_[channel_id] = rd() & 0xfffe;
		rtpfd_[channel_id] = ::socket(AF_INET, SOCK_DGRAM, 0);
		if (!SocketUtil::Bind(rtpfd_[channel_id], "0.0.0.0", local_rtp_port_[channel_id])) {
			SocketUtil::Close(rtpfd_[channel_id]);
			continue;
		}

		break;
    }

	media_channel_info_[channel_id].rtp_port = port;

	peer_rtp_addr_[channel_id].sin_family = AF_INET;
	peer_rtp_addr_[channel_id].sin_addr.s_addr = inet_addr(ip.c_str());
	peer_rtp_addr_[channel_id].sin_port = htons(port);

	media_channel_info_[channel_id].is_setup = true;
	transport_mode_ = RTP_OVER_MULTICAST;
	is_multicast_ = true;
	return true;
}

void RtpConnection::Play()
{
	for(int chn=0; chn<MAX_MEDIA_CHANNEL; chn++) {
		if (media_channel_info_[chn].is_setup) {
			media_channel_info_[chn].is_play = true;
		}
	}
}

void RtpConnection::Record()
{
	for (int chn=0; chn<MAX_MEDIA_CHANNEL; chn++) {
		if (media_channel_info_[chn].is_setup) {
			media_channel_info_[chn].is_record = true;
			media_channel_info_[chn].is_play = true;
		}
	}
}

void RtpConnection::Teardown()
{
	if(!is_closed_) {
		is_closed_ = true;
		for(int chn=0; chn<MAX_MEDIA_CHANNEL; chn++) {
			media_channel_info_[chn].is_play = false;
			media_channel_info_[chn].is_record = false;
		}
	}
}

string RtpConnection::GetMulticastIp(MediaChannelId channel_id) const
{
	return std::string(inet_ntoa(peer_rtp_addr_[channel_id].sin_addr));
}

string RtpConnection::GetRtpInfo(const std::string& rtsp_url)
{
	char buf[2048] = { 0 };
	snprintf(buf, 1024, "RTP-Info: ");

	int num_channel = 0;

	auto time_point = chrono::time_point_cast<chrono::milliseconds>(chrono::steady_clock::now());
	auto ts = time_point.time_since_epoch().count();
	for (int chn = 0; chn<MAX_MEDIA_CHANNEL; chn++) {
		uint32_t rtpTime = (uint32_t)(ts*media_channel_info_[chn].clock_rate / 1000);
		if (media_channel_info_[chn].is_setup) {
			if (num_channel != 0) {
				snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), ",");
			}			

			snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf),
					"url=%s/track%d;seq=0;rtptime=%u",
					rtsp_url.c_str(), chn, rtpTime);
			num_channel++;
		}
	}

	return std::string(buf);
}

void RtpConnection::SetFrameType(uint8_t frame_type)
{
	frame_type_ = frame_type;
	if(!has_key_frame_ && (frame_type == 0 || frame_type == VIDEO_FRAME_I)) {
		has_key_frame_ = true;
	}
}

void RtpConnection::SetRtpHeader(MediaChannelId channel_id, RtpPacket pkt)
{
	if((media_channel_info_[channel_id].is_play || media_channel_info_[channel_id].is_record) && has_key_frame_) {
		media_channel_info_[channel_id].rtp_header.marker = pkt.last;
		media_channel_info_[channel_id].rtp_header.ts = htonl(pkt.timestamp);
		media_channel_info_[channel_id].rtp_header.seq = htons(media_channel_info_[channel_id].packet_seq++);
		memcpy(pkt.data.get()+4, &media_channel_info_[channel_id].rtp_header, RTP_HEADER_SIZE);
	}
}

int RtpConnection::SendRtpPacket(MediaChannelId channel_id, RtpPacket pkt)
{    
	if (is_closed_) {
		return -1;
	}
   
	auto conn = rtsp_connection_.lock();
	if (!conn) {
		return -1;
	}

	RtspConnection *rtsp_conn = (RtspConnection *)conn.get();

	bool ret = rtsp_conn->task_scheduler_->AddTriggerEvent([this, channel_id, pkt] {
		this->SetFrameType(pkt.type);
		this->SetRtpHeader(channel_id, pkt);
		if((media_channel_info_[channel_id].is_play || media_channel_info_[channel_id].is_record) && has_key_frame_ ) {
			if(!is_multicast_) {
				// 实际交给发送路径的原包, 存一份私有拷贝供 Generic NACK 重传
				this->SaveRtpPacket(channel_id, pkt.data.get()+RTP_TCP_HEAD_SIZE, pkt.size-RTP_TCP_HEAD_SIZE);
			}
			if(transport_mode_ == RTP_OVER_TCP) {
				SendRtpOverTcp(channel_id, pkt);
			}
			else {
				SendRtpOverUdp(channel_id, pkt);
			}

			//media_channel_info_[channel_id].octetCount  += pkt.size;
			//media_channel_info_[channel_id].packetCount += 1;
		}
	});

	return ret ? 0 : -1;
}

int RtpConnection::SendRtpOverTcp(MediaChannelId channel_id, RtpPacket pkt)
{
	auto conn = rtsp_connection_.lock();
	if (!conn) {
		return -1;
	}

	uint8_t* rtpPktPtr = pkt.data.get();
	rtpPktPtr[0] = '$';
	rtpPktPtr[1] = (char)media_channel_info_[channel_id].rtp_channel;
	rtpPktPtr[2] = (char)(((pkt.size-4)&0xFF00)>>8);
	rtpPktPtr[3] = (char)((pkt.size -4)&0xFF);

	conn->Send((char*)rtpPktPtr, pkt.size);
	return pkt.size;
}

int RtpConnection::SendRtpOverUdp(MediaChannelId channel_id, RtpPacket pkt)
{
	int ret = sendto(rtpfd_[channel_id], (const char*)pkt.data.get()+4, pkt.size-4, 0,
					(struct sockaddr *)&(peer_rtp_addr_[channel_id]), sizeof(struct sockaddr_in));

	if(ret < 0) {
		Teardown();
		return -1;
	}

	return ret;
}

void RtpConnection::SaveRtpPacket(MediaChannelId channel_id, const uint8_t* data, uint32_t size)
{
	if (size < RTP_HEADER_SIZE) {
		return;
	}

	SavedRtpPacket saved;
	saved.seq = (uint16_t)((data[2] << 8) | data[3]);
	saved.size = size;
	saved.data.reset(new uint8_t[size], std::default_delete<uint8_t[]>());
	memcpy(saved.data.get(), data, size);

	std::deque<SavedRtpPacket>& history = saved_rtp_packets_[channel_id];
	history.push_back(saved);
	while (history.size() > kMaxSavedRtpPackets) {
		history.pop_front(); // 按正常发送顺序淘汰最旧的包
	}
}

bool RtpConnection::HandleRtcpFrame(uint8_t rtcp_channel, const uint8_t* data, uint32_t size)
{
	if (is_closed_ || transport_mode_ != RTP_OVER_TCP) {
		return false;
	}

	for (int chn = 0; chn < MAX_MEDIA_CHANNEL; chn++) {
		if (media_channel_info_[chn].is_setup &&
			media_channel_info_[chn].rtcp_channel == rtcp_channel) {
			HandleRtcpFeedback((MediaChannelId)chn, data, size);
			return true;
		}
	}

	return false;
}

bool RtpConnection::HandleRtcpPacket(SOCKET rtcp_sockfd, const uint8_t* data, uint32_t size)
{
	if (is_closed_ || transport_mode_ != RTP_OVER_UDP) {
		return false;
	}

	for (int chn = 0; chn < MAX_MEDIA_CHANNEL; chn++) {
		if (media_channel_info_[chn].is_setup && rtcpfd_[chn] == rtcp_sockfd) {
			HandleRtcpFeedback((MediaChannelId)chn, data, size);
			return true;
		}
	}

	return false;
}

void RtpConnection::HandleRtcpFeedback(MediaChannelId channel_id, const uint8_t* data, uint32_t size)
{
	std::vector<RtcpNackRequest> nacks;
	if (!ParseRtcpFeedback(data, size, nacks)) {
		return; // 非法的复合包不触发任何补包
	}

	uint32_t media_ssrc = ntohl(media_channel_info_[channel_id].rtp_header.ssrc);
	for (size_t n = 0; n < nacks.size(); n++) {
		if (nacks[n].media_ssrc != media_ssrc) {
			continue; // 媒体 SSRC 对不上
		}

		for (size_t k = 0; k < nacks[n].seq_list.size(); k++) {
			RetransmitRtpPacket(channel_id, nacks[n].seq_list[k]);
		}
	}
}

bool RtpConnection::RetransmitRtpPacket(MediaChannelId channel_id, uint16_t seq)
{
	std::deque<SavedRtpPacket>& history = saved_rtp_packets_[channel_id];
	for (size_t n = 0; n < history.size(); n++) {
		if (history[n].seq != seq) {
			continue;
		}

		// 原样补发第一次发送的 RTP 字节: 不推进正常序号, 也不再进入历史
		if (transport_mode_ == RTP_OVER_TCP) {
			auto conn = rtsp_connection_.lock();
			if (!conn) {
				return false;
			}

			uint32_t frame_size = history[n].size + RTP_TCP_HEAD_SIZE;
			std::shared_ptr<char> frame(new char[frame_size], std::default_delete<char[]>());
			frame.get()[0] = '$';
			frame.get()[1] = (char)media_channel_info_[channel_id].rtp_channel;
			frame.get()[2] = (char)((history[n].size & 0xFF00) >> 8);
			frame.get()[3] = (char)(history[n].size & 0xFF);
			memcpy(frame.get() + RTP_TCP_HEAD_SIZE, history[n].data.get(), history[n].size);
			conn->Send(frame, frame_size);
			return true;
		}
		else if (transport_mode_ == RTP_OVER_UDP) {
			int ret = sendto(rtpfd_[channel_id], (const char*)history[n].data.get(), history[n].size, 0,
						(struct sockaddr *)&(peer_rtp_addr_[channel_id]), sizeof(struct sockaddr_in));
			return ret >= 0;
		}

		return false;
	}

	return false; // 找不到的包直接忽略
}
