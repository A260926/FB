// Short in-memory demonstration of the RTP repair history.
// It builds one ordinary RTP packet, commits it only after the simulated
// transmitter accepts it, then answers a Generic NACK with the original bytes.

#include "rtpretransmit.h"

#include "rtcpcompoundpacket.h"
#include "rtppacketbuilder.h"
#include "rtprawpacket.h"
#include "rtptimeutilities.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

struct ManualClock {
	uint64_t nowUS;
};

static uint64_t GetClockUS(void *context)
{
	return static_cast<ManualClock *>(context)->nowUS;
}

struct MemorySink {
	const unsigned char *expectedData;
	size_t expectedLength;
	size_t writes;
};

static int WritePacket(void *context,const void *data,size_t length)
{
	MemorySink *sink = static_cast<MemorySink *>(context);
	++sink->writes;

	if (length != sink->expectedLength ||
	    std::memcmp(data,sink->expectedData,length) != 0)
		return -100;

	std::printf("repair write %u bytes, exact original RTP packet\n",
	            static_cast<unsigned>(length));
	return 0;
}

static void Put16(std::vector<unsigned char> &bytes,size_t offset,uint16_t value)
{
	bytes[offset] = static_cast<unsigned char>(value >> 8);
	bytes[offset + 1] = static_cast<unsigned char>(value);
}

static void Put32(std::vector<unsigned char> &bytes,size_t offset,uint32_t value)
{
	bytes[offset] = static_cast<unsigned char>(value >> 24);
	bytes[offset + 1] = static_cast<unsigned char>(value >> 16);
	bytes[offset + 2] = static_cast<unsigned char>(value >> 8);
	bytes[offset + 3] = static_cast<unsigned char>(value);
}

int main()
{
	RTPPacketBuilder builder;
	if (builder.Init(1500) < 0)
	{
		std::fprintf(stderr,"builder init failed\n");
		return 1;
	}

	const unsigned char audioFrame[] = {0x80,0x08,0x15,0x27};
	const uint16_t sequence = builder.GetSequenceNumber();
	const uint32_t timestamp = builder.GetTimestamp();
	const uint32_t ssrc = builder.GetSSRC();

	if (builder.BuildPacket(audioFrame,sizeof(audioFrame),96,false,160) < 0)
	{
		std::fprintf(stderr,"build failed\n");
		return 1;
	}

	ManualClock clock;
	clock.nowUS = 1000000;

	RTPRepairConfig config;
	config.maxPackets = 8;
	config.maxBytes = 4096;
	config.clockRate = 8000;
	config.baseTimestamp = timestamp;
	config.baseTimeUS = clock.nowUS;
	config.delayUS = 200000;
	config.rttUS = 10000;
	config.retryUS = 20000;

	RTPRepairHistory history;
	if (history.Configure(config) < 0)
		return 2;

	// In RTPSession this commit happens only after WriteRTPData succeeded.
	if (history.RememberSent(builder.GetPacket(),builder.GetPacketLength(),
	                         GetClockUS(&clock)) != 0)
	{
		std::fprintf(stderr,"successful packet was not remembered\n");
		return 3;
	}

	// RR (first compound packet) followed by one Generic NACK for sequence.
	std::vector<unsigned char> compound(24,0);
	compound[0] = 0x80; compound[1] = 201; Put16(compound,2,1); Put32(compound,4,9);
	compound[8] = 0x81; compound[9] = 205; Put16(compound,10,3);
	Put32(compound,12,0x11223344); // RTCP sender SSRC
	Put32(compound,16,ssrc);       // media SSRC
	Put16(compound,20,sequence);
	Put16(compound,22,0);           // BLP

	unsigned char *rawBytes = new unsigned char[compound.size()];
	std::memcpy(rawBytes,compound.data(),compound.size());
	RTPTime receiveTime(0,0);
	RTPRawPacket raw(rawBytes,compound.size(),0,receiveTime,false);
	RTCPCompoundPacket feedback(raw);
	if (feedback.GetCreationError() < 0)
	{
		std::fprintf(stderr,"NACK compound parse failed\n");
		return 4;
	}

	clock.nowUS = 1050000; // now+rtt is still before deadline
	MemorySink sink;
	sink.expectedData = builder.GetPacket();
	sink.expectedLength = builder.GetPacketLength();
	sink.writes = 0;

	int repaired = history.Retransmit(feedback,GetClockUS(&clock),WritePacket,&sink);
	if (repaired != 1 || sink.writes != 1)
	{
		std::fprintf(stderr,"expected one repair, got %d writes=%u\n",
		             repaired,static_cast<unsigned>(sink.writes));
		return 5;
	}

	std::printf("retransmit-example seq=%u ssrc=%08x cachedPackets=%u cachedBytes=%u repaired=%d\n",
	            static_cast<unsigned>(sequence),ssrc,
	            static_cast<unsigned>(history.PacketCount()),
	            static_cast<unsigned>(history.ByteCount()),repaired);
	return 0;
}
