// PHZ
// 2018-6-10

#include "RtspConnection.h"
#include "RtspServer.h"
#include "MediaSession.h"
#include "MediaSource.h"
#include "net/SocketUtil.h"

#define USER_AGENT "-_-"
#define RTSP_DEBUG 0
#define MAX_RTSP_MESSAGE_SIZE 2048

using namespace xop;
using namespace std;

RtspConnection::RtspConnection(std::shared_ptr<Rtsp> rtsp, TaskScheduler *task_scheduler, SOCKET sockfd)
	: TcpConnection(task_scheduler, sockfd)
	, rtsp_(rtsp)
	, task_scheduler_(task_scheduler)
	, rtp_channel_(new Channel(sockfd))
	, rtsp_request_(new RtspRequest)
	, rtsp_response_(new RtspResponse)
{
	this->SetReadCallback([this](std::shared_ptr<TcpConnection> conn, xop::BufferReader& buffer) {
		return this->OnRead(buffer);
	});

	this->SetCloseCallback([this](std::shared_ptr<TcpConnection> conn) {
		this->OnClose();
	});

	alive_count_ = 1;

	rtp_channel_->SetReadCallback([this]() { this->HandleRead(); });
	rtp_channel_->SetWriteCallback([this]() { this->HandleWrite(); });
	rtp_channel_->SetCloseCallback([this]() { this->HandleClose(); });
	rtp_channel_->SetErrorCallback([this]() { this->HandleError(); });

	for(int chn=0; chn<MAX_MEDIA_CHANNEL; chn++) {
		rtcp_channels_[chn] = nullptr;
	}

	has_auth_ = true;
	if (rtsp->has_auth_info_) {
		has_auth_ = false;
		auth_info_.reset(new DigestAuthentication(rtsp->realm_, rtsp->username_, rtsp->password_));
	}	
}

RtspConnection::~RtspConnection()
{

}

bool RtspConnection::OnRead(BufferReader& buffer)
{
	KeepAlive();

	int size = buffer.ReadableBytes();
	if (size <= 0) {
		return false; //close
	}

	if (conn_mode_ == RTSP_SERVER) {
		if (!HandleRtspRequest(buffer)){
			return false; 
		}
	}
	else if (conn_mode_ == RTSP_PUSHER) {
		if (!HandleRtspResponse(buffer)) {           
			return false;
		}
	}

	if (buffer.ReadableBytes() > MAX_RTSP_MESSAGE_SIZE) {
		buffer.RetrieveAll(); 
	}

	return true;
}

void RtspConnection::OnClose()
{
	if(session_id_ != 0) {
		auto rtsp = rtsp_.lock();
		if (rtsp) {
			MediaSession::Ptr media_session = rtsp->LookMediaSession(session_id_);
			if (media_session) {
				media_session->RemoveClient(this->GetSocket());
			}
		}	
	}

	for(int chn=0; chn<MAX_MEDIA_CHANNEL; chn++) {
		if(rtcp_channels_[chn] && !rtcp_channels_[chn]->IsNoneEvent()) {
			task_scheduler_->RemoveChannel(rtcp_channels_[chn]);
		}
	}
}

bool RtspConnection::HandleRtspRequest(BufferReader& buffer)
{
#if RTSP_DEBUG
	string str(buffer.Peek(), buffer.ReadableBytes());
	if (str.find("rtsp") != string::npos || str.find("RTSP") != string::npos)
	{
		std::cout << str << std::endl;
	}
#endif

	while (buffer.ReadableBytes() > 0) {
		if (buffer.Peek()[0] == '$') {
			if (!HandleRtcp(buffer)) {
				break; // 交错帧未收全, 留在接收缓冲里等后续数据
			}
			continue; // 完整的帧已按各自长度消费, 继续解析后续消息
		}

		if (!rtsp_request_->ParseRequest(&buffer)) {
			return false;
		}
		if (!rtsp_request_->GotAll()) {
			break; // RTSP 消息未收全
		}

		RtspRequest::Method method = rtsp_request_->GetMethod();
		switch (method)
		{
		case RtspRequest::OPTIONS:
			HandleCmdOption();
			break;
		case RtspRequest::DESCRIBE:
			HandleCmdDescribe();
			break;
		case RtspRequest::SETUP:
			HandleCmdSetup();
			break;
		case RtspRequest::PLAY:
			HandleCmdPlay();
			break;
		case RtspRequest::TEARDOWN:
			HandleCmdTeardown();
			break;
		case RtspRequest::GET_PARAMETER:
			HandleCmdGetParamter();
			break;
		default:
			break;
		}

		if (rtsp_request_->GotAll()) {
			rtsp_request_->Reset();
		}
	}

	return true;
}

