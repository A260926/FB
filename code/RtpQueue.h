#ifndef RTP_QUEUE
#define RTP_QUEUE

#include <cstdint>
#include <cmath>
#include <mutex>
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
	* Optional per-frame deadline capability.
	* A positive finite maxAgeSeconds enables frame deadline handling,
	* 0.0 disables it. The default implementation does not support the
	* capability and always returns false so that external implementations
	* of this interface keep compiling and keep their existing behavior.
	*/
	virtual bool setFrameDeadline(float /*maxAgeSeconds*/) { return false; }
	/*
	* Returns true if the queue supports per-frame deadline handling.
	*/
	virtual bool frameDeadlineSupported() { return false; }
	/*
	* Evict all packets of frames that have reached their deadline at
	* the given time. Returns the number of packets freed. Queues that
	* do not support frame deadlines return 0.
	*/
	virtual int expireFrameDeadlines(float /*currTs*/) { return 0; }
	/*
	* Returns true once per frame that reaches its deadline, including
	* frames whose prefix has already been transmitted and whose queue is
	* momentarily empty. The event is consumed and not repeated for the
	* same frame. Queues that do not support frame deadlines return false.
	*/
	virtual bool consumeFrameDeadlineEvent() { return false; }
};

class RtpQueueItem {
public:
	RtpQueueItem();
	void* packet;
	int size;
	uint32_t ssrc;
	unsigned short seqNr;
	unsigned long timeStamp;
	float ts;
	float frameStartTs; // Push time of the first packet of the frame this item belongs to
	bool isMark;
	bool used;
};

const int kRtpQueueSize = 1024;
class RtpQueue : public RtpQueueIface {
public:
	RtpQueue();

	bool push(void* rtpPacket, int size, uint32_t ssrc, unsigned short seqNr, bool isMark, float ts, uint32_t timeStamp);
	bool pop(void** rtpPacket, int& size, uint32_t& ssrc, unsigned short& seqNr, bool& isMark, uint32_t& timeStamp);
	int sizeOfNextRtp();
	int seqNrOfNextRtp();
	int seqNrOfLastRtp();
	int bytesInQueue(); // Number of bytes in queue
	int sizeOfQueue();  // Number of items in queue
	float getDelay(float currTs);
	bool sendPacket(void** rtpPacket, int& size, uint32_t& ssrc, unsigned short& seqNr, bool& isMark, uint32_t& timeStamp);
	int clear();
	int getSizeOfLastFrame() { return sizeOfLastFrame; };
	void setSizeOfLastFrame(int sz) { sizeOfLastFrame = sz; };
	void computeSizeOfNextRtp();

	/*
	* Enable/disable per-frame deadline. A positive finite value enables
	* frame deadline handling, 0.0 disables it. Returns false for negative
	* or non-finite values.
	*/
	bool setFrameDeadline(float maxAgeSeconds);
	bool frameDeadlineSupported() { return true; }
	/*
	* Evict all not yet transmitted packets of frames whose age exceeds
	* the configured deadline. Returns the number of packets freed.
	*/
	int expireFrameDeadlines(float currTs);
	/*
	* Consume a pending frame deadline event. Returns true at most once
	* per expired frame.
	*/
	bool consumeFrameDeadlineEvent();

	RtpQueueItem* items[kRtpQueueSize];
	int head; // Pointer to last inserted item
	int tail; // Pointer to the oldest item
	int nItems;
	int sizeOfLastFrame;

	int bytesInQueue_;
	int sizeOfQueue_;
	int sizeOfNextRtp_;
	std::mutex queue_operation_mutex_;

private:
	/*
	* Frame deadline state. A queue holds packets of one stream only and
	* packets are enqueued in encoding order. The age of the frame that is
	* currently being received is taken from the push time of its first
	* packet. The age is retained even if all packets of the frame have
	* been popped and the queue is momentarily empty, until a marker or a
	* new RTP timestamp ends the frame.
	*/
	bool frameDeadlineEnabled_;
	float frameDeadlineMaxAge_;
	bool frameActive_;        // A frame has been started and not yet ended
	uint32_t frameTimeStamp_; // RTP timestamp of the frame being received
	float frameStartTs_;      // Push time of the frame's first packet
	bool frameRejected_;      // Frame went past deadline, suffix packets are refused
	/*
	* One-shot event bookkeeping. frameDeadlineEventCount_ counts frames
	* that have reached their deadline and whose event has not been
	* consumed yet. The signaled identifiers make sure the same frame
	* signals only once, even across repeated scheduler calls.
	*/
	int frameDeadlineEventCount_;
	bool signaledValid_;
	uint32_t signaledFrameTimeStamp_;
	float signaledFrameStartTs_;
	/*
	* Free one queued item and update the queue counters.
	*/
	void freeItem(int ix);
	/*
	* Raise the frame deadline event at most once per frame, identified by
	* its RTP timestamp and first-packet push time.
	*/
	void raiseFrameDeadlineEvent(uint32_t timeStamp, float frameStartTs);
	/*
	* End the frame being received if the pushed packet belongs to a new
	* frame or carries the marker bit.
	*/
	void updateFrameBoundary(bool isMark, uint32_t timeStamp, float ts);
};

#endif
