/* chunk interleave mode tests (C99, driven by task_support/driver.c) */
#include "rtmp-server.h"
#include "rtmp-internal.h"
#include "rtmp-msgtypeid.h"
#include "rtmp-control-message.h"
#include "task_support/check.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define TEST_CAP_CHUNKS 256
#define TEST_PAYLOAD_MAX 1024
#define TEST_WIRE_MAX (32 * 1024)

struct cap_chunk
{
	uint8_t fmt;
	uint32_t cid;
	uint8_t header[18];
	size_t header_len;
	uint8_t payload[TEST_PAYLOAD_MAX];
	size_t payload_len;
};

struct capture
{
	struct cap_chunk chunks[TEST_CAP_CHUNKS];
	size_t count;
	size_t wire_len;
	uint8_t wire[TEST_WIRE_MAX];
	int fail_at; /* 0-never fail, otherwise 1-based call index that returns an error */
};

static int cap_send(void* param, const void* header, size_t len, const void* payload, size_t bytes)
{
	struct capture* cap = (struct capture*)param;
	struct cap_chunk* c;
	if (cap->fail_at && (int)(cap->count + 1) == cap->fail_at)
		return -1; /* simulated socket error: no continuation, no retry */
	if (cap->count >= TEST_CAP_CHUNKS || len > sizeof(c->header) || bytes > TEST_PAYLOAD_MAX
		|| cap->wire_len + len + bytes > TEST_WIRE_MAX)
		return -1;

	c = &cap->chunks[cap->count++];
	c->fmt = ((const uint8_t*)header)[0] >> 6;
	c->cid = ((const uint8_t*)header)[0] & 0x3F;
	memcpy(c->header, header, len);
	c->header_len = len;
	if (bytes)
		memcpy(c->payload, payload, bytes);
	c->payload_len = bytes;

	memcpy(cap->wire + cap->wire_len, header, len);
	cap->wire_len += len;
	if (bytes)
	{
		memcpy(cap->wire + cap->wire_len, payload, bytes);
		cap->wire_len += bytes;
	}
	return (int)(len + bytes); /* rtmp_server_send requires the full byte count */
}

static rtmp_server_t* test_server_create(struct capture* cap)
{
	struct rtmp_server_handler_t handler;
	memset(&handler, 0, sizeof(handler));
	handler.send = cap_send;
	memset(cap, 0, sizeof(*cap));
	return rtmp_server_create(cap, &handler);
}

static int test_drain(rtmp_server_t* server)
{
	int r;
	for (;;)
	{
		r = rtmp_server_send_step(server);
		if (r <= 0)
			return r;
	}
}

static void test_fill(uint8_t* p, size_t n, uint8_t seed)
{
	size_t i;
	for (i = 0; i < n; ++i)
		p[i] = (uint8_t)(seed + (uint8_t)i);
}

/* ---------------- read side: reassemble captured wire bytes ---------------- */

struct reader_sink
{
	uint8_t audio[TEST_PAYLOAD_MAX];
	size_t audio_len;
	uint32_t audio_ts;
	unsigned audio_calls;
	uint8_t video[TEST_PAYLOAD_MAX];
	size_t video_len;
	uint32_t video_ts;
	unsigned video_calls;
	unsigned script_calls;
};

static int sink_audio(void* param, const uint8_t* data, size_t bytes, uint32_t timestamp)
{
	struct reader_sink* sink = (struct reader_sink*)param;
	if (sink->audio_len + bytes > TEST_PAYLOAD_MAX)
		return -1;
	memcpy(sink->audio + sink->audio_len, data, bytes);
	sink->audio_len += bytes;
	sink->audio_ts = timestamp;
	++sink->audio_calls;
	return 0;
}

static int sink_video(void* param, const uint8_t* data, size_t bytes, uint32_t timestamp)
{
	struct reader_sink* sink = (struct reader_sink*)param;
	if (sink->video_len + bytes > TEST_PAYLOAD_MAX)
		return -1;
	memcpy(sink->video + sink->video_len, data, bytes);
	sink->video_len += bytes;
	sink->video_ts = timestamp;
	++sink->video_calls;
	return 0;
}

static int sink_script(void* param, const uint8_t* data, size_t bytes, uint32_t timestamp)
{
	struct reader_sink* sink = (struct reader_sink*)param;
	(void)data; (void)bytes; (void)timestamp;
	++sink->script_calls;
	return 0;
}

static void reader_open(struct rtmp_t* rtmp, struct reader_sink* sink)
{
	memset(rtmp, 0, sizeof(*rtmp));
	memset(sink, 0, sizeof(*sink));
	rtmp->parser.state = RTMP_PARSE_INIT;
	rtmp->in_chunk_size = RTMP_CHUNK_SIZE;
	rtmp->param = sink;
	rtmp->onaudio = sink_audio;
	rtmp->onvideo = sink_video;
	rtmp->onscript = sink_script;
}

