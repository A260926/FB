#ifndef _rtmp_chunk_scheduler_h_
#define _rtmp_chunk_scheduler_h_

#include "rtmp-internal.h"

/// chunk interleave scheduler (single thread only)
/// messages are queued per chunk stream and emitted one full chunk per step;
/// CSID 2 protocol control messages have priority, media chunk streams rotate
struct rtmp_chunk_scheduler_t;

struct rtmp_chunk_scheduler_t* rtmp_chunk_scheduler_create(struct rtmp_t* rtmp);

/// release the scheduler and every message still queued
void rtmp_chunk_scheduler_destroy(struct rtmp_chunk_scheduler_t* scheduler);

/// enqueue a message, the payload is copied and retained until fully sent
/// @return 0-ok, other-error
int rtmp_chunk_scheduler_submit(struct rtmp_chunk_scheduler_t* scheduler, const struct rtmp_chunk_header_t* header, const uint8_t* payload);

/// enqueue a Set Chunk Size protocol control message; the new size takes
/// effect only after this control message has been fully emitted
/// @return 0-ok, other-error
int rtmp_chunk_scheduler_submit_chunk_size(struct rtmp_chunk_scheduler_t* scheduler, uint32_t size);

/// emit at most one complete chunk through the rtmp send callback
/// @return 1-one chunk sent, 0-nothing queued, <0-send error (output stopped)
int rtmp_chunk_scheduler_step(struct rtmp_chunk_scheduler_t* scheduler);

#endif /* !_rtmp_chunk_scheduler_h_ */
