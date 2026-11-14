// Generic NACK (RFC 4585 RTPFB/FMT=1) 重传示例。
//
// 启动一个 RTSP 服务器(H264), 再用系统 socket 模拟一个客户端:
// 通过 TCP 交错通道收 RTP, 然后给第一个收到的包发一份 Generic NACK,
// 服务器会把该包的原字节重新发回来。不需要外部播放器。
//
// 用法: ./rtsp_nack_retransmit

#include "xop/RtspServer.h"
#include "xop/H264Source.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

static void need(bool ok, const char* message)
{
	if (!ok) {
		throw std::runtime_error(message);
	}
}

static void send_all(int fd, const void* data, size_t size)
{
	const char* p = (const char*)data;
	while (size > 0) {
		ssize_t n = send(fd, p, size, 0);
		need(n > 0, "send failed");
		p += n;
		size -= (size_t)n;
	}
}

static void recv_all(int fd, void* data, size_t size)
{
	char* p = (char*)data;
	while (size > 0) {
		pollfd pfd = { fd, POLLIN, 0 };
		need(poll(&pfd, 1, 3000) > 0, "receive timeout");
		ssize_t n = recv(fd, p, size, 0);
		need(n > 0, "receive closed");
		p += n;
		size -= (size_t)n;
	}
}

static std::string request(int fd, const std::string& text)
{
	send_all(fd, text.data(), text.size());
	std::string out;
	char c = 0;
	while (out.find("\r\n\r\n") == std::string::npos) {
		recv_all(fd, &c, 1);
		out += c;
		need(out.size() < 16000, "header limit");
	}
	size_t pos = out.find("Content-Length:");
	if (pos != std::string::npos) {
		int n = std::atoi(out.c_str() + pos + 15);
		while (n-- > 0) {
			recv_all(fd, &c, 1);
			out += c;
		}
	}
	need(out.find("200 OK") != std::string::npos, "RTSP rejected");
	return out;
}

// 读取一个 TCP 交错帧, 返回通道号和负载(RTP 包)
static void read_frame(int fd, uint8_t& channel, std::vector<uint8_t>& packet)
{
	uint8_t head[4];
	recv_all(fd, head, 4);
	need(head[0] == '$', "interleaved frame expected");
	channel = head[1];
	packet.resize(((uint32_t)head[2] << 8) | head[3]);
	if (!packet.empty()) {
		recv_all(fd, packet.data(), packet.size());
	}
}

int main()
{
	try {
		signal(SIGPIPE, SIG_IGN);

		xop::EventLoop loop(1);
		auto server = xop::RtspServer::Create(&loop);

		// 找一个空闲端口
		int probe = socket(AF_INET, SOCK_STREAM, 0);
		sockaddr_in addr = {};
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		need(bind(probe, (sockaddr*)&addr, sizeof(addr)) == 0, "bind probe");
		socklen_t len = sizeof(addr);
		getsockname(probe, (sockaddr*)&addr, &len);
		uint16_t port = ntohs(addr.sin_port);
		close(probe);

		need(server->Start("127.0.0.1", port), "server start");
		auto session = xop::MediaSession::CreateNew("live");
		session->AddSource(xop::channel_0, xop::H264Source::CreateNew());
		auto session_id = server->AddSession(session);

		std::string url = "rtsp://127.0.0.1:" + std::to_string(port) + "/live";
		std::cout << "URL: " << url << std::endl;

		int fd = socket(AF_INET, SOCK_STREAM, 0);
		need(connect(fd, (sockaddr*)&addr, sizeof(addr)) == 0, "connect");

		request(fd, "OPTIONS " + url + " RTSP/1.0\r\nCSeq: 1\r\n\r\n");
		request(fd, "DESCRIBE " + url + " RTSP/1.0\r\nCSeq: 2\r\nAccept: application/sdp\r\n\r\n");
		auto setup = request(fd, "SETUP " + url + "/track0 RTSP/1.0\r\nCSeq: 3\r\n"
				"Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n\r\n");
		size_t pos = setup.find("Session: ");
		need(pos != std::string::npos, "session response");
		auto token = setup.substr(pos + 9, setup.find('\r', pos) - pos - 9);
		request(fd, "PLAY " + url + " RTSP/1.0\r\nCSeq: 4\r\nSession: " + token + "\r\n\r\n");

		// 推两帧, 客户端各收一包
		std::vector<std::vector<uint8_t> > frames(2);
		for (int n = 0; n < 2; n++) {
			xop::AVFrame frame;
			frame.type = xop::VIDEO_FRAME_I;
			frame.timestamp = (uint32_t)(90000 + n * 3000);
			frame.size = 8;
			frame.buffer.reset(new uint8_t[frame.size]);
			frame.buffer.get()[0] = 0x65;
			for (uint32_t k = 1; k < frame.size; k++) {
				frame.buffer.get()[k] = (uint8_t)(0x30 * n + k);
			}
			need(server->PushFrame(session_id, xop::channel_0, frame), "push frame");

			uint8_t channel = 0;
			read_frame(fd, channel, frames[n]);
			need(channel == 0, "rtp channel");
			printf("RTP  seq=%u ts=%u bytes=%zu\n",
					(unsigned)((frames[n][2] << 8) | frames[n][3]),
					(unsigned)((frames[n][4] << 24) | (frames[n][5] << 16) |
							(frames[n][6] << 8) | frames[n][7]),
					frames[n].size());
		}

		// 对第一包发 Generic NACK (RTPFB PT=205, FMT=1): PID 为它的序号
		uint16_t lost_seq = (uint16_t)((frames[0][2] << 8) | frames[0][3]);
		uint8_t nack[16] = {
			0x81, 205, 0, 3,                 // V=2, FMT=1, PT=205, length=3
			0, 0, 0, 1,                      // sender SSRC
			frames[0][8], frames[0][9], frames[0][10], frames[0][11], // 媒体 SSRC
			(uint8_t)(lost_seq >> 8), (uint8_t)(lost_seq & 0xff), 0, 0, // PID, BLP
		};
		uint8_t head[4] = { '$', 1, 0, 16 }; // 走协商的 RTCP 通道 1
		send_all(fd, head, 4);
		send_all(fd, nack, sizeof(nack));

		// 服务器把原包补发回来, 字节与第一次发送完全一致
		uint8_t channel = 0;
		std::vector<uint8_t> again;
		read_frame(fd, channel, again);
		need(channel == 0, "retransmit channel");
		need(again == frames[0], "retransmitted bytes differ");
		printf("NACK seq=%u -> retransmitted %zu bytes, identical to the first send\n",
				(unsigned)lost_seq, again.size());

		close(fd);
		server->Stop();
		loop.Quit();
		return 0;
	}
	catch (const std::exception& e) {
		std::cerr << e.what() << std::endl;
		return 1;
	}
}