/* feed the whole stream one byte per rtmp_chunk_read call */
static int reader_feed_byte_by_byte(struct rtmp_t* rtmp, const uint8_t* wire, size_t len)
{
	size_t i;
	for (i = 0; i < len; ++i)
	{
		if (0 != rtmp_chunk_read(rtmp, wire + i, 1))
			return -1;
	}
	return 0;
}

/* ============================== interleave group ============================== */

/* submitting a message only queues it; nothing reaches the send callback until step */
static int case_queued_until_step(void)
{
	struct capture cap;
	rtmp_server_t* server = test_server_create(&cap);
	uint8_t video[300];
	test_fill(video, sizeof(video), 0x10);

	TASK_CHECK(0 == rtmp_server_enable_chunk_interleave(server));
	TASK_CHECK(0 == rtmp_server_send_video(server, video, sizeof(video), 1000));
	TASK_CHECK(0 == cap.count);
	TASK_CHECK(1 == rtmp_server_send_step(server));
	TASK_CHECK(1 == cap.count);
	TASK_CHECK(cap.chunks[0].cid == RTMP_CHANNEL_VIDEO);
	TASK_CHECK(cap.chunks[0].payload_len == 128);

	rtmp_server_destroy(server);
	return 0;
}

/* audio submitted after the video's first chunk finishes before the rest of the video */
static int case_audio_cuts_in(void)
{
	struct capture cap;
	rtmp_server_t* server = test_server_create(&cap);
	uint8_t video[300], audio[2] = { 0xAF, 0x01 };
	size_t i, video_bytes = 0;
	int r;
	test_fill(video, sizeof(video), 0x20);

	TASK_CHECK(0 == rtmp_server_enable_chunk_interleave(server));
	TASK_CHECK(0 == rtmp_server_send_video(server, video, sizeof(video), 0x100));
	TASK_CHECK(1 == rtmp_server_send_step(server)); /* only the first 128-byte video chunk */
	TASK_CHECK(cap.count == 1 && cap.chunks[0].cid == RTMP_CHANNEL_VIDEO);

	TASK_CHECK(0 == rtmp_server_send_audio(server, audio, sizeof(audio), 0x101));
	TASK_CHECK(cap.count == 1); /* submission still emits nothing */

	/* rotation wraps past the video cursor (5) back to the smallest active CSID: audio 4 */
	TASK_CHECK(1 == rtmp_server_send_step(server));
	TASK_CHECK(cap.chunks[1].cid == RTMP_CHANNEL_AUDIO);
	TASK_CHECK(cap.chunks[1].payload_len == sizeof(audio));
	TASK_CHECK(0 == memcmp(cap.chunks[1].payload, audio, sizeof(audio)));

	while ((r = rtmp_server_send_step(server)) > 0) {}
	TASK_CHECK(0 == r);
	TASK_CHECK(0 == rtmp_server_send_step(server)); /* queue empty -> 0 */

	for (i = 0; i < cap.count; ++i)
	{
		if (cap.chunks[i].cid != RTMP_CHANNEL_VIDEO)
			continue;
		TASK_CHECK(video_bytes + cap.chunks[i].payload_len <= sizeof(video));
		TASK_CHECK(0 == memcmp(video + video_bytes, cap.chunks[i].payload, cap.chunks[i].payload_len));
		video_bytes += cap.chunks[i].payload_len;
	}
	TASK_CHECK(video_bytes == sizeof(video));

	rtmp_server_destroy(server);
	return 0;
}

/* same CSID: the second message cannot start before the first one completes, FIFO order kept */
static int case_same_stream_fifo(void)
{
	struct capture cap;
	rtmp_server_t* server = test_server_create(&cap);
	uint8_t a[200], b[100];
	size_t i, got = 0;
	test_fill(a, sizeof(a), 0x30);
	test_fill(b, sizeof(b), 0x70);

	TASK_CHECK(0 == rtmp_server_enable_chunk_interleave(server));
	TASK_CHECK(0 == rtmp_server_send_audio(server, a, sizeof(a), 100));
	TASK_CHECK(0 == rtmp_server_send_audio(server, b, sizeof(b), 200));
	TASK_CHECK(0 == test_drain(server));

	/* 128 + 72 for message A, then the brand-new header of message B (100 bytes) */
	TASK_CHECK(cap.count == 3);
	TASK_CHECK(cap.chunks[0].cid == RTMP_CHANNEL_AUDIO && cap.chunks[0].payload_len == 128);
	TASK_CHECK(cap.chunks[1].cid == RTMP_CHANNEL_AUDIO && cap.chunks[1].payload_len == 72);
	TASK_CHECK(cap.chunks[2].cid == RTMP_CHANNEL_AUDIO && cap.chunks[2].payload_len == 100);
	TASK_CHECK(RTMP_CHUNK_TYPE_3 != cap.chunks[2].fmt); /* new message header, not a continuation */

	for (i = 0; i < cap.count; ++i)
	{
		TASK_CHECK(0 == memcmp((got < sizeof(a) ? a : b) + (got < sizeof(a) ? got : got - sizeof(a)),
			cap.chunks[i].payload, cap.chunks[i].payload_len));
		got += cap.chunks[i].payload_len;
	}
	TASK_CHECK(got == sizeof(a) + sizeof(b));

	rtmp_server_destroy(server);
	return 0;
}