bool RtspConnection::HandleRtspResponse(BufferReader& buffer)
{
#if RTSP_DEBUG
	string str(buffer.Peek(), buffer.ReadableBytes());
	if (str.find("rtsp") != string::npos || str.find("RTSP") != string::npos) {
		cout << str << endl;
	}		
#endif

	if (rtsp_response_->ParseResponse(&buffer)) {
		RtspResponse::Method method = rtsp_response_->GetMethod();
		switch (method)
		{
		case RtspResponse::OPTIONS:
			if (conn_mode_ == RTSP_PUSHER) {
				SendAnnounce();
			}             
			break;
		case RtspResponse::ANNOUNCE:
		case RtspResponse::DESCRIBE:
			SendSetup();
			break;
		case RtspResponse::SETUP:
			SendSetup();
			break;
		case RtspResponse::RECORD:
			HandleRecord();
			break;
		default:            
			break;
		}
	}
	else {
		return false;
	}

	return true;
}

void RtspConnection::SendRtspMessage(std::shared_ptr<char> buf, uint32_t size)
{
#if RTSP_DEBUG
	cout << buf.get() << endl;
#endif

	this->Send(buf, size);
	return;
}

bool RtspConnection::HandleRtcp(BufferReader& buffer)
{
	while (buffer.ReadableBytes() > 0) {
		char *peek = buffer.Peek();
		if (peek[0] != '$') {
			return true; // 后面是 RTSP 控制消息, 交还给请求解析
		}

		if (buffer.ReadableBytes() < RTP_TCP_HEAD_SIZE) {
			return false; // 帧头未收全, 留在接收缓冲里
		}

		uint32_t pkt_size = ((uint32_t)(uint8_t)peek[2] << 8) | (uint8_t)peek[3];
		if (buffer.ReadableBytes() < pkt_size + RTP_TCP_HEAD_SIZE) {
			return false; // 帧体未收全, 留在接收缓冲里
		}

		// 一个完整的交错帧: 按协商的 RTCP 通道交给对应的媒体通道处理
		if (rtp_conn_ != nullptr) {
			rtp_conn_->HandleRtcpFrame((uint8_t)peek[1],
					(const uint8_t*)peek + RTP_TCP_HEAD_SIZE, pkt_size);
		}
		buffer.Retrieve(pkt_size + RTP_TCP_HEAD_SIZE);
	}

	return true;
}

void RtspConnection::HandleRtcp(SOCKET sockfd)
{
	char buf[2048] = {0};
	int n = (int)recv(sockfd, buf, sizeof(buf), 0);
	if(n > 0) {
		KeepAlive();
		// UDP 下一个数据报就是一份完整的 RTCP 复合包
		if (rtp_conn_ != nullptr) {
			rtp_conn_->HandleRtcpPacket(sockfd, (const uint8_t*)buf, (uint32_t)n);
		}
	}
}

void RtspConnection::HandleCmdOption()
{
	std::shared_ptr<char> res(new char[2048], std::default_delete<char[]>());
	int size = rtsp_request_->BuildOptionRes(res.get(), 2048);
	this->SendRtspMessage(res, size);	
}

void RtspConnection::HandleCmdDescribe()
{
	if (auth_info_!=nullptr && !HandleAuthentication()) {
		return;
	}

	if (rtp_conn_ == nullptr) {
		rtp_conn_.reset(new RtpConnection(shared_from_this()));
	}

	int size = 0;
	std::shared_ptr<char> res(new char[4096], std::default_delete<char[]>());
	MediaSession::Ptr media_session = nullptr;

	auto rtsp = rtsp_.lock();
	if (rtsp) {
		media_session = rtsp->LookMediaSession(rtsp_request_->GetRtspUrlSuffix());
	}
	
	if(!rtsp || !media_session) {
		size = rtsp_request_->BuildNotFoundRes(res.get(), 4096);
	}
	else {
		session_id_ = media_session->GetMediaSessionId();
		media_session->AddClient(this->GetSocket(), rtp_conn_);

		for(int chn=0; chn<MAX_MEDIA_CHANNEL; chn++) {
			MediaSource* source = media_session->GetMediaSource((MediaChannelId)chn);
			if(source != nullptr) {
				rtp_conn_->SetClockRate((MediaChannelId)chn, source->GetClockRate());
				rtp_conn_->SetPayloadType((MediaChannelId)chn, source->GetPayloadType());
			}
		}

		std::string sdp = media_session->GetSdpMessage(SocketUtil::GetSocketIp(this->GetSocket()), rtsp->GetVersion());
		if(sdp == "") {
			size = rtsp_request_->BuildServerErrorRes(res.get(), 4096);
		}
		else {
			size = rtsp_request_->BuildDescribeRes(res.get(), 4096, sdp.c_str());		
		}
	}

	SendRtspMessage(res, size);
	return ;
}

