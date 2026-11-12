/*
 * pl_mpeg_demux_chunks.c - streaming PES consumer example
 *
 * Demonstrates plm_demux_decode_chunk(), the bounded-memory counterpart to
 * plm_demux_decode(). The demuxer never accumulates a whole PES packet: a
 * fragment is handed out as soon as its header plus at least one payload byte
 * is available, so a small ring buffer is enough even for very large PES
 * bodies.
 *
 * This example is deliberately self contained (no files, network or window):
 * it builds a small valid MPEG-PS stream in memory, feeds it into a 512-byte
 * ring buffer using writes of at most 257 bytes, and forwards each fragment to
 * a consumer immediately.
 *
 * Borrowing / lifetime contract
 * -----------------------------
 * The returned plm_packet_chunk_t (and the `data` pointer inside it) is owned
 * by the demuxer and borrows the input buffer's memory. It is only valid until
 * the next call to plm_demux_decode_chunk(), the next plm_buffer_write() or
 * buffer load callback, a rewind/seek, or the destruction of the demuxer or
 * its buffer. Any of those can move or discard the underlying bytes. So the
 * consumer below copies (or immediately processes) each fragment before
 * driving the demuxer again; it never stores the pointer for later use.
 *
 * Build: cc -std=c99 -O2 pl_mpeg_demux_chunks.c -lm -o pl_mpeg_demux_chunks
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg.h"

#define RING_CAPACITY 512
#define MAX_WRITE 257

/* Minimal MPEG-1 pack + system header, enough for plm_demux_has_headers(). */
static const uint8_t kPackAndSystemHeader[] = {
	0, 0, 1, 0xba, 0x21, 0, 1, 0, 1, 0xc3, 0x33, 0x67,
	0, 0, 1, 0xbb, 0, 9, 0xc3, 0x33, 0x67, 0, 0x21, 0xff, 0xe0, 0xe0, 0xe6
};

static void write_timestamp(uint8_t *p, int prefix, uint64_t value) {
	p[0] = (uint8_t)((prefix << 4) | (((value >> 30) & 7) << 1) | 1);
	p[1] = (uint8_t)(value >> 22);
	p[2] = (uint8_t)((((value >> 15) & 127) << 1) | 1);
	p[3] = (uint8_t)(value >> 7);
	p[4] = (uint8_t)((((uint64_t)value) & 127) << 1) | 1;
}

/* Build one MPEG-1 PES packet with a PTS and deterministic payload. */
static size_t build_pes(uint8_t *p, int type, size_t payload, uint64_t pts90k) {
	size_t n = 0;
	size_t declared = 5 + payload; /* 5-byte PTS header + payload */
	p[n++] = 0;
	p[n++] = 0;
	p[n++] = 1;
	p[n++] = (uint8_t)type;
	p[n++] = (uint8_t)(declared >> 8);
	p[n++] = (uint8_t)declared;
	write_timestamp(p + n, 2, pts90k);
	n += 5;
	for (size_t i = 0; i < payload; i++) {
		p[n + i] = (uint8_t)((i * 17 + type) % 251);
	}
	return n + payload;
}

/* A downstream sink. It owns the bytes it accumulates; it never keeps the
 * demuxer's pointer. */
typedef struct {
	int type;
	size_t total;
	int fragments;
} sink_t;

static void sink_consume(sink_t *sink, const plm_packet_chunk_t *chunk) {
	if (chunk->first) {
		sink->type = chunk->type;
		sink->total = 0;
		sink->fragments = 0;
		printf("packet start type=0x%02x pts=%.3f\n",
			chunk->type,
			chunk->pts == PLM_PACKET_INVALID_TS ? -1.0 : chunk->pts);
	}

	/* The fragment must be consumed here, before the next decode_chunk() /
	 * buffer write. A real embedded consumer would push these bytes straight
	 * into an audio/video buffer; here we just count them. */
	for (size_t i = 0; i < chunk->length; i++) {
		(void)chunk->data[i]; /* process or memcpy() into owned storage */
	}
	sink->total += chunk->length;
	sink->fragments++;

	if (chunk->last) {
		printf("packet end   type=0x%02x bytes=%zu fragments=%d\n",
			sink->type, sink->total, sink->fragments);
	}
}

int main(void) {
	/* Assemble a synthetic stream: header + a large video PES + an audio PES. */
	uint8_t stream[40000];
	size_t length = 0;
	memcpy(stream, kPackAndSystemHeader, sizeof(kPackAndSystemHeader));
	length += sizeof(kPackAndSystemHeader);
	length += build_pes(stream + length, PLM_DEMUX_PACKET_VIDEO_1, 32768, 90000);
	length += build_pes(stream + length, PLM_DEMUX_PACKET_AUDIO_1, 37, 93600);

	/* Ring buffer stays small regardless of the 32 KiB PES body. */
	plm_buffer_t *buffer = plm_buffer_create_with_capacity(RING_CAPACITY);
	plm_demux_t *demux = plm_demux_create(buffer, 1);

	sink_t sink;
	memset(&sink, 0, sizeof(sink));

	size_t offset = 0;
	while (offset < length) {
		size_t write = length - offset;
		if (write > MAX_WRITE) {
			write = MAX_WRITE;
		}
		plm_buffer_write(buffer, stream + offset, write);
		offset += write;

		/* Drain everything currently available. Each returned chunk is
		 * consumed before this loop calls decode_chunk() again. */
		plm_packet_chunk_t *chunk;
		while ((chunk = plm_demux_decode_chunk(demux)) != NULL) {
			sink_consume(&sink, chunk);
		}
	}

	plm_demux_destroy(demux); /* also frees the buffer (destroy_when_done) */
	printf("done\n");
	return 0;
}
