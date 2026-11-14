// RFC 4585 Generic NACK (RTPFB/FMT=1) 服务端重传链路测试。
//
// 通过本地真实 RTSP 连接协商媒体、推帧、读回字节, 覆盖:
//  - feedback:   NACK 触发原包重传(字节与首发完全一致)、PID/BLP 展开、
//                序号跨 65535/0、复合包内去重、RR+NACK 复合包、不支持的
//                反馈类型跳过、非法复合包(版本/长度/FCI 对齐/padding)
//                不触发补包且连接可恢复、RTCP 通道与媒体 SSRC 必须同时对上
//  - snapshots:  每连接每媒体通道 512 包历史、按发送顺序淘汰、补包不推进
//                正常序号也不再进历史、两客户端共用调度器时历史不被对方
//                的填头污染
//  - transports: TCP 交错帧分段与粘连(后面紧跟 GET_PARAMETER)、UDP
//                数据报反馈、两条媒体通道各自的 RTP channel
//
// 测试只依赖可观察的响应或调度信号往前推进, 超时仅用于防止挂起。

#include "task_support/check.h"

#include "xop/RtspServer.h"
#include "xop/RtpConnection.h"
#include "xop/H264Source.h"
#include "xop/G711ASource.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{

const int kIoTimeoutMs = 3000; // 只用于防止挂起

// 让出片刻, 使服务端的 recv 更可能落在分段中间(只是节奏提示, 断言不依赖它)
void BriefPause()
{
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
}

struct SigPipeIgnore
{
	SigPipeIgnore() { signal(SIGPIPE, SIG_IGN); }
};
SigPipeIgnore g_sigpipe_ignore;

bool SendAll(int fd, const void* data, size_t size)
{
	const char* p = (const char*)data;
	size_t left = size;
	while (left > 0) {
		ssize_t n = send(fd, p, left, 0);
		if (n <= 0) {
			return false;
		}
		p += n;
		left -= (size_t)n;
	}
	return true;
}

bool RecvAll(int fd, void* data, size_t size, int timeout_ms = kIoTimeoutMs)
{
	char* p = (char*)data;
	size_t left = size;
	while (left > 0) {
		pollfd pfd;
		pfd.fd = fd;
		pfd.events = POLLIN;
		pfd.revents = 0;
		int r = poll(&pfd, 1, timeout_ms);
		if (r <= 0) {
			return false;
		}
		ssize_t n = recv(fd, p, left, 0);
		if (n <= 0) {
			return false;
		}
		p += n;
		left -= (size_t)n;
	}
	return true;
}

void AppendBe16(std::vector<uint8_t>& out, uint16_t value)
{
	out.push_back((uint8_t)(value >> 8));
	out.push_back((uint8_t)(value & 0xff));
}

void AppendBe32(std::vector<uint8_t>& out, uint32_t value)
{
	out.push_back((uint8_t)(value >> 24));
	out.push_back((uint8_t)(value >> 16));
	out.push_back((uint8_t)(value >> 8));
	out.push_back((uint8_t)(value & 0xff));
}

uint16_t SeqOf(const std::vector<uint8_t>& rtp)
{
	return (uint16_t)((rtp[2] << 8) | rtp[3]);
}

uint32_t SsrcOf(const std::vector<uint8_t>& rtp)
{
	return ((uint32_t)rtp[8] << 24) | ((uint32_t)rtp[9] << 16) |
			((uint32_t)rtp[10] << 8) | (uint32_t)rtp[11];
}

uint32_t TsOf(const std::vector<uint8_t>& rtp)
{
	return ((uint32_t)rtp[4] << 24) | ((uint32_t)rtp[5] << 16) |
			((uint32_t)rtp[6] << 8) | (uint32_t)rtp[7];
}

// 空报告的 Receiver Report (PT=201)
std::vector<uint8_t> BuildRr(uint32_t sender_ssrc)
{
	std::vector<uint8_t> out;
	out.push_back(0x80); // V=2, RC=0
	out.push_back(201);
	AppendBe16(out, 1); // 2 个 32 位字 - 1
	AppendBe32(out, sender_ssrc);
	return out;
}

// Generic NACK (RTPFB PT=205, FMT=1), fcis 为 (PID, BLP) 列表
std::vector<uint8_t> BuildNack(uint32_t sender_ssrc, uint32_t media_ssrc,
		const std::vector<std::pair<uint16_t, uint16_t> >& fcis)
{
	std::vector<uint8_t> out;
	out.push_back(0x81); // V=2, FMT=1
	out.push_back(205);
	AppendBe16(out, (uint16_t)(2 + fcis.size())); // (12 + 4*n)/4 - 1
	AppendBe32(out, sender_ssrc);
	AppendBe32(out, media_ssrc);
	for (size_t n = 0; n < fcis.size(); n++) {
		AppendBe16(out, fcis[n].first);
		AppendBe16(out, fcis[n].second);
	}
	return out;
}

// Picture Loss Indication (PSFB PT=206, FMT=1): 合法但不支持的反馈
std::vector<uint8_t> BuildPli(uint32_t sender_ssrc, uint32_t media_ssrc)
{
	std::vector<uint8_t> out;
	out.push_back(0x81);
	out.push_back(206);
	AppendBe16(out, 2);
	AppendBe32(out, sender_ssrc);
	AppendBe32(out, media_ssrc);
	return out;
}

std::vector<uint8_t> Concat(const std::vector<std::vector<uint8_t> >& parts)
{
	std::vector<uint8_t> out;
	for (size_t n = 0; n < parts.size(); n++) {
		out.insert(out.end(), parts[n].begin(), parts[n].end());
	}
	return out;
}

// 从 RTSP TCP 连接上读一条消息: '$' 交错帧或 RTSP 响应
struct Incoming
{
	bool is_frame = false;
	uint8_t channel = 0;
	std::vector<uint8_t> body; // 帧负载(RTP 包)或完整响应文本
};

