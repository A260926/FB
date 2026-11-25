/* ogg_theora_plan: create and write a lossless Theora/Ogg edit.
 *
 * create() indexes and validates the source and reads every requested cut
 * before returning.  The resulting plan holds only packet indexes, file
 * ranges and the selected headers; no video payload is kept.  write()
 * re-reads the packet bytes from the source and feeds them through the
 * ordinary libogg packetin/pageout/flush machinery, so the output is
 * freshly paged and checksummed each time and the same plan may be written
 * repeatedly (with different serial numbers).
 */

#include "theora_internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <ogg/ogg.h>

static ogg_theora_plan *plan_alloc(void) {
  return calloc(1, sizeof(ogg_theora_plan));
}

/* Read one frame packet's bytes from the source into a fresh buffer. */
static unsigned char *read_packet(ogg_theora_plan *plan,
                                  const theora_frame *frame, size_t *size_out) {
  size_t cap = frame->bytes ? (size_t)frame->bytes : 1;
  unsigned char *buf = malloc(cap);
  if (!buf) return NULL;
  if (theora_read_ranges(plan->source, plan->ranges + frame->range_first,
                         frame->range_count, buf, frame->bytes, cap)) {
    free(buf);
    return NULL;
  }
  *size_out = (size_t)frame->bytes;
  return buf;
}

static int write_page(FILE *output, ogg_page *page) {
  if (fwrite(page->header, 1, (size_t)page->header_len, output) !=
          (size_t)page->header_len)
    return 1;
  if (page->body_len &&
      fwrite(page->body, 1, (size_t)page->body_len, output) !=
          (size_t)page->body_len)
    return 1;
  /* Flushing here makes an I/O failure visible immediately. */
  if (fflush(output)) return 1;
  return 0;
}

static int page_out(ogg_stream_state *os, FILE *output) {
  ogg_page page;
  while (ogg_stream_pageout(os, &page))
    if (write_page(output, &page)) return 1;
  return 0;
}

static int flush_out(ogg_stream_state *os, FILE *output) {
  ogg_page page;
  while (ogg_stream_flush(os, &page))
    if (write_page(output, &page)) return 1;
  return 0;
}

static int add_header(ogg_stream_state *os, FILE *output,
                      const unsigned char *data, size_t len, int bos) {
  ogg_packet op;
  memset(&op, 0, sizeof(op));
  op.packet = (unsigned char *)data;
  op.bytes = (long)len;
  op.b_o_s = bos;
  op.granulepos = 0;
  if (ogg_stream_packetin(os, &op)) return 1;
  return flush_out(os, output);
}

