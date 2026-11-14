// PHZ
// 2018-6-8

#ifndef XOP_RTP_CONNECTION_H
#define XOP_RTP_CONNECTION_H

#include <cstdint>
#include <vector>
#include <deque>
#include <string>
#include <memory>
#include <random>
#include "rtp.h"
#include "media.h"
#include "RtcpFeedback.h"
#include "net/Socket.h"
#include "net/TcpConnection.h"

namespace xop
{

class RtspConnection;

class RtpConnection
{
public:
    RtpConnection(std::weak_ptr<TcpConnection> rtsp_connection);
    virtual ~RtpConnection();

    void SetClockRate(MediaChannelId channel_id, uint32_t clock_rate)
    { media_channel_info_[channel_id].clock_rate = clock_rate; }

    void SetPayloadType(MediaChannelId channel_id, uint32_t payload)
    { media_channel_info_[channel_id].rtp_header.payload = payload; }

    bool SetupRtpOverTcp(MediaChannelId channel_id, uint16_t rtp_channel, uint16_t rtcp_channel);
    bool SetupRtpOverUdp(MediaChannelId channel_id, uint16_t rtp_port, uint16_t rtcp_port);
    bool SetupRtpOverMulticast(MediaChannelId channel_id, std::string ip, uint16_t port);

    uint32_t GetRtpSessionId() const
    { return (uint32_t)((size_t)(this)); }

    uint16_t GetRtpPort(MediaChannelId channel_id) const
    { return local_rtp_port_[channel_id]; }

    uint16_t GetRtcpPort(MediaChannelId channel_id) const
    { return local_rtcp_port_[channel_id]; }

    SOCKET GetRtpSocket(MediaChannelId channel_id) const
    { return rtpfd_[channel_id]; }

    SOCKET GetRtcpSocket(MediaChannelId channel_id) const
    { return rtcpfd_[channel_id]; }

    std::string GetIp()
    { return rtsp_ip_; }

    uint16_t GetPort()
    { return rtsp_port_; }
    
    bool IsMulticast() const
    { return is_multicast_; }

    bool IsSetup(MediaChannelId channel_id) const
    { return media_channel_info_[channel_id].is_setup; }

    std::string GetMulticastIp(MediaChannelId channel_id) const;

    void Play();
    void Record();
    void Teardown();

    std::string GetRtpInfo(const std::string& rtsp_url);
    int SendRtpPacket(MediaChannelId channel_id, RtpPacket pkt);

    /*
     * RFC 4585 Generic NACK 重传: 处理一份完整的 RTCP 复合包。
     * HandleRtcpFrame  用于 RTP_OVER_TCP, 按协商的 RTCP 通道号找到媒体通道;
     * HandleRtcpPacket 用于 RTP_OVER_UDP, 按协商的 RTCP socket 找到媒体通道。
     * 反馈里的媒体 SSRC 也要和该通道对上, 否则忽略。找不到的包直接忽略。
     */
    bool HandleRtcpFrame(uint8_t rtcp_channel, const uint8_t* data, uint32_t size);
    bool HandleRtcpPacket(SOCKET rtcp_sockfd, const uint8_t* data, uint32_t size);

    /*
     * 受控的起始序号入口: seq 在 [0, 65535] 内时, 之后创建的 RtpConnection
     * 以它作为各媒体通道的起始序号; 传入其他值(如 -1)恢复随机(默认行为)。
     * 供测试确定性地构造序号跨 65535/0 等场景。
     */
    static void SetInitialPacketSeq(int seq);

    bool IsClosed() const
    { return is_closed_; }

    int GetId() const;

    bool HasKeyFrame() const
    { return has_key_frame_; }

private:
    friend class RtspConnection;
    friend class MediaSession;
    void SetFrameType(uint8_t frameType = 0);
    void SetRtpHeader(MediaChannelId channel_id, RtpPacket pkt);
    int  SendRtpOverTcp(MediaChannelId channel_id, RtpPacket pkt);
    int  SendRtpOverUdp(MediaChannelId channel_id, RtpPacket pkt);

    /* 把实际交给发送路径的原包深拷贝一份存入历史(MediaSession 会让同一
     * 调度器上的多个客户端复用 RtpPacket 数据, 不能保存共享指针) */
    void SaveRtpPacket(MediaChannelId channel_id, const uint8_t* data, uint32_t size);
    void HandleRtcpFeedback(MediaChannelId channel_id, const uint8_t* data, uint32_t size);
    bool RetransmitRtpPacket(MediaChannelId channel_id, uint16_t seq);

    struct SavedRtpPacket
    {
        uint16_t seq = 0;
        uint32_t size = 0;                 /* RTP 包大小(不含 TCP 交错头) */
        std::shared_ptr<uint8_t> data;     /* 第一次发送时的 RTP 字节, 私有拷贝 */
    };

    /* 每个连接的每条媒体通道保留最近 512 个原包, 按发送顺序淘汰;
     * 随 RtpConnection 析构与连接一起释放 */
    static const uint32_t kMaxSavedRtpPackets = 512;
    std::deque<SavedRtpPacket> saved_rtp_packets_[MAX_MEDIA_CHANNEL];

	std::weak_ptr<TcpConnection> rtsp_connection_;
    std::string rtsp_ip_;
    uint16_t rtsp_port_;

    TransportMode transport_mode_;
    bool is_multicast_ = false;

	bool is_closed_ = false;
	bool has_key_frame_ = false;

    uint8_t  frame_type_ = 0;
    uint16_t local_rtp_port_[MAX_MEDIA_CHANNEL];
    uint16_t local_rtcp_port_[MAX_MEDIA_CHANNEL];
    SOCKET rtpfd_[MAX_MEDIA_CHANNEL];
    SOCKET rtcpfd_[MAX_MEDIA_CHANNEL];

    struct sockaddr_in peer_addr_;
    struct sockaddr_in peer_rtp_addr_[MAX_MEDIA_CHANNEL];
    struct sockaddr_in peer_rtcp_sddr_[MAX_MEDIA_CHANNEL];
    MediaChannelInfo media_channel_info_[MAX_MEDIA_CHANNEL];
};

}

#endif