/* active media CSIDs rotate from the smallest id, wrapping, cursor never reset */
static int case_round_robin_rotation(void)
{
	struct capture cap;
	rtmp_server_t* server = test_server_create(&cap);
	uint8_t script[130], audio[200], video[300];
	static const uint32_t expect[] = { 3, 4, 5, 3, 4, 5, 5 };
	size_t i;
	test_fill(script, sizeof(script), 0x40);
	test_fill(audio, sizeof(audio), 0x50);
	test_fill(video, sizeof(video), 0x60);

	TASK_CHECK(0 == rtmp_server_enable_chunk_interleave(server));
	TASK_CHECK(0 == rtmp_server_send_script(server, script, sizeof(script), 10));
	TASK_CHECK(0 == rtmp_server_send_audio(server, audio, sizeof(audio), 11));
	TASK_CHECK(0 == rtmp_server_send_video(server, video, sizeof(video), 12));
	TASK_CHECK(0 == test_drain(server));

	TASK_CHECK(cap.count == sizeof(expect) / sizeof(expect[0]));
	for (i = 0; i < cap.count; ++i)
		TASK_CHECK(cap.chunks[i].cid == expect[i]);
	TASK_CHECK(0 == rtmp_server_send_step(server));

	rtmp_server_destroy(server);
	return 0;
}

/* empty step reports 0; enabling twice keeps queued messages; destroy frees a pending queue */
static int case_repeat_enable_and_destroy(void)
{
	struct capture cap;
	rtmp_server_t* server = test_server_create(&cap);
	uint8_t video[300];
	test_fill(video, sizeof(video), 0x80);

	TASK_CHECK(0 == rtmp_server_enable_chunk_interleave(server));
	TASK_CHECK(0 == rtmp_server_send_video(server, video, sizeof(video), 100));
	TASK_CHECK(0 == rtmp_server_enable_chunk_interleave(server)); /* no state reset */
	TASK_CHECK(1 == rtmp_server_send_step(server));
	TASK_CHECK(cap.count == 1 && cap.chunks[0].payload_len == 128);

	/* leave the rest queued and destroy: must release the retained messages */
	rtmp_server_destroy(server);
	return 0;
}

/* without enabling the mode the old synchronous behavior is fully preserved */
static int case_sync_mode_unchanged(void)
{
	struct capture cap;
	rtmp_server_t* server = test_server_create(&cap);
	uint8_t video[300], audio[2] = { 0xAF, 0x01 };
	test_fill(video, sizeof(video), 0x90);

	TASK_CHECK(rtmp_server_send_step(server) < 0);
	TASK_CHECK(rtmp_server_set_chunk_size(server, 256) < 0);

	TASK_CHECK(0 == rtmp_server_send_video(server, video, sizeof(video), 0x100));
	TASK_CHECK(cap.count == 3); /* whole frame emitted inside the submit call */
	TASK_CHECK(0 == rtmp_server_send_audio(server, audio, sizeof(audio), 0x101));
	TASK_CHECK(cap.count == 4);
	TASK_CHECK(cap.chunks[0].cid == 5 && cap.chunks[1].cid == 5 && cap.chunks[2].cid == 5 && cap.chunks[3].cid == 4);
	TASK_CHECK(cap.chunks[0].payload_len == 128 && cap.chunks[2].payload_len == 44);

	rtmp_server_destroy(server);
	return 0;
}

/* a send error stops output permanently; no retry, later submits also fail */
static int case_send_error_stops_output(void)
{
	struct capture cap;
	rtmp_server_t* server = test_server_create(&cap);
	uint8_t video[300], audio[4] = { 0xAF, 1, 2, 3 };
	test_fill(video, sizeof(video), 0xA0);

	TASK_CHECK(0 == rtmp_server_enable_chunk_interleave(server));
	TASK_CHECK(0 == rtmp_server_send_video(server, video, sizeof(video), 100));
	TASK_CHECK(1 == rtmp_server_send_step(server));
	TASK_CHECK(cap.count == 1);
	cap.fail_at = 2;
	TASK_CHECK(rtmp_server_send_step(server) < 0); /* failed callback does not record a chunk */
	TASK_CHECK(cap.count == 1);
	TASK_CHECK(rtmp_server_send_step(server) < 0);
	TASK_CHECK(cap.count == 1); /* sticky: no further callback */
	TASK_CHECK(rtmp_server_send_audio(server, audio, sizeof(audio), 101) < 0);

	rtmp_server_destroy(server);
	return 0;
}

