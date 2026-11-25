#ifndef RTPRETRANSMIT_H
#define RTPRETRANSMIT_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

struct RTPRepairConfig {
	size_t maxPackets, maxBytes;
	uint32_t clockRate, baseTimestamp;
	uint64_t baseTimeUS, delayUS, rttUS, retryUS;
};

typedef int (*RTPRepairWrite)(void *ctx, const void *data, size_t len);
typedef uint64_t (*RTPRepairClock)(void *ctx);

class RTCPCompoundPacket;

class RTPRepairHistory {
public:
	RTPRepairHistory();
	~RTPRepairHistory();

	int Configure(const RTPRepairConfig &config);
	void Clear();

	// Drop stored packets/SSRC generation but keep the active configuration.
	void ResetGeneration();

	int RememberSent(const void *data, size_t len, uint64_t nowUS);
	int Retransmit(RTCPCompoundPacket &feedback, uint64_t nowUS,
	               RTPRepairWrite writer, void *writerctx);

	size_t PacketCount() const;
	size_t ByteCount() const;

private:
	struct Entry {
		uint16_t sequence;
		uint32_t timestamp;
		int64_t deadlineUS;
		std::vector<unsigned char> bytes;
		bool hasLastSuccess;
		uint64_t lastSuccessUS;
	};

	bool configured;
	RTPRepairConfig config;

	bool haveSSRC;
	uint32_t ssrc;
	bool haveFrontier;
	uint16_t frontierSeq;
	size_t storedBytes;
	std::deque<Entry> entries;

	void ClearEntries();
	void RemoveExpired(uint64_t nowUS);
	int64_t PacketDeadline(uint32_t timestamp) const;
	bool DeadlineReached(int64_t deadlineUS, uint64_t nowUS) const;
	static int64_t FloorDiv(int64_t value, int64_t divisor);
	static int16_t ForwardDelta(uint16_t sequence, uint16_t frontier);
};

#endif // RTPRETRANSMIT_H
