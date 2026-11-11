/* native_chain - regression driver for chained native FLAC decoding
 *
 * Feeds the decoder a sequence of complete, self-contained native FLAC
 * streams ("links") through the read callback, returning
 * FLAC__STREAM_DECODER_READ_STATUS_END_OF_LINK between links and
 * FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM after the last one.
 *
 * Every link has distinct channels, sample rate and frame count, and
 * carries per-channel integer samples that are checked exactly. The
 * driver also checks callback ordering (STREAMINFO before audio of the
 * same link, previous link fully delivered before the next link's
 * header), per-link sample numbering, decoder states and MD5 handling.
 */

#undef NDEBUG
#include <FLAC/stream_encoder.h>
#include <FLAC/stream_decoder.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LINKS 3
#define BLOCKSIZE 64
/* offset of the MD5 signature inside a FLAC stream: 4 bytes "fLaC" +
 * 4 bytes metadata block header + 18 bytes of STREAMINFO up to the MD5 */
#define MD5_OFFSET 26

static const int link_channels[MAX_LINKS] = { 1, 2, 3 };
static const unsigned link_rate[MAX_LINKS] = { 44100, 48000, 32000 };
static const int link_frames[MAX_LINKS] = { 2, 3, 1 };

/* distinct, exactly representable per-channel sample values */
static int sample_value(int link, int ch, int i)
{
	return link * 4096 + ch * 256 + (i % 251);
}

typedef struct {
	FLAC__byte *data;
	size_t len, cap;
	size_t rw_pos; /* write position while encoding (seekable) */
} Buf;

typedef struct {
	Buf *links;
	int nlinks;
	int one_byte;
	/* read callback state */
	int part;
	size_t pos[MAX_LINKS];
	size_t bytes_per_part[MAX_LINKS];
	/* verification state */
	int cur_link;
	int frames_in_link;
	unsigned samples_in_link;
	int streaminfo_count;
	int streaminfo_seen[MAX_LINKS];
	int writes_started[MAX_LINKS];
	int frames_per_link[MAX_LINKS];
	int meta_blocks_per_link[MAX_LINKS];
	int frames_total;
} DCtx;

/***********************************************************************
 * Encoding: build the links in memory
 ***********************************************************************/

static FLAC__StreamEncoderWriteStatus enc_wr(const FLAC__StreamEncoder *encoder, const FLAC__byte buffer[], size_t bytes, uint32_t samples, uint32_t current_frame, void *client_data)
{
	Buf *b = (Buf*)client_data;
	(void)encoder; (void)samples; (void)current_frame;
	if(b->rw_pos + bytes > b->cap) {
		b->cap = (b->rw_pos + bytes) * 2 + 4096;
		b->data = realloc(b->data, b->cap);
		assert(b->data != NULL);
	}
	memcpy(b->data + b->rw_pos, buffer, bytes);
	b->rw_pos += bytes;
	if(b->rw_pos > b->len)
		b->len = b->rw_pos;
	return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
}

static FLAC__StreamEncoderSeekStatus enc_seek(const FLAC__StreamEncoder *encoder, FLAC__uint64 absolute_byte_offset, void *client_data)
{
	Buf *b = (Buf*)client_data;
	(void)encoder;
	if(absolute_byte_offset > b->len)
		return FLAC__STREAM_ENCODER_SEEK_STATUS_ERROR;
	b->rw_pos = (size_t)absolute_byte_offset;
	return FLAC__STREAM_ENCODER_SEEK_STATUS_OK;
}

static FLAC__StreamEncoderTellStatus enc_tell(const FLAC__StreamEncoder *encoder, FLAC__uint64 *absolute_byte_offset, void *client_data)
{
	Buf *b = (Buf*)client_data;
	(void)encoder;
	*absolute_byte_offset = b->rw_pos;
	return FLAC__STREAM_ENCODER_TELL_STATUS_OK;
}