/* ================================ headers group ================================ */

/* hand-written expectations for fmt1/fmt2/fmt3 compression with another CSID in between */
static int case_compact_headers(void)
{
	struct capture cap;
	rtmp_server_t* server = test_server_create(&cap);
	uint8_t v1[300], v2[301], v3[301], v4[301], v5[301], au[10];
	static const uint8_t h_v1[] = { 0x45, 0x00, 0x03, 0xE8, 0x00, 0x01, 0x2C, 0x09 };
	static const uint8_t h_au[] = { 0x44, 0x00, 0x05, 0xDC, 0x00, 0x00, 0x0A, 0x08 };
	static const uint8_t h_v2[] = { 0x45, 0x00, 0x03, 0xE8, 0x00, 0x01, 0x2D, 0x09 };
	static const uint8_t h_v3[] = { 0x85, 0x00, 0x05, 0xDC }; /* fmt2, delta 1500 */
	static const uint8_t h_v4[] = { 0x85, 0x00, 0x03, 0xE8 }; /* fmt2, delta 1000 */
	static const uint8_t h_v5[] = { 0xC5 };                   /* fmt3 */
	test_fill(v1, sizeof(v1), 1);
	test_fill(v2, sizeof(v2), 2);
	test_fill(v3, sizeof(v3), 3);
	test_fill(v4, sizeof(v4), 4);
	test_fill(v5, sizeof(v5), 5);
	test_fill(au, sizeof(au), 6);

	TASK_CHECK(0 == rtmp_server_enable_chunk_interleave(server));

	TASK_CHECK(0 == rtmp_server_send_video(server, v1, sizeof(v1), 1000));
	TASK_CHECK(0 == test_drain(server)); /* indices 0..2 */
	TASK_CHECK(0 == rtmp_server_send_audio(server, au, sizeof(au), 1500));
	TASK_CHECK(1 == rtmp_server_send_step(server)); /* index 3 */
	TASK_CHECK(0 == rtmp_server_send_video(server, v2, sizeof(v2), 2000));
	TASK_CHECK(0 == rtmp_server_send_video(server, v3, sizeof(v3), 3500));
	TASK_CHECK(0 == rtmp_server_send_video(server, v4, sizeof(v4), 4500));
	TASK_CHECK(0 == rtmp_server_send_video(server, v5, sizeof(v5), 5500));
	TASK_CHECK(0 == test_drain(server));

	/* v1 */
	TASK_CHECK(cap.chunks[0].header_len == sizeof(h_v1));
	TASK_CHECK(0 == memcmp(cap.chunks[0].header, h_v1, sizeof(h_v1)));
	TASK_CHECK(cap.chunks[1].fmt == RTMP_CHUNK_TYPE_3 && cap.chunks[1].header[0] == 0xC5);
	TASK_CHECK(cap.chunks[2].fmt == RTMP_CHUNK_TYPE_3);
	/* audio compressed on its own CSID, unaffected by video state */
	TASK_CHECK(cap.chunks[3].header_len == sizeof(h_au));
	TASK_CHECK(0 == memcmp(cap.chunks[3].header, h_au, sizeof(h_au)));
	/* v2..v5 first chunks; each 301-byte message occupies 3 chunks */
	TASK_CHECK(cap.chunks[4].header_len == sizeof(h_v2) && 0 == memcmp(cap.chunks[4].header, h_v2, sizeof(h_v2)));
	TASK_CHECK(cap.chunks[7].header_len == sizeof(h_v3) && 0 == memcmp(cap.chunks[7].header, h_v3, sizeof(h_v3)));
	TASK_CHECK(cap.chunks[10].header_len == sizeof(h_v4) && 0 == memcmp(cap.chunks[10].header, h_v4, sizeof(h_v4)));
	TASK_CHECK(cap.chunks[13].header_len == sizeof(h_v5) && 0 == memcmp(cap.chunks[13].header, h_v5, sizeof(h_v5)));
	TASK_CHECK(cap.chunks[13].fmt == RTMP_CHUNK_TYPE_3); /* new message, but fully compressed */
	TASK_CHECK(cap.count == 16);

	rtmp_server_destroy(server);
	return 0;
}

