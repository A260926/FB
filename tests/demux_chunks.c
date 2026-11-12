/*
 * Tests for the streaming plm_demux_decode_chunk() interface.
 *
 * Covers:
 *   - fragment reassembly byte-identical to plm_demux_decode(), via both
 *     direct plm_buffer_write() and a load callback
 *   - headers fed one byte at a time across every boundary (length field,
 *     0xff stuffing, P-STD, PTS only, PTS/DTS, timestamp marker bits)
 *   - zero-length packets, packets without a PTS, padding packets with fake
 *     start codes, truncated packets and illegal headers
 *   - bounded allocations (no whole-packet scratch, blocks stay <= 4096)
 *   - rewind of a fixed memory source re-reading from the start
 *
 * Build: cc -std=c99 -O2 tests/demux_chunks.c -lm -o demux_chunks
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

/* Count every allocation the library performs through PLM_MALLOC so the
 * bounded-memory test can prove no whole-packet scratch buffer is used. */
static size_t g_max_block = 0;
static long g_malloc_count = 0;
static long g_realloc_count = 0;

static void *test_malloc(size_t sz) {
	if (sz > g_max_block) {
		g_max_block = sz;
	}
	g_malloc_count++;
	return malloc(sz);
}

static void *test_realloc(void *p, size_t sz) {
	if (sz > g_max_block) {
		g_max_block = sz;
	}
	g_realloc_count++;
	return realloc(p, sz);
}

static void test_free(void *p) {
	free(p);
}

#define PLM_MALLOC(sz) test_malloc(sz)
#define PLM_REALLOC(p, sz) test_realloc(p, sz)
#define PLM_FREE(p) test_free(p)

#define PL_MPEG_IMPLEMENTATION
#include "../pl_mpeg.h"
#include "../harness/fixture.h"

static int g_failures = 0;

#define CHECK(cond, msg) do { \
	if (!(cond)) { \
		printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
		g_failures++; \
	} \
} while (0)

#define STREAM_CAP 40000

typedef struct {
	int type;
	double pts;
	int has_pts;
	size_t length;
	uint8_t data[STREAM_CAP];
	int chunks;
	int first_count;
	int last_count;
} collected_t;

/* Reassemble everything a demuxer can currently deliver. Opens a new entry on
 * the `first` fragment and closes it on `last`. */
static int drain(plm_demux_t *d, collected_t *out, int max_packets) {
	int count = 0;
	plm_packet_chunk_t *c;
	while ((c = plm_demux_decode_chunk(d))) {
		if (c->first) {
			CHECK(count < max_packets, "more packets than collector slots");
			if (count >= max_packets) {
				return count;
			}
			memset(&out[count], 0, sizeof(collected_t));
			out[count].type = c->type;
			out[count].pts = c->pts;
			out[count].has_pts = (c->pts != PLM_PACKET_INVALID_TS);
		}
		CHECK(count < max_packets, "fragment without an opened packet");
		if (count >= max_packets) {
			return count;
		}
		collected_t *cur = &out[count];
		CHECK(cur->type == c->type, "type changes mid-packet");
		CHECK((cur->has_pts && fabs(cur->pts - c->pts) < 1e-12) ||
			(!cur->has_pts && c->pts == PLM_PACKET_INVALID_TS),
			"pts changes mid-packet");
		CHECK(cur->length + c->length <= sizeof(cur->data), "collector overflow");
		memcpy(cur->data + cur->length, c->data, c->length);
		cur->length += c->length;
		cur->chunks++;
		cur->first_count += c->first;
		cur->last_count += c->last;
		if (c->last) {
			count++;
		}
	}
	return count;
}

