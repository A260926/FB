#include "task_support/check.h"

#include "rtpretransmit.h"
#include "rtcpcompoundpacket.h"
#include "rtpipv4address.h"
#include "rtppacket.h"
#include "rtppacketbuilder.h"
#include "rtprawpacket.h"
#include "rtpsession.h"
#include "rtpsessionparams.h"
#include "rtptimeutilities.h"
#include "rtptransmitter.h"

#include <cstring>
#include <memory>
#include <queue>
#include <vector>

typedef std::vector<uint8_t> Bytes;

static void Put16(Bytes &b,size_t offset,uint16_t value)
{
	b[offset] = static_cast<uint8_t>(value >> 8);
	b[offset + 1] = static_cast<uint8_t>(value);
}

static void Put32(Bytes &b,size_t offset,uint32_t value)
{
	b[offset] = static_cast<uint8_t>(value >> 24);
	b[offset + 1] = static_cast<uint8_t>(value >> 16);
	b[offset + 2] = static_cast<uint8_t>(value >> 8);
	b[offset + 3] = static_cast<uint8_t>(value);
}

static Bytes MakeRTP(uint16_t seq,uint32_t timestamp,uint32_t ssrc,
                     const Bytes &payload = Bytes(),
                     const std::vector<uint32_t> &csrcs = std::vector<uint32_t>(),
                     bool extension = false,uint16_t extensionID = 0xbeef,
                     const Bytes &extensionData = Bytes(),bool marker = false,
                     uint8_t payloadType = 96)
{
	size_t offset = 12 + csrcs.size() * 4;
	if (extension)
		offset += 4 + extensionData.size();

	Bytes packet(offset + payload.size(),0);
	packet[0] = static_cast<uint8_t>(0x80 | (extension ? 0x10 : 0) |
	                                 (csrcs.size() & 0x0f));
	packet[1] = static_cast<uint8_t>((marker ? 0x80 : 0) | (payloadType & 0x7f));
	Put16(packet,2,seq);
	Put32(packet,4,timestamp);
	Put32(packet,8,ssrc);

	for (size_t i = 0;i < csrcs.size();++i)
		Put32(packet,12 + i * 4,csrcs[i]);

	if (extension)
	{
		size_t extOffset = 12 + csrcs.size() * 4;
		Put16(packet,extOffset,extensionID);
		Put16(packet,extOffset + 2,static_cast<uint16_t>(extensionData.size() / 4));
		if (!extensionData.empty())
			std::memcpy(&packet[extOffset + 4],extensionData.data(),extensionData.size());
	}

	if (!payload.empty())
		std::memcpy(&packet[offset],payload.data(),payload.size());
	return packet;
}

static RTPRepairConfig TestConfig(size_t maxPackets = 8,size_t maxBytes = 4096,
                                  uint32_t baseTimestamp = 1000,
                                  uint64_t baseTime = 1000000)
{
	RTPRepairConfig config;
	config.maxPackets = maxPackets;
	config.maxBytes = maxBytes;
	config.clockRate = 8000;
	config.baseTimestamp = baseTimestamp;
	config.baseTimeUS = baseTime;
	config.delayUS = 200000;
	config.rttUS = 10000;
	config.retryUS = 20000;
	return config;
}

struct MemWriter {
	int calls;
	int failOnCall;
	int failureCode;
	std::vector<uint16_t> callSequences;
	std::vector<Bytes> packets;

	MemWriter() : calls(0),failOnCall(0),failureCode(-37) { }
};

static int WriteToMem(void *context,const void *data,size_t len)
{
	MemWriter *writer = static_cast<MemWriter *>(context);
	const uint8_t *bytes = static_cast<const uint8_t *>(data);
	++writer->calls;
	uint16_t seq = static_cast<uint16_t>((static_cast<uint16_t>(bytes[2]) << 8) | bytes[3]);
	writer->callSequences.push_back(seq);

	if (writer->calls == writer->failOnCall)
		return writer->failureCode;

	writer->packets.push_back(Bytes(bytes,bytes + len));
	return 0;
}

static Bytes MakeRR(uint32_t ssrc = 9)
{
	Bytes packet(8,0);
	packet[0] = 0x80;
	packet[1] = 201;
	Put16(packet,2,1);
	Put32(packet,4,ssrc);
	return packet;
}