bool ReadIncoming(int fd, Incoming& in, int timeout_ms = kIoTimeoutMs)
{
	uint8_t first = 0;
	if (!RecvAll(fd, &first, 1, timeout_ms)) {
		return false;
	}

	if (first == '$') {
		uint8_t head[3];
		if (!RecvAll(fd, head, 3, timeout_ms)) {
			return false;
		}
		in.is_frame = true;
		in.channel = head[0];
		in.body.resize(((size_t)head[1] << 8) | head[2]);
		return in.body.empty() || RecvAll(fd, in.body.data(), in.body.size(), timeout_ms);
	}

	in.is_frame = false;
	in.channel = 0;
	in.body.clear();
	in.body.push_back(first);
	while (in.body.size() < 4 ||
			memcmp(&in.body[in.body.size() - 4], "\r\n\r\n", 4) != 0) {
		uint8_t c = 0;
		if (!RecvAll(fd, &c, 1, timeout_ms)) {
			return false;
		}
		in.body.push_back(c);
		if (in.body.size() > 65536) {
			return false;
		}
	}

	std::string head(in.body.begin(), in.body.end());
	size_t pos = head.find("Content-Length:");
	if (pos != std::string::npos) {
		long content = strtol(head.c_str() + pos + 15, nullptr, 10);
		while (content-- > 0) {
			uint8_t c = 0;
			if (!RecvAll(fd, &c, 1, timeout_ms)) {
				return false;
			}
			in.body.push_back(c);
		}
	}
	return true;
}

// 读取一帧交错 RTP
bool ReadFrame(int fd, uint8_t& channel, std::vector<uint8_t>& body)
{
	Incoming in;
	if (!ReadIncoming(fd, in) || !in.is_frame) {
		return false;
	}
	channel = in.channel;
	body = in.body;
	return true;
}

// 读取一条 RTSP 响应(下一条消息不应是交错帧)
bool ReadResponse(int fd, std::string& response)
{
	Incoming in;
	if (!ReadIncoming(fd, in) || in.is_frame) {
		return false;
	}
	response.assign(in.body.begin(), in.body.end());
	return response.find("200 OK") != std::string::npos;
}

struct TestServer
{
	xop::EventLoop loop{1}; // 单调度器: 所有连接共用一个 task scheduler
	std::shared_ptr<xop::RtspServer> server;
	xop::MediaSessionId session_id = 0;
	uint16_t port = 0;

	~TestServer() { Stop(); }

	bool Start(bool with_audio)
	{
		server = xop::RtspServer::Create(&loop);

		int probe = socket(AF_INET, SOCK_STREAM, 0);
		if (probe < 0) {
			return false;
		}
		sockaddr_in addr;
		memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		addr.sin_port = 0;
		if (bind(probe, (sockaddr*)&addr, sizeof(addr)) != 0) {
			close(probe);
			return false;
		}
		socklen_t len = sizeof(addr);
		getsockname(probe, (sockaddr*)&addr, &len);
		port = ntohs(addr.sin_port);
		close(probe);

		if (!server->Start("127.0.0.1", port)) {
			return false;
		}

		auto session = xop::MediaSession::CreateNew("live");
		session->AddSource(xop::channel_0, xop::H264Source::CreateNew());
		if (with_audio) {
			session->AddSource(xop::channel_1, xop::G711ASource::CreateNew());
		}
		session_id = server->AddSession(session);
		return session_id != 0;
	}

	bool PushVideo(uint32_t timestamp, uint8_t seed)
	{
		xop::AVFrame frame;
		frame.type = xop::VIDEO_FRAME_I;
		frame.timestamp = timestamp;
		frame.size = 24;
		frame.buffer.reset(new uint8_t[frame.size]);
		frame.buffer.get()[0] = 0x65;
		for (uint32_t n = 1; n < frame.size; n++) {
			frame.buffer.get()[n] = (uint8_t)(seed + n);
		}
		return server->PushFrame(session_id, xop::channel_0, frame);
	}

	bool PushAudio(uint32_t timestamp, uint8_t seed)
	{
		xop::AVFrame frame;
		frame.type = xop::AUDIO_FRAME;
		frame.timestamp = timestamp;
		frame.size = 20;
		frame.buffer.reset(new uint8_t[frame.size]);
		for (uint32_t n = 0; n < frame.size; n++) {
			frame.buffer.get()[n] = (uint8_t)(seed ^ n);
		}
		return server->PushFrame(session_id, xop::channel_1, frame);
	}

	void Stop()
	{
		if (server) {
			server->Stop();
		}
		loop.Quit();
	}
};

struct TestClient
{
	int fd = -1;
	uint32_t cseq = 0;
	std::string base_url;
	std::string session;

	~TestClient()
	{
		if (fd >= 0) {
			close(fd);
		}
	}

	bool Connect(uint16_t port)
	{
		fd = socket(AF_INET, SOCK_STREAM, 0);
		if (fd < 0) {
			return false;
		}
		sockaddr_in addr;
		memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		addr.sin_port = htons(port);
		if (connect(fd, (sockaddr*)&addr, sizeof(addr)) != 0) {
			return false;
		}
		char url[128];
		snprintf(url, sizeof(url), "rtsp://127.0.0.1:%u/live", port);
		base_url = url;
		return true;
	}

	// 发送一条 RTSP 请求并读取响应(此时不应有交错帧)
	bool Request(const std::string& method, const std::string& url,
			const std::string& extra_headers, std::string& response)
	{
		char head[192];
		snprintf(head, sizeof(head), "%s %s RTSP/1.0\r\nCSeq: %u\r\n",
				method.c_str(), url.c_str(), ++cseq);
		std::string text = std::string(head) + extra_headers + "\r\n";
		if (!SendAll(fd, text.data(), text.size())) {
			return false;
		}
		return ReadResponse(fd, response);
	}

	bool Options()
	{
		std::string resp;
		return Request("OPTIONS", base_url, "", resp);
	}

	bool Describe()
	{
		std::string resp;
		return Request("DESCRIBE", base_url, "Accept: application/sdp\r\n", resp) &&
				resp.find("m=") != std::string::npos;
	}

	bool ParseSession(const std::string& resp)
	{
		size_t pos = resp.find("Session: ");
		if (pos == std::string::npos) {
			return false;
		}
		size_t end = resp.find('\r', pos);
		session = resp.substr(pos + 9, end - pos - 9);
		size_t semi = session.find(';');
		if (semi != std::string::npos) {
			session = session.substr(0, semi);
		}
		return !session.empty();
	}