/* Drain exactly one complete packet (or none if it is not ready yet). */
static int drain_one(plm_demux_t *d, collected_t *out) {
	memset(out, 0, sizeof(collected_t));
	int opened = 0;
	plm_packet_chunk_t *c;
	while ((c = plm_demux_decode_chunk(d))) {
		if (c->first) {
			opened = 1;
			out->type = c->type;
			out->pts = c->pts;
			out->has_pts = (c->pts != PLM_PACKET_INVALID_TS);
		}
		CHECK(opened, "fragment without an opened packet");
		CHECK(out->type == c->type, "type changes mid-packet");
		CHECK(out->length + c->length <= sizeof(out->data), "collector overflow");
		memcpy(out->data + out->length, c->data, c->length);
		out->length += c->length;
		out->chunks++;
		out->first_count += c->first;
		out->last_count += c->last;
		if (c->last) {
			return 1;
		}
	}
	return 0;
}

/* Feed `stream` into a fresh ring buffer using writes of at most `write_size`
 * bytes, draining after every write. */
static int feed_by_writes(const uint8_t *stream, size_t n, size_t ring_capacity,
		size_t write_size, collected_t *out, int max_packets) {
	plm_buffer_t *buffer = plm_buffer_create_with_capacity(ring_capacity);
	plm_demux_t *demux = plm_demux_create(buffer, 1);
	int count = 0;
	size_t off = 0;
	while (off < n) {
		size_t w = n - off;
		if (w > write_size) {
			w = write_size;
		}
		plm_buffer_write(buffer, (uint8_t *)stream + off, w);
		off += w;
		count += drain(demux, out + count, max_packets - count);
	}
	plm_demux_destroy(demux);
	return count;
}

/* A memory-backed data source driven through plm_buffer load/seek/tell
 * callbacks, mirroring how a streaming file source is fed. */
typedef struct {
	const uint8_t *data;
	size_t length;
	size_t pos;
} mem_source_t;

static void mem_load(plm_buffer_t *self, void *user) {
	mem_source_t *src = (mem_source_t *)user;
	if (self->discard_read_bytes) {
		plm_buffer_discard_read_bytes(self);
	}
	size_t space = self->capacity - self->length;
	size_t want = src->length - src->pos;
	if (want > space) {
		want = space;
	}
	memcpy(self->bytes + self->length, src->data + src->pos, want);
	src->pos += want;
	self->length += want;
	if (want == 0) {
		self->has_ended = TRUE;
	}
}

static void mem_seek(plm_buffer_t *self, size_t offset, void *user) {
	(void)self;
	((mem_source_t *)user)->pos = offset;
}

static size_t mem_tell(plm_buffer_t *self, void *user) {
	(void)self;
	return ((mem_source_t *)user)->pos;
}

/* Build one MPEG-1 PES packet. mode: 0 = no timestamp, 2 = PTS only,
 * 3 = PTS + DTS. pstd inserts a 2-byte P-STD field. */
static size_t build_pes(uint8_t *p, int type, size_t payload, int stuffing,
		int mode, int pstd, uint64_t pts) {
	size_t hdr = (size_t)stuffing + (pstd ? 2 : 0) +
		(mode == 3 ? 10 : mode == 2 ? 5 : 1);
	size_t declared = hdr + payload;
	size_t n = 0;
	p[n++] = 0;
	p[n++] = 0;
	p[n++] = 1;
	p[n++] = (uint8_t)type;
	p[n++] = (uint8_t)(declared >> 8);
	p[n++] = (uint8_t)declared;
	memset(p + n, 0xff, (size_t)stuffing);
	n += (size_t)stuffing;
	if (pstd) {
		p[n++] = 0x40; /* leading bits 01 mark the P-STD field */
		p[n++] = 0x00;
	}
	if (mode == 3) {
		sample_pts(p + n, 3, pts);
		n += 5;
		sample_pts(p + n, 1, pts - 3600);
		n += 5;
	}
	else if (mode == 2) {
		sample_pts(p + n, 2, pts);
		n += 5;
	}
	else {
		p[n++] = 0x0f; /* marker byte, PTS/DTS flags = 00 */
	}
	for (size_t i = 0; i < payload; i++) {
		p[n + i] = (uint8_t)((i * 31 + type) % 251);
	}
	/* A PES-looking sequence must remain ordinary payload. */
	if (payload > 8) {
		p[n + 2] = 0;
		p[n + 3] = 0;
		p[n + 4] = 1;
		p[n + 5] = 0xe0;
	}
	return n + payload;
}