static Bytes MakeNACK(uint32_t mediaSSRC,uint16_t pid,uint16_t blp,
                      uint32_t senderSSRC = 0x11223344)
{
	Bytes packet(16,0);
	packet[0] = 0x81; // V2, FMT=1
	packet[1] = 205;
	Put16(packet,2,3);
	Put32(packet,4,senderSSRC);
	Put32(packet,8,mediaSSRC);
	Put16(packet,12,pid);
	Put16(packet,14,blp);
	return packet;
}

static Bytes Compound(const Bytes &first,const Bytes &second = Bytes(),
                      const Bytes &third = Bytes())
{
	Bytes result = first;
	result.insert(result.end(),second.begin(),second.end());
	result.insert(result.end(),third.begin(),third.end());
	return result;
}

static std::unique_ptr<RTCPCompoundPacket> ParseRTCP(const Bytes &bytes)
{
	uint8_t *buffer = new uint8_t[bytes.size()];
	std::memcpy(buffer,bytes.data(),bytes.size());
	RTPTime time(0,0);
	RTPRawPacket raw(buffer,bytes.size(),0,time,false);
	return std::unique_ptr<RTCPCompoundPacket>(new RTCPCompoundPacket(raw));
}

static int test_history_configuration(void)
{
	RTPRepairHistory history;
	TASK_CHECK(history.PacketCount() == 0);
	TASK_CHECK(history.ByteCount() == 0);

	Bytes packet = MakeRTP(100,1000,0x41424344,Bytes(8,0x5a));
	TASK_CHECK(history.RememberSent(packet.data(),packet.size(),1000000) < 0);
	TASK_CHECK(history.Retransmit(*ParseRTCP(Compound(MakeRR(),MakeNACK(1,2,0))),
	                              1000000,WriteToMem,0) < 0);

	RTPRepairConfig config = TestConfig(1,1024);
	TASK_CHECK(history.Configure(config) == 0);
	TASK_CHECK(history.RememberSent(packet.data(),packet.size(),1000000) == 0);
	TASK_CHECK(history.PacketCount() == 1);
	TASK_CHECK(history.ByteCount() == packet.size());
	TASK_CHECK(history.RememberSent(packet.data(),packet.size(),1000000) < 0);
	TASK_CHECK(history.PacketCount() == 1);
	TASK_CHECK(history.ByteCount() == packet.size());

	RTPRepairConfig bad = config;
	bad.maxPackets = 0;
	TASK_CHECK(history.Configure(bad) < 0);
	TASK_CHECK(history.PacketCount() == 1);

	Bytes next = MakeRTP(101,1160,0x41424344,Bytes(8,0x6b));
	TASK_CHECK(history.RememberSent(next.data(),next.size(),1000000) == 0);
	TASK_CHECK(history.PacketCount() == 1);
	TASK_CHECK(history.ByteCount() == next.size());

	history.Clear();
	TASK_CHECK(history.PacketCount() == 0);
	TASK_CHECK(history.RememberSent(next.data(),next.size(),1000000) < 0);
	return 0;
}