/* timestamps >= 0xFFFFFF carry the extended timestamp on first and fmt3 chunks */
static int case_extended_timestamp(void)
{
	struct capture cap;
	struct rtmp_t reader;
	struct reader_sink sink;
	rtmp_server_t* server = test_server_create(&cap);
	uint8_t video[300];
	static const uint8_t h_first[] = {
		0x05, 0xFF, 0xFF, 0xFF, 0x00, 0x01, 0x2C, 0x09, 0x00, 0x00, 0x00, 0x00,
		0x01, 0x00, 0x00, 0x10 };
	static const uint8_t h_cont[] = { 0xC5, 0x01, 0x00, 0x00, 0x10 };
	test_fill(video, sizeof(video), 0xB0);

	TASK_CHECK(0 == rtmp_server_enable_chunk_interleave(server));
	TASK_CHECK(0 == rtmp_server_send_video(server, video, sizeof(video), 0x01000010));
	TASK_CHECK(0 == test_drain(server));
	TASK_CHECK(cap.count == 3);
	TASK_CHECK(cap.chunks[0].header_len == sizeof(h_first) && 0 == memcmp(cap.chunks[0].header, h_first, sizeof(h_first)));
	TASK_CHECK(cap.chunks[1].header_len == sizeof(h_cont) && 0 == memcmp(cap.chunks[1].header, h_cont, sizeof(h_cont)));
	TASK_CHECK(cap.chunks[2].header_len == sizeof(h_cont) && 0 == memcmp(cap.chunks[2].header, h_cont, sizeof(h_cont)));

	reader_open(&reader, &sink);
	TASK_CHECK(0 == reader_feed_byte_by_byte(&reader, cap.wire, cap.wire_len));
	TASK_CHECK(sink.video_calls == 1 && sink.video_len == sizeof(video));
	TASK_CHECK(sink.video_ts == 0x01000010);
	TASK_CHECK(0 == memcmp(sink.video, video, sizeof(video)));

	rtmp_server_destroy(server);
	return 0;
}

/* a backwards timestamp cannot use delta compression: fmt0 absolute header comes back */
static int case_timestamp_wrap(void)
{
	struct capture cap;
	struct rtmp_t reader;
	struct reader_sink sink;
	rtmp_server_t* server = test_server_create(&cap);
	uint8_t v1[100], v2[100];
	test_fill(v1, sizeof(v1), 0xC0);
	test_fill(v2, sizeof(v2), 0xD0);

	TASK_CHECK(0 == rtmp_server_enable_chunk_interleave(server));
	TASK_CHECK(0 == rtmp_server_send_video(server, v1, sizeof(v1), 2000));
	TASK_CHECK(0 == test_drain(server));
	TASK_CHECK(0 == rtmp_server_send_video(server, v2, sizeof(v2), 1000));
	TASK_CHECK(0 == test_drain(server));

	TASK_CHECK(cap.chunks[0].fmt == RTMP_CHUNK_TYPE_1);
	TASK_CHECK(cap.chunks[1].fmt == RTMP_CHUNK_TYPE_0);
	TASK_CHECK(cap.chunks[1].header[0] == 0x05);
	TASK_CHECK(cap.chunks[1].header[1] == 0x00 && cap.chunks[1].header[2] == 0x03 && cap.chunks[1].header[3] == 0xE8);

	reader_open(&reader, &sink);
	TASK_CHECK(0 == reader_feed_byte_by_byte(&reader, cap.wire, cap.wire_len));
	TASK_CHECK(sink.video_calls == 2 && sink.video_len == sizeof(v1) + sizeof(v2));
	TASK_CHECK(0 == memcmp(sink.video, v1, sizeof(v1)));
	TASK_CHECK(0 == memcmp(sink.video + sizeof(v1), v2, sizeof(v2)));
	TASK_CHECK(sink.video_ts == 1000); /* last delivered absolute timestamp */

	rtmp_server_destroy(server);
	return 0;
}

/* byte-at-a-time network input of an interleaved audio/video stream reassembles fully */
static int case_single_byte_reader(void)
{
	struct capture cap;
	struct rtmp_t reader;
	struct reader_sink sink;
	rtmp_server_t* server = test_server_create(&cap);
	uint8_t video[300], audio[2] = { 0xAF, 0x77 };
	test_fill(video, sizeof(video), 0xE0);

	TASK_CHECK(0 == rtmp_server_enable_chunk_interleave(server));
	TASK_CHECK(0 == rtmp_server_send_video(server, video, sizeof(video), 0x200));
	TASK_CHECK(1 == rtmp_server_send_step(server));
	TASK_CHECK(0 == rtmp_server_send_audio(server, audio, sizeof(audio), 0x201));
	TASK_CHECK(0 == test_drain(server));

	reader_open(&reader, &sink);
	TASK_CHECK(0 == reader_feed_byte_by_byte(&reader, cap.wire, cap.wire_len));
	TASK_CHECK(sink.video_calls == 1 && sink.video_len == sizeof(video));
	TASK_CHECK(sink.video_ts == 0x200);
	TASK_CHECK(0 == memcmp(sink.video, video, sizeof(video)));
	TASK_CHECK(sink.audio_calls == 1 && sink.audio_len == sizeof(audio));
	TASK_CHECK(sink.audio_ts == 0x201);
	TASK_CHECK(0 == memcmp(sink.audio, audio, sizeof(audio)));

	rtmp_server_destroy(server);
	return 0;
}

