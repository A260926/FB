#include "RtpQueue.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <string.h>
#include <vector>
using namespace std;

#ifndef IGNORE_PACKET
extern void packet_free(void* buf, uint32_t ssrc);
#endif
/*
* Implements a simple RTP packet queue
*/

RtpQueueItem::RtpQueueItem() {
	used = false;
	packet = NULL;
	size = 0;
	seqNr = 0;
	timeStamp = 0;
	frameId = 0;
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
	frameDeadlineSeconds = 0.0f;
	frameDeadlineEnabled = false;
	frameDeadlineRefreshPending = false;
	nextFrameId = 1;
	currentFrameId = 0;
	rejectedFrameId = 0;
}

bool RtpQueue::push(void* rtpPacket, int size, uint32_t ssrc, unsigned short seqNr, bool isMark, float ts, uint32_t timeStamp) {
	std::unique_lock<std::mutex> lock(queue_operation_mutex_);
	uint64_t frameId = 0;

	if (frameDeadlineEnabled) {
		/*
		* A push timestamp is also a monotonic observation time. This makes
		* a late suffix fail even when the scheduler has not run between the
		* deadline and the suffix push.
		*/
		expireFrameDeadlinesLocked(ts);

		if (rejectedFrameId != 0) {
			RtpFrameDeadlineState* rejected = findFrameStateLocked(rejectedFrameId);
			if (rejected != NULL && timeStamp == rejected->timeStamp) {
				/*
				* The marker itself is still part of the expired frame and is
				* not queued. It only ends the rejection interval.
				*/
				if (isMark) {
					rejected->ended = true;
					rejectedFrameId = 0;
					currentFrameId = 0;
					pruneFrameStatesLocked();
				}
				return false;
			}
			/* A changed RTP timestamp is the next frame boundary. */
			if (rejected != NULL)
				rejected->ended = true;
			rejectedFrameId = 0;
			pruneFrameStatesLocked();
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

	if (frameDeadlineEnabled) {
		RtpFrameDeadlineState* current = findFrameStateLocked(currentFrameId);
		if (current != NULL && !current->ended && current->timeStamp == timeStamp) {
			frameId = current->id;
		}
		else {
			/* A changed timestamp ends the preceding frame. */
			if (current != NULL)
				current->ended = true;
			pruneFrameStatesLocked();

			frameId = nextFrameId++;
			RtpFrameDeadlineState frame;
			frame.id = frameId;
			frame.timeStamp = timeStamp;
			frame.firstPushTs = ts;
			frame.queuedPackets = 0;
			frame.ended = isMark;
			frame.expired = false;
			frameStates.push_back(frame);
			currentFrameId = isMark ? 0 : frameId;
		}
	}

	head = ix;
	items[head]->seqNr = seqNr;
	items[head]->timeStamp = timeStamp;
	items[head]->ssrc = ssrc;
	items[head]->size = size;
	items[head]->ts = ts;
	items[head]->isMark = isMark;
	items[head]->used = true;
	items[head]->frameId = frameId;
	bytesInQueue_ += size;
	sizeOfQueue_ += 1;
#ifndef IGNORE_PACKET
	items[head]->packet = rtpPacket;
#endif
	if (frameDeadlineEnabled) {
		RtpFrameDeadlineState* frame = findFrameStateLocked(frameId);
		if (frame != NULL) {
			frame->queuedPackets++;
			if (isMark) {
				frame->ended = true;
				currentFrameId = 0;
			}
		}
	}
	computeSizeOfNextRtp();
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
		uint64_t frameId = items[tail]->frameId;
		items[tail]->used = false;
		items[tail]->frameId = 0;
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
		if (frameDeadlineEnabled && frameId != 0) {
			RtpFrameDeadlineState* frame = findFrameStateLocked(frameId);
			if (frame != NULL) {
				frame->queuedPackets--;
				if (frame->queuedPackets < 0)
					frame->queuedPackets = 0;
			}
			pruneFrameStatesLocked();
		}
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
	/*
	* Deliberately retain an open frame's deadline state: clear() frees
	* queued packets, but it is not a marker or timestamp boundary and the
	* specification requires the frame age to survive prefix removal.
	*/
	return (freed);
}

bool RtpQueue::setFrameDeadline(float maxAgeSeconds) {
	std::lock_guard<std::mutex> lock(queue_operation_mutex_);
	if (!std::isfinite(maxAgeSeconds) || maxAgeSeconds < 0.0f)
		return false;

	if (maxAgeSeconds == 0.0f) {
		frameDeadlineEnabled = false;
		frameDeadlineSeconds = 0.0f;
		frameDeadlineRefreshPending = false;
		frameStates.clear();
		currentFrameId = 0;
		rejectedFrameId = 0;
		return true;
	}

	frameDeadlineEnabled = true;
	frameDeadlineSeconds = maxAgeSeconds;
	return true;
}

bool RtpQueue::checkFrameDeadline(float currTs) {
	std::lock_guard<std::mutex> lock(queue_operation_mutex_);
	if (!frameDeadlineEnabled)
		return false;
	expireFrameDeadlinesLocked(currTs);
	bool refreshPending = frameDeadlineRefreshPending;
	frameDeadlineRefreshPending = false;
	return refreshPending;
}

RtpFrameDeadlineState* RtpQueue::findFrameStateLocked(uint64_t frameId) {
	if (frameId == 0)
		return NULL;
	for (size_t n = 0; n < frameStates.size(); n++) {
		if (frameStates[n].id == frameId)
			return &frameStates[n];
	}
	return NULL;
}

void RtpQueue::pruneFrameStatesLocked() {
	frameStates.erase(
		std::remove_if(frameStates.begin(), frameStates.end(),
			[](const RtpFrameDeadlineState& frame) {
				return frame.ended && frame.queuedPackets == 0;
			}),
		frameStates.end());
	if (currentFrameId != 0 && findFrameStateLocked(currentFrameId) == NULL)
		currentFrameId = 0;
	if (rejectedFrameId != 0 && findFrameStateLocked(rejectedFrameId) == NULL)
		rejectedFrameId = 0;
}

void RtpQueue::expireFrameDeadlinesLocked(float currTs) {
	if (!frameDeadlineEnabled || !(frameDeadlineSeconds > 0.0f))
		return;

	/* Frames that ended and already have no queued packets need no deadline. */
	pruneFrameStatesLocked();

	bool newExpiration = false;
	for (size_t n = 0; n < frameStates.size(); n++) {
		RtpFrameDeadlineState& frame = frameStates[n];
		if (frame.expired)
			continue;
		if (currTs - frame.firstPushTs >= frameDeadlineSeconds) {
			frame.expired = true;
			newExpiration = true;
			evictFramePacketsLocked(frame.id);
			if (!frame.ended)
				rejectedFrameId = frame.id;
		}
	}
	if (newExpiration)
		frameDeadlineRefreshPending = true;
	pruneFrameStatesLocked();
}

void RtpQueue::evictFramePacketsLocked(uint64_t frameId) {
	if (frameId == 0 || sizeOfQueue_ == 0)
		return;

	const int oldSize = sizeOfQueue_;
	std::vector<RtpQueueItem> survivors;
	survivors.reserve(oldSize);

	int ix = tail;
	for (int n = 0; n < oldSize; n++) {
		RtpQueueItem* item = items[ix];
		if (item->used) {
			if (item->frameId == frameId) {
				void* packet = item->packet;
				uint32_t ssrc = item->ssrc;
				item->used = false;
				item->packet = NULL;
#ifndef IGNORE_PACKET
				if (packet != NULL)
					packet_free(packet, ssrc);
#endif
			}
			else {
				survivors.push_back(*item);
			}
		}
		ix++;
		if (ix == kRtpQueueSize)
			ix = 0;
	}

	/*
	* Rebuild the fixed ring in linear order. Copying the trivial item
	* values preserves packet ownership for survivors; all original item
	* packet pointers are detached first so a stale slot can never free a
	* survivor packet a second time.
	*/
	for (int n = 0; n < kRtpQueueSize; n++) {
		items[n]->used = false;
		items[n]->packet = NULL;
		items[n]->frameId = 0;
	}

	bytesInQueue_ = 0;
	for (size_t n = 0; n < survivors.size(); n++) {
		*items[n] = survivors[n];
		items[n]->used = true;
		bytesInQueue_ += items[n]->size;
	}

	tail = 0;
	sizeOfQueue_ = static_cast<int>(survivors.size());
	nItems = sizeOfQueue_;
	head = sizeOfQueue_ > 0 ? sizeOfQueue_ - 1 : -1;

	RtpFrameDeadlineState* frame = findFrameStateLocked(frameId);
	if (frame != NULL)
		frame->queuedPackets = 0;
	computeSizeOfNextRtp();
}