static int test_history_quota_expiry_and_ssrc(void)
{
	RTPRepairConfig config = TestConfig(3,90);
	RTPRepairHistory history;
	TASK_CHECK(history.Configure(config) == 0);

	std::vector<Bytes> packets;
	for (uint16_t seq = 10;seq < 15;++seq)
	{
		Bytes packet = MakeRTP(seq,static_cast<uint32_t>(1000 + (seq - 10) * 160),
		                       0xaaaaaaaa,Bytes(8,static_cast<uint8_t>(seq)));
		packets.push_back(packet);
		TASK_CHECK(history.RememberSent(packet.data(),packet.size(),1000000) == 0);
	}
	TASK_CHECK(history.PacketCount() == 3);
	TASK_CHECK(history.ByteCount() == packets[2].size() * 3);

	// Byte quota alone forces two 20-byte packets to remain with maxBytes=45.
	RTPRepairHistory byteHistory;
	TASK_CHECK(byteHistory.Configure(TestConfig(10,45)) == 0);
	for (int i = 0;i < 3;++i)
	{
		Bytes packet = MakeRTP(static_cast<uint16_t>(20 + i),1000 + i * 160,
		                       0xbbbbbbbb,Bytes(8,static_cast<uint8_t>(i)));
		TASK_CHECK(byteHistory.RememberSent(packet.data(),packet.size(),1000000) == 0);
	}
	TASK_CHECK(byteHistory.PacketCount() == 2);
	TASK_CHECK(byteHistory.ByteCount() == 40);

	// deadline is baseTime + delay, because timestamp equals baseTimestamp.
	Bytes expired = MakeRTP(30,1000,0xcccccccc,Bytes(4,1));
	TASK_CHECK(history.Configure(TestConfig()) == 0);
	TASK_CHECK(history.RememberSent(expired.data(),expired.size(),1200000) == 1);
	TASK_CHECK(history.PacketCount() == 0);

	// Negative media offset is validly sent but deliberately not cached.
	Bytes negativeOffset = MakeRTP(31,999,0xcccccccc,Bytes(4,2));
	TASK_CHECK(history.RememberSent(negativeOffset.data(),negativeOffset.size(),1000000) == 1);
	TASK_CHECK(history.PacketCount() == 0);

	// A packet larger than the total byte quota returns 1, not a cache error.
	RTPRepairConfig smallBytes = TestConfig(8,16);
	TASK_CHECK(history.Configure(smallBytes) == 0);
	Bytes tooLarge = MakeRTP(32,1000,0xcccccccc,Bytes(8,3)); // 20 bytes total
	TASK_CHECK(history.RememberSent(tooLarge.data(),tooLarge.size(),1000000) == 1);
	TASK_CHECK(history.PacketCount() == 0);

	// A new SSRC generation discards old bytes but keeps configuration.
	Bytes oldSSRC = MakeRTP(40,1000,0x11111111,Bytes(4,4));
	Bytes newSSRC = MakeRTP(41,1000,0x22222222,Bytes(4,5));
	TASK_CHECK(history.Configure(TestConfig()) == 0);
	TASK_CHECK(history.RememberSent(oldSSRC.data(),oldSSRC.size(),1000000) == 0);
	TASK_CHECK(history.RememberSent(newSSRC.data(),newSSRC.size(),1000000) == 0);
	TASK_CHECK(history.PacketCount() == 1);
	TASK_CHECK(history.ByteCount() == newSSRC.size());
	return 0;
}

static int test_history_bytes_order_extensions(void)
{
	RTPRepairHistory history;
	TASK_CHECK(history.Configure(TestConfig()) == 0);

	std::vector<uint32_t> csrcs;
	csrcs.push_back(0x12345678);
	csrcs.push_back(0x9abcdef0);
	Bytes extension(8,0);
	Put32(extension,0,0x01020304);
	Put32(extension,4,0x05060708);

	Bytes extended = MakeRTP(55,1320,0x42424242,Bytes(5,0xab),csrcs,true,0xbeef,extension);
	Bytes ordinary = MakeRTP(56,1480,0x42424242,Bytes(3,0xcd));
	TASK_CHECK(history.RememberSent(extended.data(),extended.size(),1000000) == 0);
	TASK_CHECK(history.RememberSent(ordinary.data(),ordinary.size(),1000000) == 0);

	MemWriter writer;
	Bytes feedback = Compound(MakeRR(),MakeNACK(0x42424242,55,1 << 0)); // 55 and 56
	std::unique_ptr<RTCPCompoundPacket> cp = ParseRTCP(feedback);
	TASK_CHECK(cp->GetCreationError() == 0);
	TASK_CHECK(history.Retransmit(*cp,1000000,&WriteToMem,&writer) == 2);
	TASK_CHECK(writer.calls == 2);
	TASK_CHECK(writer.callSequences[0] == 55);
	TASK_CHECK(writer.callSequences[1] == 56);
	TASK_CHECK(writer.packets[0] == extended);
	TASK_CHECK(writer.packets[1] == ordinary);

	uint8_t *parseBuffer = new uint8_t[extended.size()];
	std::memcpy(parseBuffer,extended.data(),extended.size());
	RTPTime parseTime(0,0);
	RTPRawPacket parseRaw(parseBuffer,extended.size(),0,parseTime,true);
	RTPPacket parsePacket(parseRaw);
	TASK_CHECK(parsePacket.GetCreationError() == 0);
	TASK_CHECK(parsePacket.GetCSRCCount() == 2);
	TASK_CHECK(parsePacket.GetCSRC(0) == 0x12345678);
	TASK_CHECK(parsePacket.GetExtensionID() == 0xbeef);
	TASK_CHECK(parsePacket.GetExtensionLength() == 8);
	TASK_CHECK(parsePacket.GetPayloadLength() == 5);
	return 0;
}

