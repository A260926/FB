#include "rtpretransmit.h"

#include "rtcpcompoundpacket.h"
#include "rtppacket.h"
#include "rtprawpacket.h"
#include "rtptimeutilities.h"

#include <new>
#include <cstring>

RTPRepairHistory::RTPRepairHistory()
	: configured(false), haveSSRC(false), ssrc(0), haveFrontier(false),
	  frontierSeq(0), storedBytes(0)
{
	std::memset(&config, 0, sizeof(config));
}

RTPRepairHistory::~RTPRepairHistory()
{
	ClearEntries();
}

void RTPRepairHistory::ClearEntries()
{
	entries.clear();
	storedBytes = 0;
	haveSSRC = false;
	haveFrontier = false;
	ssrc = 0;
	frontierSeq = 0;
}

int RTPRepairHistory::Configure(const RTPRepairConfig &newconfig)
{
	if (newconfig.maxPackets < 1 || newconfig.maxPackets > 1024)
		return -1;
	if (newconfig.maxBytes < 12)
		return -1;
	if (newconfig.clockRate == 0 || newconfig.retryUS == 0)
		return -1;

	config = newconfig;
	configured = true;

	// A new configuration generation never inherits packets or SSRC state.
	ClearEntries();
	return 0;
}

void RTPRepairHistory::Clear()
{
	configured = false;
	std::memset(&config, 0, sizeof(config));
	ClearEntries();
}

void RTPRepairHistory::ResetGeneration()
{
	ClearEntries();
}

size_t RTPRepairHistory::PacketCount() const
{
	return entries.size();
}

size_t RTPRepairHistory::ByteCount() const
{
	return storedBytes;
}

int64_t RTPRepairHistory::FloorDiv(int64_t value, int64_t divisor)
{
	int64_t quotient = value / divisor;
	int64_t remainder = value % divisor;

	// C++ division truncates toward zero; the contract specifies floor().
	if (remainder < 0)
		--quotient;
	return quotient;
}

int16_t RTPRepairHistory::ForwardDelta(uint16_t sequence, uint16_t frontier)
{
	uint32_t raw = static_cast<uint32_t>(sequence) - static_cast<uint32_t>(frontier);

	if (raw >= 32768U)
		return static_cast<int16_t>(static_cast<int32_t>(raw) - 65536);
	return static_cast<int16_t>(raw);
}

int64_t RTPRepairHistory::PacketDeadline(uint32_t timestamp) const
{
	uint32_t wrappedOffset = timestamp - config.baseTimestamp;
	int64_t sampleOffset;

	if (wrappedOffset >= 0x80000000U)
		sampleOffset = static_cast<int64_t>(wrappedOffset) - 0x100000000LL;
	else
		sampleOffset = static_cast<int64_t>(wrappedOffset);

	int64_t mediaTimeUS = FloorDiv(sampleOffset * 1000000LL,
	                               static_cast<int64_t>(config.clockRate));

	return static_cast<int64_t>(config.baseTimeUS) +
	       static_cast<int64_t>(config.delayUS) + mediaTimeUS;
}

bool RTPRepairHistory::DeadlineReached(int64_t deadlineUS, uint64_t nowUS) const
{
	if (deadlineUS <= 0)
		return true;
	return nowUS >= static_cast<uint64_t>(deadlineUS);
}

void RTPRepairHistory::RemoveExpired(uint64_t nowUS)
{
	size_t newBytes = 0;
	std::deque<Entry>::iterator it = entries.begin();

	while (it != entries.end())
	{
		if (DeadlineReached(it->deadlineUS, nowUS))
			it = entries.erase(it);
		else
		{
			newBytes += it->bytes.size();
			++it;
		}
	}
	storedBytes = newBytes;
}