	bool SetupTcp(int track, uint16_t rtp_channel, uint16_t rtcp_channel)
	{
		char extra[128];
		snprintf(extra, sizeof(extra),
				"Transport: RTP/AVP/TCP;unicast;interleaved=%u-%u\r\n",
				rtp_channel, rtcp_channel);
		std::string resp;
		if (!Request("SETUP", base_url + "/track" + std::to_string(track), extra, resp)) {
			return false;
		}
		return ParseSession(resp);
	}

	bool SetupUdp(int track, uint16_t client_rtp, uint16_t client_rtcp,
			uint16_t& server_rtp, uint16_t& server_rtcp)
	{
		char extra[128];
		snprintf(extra, sizeof(extra),
				"Transport: RTP/AVP;unicast;client_port=%u-%u\r\n",
				client_rtp, client_rtcp);
		std::string resp;
		if (!Request("SETUP", base_url + "/track" + std::to_string(track), extra, resp)) {
			return false;
		}
		if (!ParseSession(resp)) {
			return false;
		}
		size_t pos = resp.find("server_port=");
		if (pos == std::string::npos) {
			return false;
		}
		unsigned rtp = 0, rtcp = 0;
		if (sscanf(resp.c_str() + pos, "server_port=%u-%u", &rtp, &rtcp) != 2) {
			return false;
		}
		server_rtp = (uint16_t)rtp;
		server_rtcp = (uint16_t)rtcp;
		return true;
	}

	bool Play()
	{
		std::string resp;
		return Request("PLAY", base_url, "Session: " + session + "\r\n", resp);
	}

	// GET_PARAMETER 请求文本(PLAY 之后与交错帧共用连接, 响应由测试按序读取)
	std::string GetParameterText()
	{
		char buf[256];
		snprintf(buf, sizeof(buf),
				"GET_PARAMETER %s RTSP/1.0\r\nCSeq: %u\r\nSession: %s\r\n\r\n",
				base_url.c_str(), ++cseq, session.c_str());
		return std::string(buf);
	}
};

// 把一份 RTCP 包封装成交错帧的字节流
std::vector<uint8_t> RtcpFrame(uint8_t channel, const std::vector<uint8_t>& rtcp)
{
	std::vector<uint8_t> frame;
	frame.push_back('$');
	frame.push_back(channel);
	frame.push_back((uint8_t)(rtcp.size() >> 8));
	frame.push_back((uint8_t)(rtcp.size() & 0xff));
	frame.insert(frame.end(), rtcp.begin(), rtcp.end());
	return frame;
}

bool SendBytes(int fd, const std::vector<uint8_t>& bytes)
{
	return bytes.empty() || SendAll(fd, bytes.data(), bytes.size());
}

// 发一份 RTCP 反馈帧, 期望读到一帧与 expected 完全一致的 RTP
bool ExpectRetransmit(int fd, uint8_t rtcp_channel, const std::vector<uint8_t>& rtcp,
		uint8_t expect_channel, const std::vector<uint8_t>& expected)
{
	if (!SendBytes(fd, RtcpFrame(rtcp_channel, rtcp))) {
		return false;
	}
	uint8_t channel = 0;
	std::vector<uint8_t> body;
	return ReadFrame(fd, channel, body) && channel == expect_channel && body == expected;
}

// 反馈与 GET_PARAMETER 粘连发送: 期望没有任何补包, 控制请求得到 200 OK
bool ExpectNoRetransmit(TestClient& client, uint8_t rtcp_channel, const std::vector<uint8_t>& rtcp)
{
	std::vector<uint8_t> stream = RtcpFrame(rtcp_channel, rtcp);
	std::string get_parameter = client.GetParameterText();
	stream.insert(stream.end(), get_parameter.begin(), get_parameter.end());
	if (!SendBytes(client.fd, stream)) {
		return false;
	}
	std::string resp;
	return ReadResponse(client.fd, resp);
}

int MakeUdpSocket(uint16_t& port)
{
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		return -1;
	}
	sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = 0;
	if (bind(fd, (sockaddr*)&addr, sizeof(addr)) != 0) {
		close(fd);
		return -1;
	}
	socklen_t len = sizeof(addr);
	getsockname(fd, (sockaddr*)&addr, &len);
	port = ntohs(addr.sin_port);
	return fd;
}

bool SendUdpTo(int fd, uint16_t port, const std::vector<uint8_t>& data)
{
	sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = htons(port);
	return sendto(fd, data.data(), data.size(), 0, (sockaddr*)&addr,
			sizeof(addr)) == (ssize_t)data.size();
}

bool RecvUdp(int fd, std::vector<uint8_t>& out, int timeout_ms = kIoTimeoutMs)
{
	pollfd pfd;
	pfd.fd = fd;
	pfd.events = POLLIN;
	pfd.revents = 0;
	if (poll(&pfd, 1, timeout_ms) <= 0) {
		return false;
	}
	uint8_t buf[2048];
	ssize_t n = recv(fd, buf, sizeof(buf), 0);
	if (n <= 0) {
		return false;
	}
	out.assign(buf, buf + n);
	return true;
}

// UDP 负向检查: 窗口期内不应收到任何数据报
bool RecvUdpNone(int fd, int timeout_ms)
{
	pollfd pfd;
	pfd.fd = fd;
	pfd.events = POLLIN;
	pfd.revents = 0;
	return poll(&pfd, 1, timeout_ms) == 0;
}

// ---------------------------------------------------------------------------
// feedback 组
// ---------------------------------------------------------------------------