static int test_history_wrap_duplicates_and_retry(void)
{
	RTPRepairHistory history;
	TASK_CHECK(history.Configure(TestConfig()) == 0);

	uint16_t sequences[4] = {65534,65535,0,1};
	std::vector<Bytes> packets;
	for (int i = 0;i < 4;++i)
	{
		Bytes packet = MakeRTP(sequences[i],1000 + i * 160,0x55667788,Bytes(2,static_cast<uint8_t>(i)));
		packets.push_back(packet);
		TASK_CHECK(history.RememberSent(packet.data(),packet.size(),1000000) == 0);
	}
	TASK_CHECK(history.RememberSent(packets[1].data(),packets[1].size(),1000000) < 0);

	// PID=65535, BLP bits 0 and 1 request 0 and 1.
	Bytes feedback = Compound(MakeRR(),MakeNACK(0x55667788,65535,0x0003));
	std::unique_ptr<RTCPCompoundPacket> cp = ParseRTCP(feedback);
	MemWriter writer;
	TASK_CHECK(history.Retransmit(*cp,1000000,&WriteToMem,&writer) == 3);
	TASK_CHECK((writer.callSequences == std::vector<uint16_t>{65535,0,1}));
	TASK_CHECK(writer.packets[0] == packets[1]);
	TASK_CHECK(writer.packets[1] == packets[2]);
	TASK_CHECK(writer.packets[2] == packets[3]);

	// Duplicate feedback at the same time is limited by retryUS.
	TASK_CHECK(history.Retransmit(*cp,1000000,&WriteToMem,&writer) == 0);
	TASK_CHECK(writer.calls == 3);
	TASK_CHECK(history.Retransmit(*cp,1019999,&WriteToMem,&writer) == 0);
	TASK_CHECK(writer.calls == 3);
	TASK_CHECK(history.Retransmit(*cp,1020000,&WriteToMem,&writer) == 3);
	TASK_CHECK(writer.calls == 6);

	// Repeated requests inside one compound are merged.
	Bytes duplicated = Compound(MakeRR(),
	                            MakeNACK(0x55667788,0,0),
	                            MakeNACK(0x55667788,0,0));
	std::unique_ptr<RTCPCompoundPacket> dupCP = ParseRTCP(duplicated);
	MemWriter dupWriter;
	TASK_CHECK(history.Retransmit(*dupCP,1100000,&WriteToMem,&dupWriter) == 1);
	TASK_CHECK(dupWriter.calls == 1);
	return 0;
}