int RTPRepairHistory::RememberSent(const void *data, size_t len, uint64_t nowUS)
{
	if (!configured)
		return -1;
	if (data == 0 || len == 0)
		return -1;

	// Let the production RTP parser decide whether this is a legal packet.
	u_int8_t *rawbytes = new (std::nothrow) u_int8_t[len];
	if (rawbytes == 0)
		return -1;
	std::memcpy(rawbytes, data, len);

	RTPTime receivetime(0, 0);
	RTPRawPacket rawpacket(rawbytes, len, 0, receivetime, true);
	RTPPacket rtppacket(rawpacket);

	if (rtppacket.GetCreationError() < 0)
		return -1;

	const uint16_t sequence = rtppacket.GetSequenceNumber();
	const uint32_t timestamp = rtppacket.GetTimestamp();
	const uint32_t packetssrc = rtppacket.GetSSRC();

	if (haveSSRC && ssrc != packetssrc)
		ClearEntries();

	if (!haveSSRC)
	{
		haveSSRC = true;
		ssrc = packetssrc;
	}

	if (haveFrontier && ForwardDelta(sequence, frontierSeq) <= 0)
		return -1;

	haveFrontier = true;
	frontierSeq = sequence;

	// Expiration is ordinary maintenance for every valid forward submission.
	RemoveExpired(nowUS);

	uint32_t wrappedOffset = timestamp - config.baseTimestamp;
	bool negativeMediaOffset = (wrappedOffset >= 0x80000000U);
	int64_t deadlineUS = PacketDeadline(timestamp);

	if (negativeMediaOffset || DeadlineReached(deadlineUS, nowUS) ||
	    len > config.maxBytes)
	{
		return 1;
	}

	// First remove older generations by expiration (done above), then apply
	// packet and byte quotas in earliest-successful-send order.
	while (!entries.empty() &&
	       (entries.size() + 1 > config.maxPackets ||
	        storedBytes + len > config.maxBytes))
	{
		storedBytes -= entries.front().bytes.size();
		entries.pop_front();
	}

	Entry entry;
	entry.sequence = sequence;
	entry.timestamp = timestamp;
	entry.deadlineUS = deadlineUS;
	entry.bytes.assign(static_cast<const unsigned char *>(data),
	                   static_cast<const unsigned char *>(data) + len);
	entry.hasLastSuccess = false;
	entry.lastSuccessUS = 0;

	entries.push_back(entry);
	storedBytes += len;
	return 0;
}

int RTPRepairHistory::Retransmit(RTCPCompoundPacket &feedback, uint64_t nowUS,
                                 RTPRepairWrite writer, void *writerctx)
{
	if (!configured)
		return -1;

	// Validate and expand the complete compound before changing any state.
	std::vector<uint16_t> requested;
	uint32_t mediaSSRC = haveSSRC ? ssrc : 0;
	int status = feedback.GetGenericNACKs(mediaSSRC, requested);
	if (status < 0)
		return status;

	RemoveExpired(nowUS);

	if (writer == 0)
		return -1;

	std::vector<unsigned char> wanted(65536, 0);
	for (size_t i = 0; i < requested.size(); ++i)
		wanted[requested[i]] = 1;

	int retransmitted = 0;

	// Iterating storage gives earliest successful original send order.
	for (std::deque<Entry>::iterator it = entries.begin();
	     it != entries.end(); ++it)
	{
		if (!wanted[it->sequence])
			continue;

		// now + rttUS < deadlineUS, expressed without overflow.
		if (it->deadlineUS <= 0)
			continue;
		uint64_t deadlineUS = static_cast<uint64_t>(it->deadlineUS);
		if (nowUS >= deadlineUS || config.rttUS >= deadlineUS - nowUS)
			continue;

		if (it->hasLastSuccess)
		{
			if (nowUS < it->lastSuccessUS)
				continue;
			if (nowUS - it->lastSuccessUS < config.retryUS)
				continue;
		}

		int writestatus = writer(writerctx, &it->bytes[0], it->bytes.size());
		if (writestatus < 0)
			return writestatus;

		if (writestatus == 0)
		{
			it->hasLastSuccess = true;
			it->lastSuccessUS = nowUS;
			++retransmitted;
		}
	}

	return retransmitted;
}