// 基本 NACK: 原包重传、字节完全一致、BLP 展开、复合包内去重、
// RR+NACK 复合包、合法但不支持的反馈类型跳过
int CaseTcpGenericNack()
{
	TestServer srv;
	TASK_CHECK(srv.Start(false));

	TestClient client;
	TASK_CHECK(client.Connect(srv.port));
	TASK_CHECK(client.Options());
	TASK_CHECK(client.Describe());
	TASK_CHECK(client.SetupTcp(0, 0, 1));
	TASK_CHECK(client.Play());

	TASK_CHECK(srv.PushVideo(1000, 0x10));
	TASK_CHECK(srv.PushVideo(2000, 0x40));
	TASK_CHECK(srv.PushVideo(3000, 0x70));

	std::vector<std::vector<uint8_t> > frames(3);
	for (int n = 0; n < 3; n++) {
		uint8_t channel = 0;
		TASK_CHECK(ReadFrame(client.fd, channel, frames[n]));
		TASK_CHECK(channel == 0);
	}
	uint16_t s0 = SeqOf(frames[0]);
	TASK_CHECK(SeqOf(frames[1]) == (uint16_t)(s0 + 1));
	TASK_CHECK(SeqOf(frames[2]) == (uint16_t)(s0 + 2));
	uint32_t ssrc = SsrcOf(frames[0]);

	// RR + NACK 复合包: 重传中间一帧, 字节与首发完全一致
	TASK_CHECK(ExpectRetransmit(client.fd, 1,
			Concat({BuildRr(0x1234), BuildNack(0x1234, ssrc, {{(uint16_t)(s0 + 1), 0}})}),
			0, frames[1]));

	// BLP 展开: PID=s0, BLP 第 1 位置位 -> s0 与 s0+2, 按序各补一帧
	{
		TASK_CHECK(SendBytes(client.fd, RtcpFrame(1, BuildNack(0x1234, ssrc, {{s0, 0x2}}))));
		uint8_t channel = 0;
		std::vector<uint8_t> body;
		TASK_CHECK(ReadFrame(client.fd, channel, body));
		TASK_CHECK(channel == 0 && body == frames[0]);
		TASK_CHECK(ReadFrame(client.fd, channel, body));
		TASK_CHECK(channel == 0 && body == frames[2]);
	}

	// 同一份复合包里重复请求同一个包(重复 FCI + 重复子包), 只补一次;
	// 后面紧跟 GET_PARAMETER, 响应前不应再出现第二份补包
	{
		std::vector<uint8_t> nack = BuildNack(0x1234, ssrc,
				{{(uint16_t)(s0 + 1), 0}, {(uint16_t)(s0 + 1), 0}});
		std::vector<uint8_t> stream = RtcpFrame(1, Concat({nack, nack}));
		std::string get_parameter = client.GetParameterText();
		stream.insert(stream.end(), get_parameter.begin(), get_parameter.end());
		TASK_CHECK(SendBytes(client.fd, stream));

		uint8_t channel = 0;
		std::vector<uint8_t> body;
		TASK_CHECK(ReadFrame(client.fd, channel, body));
		TASK_CHECK(channel == 0 && body == frames[1]);
		std::string resp;
		TASK_CHECK(ReadResponse(client.fd, resp)); // 下一条直接是响应
	}

	// 合法但不支持的反馈类型(PLI)跳过, 同复合包里的 NACK 仍然生效
	TASK_CHECK(ExpectRetransmit(client.fd, 1,
			Concat({BuildPli(0x1234, ssrc), BuildNack(0x1234, ssrc, {{s0, 0}})}),
			0, frames[0]));

	// 只有 RR 的复合包不触发补包, 连接正常处理后续请求
	TASK_CHECK(ExpectNoRetransmit(client, 1, BuildRr(0x1234)));

	srv.Stop();
	return 0;
}

// 序号跨 65535/0: 用受控的起始序号入口构造, 不靠随机数
int CaseTcpNackWrapAround()
{
	xop::RtpConnection::SetInitialPacketSeq(65534);

	TestServer srv;
	TASK_CHECK(srv.Start(false));

	TestClient client;
	TASK_CHECK(client.Connect(srv.port));
	TASK_CHECK(client.Options());
	TASK_CHECK(client.Describe());
	TASK_CHECK(client.SetupTcp(0, 0, 1));
	TASK_CHECK(client.Play());

	std::vector<std::vector<uint8_t> > frames(4);
	for (int n = 0; n < 4; n++) {
		TASK_CHECK(srv.PushVideo((uint32_t)(5000 + n * 1000), (uint8_t)(0x20 + n)));
		uint8_t channel = 0;
		TASK_CHECK(ReadFrame(client.fd, channel, frames[n]));
		TASK_CHECK(channel == 0);
	}

	// 起始序号受控: 65534, 65535, 0, 1
	TASK_CHECK(SeqOf(frames[0]) == 65534);
	TASK_CHECK(SeqOf(frames[1]) == 65535);
	TASK_CHECK(SeqOf(frames[2]) == 0);
	TASK_CHECK(SeqOf(frames[3]) == 1);
	uint32_t ssrc = SsrcOf(frames[0]);

	// PID=65535, BLP 低两位置位 -> 65535, 0, 1 (跨零展开)
	{
		TASK_CHECK(SendBytes(client.fd, RtcpFrame(1, BuildNack(0x1234, ssrc, {{65535, 0x3}}))));
		uint8_t channel = 0;
		std::vector<uint8_t> body;
		TASK_CHECK(ReadFrame(client.fd, channel, body));
		TASK_CHECK(body == frames[1]);
		TASK_CHECK(ReadFrame(client.fd, channel, body));
		TASK_CHECK(body == frames[2]);
		TASK_CHECK(ReadFrame(client.fd, channel, body));
		TASK_CHECK(body == frames[3]);
	}

	// 跨零前的序号也能补
	TASK_CHECK(ExpectRetransmit(client.fd, 1, BuildNack(0x1234, ssrc, {{65534, 0}}), 0, frames[0]));

	xop::RtpConnection::SetInitialPacketSeq(-1);
	srv.Stop();
	return 0;
}