static int test_history_deadline_rtt_and_failures(void)
{
	RTPRepairConfig config = TestConfig();
	RTPRepairHistory history;
	TASK_CHECK(history.Configure(config) == 0);
	Bytes first = MakeRTP(70,1000,0x77777777,Bytes(2,1));
	Bytes second = MakeRTP(71,1160,0x77777777,Bytes(2,2));
	TASK_CHECK(history.RememberSent(first.data(),first.size(),1000000) == 0);
	TASK_CHECK(history.RememberSent(second.data(),second.size(),1000000) == 0);

	// First deadline 1,200,000; rtt=10,000. 1,189,999 is feasible, equality is not.
	Bytes nackFirst = Compound(MakeRR(),MakeNACK(0x77777777,70,0));
	std::unique_ptr<RTCPCompoundPacket> cp70 = ParseRTCP(nackFirst);
	MemWriter writer;
	TASK_CHECK(history.Retransmit(*cp70,1189990,&WriteToMem,&writer) == 1);
	TASK_CHECK(writer.calls == 1);

	RTPRepairHistory rttHistory;
	TASK_CHECK(rttHistory.Configure(config) == 0);
	TASK_CHECK(rttHistory.RememberSent(first.data(),first.size(),1000000) == 0);
	MemWriter rttWriter;
	TASK_CHECK(rttHistory.Retransmit(*cp70,1190000,&WriteToMem,&rttWriter) == 0);
	TASK_CHECK(rttWriter.calls == 0);
	TASK_CHECK(rttHistory.Retransmit(*cp70,1189999,&WriteToMem,&rttWriter) == 1);
	TASK_CHECK(rttWriter.calls == 1);

	TASK_CHECK(history.Retransmit(*cp70,1189999,&WriteToMem,&writer) == 0); // cooldown

	// Fail seq 70 first; at the same now it has no success cooldown and retries.
	RTPRepairHistory failHistory;
	TASK_CHECK(failHistory.Configure(config) == 0);
	TASK_CHECK(failHistory.RememberSent(first.data(),first.size(),1000000) == 0);
	MemWriter failWriter;
	failWriter.failOnCall = 1;
	TASK_CHECK(failHistory.Retransmit(*cp70,1050000,&WriteToMem,&failWriter) == failWriter.failureCode);
	TASK_CHECK(failWriter.callSequences.size() == 1);
	TASK_CHECK(failHistory.Retransmit(*cp70,1050000,&WriteToMem,&failWriter) == 1);
	TASK_CHECK((failWriter.callSequences == std::vector<uint16_t>{70,70}));

	// If 70 succeeds then 71 fails, immediate retry skips cooled-down 70 and retries only 71.
	Bytes bothNack = Compound(MakeRR(),MakeNACK(0x77777777,70,1));
	std::unique_ptr<RTCPCompoundPacket> cpBoth = ParseRTCP(bothNack);
	RTPRepairHistory stopHistory;
	TASK_CHECK(stopHistory.Configure(config) == 0);
	TASK_CHECK(stopHistory.RememberSent(first.data(),first.size(),1000000) == 0);
	TASK_CHECK(stopHistory.RememberSent(second.data(),second.size(),1000000) == 0);
	MemWriter stopWriter;
	stopWriter.failOnCall = 2;
	TASK_CHECK(stopHistory.Retransmit(*cpBoth,1050000,&WriteToMem,&stopWriter) == stopWriter.failureCode);
	TASK_CHECK((stopWriter.callSequences == std::vector<uint16_t>{70,71}));
	TASK_CHECK(stopHistory.Retransmit(*cpBoth,1050000,&WriteToMem,&stopWriter) == 1);
	TASK_CHECK((stopWriter.callSequences == std::vector<uint16_t>{70,71,71}));
	TASK_CHECK(stopWriter.packets.size() == 2); // first 70 and retried 71
	return 0;
}