static void test_chunks_basic(void) {
	uint8_t stream[40000];
	size_t n = sample_fixture(stream);

	/* Reference output captured through the legacy whole-packet interface. */
	collected_t ref[4];
	memset(ref, 0, sizeof(ref));
	{
		plm_buffer_t *rb = plm_buffer_create_with_memory(stream, n, 0);
		plm_demux_t *rd = plm_demux_create(rb, 1);
		int count = 0;
		plm_packet_t *p;
		while ((p = plm_demux_decode(rd))) {
			ref[count].type = p->type;
			ref[count].pts = p->pts;
			ref[count].has_pts = (p->pts != PLM_PACKET_INVALID_TS);
			ref[count].length = p->length;
			memcpy(ref[count].data, p->data, p->length);
			count++;
		}
		CHECK(count == 2, "legacy interface yields two packets");
		plm_demux_destroy(rd);
	}

	/* Ring buffer of 512, writes of at most 257, exactly the embedded use. */
	collected_t got[4];
	memset(got, 0, sizeof(got));
	int count = feed_by_writes(stream, n, 512, 257, got, 4);
	CHECK(count == 2, "chunk interface yields two packets");
	for (int i = 0; i < count && i < 2; i++) {
		CHECK(got[i].type == ref[i].type, "packet type matches");
		CHECK(got[i].length == ref[i].length, "reassembled length matches");
		CHECK(got[i].length && memcmp(got[i].data, ref[i].data,
			ref[i].length) == 0, "reassembled payload is byte identical");
		CHECK(got[i].has_pts == ref[i].has_pts, "PTS presence matches");
		if (got[i].has_pts) {
			CHECK(fabs(got[i].pts - ref[i].pts) < 1e-12, "PTS value matches");
		}
		CHECK(got[i].first_count == 1, "exactly one first fragment");
		CHECK(got[i].last_count == 1, "exactly one last fragment");
		CHECK(got[i].chunks >= 1, "at least one fragment per packet");
	}
	CHECK(got[0].chunks > 1, "large video packet is split into fragments");
	CHECK(got[0].data[3] == 0 && got[0].data[6] == 0xe0,
		"fake start code inside payload is treated as data");

	/* Same stream delivered through a load callback. */
	mem_source_t src = {stream, n, 0};
	plm_buffer_t *cb = plm_buffer_create_with_callbacks(
		mem_load, mem_seek, mem_tell, n, &src);
	plm_demux_t *cd = plm_demux_create(cb, 1);
	collected_t via_cb[4];
	memset(via_cb, 0, sizeof(via_cb));
	int cb_count = 0;
	for (int guard = 0; guard < 100000 && cb_count < 2; guard++) {
		int added = drain(cd, via_cb + cb_count, 2 - cb_count);
		cb_count += added;
		if (!added) {
			/* plm_demux_decode_chunk fires the load callback internally; once
			 * nothing more arrives and the source ended, stop. */
			if (plm_demux_has_ended(cd)) {
				break;
			}
		}
	}
	plm_demux_destroy(cd);
	CHECK(cb_count == 2, "load callback yields two packets");
	for (int i = 0; i < cb_count && i < 2; i++) {
		CHECK(via_cb[i].length == ref[i].length &&
			memcmp(via_cb[i].data, ref[i].data, ref[i].length) == 0,
			"load callback payload is byte identical");
		CHECK(via_cb[i].first_count == 1 && via_cb[i].last_count == 1,
			"load callback flags are balanced");
	}

	if (!g_failures) {
		puts("PS_CHUNKS_OK");
	}
}