// 非法复合包不触发任何补包, 连接之后照样处理正常请求
int CaseTcpMalformedCompoundRecovers()
{
	TestServer srv;
	TASK_CHECK(srv.Start(false));

	TestClient client;
	TASK_CHECK(client.Connect(srv.port));
	TASK_CHECK(client.Options());
	TASK_CHECK(client.Describe());
	TASK_CHECK(client.SetupTcp(0, 0, 1));
	TASK_CHECK(client.Play());

	TASK_CHECK(srv.PushVideo(1000, 0x10));
	TASK_CHECK(srv.PushVideo(2000, 0x40));

	std::vector<std::vector<uint8_t> > frames(2);
	for (int n = 0; n < 2; n++) {
		uint8_t channel = 0;
		TASK_CHECK(ReadFrame(client.fd, channel, frames[n]));
		TASK_CHECK(channel == 0);
	}
	uint16_t s0 = SeqOf(frames[0]);
	uint32_t ssrc = SsrcOf(frames[0]);

	std::vector<std::pair<uint16_t, uint16_t> > one_fci{{(uint16_t)(s0 + 1), 0}};

	// 版本号不合法 (V=3)
	{
		std::vector<uint8_t> bad = BuildNack(0x1234, ssrc, one_fci);
		bad[0] = 0xC1;
		TASK_CHECK(ExpectNoRetransmit(client, 1, bad));
	}

	// 子包长度越界: 子包声称 20 字节, 复合包实际只有 16 字节
	{
		std::vector<uint8_t> bad = BuildNack(0x1234, ssrc, one_fci);
		bad[2] = 0;
		bad[3] = 4; // length = 4 -> (4+1)*4 = 20 > 16
		TASK_CHECK(ExpectNoRetransmit(client, 1, bad));
	}

	// FCI 未按 32 位对齐: 带 1 字节 padding 后 FCI 区只剩 3 字节
	{
		std::vector<uint8_t> bad;
		bad.push_back(0xA1); // V=2, P=1, FMT=1
		bad.push_back(205);
		AppendBe16(bad, 3); // 16 字节
		AppendBe32(bad, 0x1234);
		AppendBe32(bad, ssrc);
		bad.push_back(0);   // 3 字节残缺 FCI
		bad.push_back(0);
		bad.push_back(0);
		bad.push_back(1);   // padding 计数
		TASK_CHECK(ExpectNoRetransmit(client, 1, bad));
	}

	// 最后一个子包 padding 不合法: padding 计数为 0
	{
		std::vector<uint8_t> bad;
		bad.push_back(0xA1); // V=2, P=1, FMT=1
		bad.push_back(205);
		AppendBe16(bad, 3); // 16 字节
		AppendBe32(bad, 0x1234);
		AppendBe32(bad, ssrc);
		bad.push_back(0);   // 3 字节 FCI
		bad.push_back(0);
		bad.push_back(0);
		bad.push_back(0);   // padding 计数为 0, 不合法
		TASK_CHECK(ExpectNoRetransmit(client, 1, bad));
	}

	// 合法 NACK 子包后面跟着损坏的子包: 整份复合包不触发任何补包
	{
		std::vector<uint8_t> corrupt = BuildNack(0x1234, ssrc, one_fci);
		corrupt[0] = 0x01; // V=0
		std::vector<uint8_t> compound = Concat({BuildNack(0x1234, ssrc, one_fci), corrupt});
		TASK_CHECK(ExpectNoRetransmit(client, 1, compound));
	}

	// 连接恢复: 合法 NACK 照常补包, 控制请求照常应答
	TASK_CHECK(ExpectRetransmit(client.fd, 1, BuildNack(0x1234, ssrc, one_fci), 0, frames[1]));
	{
		std::string text = client.GetParameterText();
		TASK_CHECK(SendAll(client.fd, text.data(), text.size()));
		std::string resp;
		TASK_CHECK(ReadResponse(client.fd, resp));
	}

	srv.Stop();
	return 0;
}

// 反馈必须同时对上已协商的 RTCP 通道与媒体 SSRC
int CaseTcpChannelAndSsrcMustMatch()
{
	TestServer srv;
	TASK_CHECK(srv.Start(false));

	TestClient client;
	TASK_CHECK(client.Connect(srv.port));
	TASK_CHECK(client.Options());
	TASK_CHECK(client.Describe());
	TASK_CHECK(client.SetupTcp(0, 0, 1));
	TASK_CHECK(client.Play());

	TASK_CHECK(srv.PushVideo(1000, 0x10));
	uint8_t channel = 0;
	std::vector<uint8_t> frame;
	TASK_CHECK(ReadFrame(client.fd, channel, frame));
	TASK_CHECK(channel == 0);
	uint16_t s0 = SeqOf(frame);
	uint32_t ssrc = SsrcOf(frame);

	// 媒体 SSRC 对不上: 不补包
	TASK_CHECK(ExpectNoRetransmit(client, 1, BuildNack(0x1234, ssrc + 1, {{s0, 0}})));

	// RTCP 通道号未协商(只 SETUP 了 0-1): 不补包
	TASK_CHECK(ExpectNoRetransmit(client, 3, BuildNack(0x1234, ssrc, {{s0, 0}})));

	// 通道与 SSRC 都对上: 补包
	TASK_CHECK(ExpectRetransmit(client.fd, 1, BuildNack(0x1234, ssrc, {{s0, 0}}), 0, frame));

	srv.Stop();
	return 0;
}

// ---------------------------------------------------------------------------
// snapshots 组
// ---------------------------------------------------------------------------

// 补出的 RTP 字节与第一次发送完全一致(marker/时间戳/负载都在内),
// 补包不推进正常序号
int CaseHistoryExactBytesAndSeq()
{
	TestServer srv;
	TASK_CHECK(srv.Start(false));

	TestClient client;
	TASK_CHECK(client.Connect(srv.port));
	TASK_CHECK(client.Options());
	TASK_CHECK(client.Describe());
	TASK_CHECK(client.SetupTcp(0, 0, 1));
	TASK_CHECK(client.Play());

	TASK_CHECK(srv.PushVideo(1111, 0x11));
	TASK_CHECK(srv.PushVideo(2222, 0x55));
	TASK_CHECK(srv.PushVideo(3333, 0x99));

	std::vector<std::vector<uint8_t> > frames(3);
	for (int n = 0; n < 3; n++) {
		uint8_t channel = 0;
		TASK_CHECK(ReadFrame(client.fd, channel, frames[n]));
		TASK_CHECK(channel == 0);
	}
	uint16_t s0 = SeqOf(frames[0]);
	uint32_t ssrc = SsrcOf(frames[0]);
	TASK_CHECK(TsOf(frames[1]) == 2222);
	TASK_CHECK((frames[1][1] & 0x80) != 0); // 单 NAL 帧 marker 置位

	// 重传字节与首发完全一致
	TASK_CHECK(ExpectRetransmit(client.fd, 1, BuildNack(0x1234, ssrc, {{(uint16_t)(s0 + 1), 0}}),
			0, frames[1]));

	// 补包不推进正常序号: 后续推帧的序号紧接原有序列
	TASK_CHECK(srv.PushVideo(4444, 0x21));
	TASK_CHECK(srv.PushVideo(5555, 0x31));
	uint8_t channel = 0;
	std::vector<uint8_t> body;
	TASK_CHECK(ReadFrame(client.fd, channel, body));
	TASK_CHECK(SeqOf(body) == (uint16_t)(s0 + 3));
	TASK_CHECK(TsOf(body) == 4444);
	TASK_CHECK(ReadFrame(client.fd, channel, body));
	TASK_CHECK(SeqOf(body) == (uint16_t)(s0 + 4));

	// 不同复合包各自请求同一个包, 各自都能补到
	TASK_CHECK(ExpectRetransmit(client.fd, 1, BuildNack(0x1234, ssrc, {{s0, 0}}), 0, frames[0]));

	srv.Stop();
	return 0;
}

