#include "RtpQueue.h"
#include <iostream>
#include <string.h>
using namespace std;
/*
* Implements a simple RTP packet queue
*/

RtpQueueItem::RtpQueueItem() {
	used = false;
	size = 0;
	seqNr = 0;
	timeStamp = 0;
	frameStartTs = 0.0f;
}


RtpQueue::RtpQueue() {
	for (int n = 0; n < kRtpQueueSize; n++) {
		items[n] = new RtpQueueItem();
	}
	head = -1;
	tail = 0;
	nItems = 0;
	sizeOfLastFrame = 0;
	bytesInQueue_ = 0;
	sizeOfQueue_ = 0;
	sizeOfNextRtp_ = -1;
	frameDeadlineEnabled_ = false;
	frameDeadlineMaxAge_ = 0.0f;
	frameActive_ = false;
	frameRejected_ = false;
	frameTimeStamp_ = 0;
	frameStartTs_ = 0.0f;
	frameDeadlineEventCount_ = 0;
	signaledValid_ = false;
	signaledFrameTimeStamp_ = 0;
	signaledFrameStartTs_ = 0.0f;
}

#ifndef IGNORE_PACKET
extern void packet_free(void* buf, uint32_t ssrc);
#endif

void RtpQueue::freeItem(int ix) {
	RtpQueueItem* item = items[ix];
	if (!item->used) {
		return;
	}
#ifndef IGNORE_PACKET
	if (item->packet != NULL) {
		packet_free(item->packet, item->ssrc);
	}
#endif
	item->packet = NULL;
	item->used = false;
	bytesInQueue_ -= item->size;
	sizeOfQueue_ -= 1;
}

void RtpQueue::raiseFrameDeadlineEvent(uint32_t timeStamp, float frameStartTs) {
	/*
	* Signal at most once for a given frame, identified by its RTP
	* timestamp and the push time of its first packet.
	*/
	if (signaledValid_ &&
		signaledFrameTimeStamp_ == timeStamp &&
		signaledFrameStartTs_ == frameStartTs) {
		return;
	}
	signaledValid_ = true;
	signaledFrameTimeStamp_ = timeStamp;
	signaledFrameStartTs_ = frameStartTs;
	frameDeadlineEventCount_++;
}

void RtpQueue::updateFrameBoundary(bool isMark, uint32_t timeStamp, float ts) {
	/*
	* A changed RTP timestamp ends the previous frame. Only equality is
	* compared, which makes the check safe across the 32 bit wrap-around.
	*/
	if (frameActive_ && timeStamp != frameTimeStamp_) {
		frameActive_ = false;
		frameRejected_ = false;
	}
	if (!frameActive_) {
		/*
		* First packet of a new frame, its age starts at this push time.
		*/
		frameActive_ = true;
		frameTimeStamp_ = timeStamp;
		frameStartTs_ = ts;
		frameRejected_ = false;
	}
}

bool RtpQueue::setFrameDeadline(float maxAgeSeconds) {
	if (!std::isfinite(maxAgeSeconds) || maxAgeSeconds < 0.0f) {
		return false;
	}
	std::unique_lock<std::mutex> lock(queue_operation_mutex_);
	frameDeadlineMaxAge_ = maxAgeSeconds;
	frameDeadlineEnabled_ = maxAgeSeconds > 0.0f;
	frameDeadlineEventCount_ = 0;
	signaledValid_ = false;
	if (!frameDeadlineEnabled_) {
		/*
		* Disabling returns the queue to the whole-queue discard strategy.
		*/
		frameActive_ = false;
		frameRejected_ = false;
	}
	return true;
}

bool RtpQueue::push(void* rtpPacket, int size, uint32_t ssrc, unsigned short seqNr, bool isMark, float ts, uint32_t timeStamp) {
	std::unique_lock<std::mutex> lock(queue_operation_mutex_);

	if (frameDeadlineEnabled_) {
		updateFrameBoundary(isMark, timeStamp, ts);
		if (frameRejected_) {
			/*
			* The frame this packet belongs to has passed its deadline, the
			* packet is refused and ownership stays with the caller. The
			* rejection state ends at the marker or at the next frame boundary.
			*/
			if (isMark) {
				frameActive_ = false;
				frameRejected_ = false;
			}
			return (false);
		}
	}

	int ix = head + 1;
	if (ix == kRtpQueueSize) ix = 0;
	if (items[ix]->used) {
		/*
		* RTP queue is full, do a drop tail i.e ignore new RTP packets
		*/
		return (false);
	}
	head = ix;
	items[head]->seqNr = seqNr;
	items[head]->timeStamp = timeStamp;
	items[head]->ssrc = ssrc;
	items[head]->size = size;
	items[head]->ts = ts;
	items[head]->isMark = isMark;
	items[head]->used = true;
	items[head]->frameStartTs = frameDeadlineEnabled_ ? frameStartTs_ : ts;
	bytesInQueue_ += size;
	sizeOfQueue_ += 1;
#ifndef IGNORE_PACKET
	items[head]->packet = rtpPacket;
#endif
	computeSizeOfNextRtp();
	if (frameDeadlineEnabled_ && isMark) {
		/*
		* Marker ends the frame.
		*/
		frameActive_ = false;
		frameRejected_ = false;
	}
	return (true);
}
bool RtpQueue::pop(void** rtpPacket, int& size, uint32_t& ssrc, unsigned short& seqNr, bool& isMark, uint32_t& timeStamp)
{
	std::unique_lock<std::mutex> lock(queue_operation_mutex_);
	if (items[tail]->used == false) {
		*rtpPacket = NULL;
		sizeOfNextRtp_ = -1;
		return false;
	}
	else {
		size = items[tail]->size;

#ifndef IGNORE_PACKET
	* rtpPacket = items[tail]->packet;
#endif
		seqNr = items[tail]->seqNr;
		timeStamp = items[tail]->timeStamp;
		ssrc = items[tail]->ssrc;
		isMark = items[tail]->isMark;
		items[tail]->used = false;
		/*
		* Thread safe update of tail to avoid that tail points outside
		*  array, which can cause e.g RtpQueue::getDelay to give a
		*  segmentation fault.
		* This should not really be needed because
		*  we use mutex to make the pop function atomic
		*/
		if (tail == kRtpQueueSize - 1)
			tail = 0;
		else
			tail++;
		bytesInQueue_ -= size;
		sizeOfQueue_ -= 1;
		computeSizeOfNextRtp();
		return true;
	}
}