/* ============================== chunk_size group ============================== */

/* invalid sizes are rejected without output; the API only exists in interleave mode */
static int case_invalid_chunk_size(void)
{
	struct capture cap;
	rtmp_server_t* server = test_server_create(&cap);

	TASK_CHECK(rtmp_server_set_chunk_size(server, 64) < 0); /* mode not enabled yet */
	TASK_CHECK(0 == rtmp_server_enable_chunk_interleave(server));
	TASK_CHECK(rtmp_server_set_chunk_size(server, 0) < 0);
	TASK_CHECK(rtmp_server_set_chunk_size(server, 63) < 0);
	TASK_CHECK(rtmp_server_set_chunk_size(server, 0x800001) < 0);
	TASK_CHECK(0 == cap.count);
	TASK_CHECK(0 == rtmp_server_send_step(server)); /* nothing queued */
	TASK_CHECK(0 == rtmp_server_set_chunk_size(server, 64));
	TASK_CHECK(0 == rtmp_server_set_chunk_size(server, 0x800000));
	TASK_CHECK(0 == test_drain(server));
	TASK_CHECK(cap.count == 2 && cap.chunks[0].cid == RTMP_CHANNEL_PROTOCOL);

	rtmp_server_destroy(server);
	return 0;
}

/* grow to 256 after the video's first 128-byte chunk; the remainder becomes one chunk */
static int case_grow_mid_message(void)
{
	struct capture cap;
	struct rtmp_t reader;
	struct reader_sink sink;
	rtmp_server_t* server = test_server_create(&cap);
	uint8_t video[300], video2[600];
	test_fill(video, sizeof(video), 0x11);
	test_fill(video2, sizeof(video2), 0x22);

	TASK_CHECK(0 == rtmp_server_enable_chunk_interleave(server));
	TASK_CHECK(0 == rtmp_server_send_video(server, video, sizeof(video), 100));
	TASK_CHECK(1 == rtmp_server_send_step(server)); /* 128 bytes out, old size still on the wire */
	TASK_CHECK(cap.chunks[0].payload_len == 128);
	TASK_CHECK(0 == rtmp_server_set_chunk_size(server, 256));
	TASK_CHECK(cap.count == 1); /* submitting the change emits nothing */

	TASK_CHECK(1 == rtmp_server_send_step(server)); /* CSID 2 control first */
	TASK_CHECK(cap.chunks[1].cid == RTMP_CHANNEL_PROTOCOL && cap.chunks[1].payload_len == 4);
	TASK_CHECK(1 == rtmp_server_send_step(server)); /* remaining 172 bytes fit a single 256 chunk */
	TASK_CHECK(cap.chunks[2].cid == RTMP_CHANNEL_VIDEO && cap.chunks[2].payload_len == 172);
	TASK_CHECK(0 == rtmp_server_send_step(server));

	/* later messages use the new size for all their chunks */
	TASK_CHECK(0 == rtmp_server_send_video(server, video2, sizeof(video2), 200));
	TASK_CHECK(0 == test_drain(server));
	TASK_CHECK(cap.chunks[3].payload_len == 256 && cap.chunks[4].payload_len == 256 && cap.chunks[5].payload_len == 88);

	reader_open(&reader, &sink);
	TASK_CHECK(0 == reader_feed_byte_by_byte(&reader, cap.wire, cap.wire_len));
	TASK_CHECK(sink.video_calls == 2);
	TASK_CHECK(sink.video_len == sizeof(video) + sizeof(video2));

	rtmp_server_destroy(server);
	return 0;
}