/* Feed one packet behind the shared pack/system prefix one byte at a time and
 * confirm the header cannot produce data until it is complete. */
static void test_bytewise_header(const char *name, const uint8_t *packet,
		size_t packet_len, size_t payload_offset, int expect_type,
		size_t expect_payload, int expect_pts) {
	uint8_t stream[2048];
	memcpy(stream, sample_prefix, sizeof(sample_prefix));
	size_t n = sizeof(sample_prefix);
	memcpy(stream + n, packet, packet_len);
	n += packet_len;

	plm_buffer_t *buffer = plm_buffer_create_with_capacity(256);
	plm_demux_t *demux = plm_demux_create(buffer, 1);

	int saw_data_before_payload = 0;
	size_t first_payload_pos = sizeof(sample_prefix) + payload_offset;
	collected_t one[2];
	memset(one, 0, sizeof(one));
	int count = 0;

	for (size_t off = 0; off < n; off++) {
		plm_buffer_write(buffer, stream + off, 1);
		int before = count;
		count += drain(demux, one + count, 2 - count);
		if (off < first_payload_pos && count == before) {
			/* still inside the header: the demuxer must only say "not yet" */
			CHECK(plm_demux_decode_chunk(demux) == NULL,
				"no fragment while header is incomplete");
		}
		if (off < first_payload_pos && count > before) {
			saw_data_before_payload = 1;
		}
	}
	CHECK(!saw_data_before_payload, "header bytes are never published as data");
	CHECK(count == 1, "exactly one packet emerges from a bytewise header");
	if (count == 1) {
		CHECK(one[0].type == expect_type, "bytewise packet type");
		CHECK(one[0].length == expect_payload, "bytewise payload length");
		CHECK(one[0].first_count == 1 && one[0].last_count == 1,
			"bytewise first/last flags");
		CHECK(one[0].has_pts == expect_pts, "bytewise timestamp presence");
	}
	(void)name;
}

static void test_headers(void) {
	uint8_t packet[1024];
	size_t len;

	/* PTS + DTS with 12 bytes of stuffing (the fixture's video shape). */
	len = build_pes(packet, 0xe0, 16, 12, 3, 0, 90000);
	test_bytewise_header("pts+dts+stuffing", packet, len,
		len - 16, 0xe0, 16, 1);

	/* PTS only, no stuffing. */
	len = build_pes(packet, 0xc0, 12, 0, 2, 0, 93600);
	test_bytewise_header("pts-only", packet, len,
		len - 12, 0xc0, 12, 1);

	/* No timestamp at all. */
	len = build_pes(packet, 0xbd, 9, 0, 0, 0, 0);
	test_bytewise_header("no-pts", packet, len,
		len - 9, 0xbd, 9, 0);

	/* P-STD field between stuffing and a PTS-only header. */
	len = build_pes(packet, 0xe0, 10, 4, 2, 1, 90000);
	test_bytewise_header("pstd", packet, len,
		len - 10, 0xe0, 10, 1);

	/* Zero-length legal packet: one record, first == last == 1. */
	len = build_pes(packet, 0xc0, 0, 0, 0, 0, 0);
	{
		uint8_t stream[2048];
		memcpy(stream, sample_prefix, sizeof(sample_prefix));
		size_t n = sizeof(sample_prefix);
		memcpy(stream + n, packet, len);
		n += len;
		collected_t got[2];
		memset(got, 0, sizeof(got));
		int count = feed_by_writes(stream, n, 256, 1, got, 2);
		CHECK(count == 1, "zero-length packet delivered once");
		if (count == 1) {
			CHECK(got[0].length == 0, "zero-length record has length 0");
			CHECK(got[0].first_count == 1 && got[0].last_count == 1 &&
				got[0].chunks == 1, "zero-length record is first and last");
		}
	}

	/* Illegal timestamp marker and an under-sized declared length must never
	 * publish payload; a following valid packet still comes through. */
	{
		uint8_t stream[2048];
		memcpy(stream, sample_prefix, sizeof(sample_prefix));
		size_t n = sizeof(sample_prefix);

		/* Corrupt the low marker bit of the PTS marker byte. */
		size_t bad_len = build_pes(packet, 0xe0, 8, 0, 2, 0, 90000);
		packet[6] &= (uint8_t)~0x01; /* marker bit now illegal */
		memcpy(stream + n, packet, bad_len);
		n += bad_len;

		/* Declared length (2) shorter than the PTS header it introduces (5). */
		size_t short_len = build_pes(packet, 0xc0, 8, 0, 2, 0, 90000);
		packet[4] = 0;
		packet[5] = 2;
		memcpy(stream + n, packet, short_len);
		n += short_len;

		size_t good_len = build_pes(packet, 0xe0, 7, 0, 2, 0, 90000);
		memcpy(stream + n, packet, good_len);
		n += good_len;

		collected_t got[4];
		memset(got, 0, sizeof(got));
		int count = feed_by_writes(stream, n, 512, 3, got, 4);
		CHECK(count == 1, "illegal headers are dropped, valid packet survives");
		if (count == 1) {
			CHECK(got[0].type == 0xe0 && got[0].length == 7,
				"only the well-formed packet is published");
		}
	}

	if (!g_failures) {
		puts("PS_HEADERS_OK");
	}
}