void RtpQueue::computeSizeOfNextRtp() {
	if (!items[tail]->used) {
		sizeOfNextRtp_ = -1;
	}
	else {
		sizeOfNextRtp_ = items[tail]->size;
	}
}

int RtpQueue::sizeOfNextRtp() {
	return sizeOfNextRtp_;
}

int RtpQueue::seqNrOfNextRtp() {
	if (!items[tail]->used) {
		return -1;
	}
	else {
		return items[tail]->seqNr;
	}
}

int RtpQueue::seqNrOfLastRtp() {
	if (head < 0 || !items[head]->used) {
		return -1;
	}
	else {
		return items[head]->seqNr;
	}
}

int RtpQueue::bytesInQueue() {
	return bytesInQueue_;
}

int RtpQueue::sizeOfQueue() {
	return sizeOfQueue_;
}

float RtpQueue::getDelay(float currTs) {
	if (items[tail]->used == false) {
		return 0;
	}
	else {
		return currTs - items[tail]->ts;
	}
}

bool RtpQueue::sendPacket(void** rtpPacket, int& size, uint32_t& ssrc, unsigned short& seqNr, bool& isMark, uint32_t& timeStamp) {
	if (sizeOfQueue() > 0) {
		pop(rtpPacket, size, ssrc, seqNr, isMark, timeStamp);
		return true;
	}
	return false;
}

int RtpQueue::clear() {
	uint16_t seqNr;
	uint32_t timeStamp;
	uint32_t ssrc;
	int freed = 0;
	int size;
	void* buf;
	while (sizeOfQueue() > 0) {
		bool isMark;
		pop(&buf, size, ssrc, seqNr, isMark, timeStamp);
		if (buf != NULL) {
			freed++;
#ifndef IGNORE_PACKET
			packet_free(buf, ssrc);
#endif
		}
	}
	head = -1;
	tail = 0;
	sizeOfNextRtp_ = -1;
	return (freed);
}

bool RtpQueue::consumeFrameDeadlineEvent() {
	std::unique_lock<std::mutex> lock(queue_operation_mutex_);
	if (frameDeadlineEventCount_ == 0) {
		return false;
	}
	frameDeadlineEventCount_--;
	return true;
}

int RtpQueue::expireFrameDeadlines(float currTs) {
	std::unique_lock<std::mutex> lock(queue_operation_mutex_);
	if (!frameDeadlineEnabled_) {
		return 0;
	}

	int freed = 0;

	/*
	* Evict queued packets of expired frames. Packets are stored in
	* encoding order in the ring buffer, therefore the expired frames are
	* always the oldest ones and the walk can stop at the first fresh
	* packet. An event is raised once per distinct expired frame.
	*/
	int remaining = sizeOfQueue_;
	int ix = tail;
	bool haveFrame = false;
	uint32_t curFrameTimeStamp = 0;
	float curFrameStartTs = 0.0f;
	while (remaining > 0) {
		RtpQueueItem* item = items[ix];
		if (item->used && currTs - item->frameStartTs >= frameDeadlineMaxAge_) {
			uint32_t itemTimeStamp = (uint32_t)item->timeStamp;
			if (!haveFrame || itemTimeStamp != curFrameTimeStamp) {
				haveFrame = true;
				curFrameTimeStamp = itemTimeStamp;
				curFrameStartTs = item->frameStartTs;
				raiseFrameDeadlineEvent(curFrameTimeStamp, curFrameStartTs);
			}
			freeItem(ix);
			freed++;
			if (ix == kRtpQueueSize - 1)
				tail = 0;
			else
				tail = ix + 1;
		}
		else {
			break;
		}
		if (ix == kRtpQueueSize - 1)
			ix = 0;
		else
			ix++;
		remaining--;
	}

	/*
	* The prefix of the active frame may already have been popped, leaving
	* the queue momentarily empty. The frame age is retained and the
	* deadline is still enforced; this is what makes the stale suffix
	* visible even though the head-of-queue delay reads zero. The event
	* bookkeeping makes sure the frame signals only once.
	*/
	if (frameActive_ && sizeOfQueue_ == 0 &&
		currTs - frameStartTs_ >= frameDeadlineMaxAge_) {
		frameRejected_ = true;
		raiseFrameDeadlineEvent(frameTimeStamp_, frameStartTs_);
	}

	if (sizeOfQueue_ == 0) {
		head = -1;
		tail = 0;
	}
	computeSizeOfNextRtp();
	return freed;
}