static int test_feedback_expansion_validation(void)
{
	uint32_t media = 0xabcdef01;
	Bytes nack1 = MakeNACK(media,10,static_cast<uint16_t>((1 << 0) | (1 << 2)));
	Bytes nack2 = MakeNACK(media,12,1); // requests 12 and duplicate 13
	Bytes other = MakeNACK(0x99999999,99,0xffff);
	Bytes bytes = Compound(MakeRR(),nack1,nack2);
	bytes.insert(bytes.end(),other.begin(),other.end());
	std::unique_ptr<RTCPCompoundPacket> cp = ParseRTCP(bytes);
	TASK_CHECK(cp->GetCreationError() == 0);

	std::vector<uint16_t> out;
	TASK_CHECK(cp->GetGenericNACKs(media,out) == 0);
	TASK_CHECK((out == std::vector<uint16_t>{10,11,13,12}));

	// Wrong media SSRC still validates structure and replaces out with empty.
	TASK_CHECK(cp->GetGenericNACKs(0x1234,out) == 0);
	TASK_CHECK(out.empty());

	// A structurally short NACK has no FCI.
	Bytes noFCI(12,0);
	noFCI[0] = 0x81;
	noFCI[1] = 205;
	Put16(noFCI,2,2);
	Put32(noFCI,8,media);
	std::unique_ptr<RTCPCompoundPacket> bad = ParseRTCP(Compound(MakeRR(),noFCI));
	TASK_CHECK(bad->GetCreationError() == 0);
	out.push_back(777);
	TASK_CHECK(bad->GetGenericNACKs(media,out) < 0);
	TASK_CHECK(out.size() == 1 && out[0] == 777);

	// The same malformed NACK must be rejected even for another media SSRC.
	TASK_CHECK(bad->GetGenericNACKs(42,out) < 0);
	TASK_CHECK(out.size() == 1 && out[0] == 777);

	// Valid four-byte padding.
	Bytes padded = MakeNACK(media,40,0);
	padded[0] = 0xa1;
	Put16(padded,2,4);
	Bytes padding{0,0,0,4};
	padded.insert(padded.end(),padding.begin(),padding.end());
	std::unique_ptr<RTCPCompoundPacket> padCP = ParseRTCP(Compound(MakeRR(),padded));
	TASK_CHECK(padCP->GetCreationError() == 0);
	out.clear();
	TASK_CHECK(padCP->GetGenericNACKs(media,out) == 0);
	TASK_CHECK((out == std::vector<uint16_t>{40}));

	// Bad padding count is a NACK structure error.
	Bytes badPad = padded;
	badPad[badPad.size() - 1] = 9;
	std::unique_ptr<RTCPCompoundPacket> badPadCP = ParseRTCP(Compound(MakeRR(),badPad));
	TASK_CHECK(badPadCP->GetCreationError() == 0);
	TASK_CHECK(badPadCP->GetGenericNACKs(media,out) < 0);

	// FCI length 6 is not four-byte aligned.
	Bytes misaligned = MakeNACK(media,41,0);
	misaligned[0] = 0xa1;
	Put16(misaligned,2,4); // 20 bytes total
	Bytes alignmentPadding{0,0,2,0}; // pad count 2
	misaligned.insert(misaligned.end(),alignmentPadding.begin(),alignmentPadding.end());
	std::unique_ptr<RTCPCompoundPacket> alignCP = ParseRTCP(Compound(MakeRR(),misaligned));
	TASK_CHECK(alignCP->GetCreationError() == 0);
	TASK_CHECK(alignCP->GetGenericNACKs(media,out) < 0);
	return 0;
}

static int test_builder_history_generation(void)
{
	RTPPacketBuilder builder;
	TASK_CHECK(builder.Init(1500) == 0);
	TASK_CHECK(builder.AddCSRC(0x11223344) == 0);

	RTPRepairConfig config = TestConfig(8,4096,builder.GetTimestamp(),1000000);
	TASK_CHECK(builder.ConfigureRepair(config) == 0);

	Bytes payload{1,2,3,4};
	TASK_CHECK(builder.BuildPacketEx(payload.data(),payload.size(),96,false,160,0xbeef,
	                                 Bytes(4,7).data(),1) == 0);
	TASK_CHECK(builder.RememberSentForRepair(1000000) == 0);
	TASK_CHECK(builder.RepairHistory().PacketCount() == 1);
	TASK_CHECK(builder.RememberSentForRepair(1000000) < 0);
	TASK_CHECK(builder.RepairHistory().PacketCount() == 1);

	// An oversized illegal build must not make the old buffer committable.
	size_t huge = 2000;
	Bytes hugePayload(huge,1);
	TASK_CHECK(builder.BuildPacket(hugePayload.data(),hugePayload.size(),96,false,160) < 0);
	TASK_CHECK(builder.RememberSentForRepair(1000000) < 0);
	TASK_CHECK(builder.RepairHistory().PacketCount() == 1);

	uint32_t oldSSRC = builder.GetSSRC();
	uint32_t newSSRC = builder.CreateNewSSRC();
	TASK_CHECK(newSSRC != oldSSRC);
	TASK_CHECK(builder.RepairHistory().PacketCount() == 0);

	builder.Destroy();
	TASK_CHECK(builder.RepairHistory().PacketCount() == 0);
	TASK_CHECK(builder.RememberSentForRepair(1000000) < 0);
	return 0;
}

class FakeTransmitter : public RTPTransmitter {
public:
	int rtpResult;
	uint32_t rtpCalls;
	std::vector<Bytes> calls;
	std::queue<RTPRawPacket *> incoming;