/* shrink to 96 inside an in-flight video message */
static int case_shrink_mid_message(void)
{
	struct capture cap;
	struct rtmp_t reader;
	struct reader_sink sink;
	rtmp_server_t* server = test_server_create(&cap);
	uint8_t video[300];
	test_fill(video, sizeof(video), 0x33);

	TASK_CHECK(0 == rtmp_server_enable_chunk_interleave(server));
	TASK_CHECK(0 == rtmp_server_send_video(server, video, sizeof(video), 100));
	TASK_CHECK(1 == rtmp_server_send_step(server)); /* 128 */
	TASK_CHECK(0 == rtmp_server_set_chunk_size(server, 96));
	TASK_CHECK(0 == test_drain(server));

	/* 128 video, 4 control, then 96 + 76 of the same message */
	TASK_CHECK(cap.count == 4);
	TASK_CHECK(cap.chunks[0].payload_len == 128);
	TASK_CHECK(cap.chunks[1].cid == RTMP_CHANNEL_PROTOCOL && cap.chunks[1].payload_len == 4);
	TASK_CHECK(cap.chunks[2].payload_len == 96);
	TASK_CHECK(cap.chunks[3].payload_len == 76);

	reader_open(&reader, &sink);
	TASK_CHECK(0 == reader_feed_byte_by_byte(&reader, cap.wire, cap.wire_len));
	TASK_CHECK(sink.video_calls == 1 && sink.video_len == sizeof(video));
	TASK_CHECK(0 == memcmp(sink.video, video, sizeof(video)));

	rtmp_server_destroy(server);
	return 0;
}

/* two queued size changes leave in FIFO order; each applies at its own send-completion point */
static int case_consecutive_chunk_sizes(void)
{
	struct capture cap;
	rtmp_server_t* server = test_server_create(&cap);
	uint8_t video[740]; /* 128 + 96*6 + 36 once both control messages are out */
	uint8_t later[400];
	uint8_t last[200];
	size_t base;
	test_fill(video, sizeof(video), 0x44);
	test_fill(later, sizeof(later), 0x45);
	test_fill(last, sizeof(last), 0x46);

	TASK_CHECK(0 == rtmp_server_enable_chunk_interleave(server));
	TASK_CHECK(0 == rtmp_server_send_video(server, video, sizeof(video), 100));
	TASK_CHECK(1 == rtmp_server_send_step(server)); /* first chunk at the old 128 size */
	TASK_CHECK(0 == rtmp_server_set_chunk_size(server, 256));
	TASK_CHECK(0 == rtmp_server_set_chunk_size(server, 96));
	TASK_CHECK(cap.count == 1); /* sizes must not change at submit time */
	TASK_CHECK(0 == test_drain(server));

	/* CSID 2 has priority: both control messages leave back to back, in FIFO order */
	TASK_CHECK(cap.count == 10);
	TASK_CHECK(cap.chunks[0].cid == RTMP_CHANNEL_VIDEO && cap.chunks[0].payload_len == 128);
	TASK_CHECK(cap.chunks[1].cid == RTMP_CHANNEL_PROTOCOL && cap.chunks[1].payload_len == 4);
	TASK_CHECK(cap.chunks[1].payload[0] == 0 && cap.chunks[1].payload[3] == 0x00); /* 256 = 0x100 */
	TASK_CHECK(cap.chunks[1].payload[2] == 0x01);
	TASK_CHECK(cap.chunks[2].cid == RTMP_CHANNEL_PROTOCOL && cap.chunks[2].payload_len == 4);
	TASK_CHECK(cap.chunks[2].payload[3] == 96); /* 96 = 0x60 */
	TASK_CHECK(cap.chunks[3].payload_len == 96 && cap.chunks[4].payload_len == 96);
	TASK_CHECK(cap.chunks[5].payload_len == 96 && cap.chunks[6].payload_len == 96);
	TASK_CHECK(cap.chunks[7].payload_len == 96 && cap.chunks[8].payload_len == 96);
	TASK_CHECK(cap.chunks[9].payload_len == 36); /* 612 = 6*96 + 36 */

	/* stepwise submission proves the size flips exactly when the control message finishes */
	base = cap.count;
	TASK_CHECK(0 == rtmp_server_set_chunk_size(server, 256));
	TASK_CHECK(1 == rtmp_server_send_step(server)); /* control out: 256 is now effective */
	TASK_CHECK(cap.chunks[base].cid == RTMP_CHANNEL_PROTOCOL);
	TASK_CHECK(0 == rtmp_server_send_video(server, later, sizeof(later), 200));
	TASK_CHECK(0 == test_drain(server));
	TASK_CHECK(cap.chunks[base + 1].payload_len == 256 && cap.chunks[base + 2].payload_len == 144);

	base = cap.count;
	TASK_CHECK(0 == rtmp_server_set_chunk_size(server, 96));
	TASK_CHECK(0 == test_drain(server));
	TASK_CHECK(cap.chunks[base].cid == RTMP_CHANNEL_PROTOCOL);
	TASK_CHECK(0 == rtmp_server_send_video(server, last, sizeof(last), 300));
	TASK_CHECK(0 == test_drain(server));
	TASK_CHECK(cap.chunks[base + 1].payload_len == 96 && cap.chunks[base + 2].payload_len == 96 && cap.chunks[base + 3].payload_len == 8);

	rtmp_server_destroy(server);
	return 0;
}

