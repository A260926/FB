#ifndef RTP_QUEUE
#define RTP_QUEUE

#include <cstdint>
#include <mutex>
#include <vector>
/*
* Implements a simple RTP packet queue, one RTP queue
* per stream {SSRC,PT}
*/

class RtpQueueIface {
public:
	virtual ~RtpQueueIface() {}
	virtual int clear() = 0;
	virtual int sizeOfNextRtp() = 0;
	virtual int seqNrOfNextRtp() = 0;
	virtual int seqNrOfLastRtp() = 0;
	virtual int bytesInQueue() = 0; // Number of bytes in queue
	virtual int sizeOfQueue() = 0;  // Number of items in queue
	virtual float getDelay(float currTs) = 0;
	virtual int getSizeOfLastFrame() = 0;

	/*
	* Optional per-frame maximum age. The default implementation reports
	* that frame deadlines are not supported by an external queue.
	*/
	virtual bool setFrameDeadline(float maxAgeSeconds) {
		(void)maxAgeSeconds;
		return false;
	}

	/*
	* Evict frames whose deadline has passed. Returns true once for a new
	* expiration that should trigger a video refresh. The default queue
	* implementation has no deadline support.
	*/
	virtual bool checkFrameDeadline(float currTs) {
		(void)currTs;
		return false;
	}
};

struct RtpQueueItem {
public:
	RtpQueueItem();
	void* packet;
	int size;
	uint32_t ssrc;
	unsigned short seqNr;
	unsigned long timeStamp;
	float ts;
	bool isMark;
	bool used;
	uint64_t frameId;
};

struct RtpFrameDeadlineState {
	uint64_t id;
	uint32_t timeStamp;
	float firstPushTs;
	int queuedPackets;
	bool ended;   // Marker or a newer timestamp ended the frame
	bool expired; // Deadline has already been reached
};

const int kRtpQueueSize = 1024;
class RtpQueue : public RtpQueueIface {
public:
	RtpQueue();

	bool push(void* rtpPacket, int size, uint32_t ssrc, unsigned short seqNr, bool isMark, float ts, uint32_t timeStamp);
	bool pop(void** rtpPacket, int& size, uint32_t& ssrc, unsigned short& seqNr, bool& isMark, uint32_t& timeStamp);
	int sizeOfNextRtp() override;
	int seqNrOfNextRtp() override;
	int seqNrOfLastRtp() override;
	int bytesInQueue() override; // Number of bytes in queue
	int sizeOfQueue() override;  // Number of items in queue
	float getDelay(float currTs) override;
	bool sendPacket(void** rtpPacket, int& size, uint32_t& ssrc, unsigned short& seqNr, bool& isMark, uint32_t& timeStamp);
	int clear() override;
	bool setFrameDeadline(float maxAgeSeconds) override;
	bool checkFrameDeadline(float currTs) override;
	int getSizeOfLastFrame() override { return sizeOfLastFrame; };
	void setSizeOfLastFrame(int sz) { sizeOfLastFrame = sz; };
	void computeSizeOfNextRtp();

	void expireFrameDeadlinesLocked(float currTs);
	void evictFramePacketsLocked(uint64_t frameId);
	void pruneFrameStatesLocked();
	RtpFrameDeadlineState* findFrameStateLocked(uint64_t frameId);

	RtpQueueItem* items[kRtpQueueSize];
	int head; // Pointer to last inserted item
	int tail; // Pointer to the oldest item
	int nItems;
	int sizeOfLastFrame;

	int bytesInQueue_;
	int sizeOfQueue_;
	int sizeOfNextRtp_;
	float frameDeadlineSeconds;
	bool frameDeadlineEnabled;
	bool frameDeadlineRefreshPending;
	uint64_t nextFrameId;
	uint64_t currentFrameId;
	uint64_t rejectedFrameId;
	std::vector<RtpFrameDeadlineState> frameStates;
	std::mutex queue_operation_mutex_;
};

#endif