// 每连接每媒体通道保留最近 512 个原包, 按发送顺序淘汰, 补包不进历史
int CaseHistoryEviction()
{
	const int kPushes = 520;

	TestServer srv;
	TASK_CHECK(srv.Start(false));

	TestClient client;
	TASK_CHECK(client.Connect(srv.port));
	TASK_CHECK(client.Options());
	TASK_CHECK(client.Describe());
	TASK_CHECK(client.SetupTcp(0, 0, 1));
	TASK_CHECK(client.Play());

	std::vector<std::vector<uint8_t> > frames(kPushes);
	for (int n = 0; n < kPushes; n++) {
		TASK_CHECK(srv.PushVideo((uint32_t)(1000 + n), (uint8_t)n));
		uint8_t channel = 0;
		TASK_CHECK(ReadFrame(client.fd, channel, frames[n]));
		TASK_CHECK(channel == 0);
	}
	uint16_t s0 = SeqOf(frames[0]);
	uint32_t ssrc = SsrcOf(frames[0]);
	for (int n = 1; n < kPushes; n++) {
		TASK_CHECK(SeqOf(frames[n]) == (uint16_t)(s0 + n));
	}

	// 已被淘汰的最早一包: 不补
	TASK_CHECK(ExpectNoRetransmit(client, 1, BuildNack(0x1234, ssrc, {{s0, 0}})));

	// 历史边界(第 520-512=8 包起)与最新一包: 补出的字节与首发一致
	TASK_CHECK(ExpectRetransmit(client.fd, 1, BuildNack(0x1234, ssrc, {{(uint16_t)(s0 + 8), 0}}),
			0, frames[8]));
	TASK_CHECK(ExpectRetransmit(client.fd, 1, BuildNack(0x1234, ssrc, {{(uint16_t)(s0 + 519), 0}}),
			0, frames[519]));

	// 补包不进历史: 上一把补 s0+8 没有把它顶出去, 紧邻的 s0+9 仍然能补
	TASK_CHECK(ExpectRetransmit(client.fd, 1, BuildNack(0x1234, ssrc, {{(uint16_t)(s0 + 9), 0}}),
			0, frames[9]));

	srv.Stop();
	return 0;
}

// 两个客户端共用同一调度器: MediaSession 复用 RtpPacket 数据,
// 各客户端的历史必须保存自己那一份字节, 不被对方的填头改掉
int CaseTwoClientsSharedScheduler()
{
	TestServer srv;
	TASK_CHECK(srv.Start(false));

	TestClient client_a;
	TestClient client_b;
	TASK_CHECK(client_a.Connect(srv.port));
	TASK_CHECK(client_a.Options());
	TASK_CHECK(client_a.Describe());
	TASK_CHECK(client_a.SetupTcp(0, 0, 1));
	TASK_CHECK(client_a.Play());

	TASK_CHECK(client_b.Connect(srv.port));
	TASK_CHECK(client_b.Options());
	TASK_CHECK(client_b.Describe());
	TASK_CHECK(client_b.SetupTcp(0, 0, 1));
	TASK_CHECK(client_b.Play());

	TASK_CHECK(srv.PushVideo(1000, 0x10));
	TASK_CHECK(srv.PushVideo(2000, 0x40));

	std::vector<std::vector<uint8_t> > frames_a(2), frames_b(2);
	for (int n = 0; n < 2; n++) {
		uint8_t channel = 0;
		TASK_CHECK(ReadFrame(client_a.fd, channel, frames_a[n]));
		TASK_CHECK(channel == 0);
	}
	for (int n = 0; n < 2; n++) {
		uint8_t channel = 0;
		TASK_CHECK(ReadFrame(client_b.fd, channel, frames_b[n]));
		TASK_CHECK(channel == 0);
	}

	// 两个连接各自的 SSRC 不同, 同一份帧数据被分别填头
	uint32_t ssrc_a = SsrcOf(frames_a[0]);
	uint32_t ssrc_b = SsrcOf(frames_b[0]);
	TASK_CHECK(ssrc_a != ssrc_b);
	TASK_CHECK(frames_a[0] != frames_b[0]);

	// A 补到的必须是 A 当初收到的字节, 而不是被 B 的填头污染的内容
	TASK_CHECK(ExpectRetransmit(client_a.fd, 1,
			BuildNack(0x1234, ssrc_a, {{SeqOf(frames_a[0]), 0}}), 0, frames_a[0]));

	// B 同理
	TASK_CHECK(ExpectRetransmit(client_b.fd, 1,
			BuildNack(0x1234, ssrc_b, {{SeqOf(frames_b[1]), 0}}), 0, frames_b[1]));

	// 正常推流不受补包影响, 两个客户端都继续收
	TASK_CHECK(srv.PushVideo(3000, 0x70));
	uint8_t channel = 0;
	std::vector<uint8_t> body;
	TASK_CHECK(ReadFrame(client_a.fd, channel, body));
	TASK_CHECK(channel == 0 && SeqOf(body) == (uint16_t)(SeqOf(frames_a[1]) + 1));
	TASK_CHECK(ReadFrame(client_b.fd, channel, body));
	TASK_CHECK(channel == 0 && SeqOf(body) == (uint16_t)(SeqOf(frames_b[1]) + 1));

	srv.Stop();
	return 0;
}