	FakeTransmitter() : rtpResult(0),rtpCalls(0) { }

	int Init(bool) override { return 0; }
	int Create(size_t,const RTPTransmissionParams *) override { return 0; }
	void Destroy() override { }
	int GetLocalHostName(u_int8_t *buffer,size_t *length) override
	{
		if (*length < 9)
			return -1;
		std::memcpy(buffer,"fake-host",9);
		*length = 9;
		return 0;
	}
	bool ComesFromThisTransmitter(const RTPAddress *) override { return false; }
	size_t GetHeaderOverhead() override { return 0; }
	int Poll() override { return 0; }
	int WaitForIncomingData(const RTPTime &) override { return 0; }
	int AbortWait() override { return 0; }
	int SendRTPData(const void *data,size_t len) override
	{
		const uint8_t *bytes = static_cast<const uint8_t *>(data);
		++rtpCalls;
		calls.push_back(Bytes(bytes,bytes + len));
		return rtpResult;
	}
	int SendRTCPData(const void *,size_t) override { return 0; }
	void ResetPacketCount() override { rtpCalls = 0; }
	u_int32_t GetNumRTPPacketsSent() override { return rtpCalls; }
	u_int32_t GetNumRTCPPacketsSent() override { return 0; }
	int AddDestination(const RTPAddress &) override { return 0; }
	int DeleteDestination(const RTPAddress &) override { return 0; }
	void ClearDestinations() override { }
	bool SupportsMulticasting() override { return false; }
	int JoinMulticastGroup(const RTPAddress &) override { return -1; }
	int LeaveMulticastGroup(const RTPAddress &) override { return -1; }
	void LeaveAllMulticastGroups() override { }
	int SetReceiveMode(ReceiveMode) override { return 0; }
	int AddToIgnoreList(const RTPAddress &) override { return 0; }
	int DeleteFromIgnoreList(const RTPAddress &) override { return 0; }
	void ClearIgnoreList() override { }
	int AddToAcceptList(const RTPAddress &) override { return 0; }
	int DeleteFromAcceptList(const RTPAddress &) override { return 0; }
	void ClearAcceptList() override { }
	int SetMaximumPacketSize(size_t) override { return 0; }
	bool NewDataAvailable() override { return !incoming.empty(); }
	RTPRawPacket *GetNextPacket() override
	{
		if (incoming.empty())
			return 0;
		RTPRawPacket *packet = incoming.front();
		incoming.pop();
		return packet;
	}

	void EnqueueRTCP(const Bytes &bytes)
	{
		uint8_t *buffer = new uint8_t[bytes.size()];
		std::memcpy(buffer,bytes.data(),bytes.size());
		RTPTime time(0,0);
		RTPIPv4Address *address = new RTPIPv4Address(0x0a000001,4000);
		incoming.push(new RTPRawPacket(buffer,bytes.size(),address,time,false));
	}
};

class TestSession : public RTPSession {
public:
	FakeTransmitter *fake;
	TestSession() : RTPSession(RTPTransmitter::IPv4UDPProto),fake(0) { }
protected:
	RTPTransmitter *CreateTransmitter(RTPTransmitter::TransmissionProtocol) override
	{
		fake = new FakeTransmitter();
		return fake;
	}
};

static uint64_t TestSessionClock(void *context)
{
	return *static_cast<uint64_t *>(context);
}

static int StartTestSession(TestSession &session,uint64_t &clockValue)
{
	RTPSessionParams params;
	params.SetResolveLocalHostname(false);
	params.SetOwnTimestampUnit(1.0 / 8000.0);
	if (session.Create(params,0) < 0)
		return 1;
	if (session.SetDefaultPayloadType(96) < 0 ||
	    session.SetDefaultMark(false) < 0 ||
	    session.SetDefaultTimestampIncrement(160) < 0)
		return 2;

	clockValue = 1000000;
	RTPRepairConfig config = TestConfig(16,8192,session.GetNextTimestamp(),clockValue);
	if (session.ConfigureRepair(config,TestSessionClock,&clockValue) < 0)
		return 3;
	return 0;
}

