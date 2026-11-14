#include "rtmp-chunk-scheduler.h"
#include "rtmp-msgtypeid.h"
#include "rtmp-util.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>

struct rtmp_scheduled_msg_t
{
	struct rtmp_scheduled_msg_t* next;
	struct rtmp_chunk_header_t input; // header as submitted, zipped on first emit
	struct rtmp_chunk_header_t zipped;
	uint8_t* payload; // library owned copy
	size_t length;
	size_t offset; // payload bytes already emitted
	int started; // first chunk already emitted
	int is_chunk_size; // apply new_chunk_size once the whole message is out
	uint32_t new_chunk_size;
};

struct rtmp_chunk_queue_t
{
	uint32_t cid; // 0-unused slot
	struct rtmp_scheduled_msg_t* head;
	struct rtmp_scheduled_msg_t* tail;
};

struct rtmp_chunk_scheduler_t
{
	struct rtmp_t* rtmp;
	struct rtmp_chunk_queue_t queues[N_CHUNK_STREAM];
	uint32_t cursor; // last emitted media CSID, 0-start from the minimum
	int failed; // sticky send error, output stopped
	int error;
};

struct rtmp_chunk_scheduler_t* rtmp_chunk_scheduler_create(struct rtmp_t* rtmp)
{
	struct rtmp_chunk_scheduler_t* scheduler;
	scheduler = (struct rtmp_chunk_scheduler_t*)calloc(1, sizeof(*scheduler));
	if (!scheduler)
		return NULL;
	scheduler->rtmp = rtmp;
	return scheduler;
}

void rtmp_chunk_scheduler_destroy(struct rtmp_chunk_scheduler_t* scheduler)
{
	uint32_t i;
	struct rtmp_scheduled_msg_t* msg;
	struct rtmp_scheduled_msg_t* next;
	if (!scheduler)
		return;

	for (i = 0; i < N_CHUNK_STREAM; i++)
	{
		msg = scheduler->queues[i].head;
		while (msg)
		{
			next = msg->next;
			free(msg->payload);
			free(msg);
			msg = next;
		}
	}
	free(scheduler);
}

static struct rtmp_chunk_queue_t* rtmp_scheduler_queue_find(struct rtmp_chunk_scheduler_t* scheduler, uint32_t cid, int create)
{
	uint32_t i;
	struct rtmp_chunk_queue_t* empty = NULL;
	for (i = 0; i < N_CHUNK_STREAM; i++)
	{
		if (scheduler->queues[i].cid == cid)
			return scheduler->queues + i;
		if (create && 0 == scheduler->queues[i].cid && NULL == empty)
			empty = scheduler->queues + i;
	}
	if (empty)
		empty->cid = cid;
	return empty;
}

static int rtmp_scheduler_enqueue(struct rtmp_chunk_scheduler_t* scheduler, const struct rtmp_chunk_header_t* header, const uint8_t* payload, int is_chunk_size, uint32_t new_chunk_size)
{
	struct rtmp_chunk_queue_t* queue;
	struct rtmp_scheduled_msg_t* msg;

	if (scheduler->failed)
		return scheduler->error;
	if (header->length >= 0xFFFFFF)
		return -EINVAL;

	queue = rtmp_scheduler_queue_find(scheduler, header->cid, 1);
	if (!queue)
		return -ENOMEM;

	msg = (struct rtmp_scheduled_msg_t*)calloc(1, sizeof(*msg));
	if (!msg)
		return -ENOMEM;
	msg->input = *header;
	msg->length = header->length;
	msg->is_chunk_size = is_chunk_size;
	msg->new_chunk_size = new_chunk_size;
	if (header->length > 0)
	{
		msg->payload = (uint8_t*)malloc(header->length);
		if (!msg->payload)
		{
			free(msg);
			return -ENOMEM;
		}
		memcpy(msg->payload, payload, header->length);
	}

	if (queue->tail)
		queue->tail->next = msg;
	else
		queue->head = msg;
	queue->tail = msg;
	return 0;
}

int rtmp_chunk_scheduler_submit(struct rtmp_chunk_scheduler_t* scheduler, const struct rtmp_chunk_header_t* header, const uint8_t* payload)
{
	return rtmp_scheduler_enqueue(scheduler, header, payload, 0, 0);
}

int rtmp_chunk_scheduler_submit_chunk_size(struct rtmp_chunk_scheduler_t* scheduler, uint32_t size)
{
	struct rtmp_chunk_header_t header;
	uint8_t payload[4];

	if (size < 64 || size > 0x800000)
		return -EINVAL;

	// 5.4.1. Set Chunk Size (1), protocol control message on CSID 2, stream id 0
	memset(&header, 0, sizeof(header));
	header.fmt = RTMP_CHUNK_TYPE_0; // disable compact header
	header.cid = RTMP_CHANNEL_PROTOCOL;
	header.timestamp = 0;
	header.length = 4;
	header.type = RTMP_TYPE_SET_CHUNK_SIZE;
	header.stream_id = 0;
	be_write_uint32(payload, size & 0x7FFFFFFF); // first bit MUST be zero
	return rtmp_scheduler_enqueue(scheduler, &header, payload, 1, size);
}