static void encode_link(Buf *buf, int L)
{
	FLAC__StreamEncoder *e;
	FLAC__int32 *pcm;
	int ns = link_frames[L] * BLOCKSIZE;
	int i, ch;

	memset(buf, 0, sizeof(*buf));
	e = FLAC__stream_encoder_new();
	assert(e != NULL);
	assert(FLAC__stream_encoder_set_channels(e, (unsigned)link_channels[L]));
	assert(FLAC__stream_encoder_set_bits_per_sample(e, 16));
	assert(FLAC__stream_encoder_set_sample_rate(e, link_rate[L]));
	assert(FLAC__stream_encoder_set_blocksize(e, BLOCKSIZE));
	assert(FLAC__stream_encoder_init_stream(e, enc_wr, enc_seek, enc_tell, NULL, buf) == FLAC__STREAM_ENCODER_INIT_STATUS_OK);
	pcm = malloc(sizeof(FLAC__int32) * (size_t)ns * (size_t)link_channels[L]);
	assert(pcm != NULL);
	for(i = 0; i < ns; i++)
		for(ch = 0; ch < link_channels[L]; ch++)
			pcm[i * link_channels[L] + ch] = sample_value(L, ch, i);
	assert(FLAC__stream_encoder_process_interleaved(e, pcm, (unsigned)ns));
	assert(FLAC__stream_encoder_finish(e));
	FLAC__stream_encoder_delete(e);
	free(pcm);
	/* sanity: STREAMINFO is the first metadata block, and the encoder
	 * rewrote it with a real (non-zero) MD5 signature */
	assert(buf->len > MD5_OFFSET + 16);
	assert(0 == memcmp(buf->data, "fLaC", 4));
	assert((buf->data[4] & 0x7f) == 0 /* STREAMINFO */);
	assert(0 != memcmp(buf->data + MD5_OFFSET, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 16));
}

/* overwrite the link's STREAMINFO MD5 signature with a non-zero wrong value */
static void corrupt_md5(Buf *buf)
{
	memset(buf->data + MD5_OFFSET, 0x5A, 16);
}

/* blank the link's STREAMINFO MD5 signature ("no signature") */
static void zero_md5(Buf *buf)
{
	memset(buf->data + MD5_OFFSET, 0, 16);
}

/***********************************************************************
 * Decoding: read/write/metadata/error callbacks with verification
 ***********************************************************************/

static FLAC__StreamDecoderReadStatus dec_rd(const FLAC__StreamDecoder *decoder, FLAC__byte buffer[], size_t *bytes, void *client_data)
{
	DCtx *c = (DCtx*)client_data;
	Buf *cur;
	size_t avail, want = *bytes;
	(void)decoder;

	assert(c->part < c->nlinks);
	cur = &c->links[c->part];
	avail = cur->len - c->pos[c->part];
	if(avail == 0) {
		*bytes = 0;
		if(c->part + 1 < c->nlinks) {
			c->part++;
			return FLAC__STREAM_DECODER_READ_STATUS_END_OF_LINK;
		}
		return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
	}
	if(c->one_byte && want > 1)
		want = 1;
	if(want > avail)
		want = avail;
	memcpy(buffer, cur->data + c->pos[c->part], want);
	c->pos[c->part] += want;
	c->bytes_per_part[c->part] += want;
	*bytes = want;
	return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}