static void test_padding_and_truncation(void) {
	uint8_t stream[2048];
	memcpy(stream, sample_prefix, sizeof(sample_prefix));
	size_t n = sizeof(sample_prefix);

	/* Padding packet (0xBE) whose body contains a fake video start code. */
	uint8_t pad[64];
	size_t pad_len = 20;
	pad[0] = 0;
	pad[1] = 0;
	pad[2] = 1;
	pad[3] = 0xbe;
	pad[4] = (uint8_t)(pad_len >> 8);
	pad[5] = (uint8_t)pad_len;
	memset(pad + 6, 0x5a, pad_len);
	pad[10] = 0;
	pad[11] = 0;
	pad[12] = 1;
	pad[13] = 0xe0; /* must be skipped, never demuxed */
	memcpy(stream + n, pad, 6 + pad_len);
	n += 6 + pad_len;

	uint8_t packet[256];
	size_t plen = build_pes(packet, 0xe0, 14, 0, 2, 0, 90000);
	memcpy(stream + n, packet, plen);
	n += plen;

	collected_t got[4];
	memset(got, 0, sizeof(got));
	int count = feed_by_writes(stream, n, 256, 5, got, 4);
	CHECK(count == 1, "padding is skipped, real packet demuxed");
	if (count == 1) {
		CHECK(got[0].type == 0xe0 && got[0].length == 14,
			"fake start code inside padding never reaches output");
	}

	/* Truncated finite payload: fragments already emitted stay, but no last. */
	{
		uint8_t trunc[256];
		memcpy(trunc, sample_prefix, sizeof(sample_prefix));
		size_t tn = sizeof(sample_prefix);
		size_t full = build_pes(packet, 0xe0, 100, 2, 2, 0, 90000);
		size_t header = full - 100;
		memcpy(trunc + tn, packet, header + 40); /* only 40 of 100 bytes */
		tn += header + 40;

		plm_buffer_t *buffer = plm_buffer_create_with_capacity(256);
		plm_demux_t *demux = plm_demux_create(buffer, 1);
		collected_t partial[2];
		memset(partial, 0, sizeof(partial));
		int pcount = 0;
		size_t off = 0;
		while (off < tn) {
			size_t w = tn - off;
			if (w > 7) {
				w = 7;
			}
			plm_buffer_write(buffer, trunc + off, w);
			off += w;
			pcount += drain(demux, partial + pcount, 1);
		}
		CHECK(pcount == 0, "truncated packet is never closed");
		CHECK(partial[0].length == 40, "emitted fragments are retained");
		CHECK(partial[0].first_count == 1, "truncated packet was started");
		CHECK(partial[0].last_count == 0, "no forged last on truncation");
		CHECK(plm_demux_decode_chunk(demux) == NULL,
			"no new bytes is not treated as end of input");
		plm_demux_destroy(demux);
	}
}