// pick the queue whose head message gets emitted next
static struct rtmp_chunk_queue_t* rtmp_scheduler_pick(struct rtmp_chunk_scheduler_t* scheduler)
{
	uint32_t i;
	uint32_t min_cid = 0;
	uint32_t next_cid = 0;
	struct rtmp_chunk_queue_t* queue;

	// protocol control stream (CSID 2) always goes first, FIFO inside it
	queue = rtmp_scheduler_queue_find(scheduler, RTMP_CHANNEL_PROTOCOL, 0);
	if (queue && queue->head)
		return queue;

	// media chunk streams: smallest active CSID greater than the cursor,
	// wrap to the smallest active CSID; cursor itself is never reset
	for (i = 0; i < N_CHUNK_STREAM; i++)
	{
		queue = scheduler->queues + i;
		if (!queue->head || queue->cid <= RTMP_CHANNEL_PROTOCOL)
			continue;
		if (0 == min_cid || queue->cid < min_cid)
			min_cid = queue->cid;
		if (queue->cid > scheduler->cursor && (0 == next_cid || queue->cid < next_cid))
			next_cid = queue->cid;
	}

	if (0 == next_cid)
		next_cid = min_cid; // wrap around
	if (0 == next_cid)
		return NULL; // empty media queues
	return rtmp_scheduler_queue_find(scheduler, next_cid, 0);
}

static void rtmp_scheduler_message_done(struct rtmp_chunk_scheduler_t* scheduler, struct rtmp_chunk_queue_t* queue, struct rtmp_scheduled_msg_t* msg)
{
	assert(queue->head == msg);
	queue->head = msg->next;
	if (!queue->head)
		queue->tail = NULL;

	// the new chunk size is effective only after the control message is fully sent
	if (msg->is_chunk_size)
		scheduler->rtmp->out_chunk_size = msg->new_chunk_size;

	free(msg->payload);
	free(msg);
}

int rtmp_chunk_scheduler_step(struct rtmp_chunk_scheduler_t* scheduler)
{
	int r;
	uint32_t chunk_size, header_size;
	uint8_t header_buffer[MAX_CHUNK_HEADER];
	struct rtmp_chunk_queue_t* queue;
	struct rtmp_scheduled_msg_t* msg;
	const struct rtmp_chunk_header_t* header;

	if (scheduler->failed)
		return scheduler->error;

	queue = rtmp_scheduler_pick(scheduler);
	if (!queue || !queue->head)
		return 0; // nothing queued
	msg = queue->head;

	if (!msg->started)
	{
		// share the exact same header encoder as the synchronous rtmp_chunk_write path
		header = rtmp_chunk_header_zip_ex(scheduler->rtmp, &msg->input);
		if (!header)
		{
			scheduler->failed = 1;
			scheduler->error = -EINVAL;
			return scheduler->error;
		}
		msg->zipped = *header;

		header_size = rtmp_chunk_basic_header_write(header_buffer, header->fmt, header->cid);
		header_size += rtmp_chunk_message_header_write(header_buffer + header_size, header);
		if (header->timestamp >= 0xFFFFFF)
			header_size += rtmp_chunk_extended_timestamp_write(header_buffer + header_size, header->timestamp);
		assert(header_size < MAX_CHUNK_HEADER);
		msg->started = 1;
	}
	else
	{
		header = &msg->zipped;
		header_size = rtmp_chunk_continue_header_write(header_buffer, header);
	}

	chunk_size = (uint32_t)(msg->length - msg->offset);
	if (chunk_size > scheduler->rtmp->out_chunk_size)
		chunk_size = scheduler->rtmp->out_chunk_size;

	// one complete header + body per callback; short write / EAGAIN is not retried
	r = scheduler->rtmp->send(scheduler->rtmp->param, header_buffer, header_size, msg->payload + msg->offset, chunk_size);
	if (0 != r)
	{
		scheduler->failed = 1;
		scheduler->error = r;
		return scheduler->error;
	}

	msg->offset += chunk_size;
	if (queue->cid != RTMP_CHANNEL_PROTOCOL)
		scheduler->cursor = queue->cid; // control chunks do not move the rotation cursor

	if (msg->offset >= msg->length)
		rtmp_scheduler_message_done(scheduler, queue, msg);

	return 1;
}