static FLAC__StreamDecoderWriteStatus dec_wr(const FLAC__StreamDecoder *decoder, const FLAC__Frame *frame, const FLAC__int32 *const buffer[], void *client_data)
{
	DCtx *c = (DCtx*)client_data;
	int L = c->cur_link;
	unsigned i;
	int ch;
	(void)decoder;

	/* the link's own STREAMINFO must have been delivered before its audio */
	assert(c->streaminfo_seen[L]);
	c->writes_started[L] = 1;
	/* format of this link */
	assert(frame->header.channels == (unsigned)link_channels[L]);
	assert(frame->header.sample_rate == link_rate[L]);
	assert(frame->header.bits_per_sample == 16);
	assert(frame->header.blocksize == BLOCKSIZE);
	/* sample numbering is interpreted afresh from this link's own header */
	assert(frame->header.number_type == FLAC__FRAME_NUMBER_TYPE_SAMPLE_NUMBER);
	assert(frame->header.number.sample_number == (FLAC__uint64)c->samples_in_link);
	/* exact PCM */
	for(i = 0; i < frame->header.blocksize; i++) {
		int idx = (int)(c->samples_in_link + i);
		for(ch = 0; ch < link_channels[L]; ch++)
			assert(buffer[ch][i] == sample_value(L, ch, idx));
	}
	c->samples_in_link += frame->header.blocksize;
	c->frames_in_link++;
	c->frames_per_link[L]++;
	c->frames_total++;
	return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

static void dec_md(const FLAC__StreamDecoder *decoder, const FLAC__StreamMetadata *metadata, void *client_data)
{
	DCtx *c = (DCtx*)client_data;
	(void)decoder;

	if(metadata->type == FLAC__METADATA_TYPE_STREAMINFO) {
		int L = c->streaminfo_count;
		assert(L < c->nlinks);
		if(L > 0) {
			/* the previous link must have been fully delivered
			 * before the next link's header shows up */
			assert(c->frames_in_link == link_frames[L-1]);
			assert(c->samples_in_link == (unsigned)(link_frames[L-1] * BLOCKSIZE));
		}
		assert(metadata->data.stream_info.channels == (unsigned)link_channels[L]);
		assert(metadata->data.stream_info.sample_rate == link_rate[L]);
		assert(metadata->data.stream_info.bits_per_sample == 16);
		assert(metadata->data.stream_info.total_samples == (FLAC__uint64)(link_frames[L] * BLOCKSIZE));
		c->streaminfo_seen[L] = 1;
		c->streaminfo_count++;
		c->cur_link = L;
		c->frames_in_link = 0;
		c->samples_in_link = 0;
	}
	else {
		/* other metadata belongs to the current link and must
		 * arrive before its first audio frame */
		assert(c->streaminfo_seen[c->cur_link]);
		assert(!c->writes_started[c->cur_link]);
	}
	c->meta_blocks_per_link[c->cur_link]++;
}

static void dec_er(const FLAC__StreamDecoder *decoder, FLAC__StreamDecoderErrorStatus status, void *client_data)
{
	(void)decoder; (void)status; (void)client_data;
	/* none of the scenarios may produce a decode error */
	abort();
}

/***********************************************************************
 * Scenario helpers
 ***********************************************************************/

static Buf g_links[MAX_LINKS];

static void init_ctx(DCtx *c, Buf *links, int nlinks, int one_byte)
{
	memset(c, 0, sizeof(*c));
	c->links = links;
	c->nlinks = nlinks;
	c->one_byte = one_byte;
}

static FLAC__StreamDecoder *new_decoder(DCtx *c, int chained)
{
	FLAC__StreamDecoder *d = FLAC__stream_decoder_new();
	assert(d != NULL);
	assert(FLAC__stream_decoder_set_md5_checking(d, 1));
	if(chained) {
		assert(FLAC__stream_decoder_set_decode_chained_stream(d, 1));
		assert(FLAC__stream_decoder_get_decode_chained_stream(d));
	}
	assert(FLAC__stream_decoder_init_stream(d, dec_rd, NULL, NULL, NULL, NULL, dec_wr, dec_md, dec_er, c) == FLAC__STREAM_DECODER_INIT_STATUS_OK);
	return d;
}

static void expect_link_done(DCtx *c, int L)
{
	assert(c->streaminfo_seen[L]);
	assert(c->frames_per_link[L] == link_frames[L]);
}

static void expect_all_links_done(DCtx *c)
{
	int L;
	for(L = 0; L < c->nlinks; L++)
		expect_link_done(c, L);
	assert(c->streaminfo_count == c->nlinks);
}

/***********************************************************************
 * Scenarios
 ***********************************************************************/

/* single link, chained decoding enabled: ends in plain END_OF_STREAM */
static void scen_single(void)
{
	DCtx c;
	FLAC__StreamDecoder *d;

	init_ctx(&c, g_links, 1, 0);
	d = FLAC__stream_decoder_new();
	assert(d != NULL);
	/* the flag defaults to false, can be set before init and read back */
	assert(!FLAC__stream_decoder_get_decode_chained_stream(d));
	assert(FLAC__stream_decoder_set_md5_checking(d, 1));
	assert(FLAC__stream_decoder_set_decode_chained_stream(d, 1));
	assert(FLAC__stream_decoder_get_decode_chained_stream(d));
	assert(FLAC__stream_decoder_init_stream(d, dec_rd, NULL, NULL, NULL, NULL, dec_wr, dec_md, dec_er, &c) == FLAC__STREAM_DECODER_INIT_STATUS_OK);
	/* once initialized, changing the flag is rejected */
	assert(!FLAC__stream_decoder_set_decode_chained_stream(d, 0));
	assert(FLAC__stream_decoder_get_decode_chained_stream(d));

	assert(FLAC__stream_decoder_process_until_end_of_stream(d));
	assert(FLAC__stream_decoder_get_state(d) == FLAC__STREAM_DECODER_END_OF_STREAM);
	expect_all_links_done(&c);
	assert(FLAC__stream_decoder_finish(d));
	FLAC__stream_decoder_delete(d);
	puts("single_link ok");
}

/* two links, chained decoding disabled: first link boundary is end of stream */
static void scen_flag_off(void)
{
	DCtx c;
	FLAC__StreamDecoder *d;

	init_ctx(&c, g_links, 2, 0);
	d = FLAC__stream_decoder_new();
	assert(d != NULL);
	assert(!FLAC__stream_decoder_get_decode_chained_stream(d)); /* default */
	assert(FLAC__stream_decoder_set_md5_checking(d, 1));
	assert(FLAC__stream_decoder_init_stream(d, dec_rd, NULL, NULL, NULL, NULL, dec_wr, dec_md, dec_er, &c) == FLAC__STREAM_DECODER_INIT_STATUS_OK);

	assert(FLAC__stream_decoder_process_until_end_of_stream(d));
	assert(FLAC__stream_decoder_get_state(d) == FLAC__STREAM_DECODER_END_OF_STREAM);
	/* only the first link was decoded ... */
	expect_link_done(&c, 0);
	assert(c.streaminfo_count == 1);
	assert(c.frames_per_link[1] == 0);
	/* ... and the second link was never touched: its header was not
	 * read and its metadata was not published */
	assert(c.bytes_per_part[1] == 0);
	assert(FLAC__stream_decoder_finish(d));
	FLAC__stream_decoder_delete(d);
	puts("flag_off_two_links ok");
}

/* three links, chained decoding enabled, manual link-by-link processing */
static void scen_manual_three(void)
{
	DCtx c;
	FLAC__StreamDecoder *d;
	int L;

	init_ctx(&c, g_links, 3, 0);
	d = new_decoder(&c, 1);

	for(L = 0; L < 3; L++) {
		assert(FLAC__stream_decoder_process_until_end_of_link(d));
		expect_link_done(&c, L);
		if(L < 2) {
			assert(FLAC__stream_decoder_get_state(d) == FLAC__STREAM_DECODER_END_OF_LINK);
			assert(FLAC__stream_decoder_finish_link(d));
		}
		else {
			/* physical end of the last link is plain END_OF_STREAM */
			assert(FLAC__stream_decoder_get_state(d) == FLAC__STREAM_DECODER_END_OF_STREAM);
		}
	}
	assert(c.streaminfo_count == 3);
	assert(FLAC__stream_decoder_finish(d));
	FLAC__stream_decoder_delete(d);
	puts("manual_three_links ok");
}

/* three links, chained decoding enabled, automatic whole-stream processing */
static void scen_auto_three(void)
{
	DCtx c;
	FLAC__StreamDecoder *d;
	int L;

	init_ctx(&c, g_links, 3, 0);
	d = FLAC__stream_decoder_new();
	assert(d != NULL);
	assert(FLAC__stream_decoder_set_md5_checking(d, 1));
	assert(FLAC__stream_decoder_set_decode_chained_stream(d, 1));
	/* a metadata filter set beforehand keeps applying to every link */
	assert(FLAC__stream_decoder_set_metadata_respond_all(d));
	assert(FLAC__stream_decoder_init_stream(d, dec_rd, NULL, NULL, NULL, NULL, dec_wr, dec_md, dec_er, &c) == FLAC__STREAM_DECODER_INIT_STATUS_OK);

	assert(FLAC__stream_decoder_process_until_end_of_stream(d));
	assert(FLAC__stream_decoder_get_state(d) == FLAC__STREAM_DECODER_END_OF_STREAM);
	expect_all_links_done(&c);
	/* every link delivered the same (non-trivial) set of metadata blocks */
	assert(c.meta_blocks_per_link[0] >= 2);
	for(L = 1; L < 3; L++)
		assert(c.meta_blocks_per_link[L] == c.meta_blocks_per_link[0]);
	assert(FLAC__stream_decoder_finish(d));
	FLAC__stream_decoder_delete(d);
	puts("auto_three_links ok");
}

/* three links driven through FLAC__stream_decoder_process_single() */
static void scen_process_single(void)
{
	DCtx c;
	FLAC__StreamDecoder *d;

	init_ctx(&c, g_links, 3, 0);
	d = new_decoder(&c, 1);

	while(1) {
		FLAC__StreamDecoderState st;
		assert(FLAC__stream_decoder_process_single(d));
		st = FLAC__stream_decoder_get_state(d);
		if(st == FLAC__STREAM_DECODER_END_OF_LINK) {
			/* process_single leaves the decoder on the link boundary */
			assert(FLAC__stream_decoder_finish_link(d));
		}
		else if(st == FLAC__STREAM_DECODER_END_OF_STREAM)
			break;
	}
	expect_all_links_done(&c);
	assert(FLAC__stream_decoder_finish(d));
	FLAC__stream_decoder_delete(d);
	puts("process_single_three_links ok");
}

/* two links, read callback hands over a single byte per call */
static void scen_one_byte(void)
{
	DCtx c;
	FLAC__StreamDecoder *d;

	init_ctx(&c, g_links, 2, 1);
	d = new_decoder(&c, 1);

	assert(FLAC__stream_decoder_process_until_end_of_stream(d));
	assert(FLAC__stream_decoder_get_state(d) == FLAC__STREAM_DECODER_END_OF_STREAM);
	expect_all_links_done(&c);
	assert(FLAC__stream_decoder_finish(d));
	FLAC__stream_decoder_delete(d);
	puts("one_byte_reads ok");
}

/* middle link carries a wrong (non-zero) MD5: automatic processing must
 * fail at that link's boundary and must not deliver the next link */
static void scen_md5_auto_bad(void)
{
	DCtx c;
	FLAC__StreamDecoder *d;
	Buf links[MAX_LINKS];
	int L;

	for(L = 0; L < 3; L++) {
		links[L] = g_links[L];
		links[L].data = malloc(g_links[L].len);
		assert(links[L].data != NULL);
		memcpy(links[L].data, g_links[L].data, g_links[L].len);
	}
	corrupt_md5(&links[1]);

	init_ctx(&c, links, 3, 0);
	d = new_decoder(&c, 1);

	assert(!FLAC__stream_decoder_process_until_end_of_stream(d));
	/* stopped on the boundary of the link with the bad signature */
	assert(FLAC__stream_decoder_get_state(d) == FLAC__STREAM_DECODER_END_OF_LINK);
	/* the audio before and inside the bad link was delivered and checked */
	expect_link_done(&c, 0);
	expect_link_done(&c, 1);
	/* the next link was not started: no metadata, no audio, not even read */
	assert(c.streaminfo_count == 2);
	assert(c.frames_per_link[2] == 0);
	assert(c.bytes_per_part[2] == 0);
	/* the MD5 failure is still reported at finish() */
	assert(!FLAC__stream_decoder_finish(d));
	FLAC__stream_decoder_delete(d);
	for(L = 0; L < 3; L++)
		free(links[L].data);
	puts("md5_auto_bad_link ok");
}

/* middle link carries a wrong (non-zero) MD5, last link carries none:
 * manual finish_link reports the bad link, the link without signature
 * follows the usual no-check rule */
static void scen_md5_manual_bad(void)
{
	DCtx c;
	FLAC__StreamDecoder *d;
	Buf links[MAX_LINKS];
	int L;

	for(L = 0; L < 3; L++) {
		links[L] = g_links[L];
		links[L].data = malloc(g_links[L].len);
		assert(links[L].data != NULL);
		memcpy(links[L].data, g_links[L].data, g_links[L].len);
	}
	corrupt_md5(&links[1]);
	zero_md5(&links[2]);

	init_ctx(&c, links, 3, 0);
	d = new_decoder(&c, 1);

	assert(FLAC__stream_decoder_process_until_end_of_link(d));
	assert(FLAC__stream_decoder_get_state(d) == FLAC__STREAM_DECODER_END_OF_LINK);
	expect_link_done(&c, 0);
	assert(FLAC__stream_decoder_finish_link(d)); /* first link checks out */

	assert(FLAC__stream_decoder_process_until_end_of_link(d));
	assert(FLAC__stream_decoder_get_state(d) == FLAC__STREAM_DECODER_END_OF_LINK);
	expect_link_done(&c, 1);
	assert(!FLAC__stream_decoder_finish_link(d)); /* bad signature reported */

	assert(FLAC__stream_decoder_process_until_end_of_link(d));
	assert(FLAC__stream_decoder_get_state(d) == FLAC__STREAM_DECODER_END_OF_STREAM);
	expect_link_done(&c, 2);
	assert(c.streaminfo_count == 3);
	assert(FLAC__stream_decoder_finish(d)); /* no signature in last link: no check */
	FLAC__stream_decoder_delete(d);
	for(L = 0; L < 3; L++)
		free(links[L].data);
	puts("md5_manual_bad_link ok");
}

int main(void)
{
	int L;

	for(L = 0; L < MAX_LINKS; L++)
		encode_link(&g_links[L], L);

	scen_single();
	scen_flag_off();
	scen_manual_three();
	scen_auto_three();
	scen_process_single();
	scen_one_byte();
	scen_md5_auto_bad();
	scen_md5_manual_bad();

	for(L = 0; L < MAX_LINKS; L++)
		free(g_links[L].data);

	puts("NATIVE_CHAIN_DRIVER_OK");
	return 0;
}