void RtspConnection::HandleCmdSetup()
{
	if (auth_info_ != nullptr && !HandleAuthentication()) {
		return;
	}

	int size = 0;
	std::shared_ptr<char> res(new char[4096], std::default_delete<char[]>());
	MediaChannelId channel_id = rtsp_request_->GetChannelId();
	MediaSession::Ptr media_session = nullptr;

	auto rtsp = rtsp_.lock();
	if (rtsp) {
		media_session = rtsp->LookMediaSession(session_id_);
	}

	if(!rtsp || !media_session)  {
		goto server_error;
	}

	if(media_session->IsMulticast())  {
		std::string multicast_ip = media_session->GetMulticastIp();
		if(rtsp_request_->GetTransportMode() == RTP_OVER_MULTICAST) {
			uint16_t port = media_session->GetMulticastPort(channel_id);
			uint16_t session_id = rtp_conn_->GetRtpSessionId();
			if (!rtp_conn_->SetupRtpOverMulticast(channel_id, multicast_ip.c_str(), port)) {
				goto server_error;
			}

			size = rtsp_request_->BuildSetupMulticastRes(res.get(), 4096, multicast_ip.c_str(), port, session_id);
		}
		else {
			goto transport_unsupport;
		}
	}
	else {
		if(rtsp_request_->GetTransportMode() == RTP_OVER_TCP) {
			uint16_t rtp_channel = rtsp_request_->GetRtpChannel();
			uint16_t rtcp_channel = rtsp_request_->GetRtcpChannel();
			uint16_t session_id = rtp_conn_->GetRtpSessionId();

			rtp_conn_->SetupRtpOverTcp(channel_id, rtp_channel, rtcp_channel);
			size = rtsp_request_->BuildSetupTcpRes(res.get(), 4096, rtp_channel, rtcp_channel, session_id);
		}
		else if(rtsp_request_->GetTransportMode() == RTP_OVER_UDP) {
			uint16_t peer_rtp_port = rtsp_request_->GetRtpPort();
			uint16_t peer_rtcp_port = rtsp_request_->GetRtcpPort();
			uint16_t session_id = rtp_conn_->GetRtpSessionId();

			if(rtp_conn_->SetupRtpOverUdp(channel_id, peer_rtp_port, peer_rtcp_port)) {
				SOCKET rtcp_fd = rtp_conn_->GetRtcpSocket(channel_id);
				rtcp_channels_[channel_id].reset(new Channel(rtcp_fd));
				rtcp_channels_[channel_id]->SetReadCallback([rtcp_fd, this]() { this->HandleRtcp(rtcp_fd); });
				rtcp_channels_[channel_id]->EnableReading();
				task_scheduler_->UpdateChannel(rtcp_channels_[channel_id]);
			}
			else {
				goto server_error;
			}

			uint16_t serRtpPort = rtp_conn_->GetRtpPort(channel_id);
			uint16_t serRtcpPort = rtp_conn_->GetRtcpPort(channel_id);
			size = rtsp_request_->BuildSetupUdpRes(res.get(), 4096, serRtpPort, serRtcpPort, session_id);
		}
		else {          
			goto transport_unsupport;
		}
	}

	SendRtspMessage(res, size);
	return ;

transport_unsupport:
	size = rtsp_request_->BuildUnsupportedRes(res.get(), 4096);
	SendRtspMessage(res, size);
	return ;

server_error:
	size = rtsp_request_->BuildServerErrorRes(res.get(), 4096);
	SendRtspMessage(res, size);
	return ;
}

void RtspConnection::HandleCmdPlay()
{
	if (auth_info_ != nullptr) {
		if (!HandleAuthentication()) {
			return;
		}
	}

	if (rtp_conn_ == nullptr) {
		return;
	}

	conn_state_ = START_PLAY;
	rtp_conn_->Play();

	uint16_t session_id = rtp_conn_->GetRtpSessionId();
	std::shared_ptr<char> res(new char[2048], std::default_delete<char[]>());

	int size = rtsp_request_->BuildPlayRes(res.get(), 2048, nullptr, session_id);
	SendRtspMessage(res, size);
}

void RtspConnection::HandleCmdTeardown()
{
	if (rtp_conn_ == nullptr) {
		return;
	}

	rtp_conn_->Teardown();

	uint16_t session_id = rtp_conn_->GetRtpSessionId();
	std::shared_ptr<char> res(new char[2048], std::default_delete<char[]>());
	int size = rtsp_request_->BuildTeardownRes(res.get(), 2048, session_id);
	SendRtspMessage(res, size);

	//HandleClose();
}