// ---------------------------------------------------------------------------
// transports 组
// ---------------------------------------------------------------------------

// TCP 反馈可以在任意字节处拆开, 也可以几帧粘在一起且后面紧跟 GET_PARAMETER
int CaseTcpFragmentedAndCoalesced()
{
	TestServer srv;
	TASK_CHECK(srv.Start(false));

	TestClient client;
	TASK_CHECK(client.Connect(srv.port));
	TASK_CHECK(client.Options());
	TASK_CHECK(client.Describe());
	TASK_CHECK(client.SetupTcp(0, 0, 1));
	TASK_CHECK(client.Play());

	TASK_CHECK(srv.PushVideo(1000, 0x10));
	TASK_CHECK(srv.PushVideo(2000, 0x40));

	std::vector<std::vector<uint8_t> > frames(2);
	for (int n = 0; n < 2; n++) {
		uint8_t channel = 0;
		TASK_CHECK(ReadFrame(client.fd, channel, frames[n]));
		TASK_CHECK(channel == 0);
	}
	uint16_t s0 = SeqOf(frames[0]);
	uint32_t ssrc = SsrcOf(frames[0]);

	// 把 NACK 帧拆成多段发送(段间稍作停顿让服务端按残缺帧处理),
	// 没收全的帧留在接收缓冲里, 收全后照常补包
	{
		std::vector<uint8_t> frame = RtcpFrame(1, BuildNack(0x1234, ssrc, {{(uint16_t)(s0 + 1), 0}}));
		size_t cuts[3] = {1, 3, 4};
		size_t sent = 0;
		for (int n = 0; n < 3; n++) {
			TASK_CHECK(SendAll(client.fd, frame.data() + sent, cuts[n]));
			sent += cuts[n];
			BriefPause();
		}
		TASK_CHECK(SendAll(client.fd, frame.data() + sent, frame.size() - sent));

		uint8_t channel = 0;
		std::vector<uint8_t> body;
		TASK_CHECK(ReadFrame(client.fd, channel, body));
		TASK_CHECK(channel == 0 && body == frames[1]);
	}

	// 两份反馈帧粘连且后面紧跟 GET_PARAMETER: 完整的帧按各自长度消费,
	// 控制请求不能被吞掉
	{
		std::vector<uint8_t> stream = RtcpFrame(1, BuildNack(0x1234, ssrc, {{s0, 0}}));
		std::vector<uint8_t> second = RtcpFrame(1, BuildNack(0x1234, ssrc, {{(uint16_t)(s0 + 1), 0}}));
		stream.insert(stream.end(), second.begin(), second.end());
		std::string get_parameter = client.GetParameterText();
		stream.insert(stream.end(), get_parameter.begin(), get_parameter.end());
		TASK_CHECK(SendBytes(client.fd, stream));

		uint8_t channel = 0;
		std::vector<uint8_t> body;
		TASK_CHECK(ReadFrame(client.fd, channel, body));
		TASK_CHECK(channel == 0 && body == frames[0]);
		TASK_CHECK(ReadFrame(client.fd, channel, body));
		TASK_CHECK(channel == 0 && body == frames[1]);
		std::string resp;
		TASK_CHECK(ReadResponse(client.fd, resp));
	}

	// 帧与控制请求混在同一段里、跨段边界拆开: 仍能各归各位
	{
		std::vector<uint8_t> stream = RtcpFrame(1, BuildNack(0x1234, ssrc, {{s0, 0}}));
		std::string get_parameter = client.GetParameterText();
		stream.insert(stream.end(), get_parameter.begin(), get_parameter.end());
		size_t cut = stream.size() - 7; // 最后 7 字节第二段发
		TASK_CHECK(SendAll(client.fd, stream.data(), cut));
		BriefPause();
		TASK_CHECK(SendAll(client.fd, stream.data() + cut, stream.size() - cut));

		uint8_t channel = 0;
		std::vector<uint8_t> body;
		TASK_CHECK(ReadFrame(client.fd, channel, body));
		TASK_CHECK(channel == 0 && body == frames[0]);
		std::string resp;
		TASK_CHECK(ReadResponse(client.fd, resp));
	}

	srv.Stop();
	return 0;
}