int ogg_theora_plan_create(FILE *source, const ogg_theora_cut *cuts,
                           size_t count, ogg_theora_plan **out) {
  ogg_theora_plan *plan;
  size_t i;
  size_t chosen = (size_t)-1u;
  uint64_t n = 0;
  uint64_t key_out = 0;
  int have_key = 0;

  if (!out) return 1;
  *out = NULL;
  if (!source) return 1;
  if (count == 0) return 1;
  if (!cuts) return 1;

  plan = plan_alloc();
  if (!plan) return 1;
  plan->source = source;

  if (theora_index_build(plan)) goto fail;

  if (plan->stream_count == 0) goto fail;
  for (i = 0; i < plan->stream_count; ++i) {
    const theora_stream *s = &plan->streams[i];
    if (!s->ident || !s->comment || !s->setup) goto fail;
    if (s->ident_len < THEORA_IDENT_MIN || s->setup_len == 0 ||
        s->comment_len == 0)
      goto fail;
  }

  /* Copy the requested cuts and validate each against the index. */
  plan->cuts = malloc(count * sizeof(*plan->cuts));
  if (!plan->cuts) goto fail;
  memcpy(plan->cuts, cuts, count * sizeof(*plan->cuts));
  plan->cut_count = count;

  for (i = 0; i < count; ++i) {
    const ogg_theora_cut *c = &plan->cuts[i];
    theora_stream *s;
    if (c->stream >= plan->stream_count) goto fail;
    s = &plan->streams[c->stream];
    if (c->begin >= c->end) goto fail;             /* empty or reversed */
    if (c->end > s->frame_count) goto fail;        /* out of range */
    if (!plan->frames[s->frame_first + c->begin].keyframe) goto fail;
    s->selected = 1;
  }

  /* The first selected logical stream (the stream of the first cut) supplies
   * the three output headers.  All selected streams share ident and setup
   * byte for byte; only the comment may differ. */
  chosen = cuts[0].stream;

  /* All selected streams must agree byte for byte on ident and setup. */
  for (i = 0; i < plan->stream_count; ++i) {
    const theora_stream *s = &plan->streams[i];
    const theora_stream *c = &plan->streams[chosen];
    if (!s->selected) continue;
    if (s->ident_len != c->ident_len ||
        memcmp(s->ident, c->ident, s->ident_len) != 0)
      goto fail;
    if (s->setup_len != c->setup_len ||
        memcmp(s->setup, c->setup, s->setup_len) != 0)
      goto fail;
  }

  plan->shift = plan->streams[chosen].shift;

  plan->ident = malloc(plan->streams[chosen].ident_len);
  plan->comment = malloc(plan->streams[chosen].comment_len);
  plan->setup = malloc(plan->streams[chosen].setup_len);
  if (!plan->ident || !plan->comment || !plan->setup) goto fail;
  memcpy(plan->ident, plan->streams[chosen].ident,
         plan->streams[chosen].ident_len);
  plan->ident_len = plan->streams[chosen].ident_len;
  memcpy(plan->comment, plan->streams[chosen].comment,
         plan->streams[chosen].comment_len);
  plan->comment_len = plan->streams[chosen].comment_len;
  memcpy(plan->setup, plan->streams[chosen].setup,
         plan->streams[chosen].setup_len);
  plan->setup_len = plan->streams[chosen].setup_len;

  /* Index data for the streams is no longer needed; keep only the chosen
   * headers and the per-frame range table. */
  for (i = 0; i < plan->stream_count; ++i) {
    free(plan->streams[i].ident);
    free(plan->streams[i].comment);
    free(plan->streams[i].setup);
    plan->streams[i].ident = NULL;
    plan->streams[i].comment = NULL;
    plan->streams[i].setup = NULL;
    plan->streams[i].ident_len = 0;
    plan->streams[i].comment_len = 0;
    plan->streams[i].setup_len = 0;
  }

  /* Pre-compute the output clock over the whole edit so create() can reject
   * any granule overflow before a single byte is written. */
  for (i = 0; i < count; ++i) {
    const ogg_theora_cut *c = &plan->cuts[i];
    const theora_stream *s = &plan->streams[c->stream];
    uint64_t b;
    for (b = c->begin; b < c->end; ++b) {
      const theora_frame *f = &plan->frames[s->frame_first + b];
      ogg_int64_t g;
      if (f->keyframe) {
        key_out = n;
        have_key = 1;
      }
      if (!have_key) goto fail;
      if (theora_calc_granule(plan->shift, key_out, n, &g)) goto fail;
      ++n;
    }
  }
  plan->total_frames = n;

  *out = plan;
  return 0;

fail:
  ogg_theora_plan_destroy(plan);
  return 1;
}

int ogg_theora_plan_write(ogg_theora_plan *plan, FILE *output, int serial) {
  ogg_stream_state os;
  int os_init = 0;
  int rc = 1;
  uint64_t n = 0;
  uint64_t key_out = 0;
  int have_key = 0;
  size_t ci;
  unsigned char *buf = NULL;
  size_t buf_len = 0;

  if (!plan || !output) return 1;
  if (output == plan->source) return 1;
  if (plan->total_frames == 0) return 1;
  if (!plan->ident || !plan->comment || !plan->setup) return 1;

  if (ogg_stream_init(&os, serial)) return 1;
  os_init = 1;

  /* Headers, one per page, at time zero.  The ident page is BOS. */
  if (add_header(&os, output, plan->ident, plan->ident_len, 1)) goto done;
  if (add_header(&os, output, plan->comment, plan->comment_len, 0)) goto done;
  if (add_header(&os, output, plan->setup, plan->setup_len, 0)) goto done;

  for (ci = 0; ci < plan->cut_count; ++ci) {
    const ogg_theora_cut *c = &plan->cuts[ci];
    const theora_stream *s = &plan->streams[c->stream];
    uint64_t b;
    for (b = c->begin; b < c->end; ++b) {
      const theora_frame *f = &plan->frames[s->frame_first + b];
      ogg_packet op;
      ogg_int64_t g;

      if (f->keyframe) {
        key_out = n;
        have_key = 1;
      }
      if (!have_key) goto done;
      if (theora_calc_granule(plan->shift, key_out, n, &g)) goto done;

      free(buf);
      buf = read_packet(plan, f, &buf_len);
      if (!buf) goto done;

      memset(&op, 0, sizeof(op));
      op.packet = buf;
      op.bytes = (long)buf_len;
      op.packetno = (ogg_int64_t)n;
      op.granulepos = g;
      op.e_o_s = (n + 1 == plan->total_frames) ? 1 : 0;
      if (ogg_stream_packetin(&os, &op)) goto done;
      if (page_out(&os, output)) goto done;
      ++n;
    }
  }

  /* Drain everything left, including the EOS page. */
  if (flush_out(&os, output)) goto done;
  if (ferror(output)) goto done;
  rc = 0;

done:
  free(buf);
  if (os_init) ogg_stream_clear(&os);
  return rc;
}

void ogg_theora_plan_destroy(ogg_theora_plan *plan) {
  if (!plan) return;
  theora_plan_free_contents(plan);
  free(plan);
}