void RtspConnection::HandleCmdGetParamter()
{
	if (rtp_conn_ == nullptr) {
		return;
	}

	uint16_t session_id = rtp_conn_->GetRtpSessionId();
	std::shared_ptr<char> res(new char[2048], std::default_delete<char[]>());
	int size = rtsp_request_->BuildGetParamterRes(res.get(), 2048, session_id);
	SendRtspMessage(res, size);
}

bool RtspConnection::HandleAuthentication()
{
	if (auth_info_ != nullptr && !has_auth_) {
		std::string cmd = rtsp_request_->MethodToString[rtsp_request_->GetMethod()];
		std::string url = rtsp_request_->GetRtspUrl();

		if (_nonce.size() > 0 && (auth_info_->GetResponse(_nonce, cmd, url) == rtsp_request_->GetAuthResponse())) {
			_nonce.clear();
			has_auth_ = true;
		}
		else {
			std::shared_ptr<char> res(new char[4096], std::default_delete<char[]>());
			_nonce = auth_info_->GetNonce();
			int size = rtsp_request_->BuildUnauthorizedRes(res.get(), 4096, auth_info_->GetRealm().c_str(), _nonce.c_str());
			SendRtspMessage(res, size);
			return false;
		}
	}

	return true;
}

void RtspConnection::SendOptions(ConnectionMode mode)
{
	if (rtp_conn_ == nullptr) {
		rtp_conn_.reset(new RtpConnection(shared_from_this()));
	}

	auto rtsp = rtsp_.lock();
	if (!rtsp) {
		HandleClose();
		return;
	}	

	conn_mode_ = mode;
	rtsp_response_->SetUserAgent(USER_AGENT);
	rtsp_response_->SetRtspUrl(rtsp->GetRtspUrl().c_str());

	std::shared_ptr<char> req(new char[2048], std::default_delete<char[]>());
	int size = rtsp_response_->BuildOptionReq(req.get(), 2048);
	SendRtspMessage(req, size);
}

void RtspConnection::SendAnnounce()
{
	MediaSession::Ptr media_session = nullptr;

	auto rtsp = rtsp_.lock();
	if (rtsp) {
		media_session = rtsp->LookMediaSession(1);
	}

	if (!rtsp || !media_session) {
		HandleClose();
		return;
	}
	else {
		session_id_ = media_session->GetMediaSessionId();
		media_session->AddClient(this->GetSocket(), rtp_conn_);

		for (int chn = 0; chn<2; chn++) {
			MediaSource* source = media_session->GetMediaSource((MediaChannelId)chn);
			if (source != nullptr) {
				rtp_conn_->SetClockRate((MediaChannelId)chn, source->GetClockRate());
				rtp_conn_->SetPayloadType((MediaChannelId)chn, source->GetPayloadType());
			}
		}
	}

	std::string sdp = media_session->GetSdpMessage(SocketUtil::GetSocketIp(this->GetSocket()), rtsp->GetVersion());
	if (sdp == "") {
		HandleClose();
		return;
	}

	std::shared_ptr<char> req(new char[4096], std::default_delete<char[]>());
	int size = rtsp_response_->BuildAnnounceReq(req.get(), 4096, sdp.c_str());
	SendRtspMessage(req, size);
}

void RtspConnection::SendDescribe()
{
	std::shared_ptr<char> req(new char[2048], std::default_delete<char[]>());
	int size = rtsp_response_->BuildDescribeReq(req.get(), 2048);
	SendRtspMessage(req, size);
}

void RtspConnection::SendSetup()
{
	int size = 0;
	std::shared_ptr<char> buf(new char[2048], std::default_delete<char[]>());
	MediaSession::Ptr media_session = nullptr;

	auto rtsp = rtsp_.lock();
	if (rtsp) {
		media_session = rtsp->LookMediaSession(session_id_);
	}
	
	if (!rtsp || !media_session) {
		HandleClose();
		return;
	}

	if (media_session->GetMediaSource(channel_0) && !rtp_conn_->IsSetup(channel_0)) {
		rtp_conn_->SetupRtpOverTcp(channel_0, 0, 1);
		size = rtsp_response_->BuildSetupTcpReq(buf.get(), 2048, channel_0);
	}
	else if (media_session->GetMediaSource(channel_1) && !rtp_conn_->IsSetup(channel_1)) {
		rtp_conn_->SetupRtpOverTcp(channel_1, 2, 3);
		size = rtsp_response_->BuildSetupTcpReq(buf.get(), 2048, channel_1);
	}
	else {
		size = rtsp_response_->BuildRecordReq(buf.get(), 2048);
	}

	SendRtspMessage(buf, size);
}

void RtspConnection::HandleRecord()
{
	conn_state_ = START_PUSH;
	rtp_conn_->Record();
}