/* one message spanning 128 -> 256 -> 96 must not treat its accumulated offset as a chunk boundary */
static int case_reader_size_switch_within_message(void)
{
	struct capture cap;
	struct rtmp_t reader;
	struct reader_sink sink;
	rtmp_server_t* server = test_server_create(&cap);
	uint8_t video[530]; /* 128 + 256 + 96 + 50 */
	test_fill(video, sizeof(video), 0x55);

	TASK_CHECK(0 == rtmp_server_enable_chunk_interleave(server));
	TASK_CHECK(0 == rtmp_server_send_video(server, video, sizeof(video), 300));
	TASK_CHECK(1 == rtmp_server_send_step(server)); /* 128 */
	TASK_CHECK(0 == rtmp_server_set_chunk_size(server, 256));
	TASK_CHECK(1 == rtmp_server_send_step(server)); /* control 256 */
	TASK_CHECK(1 == rtmp_server_send_step(server)); /* 256 video, 146 remain */
	TASK_CHECK(0 == rtmp_server_set_chunk_size(server, 96));
	TASK_CHECK(0 == test_drain(server)); /* control 96, then 96 + 50 video */

	TASK_CHECK(cap.chunks[0].payload_len == 128);
	TASK_CHECK(cap.chunks[2].payload_len == 256);
	TASK_CHECK(cap.chunks[4].payload_len == 96);
	TASK_CHECK(cap.chunks[5].payload_len == 50);

	reader_open(&reader, &sink);
	TASK_CHECK(0 == reader_feed_byte_by_byte(&reader, cap.wire, cap.wire_len));
	TASK_CHECK(sink.video_calls == 1);
	TASK_CHECK(sink.video_len == sizeof(video));
	TASK_CHECK(0 == memcmp(sink.video, video, sizeof(video)));
	TASK_CHECK(sink.video_ts == 300);

	rtmp_server_destroy(server);
	return 0;
}

/* a zero-length message is one header-only chunk and must not block its queue */
static int case_zero_length_message(void)
{
	struct capture cap;
	struct rtmp_t reader;
	struct reader_sink sink;
	rtmp_server_t* server = test_server_create(&cap);
	uint8_t audio[5];
	test_fill(audio, sizeof(audio), 0xF1);

	TASK_CHECK(0 == rtmp_server_enable_chunk_interleave(server));
	TASK_CHECK(0 == rtmp_server_send_script(server, NULL, 0, 9)); /* header-only message */
	TASK_CHECK(0 == rtmp_server_send_audio(server, audio, sizeof(audio), 10));
	TASK_CHECK(1 == rtmp_server_send_step(server));
	TASK_CHECK(cap.chunks[0].cid == RTMP_CHANNEL_INVOKE && cap.chunks[0].payload_len == 0);
	TASK_CHECK(0 == test_drain(server));
	TASK_CHECK(cap.chunks[1].cid == RTMP_CHANNEL_AUDIO && cap.chunks[1].payload_len == sizeof(audio));

	reader_open(&reader, &sink);
	TASK_CHECK(0 == reader_feed_byte_by_byte(&reader, cap.wire, cap.wire_len));
	TASK_CHECK(sink.script_calls == 1);
	TASK_CHECK(sink.audio_calls == 1 && sink.audio_len == sizeof(audio));

	rtmp_server_destroy(server);
	return 0;
}

const struct task_case task_cases[] = {
	{ "interleave", "queued_until_step", case_queued_until_step },
	{ "interleave", "audio_cuts_in", case_audio_cuts_in },
	{ "interleave", "same_stream_fifo", case_same_stream_fifo },
	{ "interleave", "round_robin_rotation", case_round_robin_rotation },
	{ "interleave", "repeat_enable_and_destroy", case_repeat_enable_and_destroy },
	{ "interleave", "sync_mode_unchanged", case_sync_mode_unchanged },
	{ "interleave", "send_error_stops_output", case_send_error_stops_output },
	{ "interleave", "zero_length_message", case_zero_length_message },
	{ "headers", "compact_headers", case_compact_headers },
	{ "headers", "extended_timestamp", case_extended_timestamp },
	{ "headers", "timestamp_wrap", case_timestamp_wrap },
	{ "headers", "single_byte_reader", case_single_byte_reader },
	{ "chunk_size", "invalid_chunk_size", case_invalid_chunk_size },
	{ "chunk_size", "grow_mid_message", case_grow_mid_message },
	{ "chunk_size", "shrink_mid_message", case_shrink_mid_message },
	{ "chunk_size", "consecutive_chunk_sizes", case_consecutive_chunk_sizes },
	{ "chunk_size", "reader_size_switch_within_message", case_reader_size_switch_within_message },
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