// UDP: 一个数据报就是一份完整 RTCP, 反馈要到协商的 RTCP socket 且 SSRC 对上
int CaseUdpGenericNack()
{
	TestServer srv;
	TASK_CHECK(srv.Start(true)); // 视频 + 音频两条媒体

	uint16_t client_rtp_v = 0, client_rtcp_v = 0, client_rtp_a = 0, client_rtcp_a = 0;
	int rtp_v = MakeUdpSocket(client_rtp_v);
	int rtcp_v = MakeUdpSocket(client_rtcp_v);
	int rtp_a = MakeUdpSocket(client_rtp_a);
	int rtcp_a = MakeUdpSocket(client_rtcp_a);
	TASK_CHECK(rtp_v >= 0 && rtcp_v >= 0 && rtp_a >= 0 && rtcp_a >= 0);

	TestClient client;
	TASK_CHECK(client.Connect(srv.port));
	TASK_CHECK(client.Options());
	TASK_CHECK(client.Describe());
	uint16_t server_rtp_v = 0, server_rtcp_v = 0, server_rtp_a = 0, server_rtcp_a = 0;
	TASK_CHECK(client.SetupUdp(0, client_rtp_v, client_rtcp_v, server_rtp_v, server_rtcp_v));
	TASK_CHECK(client.SetupUdp(1, client_rtp_a, client_rtcp_a, server_rtp_a, server_rtcp_a));
	TASK_CHECK(client.Play());

	// 先推视频 I 帧(让连接进入可发状态), 再推音频
	TASK_CHECK(srv.PushVideo(1000, 0x10));
	std::vector<uint8_t> video_frame;
	TASK_CHECK(RecvUdp(rtp_v, video_frame));
	TASK_CHECK(srv.PushAudio(500, 0x33));
	std::vector<uint8_t> audio_frame;
	TASK_CHECK(RecvUdp(rtp_a, audio_frame));

	uint16_t seq_v = SeqOf(video_frame);
	uint32_t ssrc_v = SsrcOf(video_frame);
	uint16_t seq_a = SeqOf(audio_frame);
	uint32_t ssrc_a = SsrcOf(audio_frame);
	TASK_CHECK(ssrc_v != ssrc_a);

	// 视频 NACK 到视频的 RTCP 端口: 重传原样回到视频的 RTP 端口
	TASK_CHECK(SendUdpTo(rtcp_v, server_rtcp_v, BuildNack(0x1234, ssrc_v, {{seq_v, 0}})));
	{
		std::vector<uint8_t> body;
		TASK_CHECK(RecvUdp(rtp_v, body));
		TASK_CHECK(body == video_frame);
	}

	// RR + NACK 复合包到音频的 RTCP 端口
	TASK_CHECK(SendUdpTo(rtcp_a, server_rtcp_a,
			Concat({BuildRr(0x1234), BuildNack(0x1234, ssrc_a, {{seq_a, 0}})})));
	{
		std::vector<uint8_t> body;
		TASK_CHECK(RecvUdp(rtp_a, body));
		TASK_CHECK(body == audio_frame);
	}

	// 反馈到了另一条媒体的 RTCP socket: SSRC 对不上, 不补包
	TASK_CHECK(SendUdpTo(rtcp_a, server_rtcp_a, BuildNack(0x1234, ssrc_v, {{seq_v, 0}})));
	TASK_CHECK(RecvUdpNone(rtp_v, 200));

	// 损坏的复合包: 不补包, 连接之后照常
	{
		std::vector<uint8_t> bad = BuildNack(0x1234, ssrc_v, {{seq_v, 0}});
		bad[0] = 0xC1; // V=3
		TASK_CHECK(SendUdpTo(rtcp_v, server_rtcp_v, bad));
		TASK_CHECK(RecvUdpNone(rtp_v, 200));
	}

	// 控制连接照常应答, 合法 NACK 照常补包
	{
		std::string resp;
		TASK_CHECK(client.Request("GET_PARAMETER", client.base_url,
				"Session: " + client.session + "\r\n", resp));
	}
	TASK_CHECK(SendUdpTo(rtcp_v, server_rtcp_v, BuildNack(0x1234, ssrc_v, {{seq_v, 0}})));
	{
		std::vector<uint8_t> body;
		TASK_CHECK(RecvUdp(rtp_v, body));
		TASK_CHECK(body == video_frame);
		TASK_CHECK(RecvUdpNone(rtp_v, 200)); // 只补一次
	}

	close(rtp_v);
	close(rtcp_v);
	close(rtp_a);
	close(rtcp_a);
	srv.Stop();
	return 0;
}

// 两条媒体通道(TCP): 各自的反馈走各自的 RTCP 通道, 补包走各自的 RTP 通道
int CaseTcpTwoMediaChannels()
{
	TestServer srv;
	TASK_CHECK(srv.Start(true)); // 视频 + 音频

	TestClient client;
	TASK_CHECK(client.Connect(srv.port));
	TASK_CHECK(client.Options());
	TASK_CHECK(client.Describe());
	TASK_CHECK(client.SetupTcp(0, 0, 1));
	TASK_CHECK(client.SetupTcp(1, 2, 3));
	TASK_CHECK(client.Play());

	// 先视频 I 帧, 再音频
	TASK_CHECK(srv.PushVideo(1000, 0x10));
	uint8_t channel = 0;
	std::vector<uint8_t> video_frame;
	TASK_CHECK(ReadFrame(client.fd, channel, video_frame));
	TASK_CHECK(channel == 0);

	TASK_CHECK(srv.PushAudio(500, 0x33));
	std::vector<uint8_t> audio_frame;
	TASK_CHECK(ReadFrame(client.fd, channel, audio_frame));
	TASK_CHECK(channel == 2);

	uint16_t seq_v = SeqOf(video_frame);
	uint32_t ssrc_v = SsrcOf(video_frame);
	uint16_t seq_a = SeqOf(audio_frame);
	uint32_t ssrc_a = SsrcOf(audio_frame);
	TASK_CHECK(ssrc_v != ssrc_a);

	// 视频反馈走 RTCP 通道 1, 补包回到 RTP 通道 0
	TASK_CHECK(ExpectRetransmit(client.fd, 1, BuildNack(0x1234, ssrc_v, {{seq_v, 0}}),
			0, video_frame));

	// 音频反馈走 RTCP 通道 3, 补包回到 RTP 通道 2
	TASK_CHECK(ExpectRetransmit(client.fd, 3, BuildNack(0x1234, ssrc_a, {{seq_a, 0}}),
			2, audio_frame));

	// 视频的 SSRC 送到音频的 RTCP 通道: 对不上, 不补包
	TASK_CHECK(ExpectNoRetransmit(client, 3, BuildNack(0x1234, ssrc_v, {{seq_v, 0}})));

	srv.Stop();
	return 0;
}

} // namespace

const struct task_case task_cases[] =
{
	{ "feedback", "tcp_generic_nack", CaseTcpGenericNack },
	{ "feedback", "tcp_nack_wrap_around", CaseTcpNackWrapAround },
	{ "feedback", "tcp_malformed_compound_recovers", CaseTcpMalformedCompoundRecovers },
	{ "feedback", "tcp_channel_and_ssrc_must_match", CaseTcpChannelAndSsrcMustMatch },
	{ "snapshots", "history_exact_bytes_and_seq", CaseHistoryExactBytesAndSeq },
	{ "snapshots", "history_eviction_512", CaseHistoryEviction },
	{ "snapshots", "two_clients_shared_scheduler", CaseTwoClientsSharedScheduler },
	{ "transports", "tcp_fragmented_and_coalesced", CaseTcpFragmentedAndCoalesced },
	{ "transports", "udp_generic_nack", CaseUdpGenericNack },
	{ "transports", "tcp_two_media_channels", CaseTcpTwoMediaChannels },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