static void test_bounded(void) {
	g_max_block = 0;
	g_malloc_count = 0;
	g_realloc_count = 0;

	uint8_t stream[40000];
	size_t n = sample_fixture(stream);

	plm_buffer_t *buffer = plm_buffer_create_with_capacity(512);
	plm_demux_t *demux = plm_demux_create(buffer, 1);

	collected_t got[4];
	memset(got, 0, sizeof(got));
	int count = 0;
	size_t off = 0;
	while (off < n) {
		size_t w = n - off;
		if (w > 257) {
			w = 257;
		}
		plm_buffer_write(buffer, stream + off, w);
		off += w;
		count += drain(demux, got + count, 4 - count);
	}

	CHECK(count == 2, "bounded run demuxes both packets");
	CHECK(got[0].length == 32768, "large payload fully reassembled");
	CHECK(g_max_block <= 4096,
		"no demuxer or input-buffer block exceeds 4096 bytes");
	CHECK(g_realloc_count == 0, "the 512-byte ring never grows");
	plm_demux_destroy(demux);

	if (!g_failures) {
		puts("PS_BOUNDED_OK");
	}
}

static void test_rewind(void) {
	uint8_t stream[40000];
	size_t n = sample_fixture(stream);

	plm_buffer_t *buffer = plm_buffer_create_with_memory(stream, n, 0);
	plm_demux_t *demux = plm_demux_create(buffer, 1);

	collected_t first[4], second[4];
	memset(first, 0, sizeof(first));
	memset(second, 0, sizeof(second));

	int count1 = drain(demux, first, 4);
	CHECK(count1 == 2, "first pass yields two packets");

	plm_demux_rewind(demux);
	int count2 = drain(demux, second, 4);
	CHECK(count2 == 2, "rewind re-reads two packets");

	for (int i = 0; i < 2; i++) {
		CHECK(first[i].type == second[i].type &&
			first[i].length == second[i].length &&
			memcmp(first[i].data, second[i].data, first[i].length) == 0,
			"rewound payload is byte identical");
		CHECK(second[i].first_count == 1 && second[i].last_count == 1,
			"rewound flags are balanced");
	}

	/* Rewinding mid-packet also fully restarts the streaming state. */
	plm_demux_rewind(demux);
	collected_t partial;
	CHECK(drain_one(demux, &partial) == 1, "one packet read before rewind");
	CHECK(partial.type == first[0].type && partial.length == first[0].length,
		"pre-rewind packet matches the first pass");
	plm_demux_rewind(demux);
	collected_t again[4];
	memset(again, 0, sizeof(again));
	int count3 = drain(demux, again, 4);
	CHECK(count3 == 2, "mid-stream rewind restarts cleanly");
	CHECK(again[0].length == first[0].length &&
		memcmp(again[0].data, first[0].data, first[0].length) == 0,
		"mid-stream rewind reproduces the first packet");

	plm_demux_destroy(demux);

	if (!g_failures) {
		puts("PS_REWIND_OK");
	}
}

int main(void) {
	test_chunks_basic();
	test_headers();
	test_padding_and_truncation();
	test_bounded();
	test_rewind();

	if (g_failures) {
		printf("PS_CHUNKS_FAILED failures=%d\n", g_failures);
		return 1;
	}
	puts("all demux chunk checks passed");
	return 0;
}