static int test_session_all_send_entries(void)
{
	TestSession session;
	uint64_t clockValue;
	TASK_CHECK(StartTestSession(session,clockValue) == 0);
	TASK_CHECK(session.fake != 0);

	Bytes payload{1,2,3};
	Bytes extension(4,0x33);

	TASK_CHECK(session.SendPacket(payload.data(),payload.size(),96,false,160) == 0);
	session.fake->rtpResult = -51;
	TASK_CHECK(session.SendPacket(payload.data(),payload.size()) < 0);
	session.fake->rtpResult = 0;
	TASK_CHECK(session.SendPacketEx(payload.data(),payload.size(),0xbeef,
	                                extension.data(),1) == 0);
	session.fake->rtpResult = -52;
	TASK_CHECK(session.SendPacketEx(payload.data(),payload.size(),96,true,160,
	                                0x1234,extension.data(),1) < 0);
	session.fake->rtpResult = 0;
	TASK_CHECK(session.fake->calls.size() == 4);

	uint16_t seq0 = static_cast<uint16_t>((session.fake->calls[0][2] << 8) |
	                                      session.fake->calls[0][3]);
	uint32_t ssrc = session.GetLocalSSRC();
	uint32_t timestampBeforeFeedback = session.GetNextTimestamp();

	// Request all four consecutive sequence positions; only sends 0 and 2 succeeded.
	Bytes feedback = Compound(MakeRR(),MakeNACK(ssrc,seq0,0x000f));
	session.fake->EnqueueRTCP(feedback);
	clockValue = 1050000;
	TASK_CHECK(session.Poll() == 0);
	TASK_CHECK(session.fake->calls.size() == 6);
	TASK_CHECK(session.fake->calls[4] == session.fake->calls[0]);
	TASK_CHECK(session.fake->calls[5] == session.fake->calls[2]);
	TASK_CHECK(session.GetNextTimestamp() == timestampBeforeFeedback);
	return 0;
}

static int test_session_feedback_failure_retry_and_destroy(void)
{
	TestSession session;
	uint64_t clockValue;
	TASK_CHECK(StartTestSession(session,clockValue) == 0);

	Bytes payload{9,8,7};
	TASK_CHECK(session.SendPacket(payload.data(),payload.size(),96,false,160) == 0);
	Bytes original = session.fake->calls.back();
	uint16_t seq = static_cast<uint16_t>((original[2] << 8) | original[3]);
	uint32_t ssrc = session.GetLocalSSRC();

	Bytes feedback = Compound(MakeRR(),MakeNACK(ssrc,seq,0));
	session.fake->EnqueueRTCP(feedback);
	session.fake->rtpResult = -63;
	clockValue = 1050000;
	TASK_CHECK(session.Poll() == -63);
	TASK_CHECK(session.fake->calls.size() == 2);

	// The failed write has no cooldown; redelivered feedback repairs immediately.
	session.fake->rtpResult = 0;
	session.fake->EnqueueRTCP(feedback);
	TASK_CHECK(session.Poll() == 0);
	TASK_CHECK(session.fake->calls.size() == 3);
	TASK_CHECK(session.fake->calls[2] == original);

	RTPRepairConfig lateConfig = TestConfig();
	TASK_CHECK(session.ConfigureRepair(lateConfig,TestSessionClock,&clockValue) < 0);

	session.Destroy();
	RTPRepairConfig config = TestConfig();
	TASK_CHECK(session.ConfigureRepair(config,TestSessionClock,&clockValue) < 0);
	return 0;
}

const struct task_case task_cases[] = {
	{"history","configuration",test_history_configuration},
	{"history","quota-expiry-ssrc",test_history_quota_expiry_and_ssrc},
	{"history","bytes-order-extension-csrc",test_history_bytes_order_extensions},
	{"history","wrap-duplicates-retry",test_history_wrap_duplicates_and_retry},
	{"history","deadline-rtt-failure",test_history_deadline_rtt_and_failures},
	{"feedback","expansion-validation",test_feedback_expansion_validation},
	{"session","builder-generation",test_builder_history_generation},
	{"session","all-send-entries",test_session_all_send_entries},
	{"session","feedback-failure-destroy",test_session_feedback_failure_retry_and_destroy},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
