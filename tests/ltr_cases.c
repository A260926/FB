#include "task_support/check.h"

#include "kvazaar.h"
#include "kvazaar_internal.h"
#include "encoderstate.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_W 64
#define TEST_H 64
#define TEST_FRAMES 48

/* ------------------------------------------------------------------ */
/* Small byte buffer and API encoding harness                          */
/* ------------------------------------------------------------------ */

typedef struct {
  unsigned char *data;
  size_t len;
  size_t cap;
} ByteBuffer;

static int buf_append(ByteBuffer *b, const void *src, size_t n)
{
  if (n == 0) return 0;
  if (b->len + n > b->cap) {
    size_t cap = b->cap ? b->cap * 2 : 4096;
    while (cap < b->len + n) cap *= 2;
    unsigned char *p = realloc(b->data, cap);
    if (!p) return -1;
    b->data = p;
    b->cap = cap;
  }
  memcpy(b->data + b->len, src, n);
  b->len += n;
  return 0;
}

static void buf_free(ByteBuffer *b)
{
  free(b->data);
  memset(b, 0, sizeof(*b));
}

typedef struct {
  int done;
  ByteBuffer bits;
  kvz_picture *rec[96];
  kvz_frame_info info[96];
} EncodeResult;

static void result_free(const kvz_api *api, EncodeResult *r)
{
  for (int i = 0; i < 96; ++i) {
    if (r->rec[i]) api->picture_free(r->rec[i]);
  }
  buf_free(&r->bits);
  memset(r, 0, sizeof(*r));
}

typedef struct {
  kvz_config *cfg;
  kvz_encoder *enc;
} EncoderCtx;

typedef struct {
  int owf;
  int threads;
  int wpp;
  int ref_frames;
  int ltr_slots;
} EncOpts;

static int ctx_open(const kvz_api *api, const EncOpts *opts, EncoderCtx *ctx)
{
  EncOpts o = *opts;
  if (o.ref_frames == 0) o.ref_frames = 2;
  if (o.ltr_slots == 0) o.ltr_slots = 0;

  ctx->cfg = api->config_alloc();
  if (!ctx->cfg || !api->config_init(ctx->cfg)) return 0;
  ctx->cfg->enable_logging_output = 0;
  if (!api->config_parse(ctx->cfg, "preset", "ultrafast")) return 0;

  ctx->cfg->width = TEST_W;
  ctx->cfg->height = TEST_H;
  ctx->cfg->gop_len = 0;
  ctx->cfg->intra_period = 0;
  ctx->cfg->bipred = 0;
  ctx->cfg->tmvp_enable = 0;
  ctx->cfg->ref_frames = o.ref_frames;
  ctx->cfg->owf = o.owf;
  ctx->cfg->threads = o.threads > 0 ? o.threads : 2;
  ctx->cfg->wpp = o.wpp;
  ctx->cfg->cpuid = 0;
  ctx->cfg->qp = 24;
  ctx->cfg->hash = KVZ_HASH_NONE;
  ctx->cfg->sao_type = 0;
  ctx->cfg->enable_logging_output = 0;

  if (!api->config_parse(ctx->cfg, "ltr-slots",
                         o.ltr_slots == 2 ? "2" : "0")) {
    return 0;
  }
  if (ctx->cfg->ltr_slots != o.ltr_slots) return 0;

  ctx->enc = api->encoder_open(ctx->cfg);
  return ctx->enc != NULL;
}

static void ctx_close(const kvz_api *api, EncoderCtx *ctx)
{
  if (ctx->enc) api->encoder_close(ctx->enc);
  if (ctx->cfg) api->config_destroy(ctx->cfg);
  ctx->enc = NULL;
  ctx->cfg = NULL;
}

static kvz_picture *make_picture(const kvz_api *api, int n)
{
  kvz_picture *p = api->picture_alloc(TEST_W, TEST_H);
  if (!p) return NULL;
  p->pts = n;
  p->ltr_mark = -1;
  p->ltr_use = -1;

  const int static_picture = (n < 8 || n >= 24);
  for (int y = 0; y < TEST_H; ++y) {
    for (int x = 0; x < TEST_W; ++x) {
      uint8_t v;
      if (static_picture) {
        int block = ((x >> 3) ^ (y >> 3)) & 1;
        int diagonal = ((x + 2 * y) % 17) < 2;
        v = block ? 188 : 58;
        if (diagonal) v = (uint8_t)(v < 100 ? 220 : 28);
        if ((x % 16) == 0 || (y % 16) == 0) v = (uint8_t)(v / 2);
      } else {
        int xx = (x + 5 * n) & 63;
        int yy = (y + 3 * n) & 63;
        v = (uint8_t)(((xx * 7 + yy * 11 + n * 23) ^ ((xx + n) % 31)) & 255);
      }
      p->y[y * p->stride + x] = v;
    }
  }

  for (int y = 0; y < p->height_c; ++y) {
    for (int x = 0; x < p->width_c; ++x) {
      p->u[y * p->stride_c + x] = static_picture ? 110 : (uint8_t)(128 + n);
      p->v[y * p->stride_c + x] = static_picture ? 150 : (uint8_t)(96 + n * 2);
    }
  }
  return p;
}

static int collect_output(const kvz_api *api,
                          int ok,
                          kvz_data_chunk *data,
                          uint32_t len,
                          kvz_picture *rec,
                          kvz_picture *src,
                          const kvz_frame_info *info,
                          EncodeResult *r)
{
  if (!ok) return -1;

  uint32_t counted = 0;
  for (kvz_data_chunk *q = data; q; q = q->next) {
    counted += q->len;
    if (buf_append(&r->bits, q->data, q->len)) return -1;
  }
  if (counted != len) return -1;

  if (rec) {
    if (!src || !data || len == 0 || !info || info->poc < 0 || info->poc >= 96) {
      return -1;
    }
    if (r->rec[info->poc] != NULL) return -1;
    r->rec[info->poc] = rec;
    r->info[info->poc] = *info;
    ++r->done;
    api->picture_free(src);
  } else {
    if (src || data || len) return -1;
    api->picture_free(rec);
    api->picture_free(src);
  }

  api->chunk_free(data);
  return 0;
}

static int send_picture(const kvz_api *api, EncoderCtx *ctx,
                        kvz_picture *p, EncodeResult *r)
{
  kvz_data_chunk *data = NULL;
  uint32_t len = 0;
  kvz_picture *rec = NULL, *src = NULL;
  kvz_frame_info info;
  memset(&info, 0, sizeof(info));

  int ok = api->encoder_encode(ctx->enc, p, &data, &len, &rec, &src, &info);
  /* The command is a value in the public struct; releasing the caller's
   * reference immediately after this call must not change its meaning. */
  api->picture_free(p);
  return collect_output(api, ok, data, len, rec, src, &info, r);
}

static int call_null(const kvz_api *api, EncoderCtx *ctx, EncodeResult *r)
{
  kvz_data_chunk *data = NULL;
  uint32_t len = 0;
  kvz_picture *rec = NULL, *src = NULL;
  kvz_frame_info info;
  memset(&info, 0, sizeof(info));
  int ok = api->encoder_encode(ctx->enc, NULL, &data, &len, &rec, &src, &info);
  return collect_output(api, ok, data, len, rec, src, &info, r);
}

static void standard_commands(int n, int *mark, int *use)
{
  *mark = -1;
  *use = -1;
  if (n == 0) *mark = 0;
  if (n == 4) *mark = 1;
  if (n == 24 || n == 40) *use = 0;
  if (n == 32) *use = 1;
}

static int run_standard(const kvz_api *api, EncOpts opts,
                        EncodeResult *r, EncoderCtx *kept)
{
  EncoderCtx ctx = {0};
  if (!ctx_open(api, &opts, &ctx)) return -1;

  for (int n = 0; n < TEST_FRAMES; ++n) {
    kvz_picture *p = make_picture(api, n);
    int mark, use;
    standard_commands(n, &mark, &use);
    if (!p) goto fail;
    p->ltr_mark = mark;
    p->ltr_use = use;
    if (send_picture(api, &ctx, p, r)) goto fail;
  }

  for (int i = 0; i < 24 && r->done < TEST_FRAMES; ++i) {
    if (call_null(api, &ctx, r)) goto fail;
  }
  if (r->done != TEST_FRAMES) goto fail;

  if (kept) *kept = ctx;
  else ctx_close(api, &ctx);
  return 0;

fail:
  ctx_close(api, &ctx);
  return -1;
}

/* ------------------------------------------------------------------ */
/* Annex-B NAL extraction and a small HEVC bit reader                 */
/* ------------------------------------------------------------------ */

typedef struct {
  int type;
  uint8_t *rbsp;
  size_t len;
} ParsedNal;

static void free_nals(ParsedNal *nals, int count)
{
  if (!nals) return;
  for (int i = 0; i < count; ++i) free(nals[i].rbsp);
  free(nals);
}

static int extract_nals(const ByteBuffer *b, ParsedNal **out_nals, int *out_count)
{
  *out_nals = NULL;
  *out_count = 0;

  int starts[512];
  int sc_len[512];
  int count = 0;
  for (size_t i = 0; i + 3 < b->len && count < 512; ) {
    if (i + 4 <= b->len && b->data[i] == 0 && b->data[i + 1] == 0 &&
        b->data[i + 2] == 0 && b->data[i + 3] == 1) {
      starts[count] = (int)i + 4;
      sc_len[count] = 4;
      ++count;
      i += 4;
    } else if (b->data[i] == 0 && b->data[i + 1] == 0 && b->data[i + 2] == 1) {
      starts[count] = (int)i + 3;
      sc_len[count] = 3;
      ++count;
      i += 3;
    } else {
      ++i;
    }
  }

  ParsedNal *nals = calloc((size_t)count > 0 ? count : 1, sizeof(*nals));
  if (!nals) return -1;

  for (int i = 0; i < count; ++i) {
    int end = (i + 1 < count) ? starts[i + 1] - sc_len[i + 1] : (int)b->len;
    int begin = starts[i];
    if (end <= begin + 2) continue;
    size_t raw_len = (size_t)(end - begin);
    const uint8_t *raw = b->data + begin;
    uint8_t *rbsp = malloc(raw_len);
    if (!rbsp) { free_nals(nals, count); return -1; }

    size_t w = 0;
    for (size_t j = 0; j < raw_len; ++j) {
      if (j + 2 < raw_len && raw[j] == 0 && raw[j + 1] == 0 && raw[j + 2] == 3) {
        rbsp[w++] = 0;
        rbsp[w++] = 0;
        j += 2;
      } else {
        rbsp[w++] = raw[j];
      }
    }

    nals[i].type = (raw[0] >> 1) & 0x3f;
    nals[i].rbsp = rbsp;
    nals[i].len = w;
  }

  *out_nals = nals;
  *out_count = count;
  return 0;
}

typedef struct {
  const uint8_t *d;
  size_t n;
  size_t byte;
  int bit;
  int err;
} BitReader;

static uint32_t br_bits(BitReader *r, int n)
{
  uint32_t v = 0;
  for (int i = 0; i < n; ++i) {
    if (r->byte >= r->n) { r->err = 1; return 0; }
    int b = (r->d[r->byte] >> (7 - r->bit)) & 1;
    v = (v << 1) | (uint32_t)b;
    if (++r->bit == 8) { r->bit = 0; ++r->byte; }
  }
  return v;
}

static uint32_t br_ue(BitReader *r)
{
  int zeros = 0;
  while (!r->err && br_bits(r, 1) == 0 && zeros < 32) ++zeros;
  if (r->err || zeros >= 32) { r->err = 1; return 0; }
  uint32_t suffix = zeros ? br_bits(r, zeros) : 0;
  return ((1u << zeros) - 1u) + suffix;
}

typedef struct {
  int valid;
  int poc_bits;
  int lt_present;
  int sps_lt_count;
  int max_dpb_minus1;
} SpsInfo;

static int skip_profile_tier_level(BitReader *r, int max_sub_minus1)
{
  (void)br_bits(r, 2 + 1 + 5);
  (void)br_bits(r, 32);
  (void)br_bits(r, 4);
  (void)br_bits(r, 44);
  (void)br_bits(r, 8);

  int sub_profile[8] = {0};
  int sub_level[8] = {0};
  for (int i = 0; i < max_sub_minus1 && i < 8; ++i) {
    sub_profile[i] = (int)br_bits(r, 1);
    sub_level[i] = (int)br_bits(r, 1);
  }
  for (int i = max_sub_minus1; i < 8; ++i) (void)br_bits(r, 2);
  for (int i = 0; i < max_sub_minus1 && i < 8; ++i) {
    if (sub_profile[i]) return -1;
    if (sub_level[i]) (void)br_bits(r, 8);
  }
  return r->err ? -1 : 0;
}

static SpsInfo parse_sps_nal(const ParsedNal *nal)
{
  SpsInfo out = {0};
  if (nal->type != 33 || nal->len < 2) return out;
  BitReader r = { nal->rbsp + 2, nal->len - 2, 0, 0, 0 };

  (void)br_bits(&r, 4);
  int max_sub = (int)br_bits(&r, 3);
  (void)br_bits(&r, 1);
  if (skip_profile_tier_level(&r, max_sub)) return out;

  (void)br_ue(&r); /* sps id */
  int chroma = (int)br_ue(&r);
  if (chroma == 3) (void)br_bits(&r, 1);
  (void)br_ue(&r); /* width */
  (void)br_ue(&r); /* height */
  int conf = (int)br_bits(&r, 1);
  if (conf) {
    (void)br_ue(&r); (void)br_ue(&r); (void)br_ue(&r); (void)br_ue(&r);
  }
  (void)br_ue(&r); /* bitdepth luma minus8 */
  (void)br_ue(&r); /* bitdepth chroma minus8 */
  out.poc_bits = (int)br_ue(&r) + 4;

  (void)br_bits(&r, 1); /* sub-layer ordering present */
  out.max_dpb_minus1 = (int)br_ue(&r);
  (void)br_ue(&r);
  (void)br_ue(&r);

  (void)br_ue(&r); (void)br_ue(&r); (void)br_ue(&r); (void)br_ue(&r);
  (void)br_ue(&r); (void)br_ue(&r);

  int scaling = (int)br_bits(&r, 1);
  if (scaling) return out; /* tests configure scaling lists off */
  (void)br_bits(&r, 1); /* amp */
  (void)br_bits(&r, 1); /* sao */
  int pcm = (int)br_bits(&r, 1);
  if (pcm) {
    (void)br_bits(&r, 4);
    (void)br_bits(&r, 4);
    (void)br_ue(&r); (void)br_ue(&r);
    (void)br_bits(&r, 1);
  }

  int num_st = (int)br_ue(&r);
  if (num_st != 0) return out;
  out.lt_present = (int)br_bits(&r, 1);
  if (out.lt_present) {
    out.sps_lt_count = (int)br_ue(&r);
    for (int i = 0; i < out.sps_lt_count; ++i) {
      (void)br_bits(&r, out.poc_bits);
      (void)br_bits(&r, 1);
    }
  }

  if (r.err) return (SpsInfo){0};
  out.valid = 1;
  return out;
}

typedef struct {
  int valid;
  int nal_type;
  int slice_type;
  int poc_lsb;
  int num_neg;
  int num_pos;
  int neg_delta[16];
  int neg_used[16];
  int num_lt;
  int lt_lsb[2];
  int lt_used[2];
  int lt_msb_present[2];
  int lt_delta[2];
} SliceInfo;

static SliceInfo parse_slice_nal(const ParsedNal *nal, const SpsInfo *sps)
{
  SliceInfo s = {0};
  s.nal_type = nal->type;
  if ((nal->type != 1 && nal->type != 19 && nal->type != 20) || nal->len < 2) {
    return s;
  }

  BitReader r = { nal->rbsp + 2, nal->len - 2, 0, 0, 0 };
  int first = (int)br_bits(&r, 1);
  if (nal->type >= 16 && nal->type <= 23) (void)br_bits(&r, 1);
  (void)br_ue(&r); /* pps id */
  if (!first || r.err) return s;

  s.slice_type = (int)br_ue(&r);
  if (nal->type == 19 || nal->type == 20) {
    s.valid = !r.err;
    return s;
  }

  s.poc_lsb = (int)br_bits(&r, sps->poc_bits);
  int sps_flag = (int)br_bits(&r, 1);
  if (sps_flag) {
    (void)br_ue(&r);
    return s; /* these tests always write an in-slice RPS */
  }

  s.num_neg = (int)br_ue(&r);
  s.num_pos = (int)br_ue(&r);
  if (s.num_neg > 16 || s.num_pos > 16) return s;

  int cumulative = 0;
  for (int i = 0; i < s.num_neg; ++i) {
    uint32_t minus1 = br_ue(&r);
    cumulative += (int)minus1 + 1;
    s.neg_delta[i] = cumulative;
    s.neg_used[i] = (int)br_bits(&r, 1);
  }
  cumulative = 0;
  for (int i = 0; i < s.num_pos; ++i) {
    uint32_t minus1 = br_ue(&r);
    cumulative += (int)minus1 + 1;
    (void)cumulative;
    (void)br_bits(&r, 1);
  }

  if (sps->lt_present) {
    /* The SPS LT count is zero, therefore num_long_term_sps is absent. */
    s.num_lt = (int)br_ue(&r);
    if (s.num_lt > 2) return s;
    for (int i = 0; i < s.num_lt; ++i) {
      s.lt_lsb[i] = (int)br_bits(&r, sps->poc_bits);
      s.lt_used[i] = (int)br_bits(&r, 1);
      s.lt_msb_present[i] = (int)br_bits(&r, 1);
      if (s.lt_msb_present[i]) s.lt_delta[i] = (int)br_ue(&r);
    }
  }

  if (r.err) return (SliceInfo){0};
  s.valid = 1;
  return s;
}

static int parse_stream(const EncodeResult *r, SpsInfo *sps,
                        SliceInfo *slices, int max_slices, int *slice_count)
{
  ParsedNal *nals = NULL;
  int nal_count = 0;
  *slice_count = 0;
  if (extract_nals(&r->bits, &nals, &nal_count)) return -1;

  int found_sps = 0;
  for (int i = 0; i < nal_count; ++i) {
    if (nals[i].type == 33) {
      *sps = parse_sps_nal(&nals[i]);
      if (!sps->valid) { free_nals(nals, nal_count); return -1; }
      found_sps = 1;
      break;
    }
  }
  if (!found_sps) { free_nals(nals, nal_count); return -1; }

  for (int i = 0; i < nal_count && *slice_count < max_slices; ++i) {
    if (nals[i].type == 1 || nals[i].type == 19 || nals[i].type == 20) {
      SliceInfo s = parse_slice_nal(&nals[i], sps);
      if (!s.valid) { free_nals(nals, nal_count); return -1; }
      slices[(*slice_count)++] = s;
    }
  }

  free_nals(nals, nal_count);
  return *slice_count == r->done ? 0 : -1;
}

static int lt_full_poc(const SpsInfo *sps, const SliceInfo *s,
                       int idx, int current_poc)
{
  int period = 1 << sps->poc_bits;
  int curr_msb = current_poc / period;
  int curr_lsb = current_poc & (period - 1);
  int lsb = s->lt_lsb[idx];

  if (s->lt_msb_present[idx]) {
    return lsb + (curr_msb - s->lt_delta[idx]) * period;
  }
  int msb = curr_msb - (lsb > curr_lsb ? 1 : 0);
  return lsb + msb * period;
}

static uint64_t luma_sad(const kvz_picture *a, const kvz_picture *b)
{
  uint64_t sad = 0;
  for (int y = 0; y < TEST_H; ++y) {
    for (int x = 0; x < TEST_W; ++x) {
      int av = a->y[y * a->stride + x];
      int bv = b->y[y * b->stride + x];
      sad += (uint64_t)(av > bv ? av - bv : bv - av);
    }
  }
  return sad;
}

/* ------------------------------------------------------------------ */
/* slots group                                                        */
/* ------------------------------------------------------------------ */

static int case_validate_retry(void)
{
  const kvz_api *api = kvz_api_get(8);
  TASK_CHECK(api);

  EncoderCtx ctx = {0};
  EncOpts opts = { .owf = 0, .threads = 2, .wpp = 1, .ref_frames = 1, .ltr_slots = 2 };
  TASK_CHECK(ctx_open(api, &opts, &ctx));

  EncodeResult r = {0};
  kvz_picture *p = make_picture(api, 0);
  TASK_CHECK(p);
  p->ltr_use = 0; /* first IDR cannot use */
  kvz_data_chunk *data = (void *)1;
  uint32_t len = 123;
  kvz_picture *rec = (void *)1, *src = (void *)1;
  int rc = api->encoder_encode(ctx.enc, p, &data, &len, &rec, &src, NULL);
  TASK_CHECK(rc == 0);
  TASK_CHECK(data == NULL && len == 0 && rec == NULL && src == NULL);

  struct kvz_encoder *ke = (struct kvz_encoder *)ctx.enc;
  TASK_CHECK(ke->ltr.accepted_pics == 0);
  TASK_CHECK(ke->input_buffer.num_in == 0);
  TASK_CHECK(!ke->ltr.slots[0].initialized);

  p->ltr_use = 7;
  p->ltr_mark = 9;
  rc = api->encoder_encode(ctx.enc, p, &data, &len, &rec, &src, NULL);
  TASK_CHECK(rc == 0);
  TASK_CHECK(ke->ltr.accepted_pics == 0);

  p->ltr_use = 1; /* legal slot number, but uninitialized */
  p->ltr_mark = -1;
  rc = api->encoder_encode(ctx.enc, p, &data, &len, &rec, &src, NULL);
  TASK_CHECK(rc == 0);
  TASK_CHECK(ke->ltr.accepted_pics == 0);

  /* Correct it in place and retry the same input identity. */
  p->ltr_use = -1;
  p->ltr_mark = 0;
  TASK_CHECK(send_picture(api, &ctx, p, &r) == 0);
  TASK_CHECK(r.done == 1 && r.rec[0]);
  TASK_CHECK(ke->ltr.accepted_pics == 1);
  TASK_CHECK(ke->ltr.slots[0].initialized && ke->ltr.slots[0].poc == 0);

  kvz_picture *p1 = make_picture(api, 1);
  p1->ltr_use = 1; /* still uninitialized */
  rc = api->encoder_encode(ctx.enc, p1, &data, &len, &rec, &src, NULL);
  TASK_CHECK(rc == 0);
  TASK_CHECK(data == NULL && len == 0 && rec == NULL && src == NULL);
  TASK_CHECK(ke->ltr.accepted_pics == 1);
  api->picture_free(p1);

  p1 = make_picture(api, 1);
  p1->ltr_use = 0;
  TASK_CHECK(send_picture(api, &ctx, p1, &r) == 0);
  TASK_CHECK(r.done == 2);
  TASK_CHECK(r.info[1].ref_list_len[0] == 1);
  TASK_CHECK(r.info[1].ref_list[0][0] == 0);

  result_free(api, &r);
  ctx_close(api, &ctx);
  return 0;
}

static int case_same_slot_overwrite(void)
{
  const kvz_api *api = kvz_api_get(8);
  TASK_CHECK(api);
  EncoderCtx ctx = {0};
  EncOpts opts = { .owf = 0, .threads = 2, .wpp = 1, .ref_frames = 2, .ltr_slots = 2 };
  TASK_CHECK(ctx_open(api, &opts, &ctx));
  EncodeResult r = {0};

  int cmds[6][2] = {
    {0, -1}, /* POC0 marks slot0 */
    {0,  0}, /* uses old POC0, then overwrites slot0 with POC1 */
    {-1, 0}, /* must see POC1 */
    {-1,-1},
    {-1, 0},
    {-1,-1}
  };

  for (int n = 0; n < 6; ++n) {
    kvz_picture *p = make_picture(api, n);
    TASK_CHECK(p);
    p->ltr_mark = cmds[n][0];
    p->ltr_use = cmds[n][1];
    TASK_CHECK(send_picture(api, &ctx, p, &r) == 0);
  }
  for (int i = 0; i < 4 && r.done < 6; ++i) TASK_CHECK(call_null(api, &ctx, &r) == 0);

  TASK_CHECK(r.done == 6);
  TASK_CHECK(r.info[1].ref_list_len[0] == 1 && r.info[1].ref_list[0][0] == 0);
  TASK_CHECK(r.info[2].ref_list_len[0] == 1 && r.info[2].ref_list[0][0] == 1);
  TASK_CHECK(r.info[4].ref_list_len[0] == 1 && r.info[4].ref_list[0][0] == 1);

  struct kvz_encoder *ke = (struct kvz_encoder *)ctx.enc;
  TASK_CHECK(ke->ltr.slots[0].poc == 1);

  SpsInfo sps;
  SliceInfo slices[16];
  int nsl = 0;
  TASK_CHECK(parse_stream(&r, &sps, slices, 16, &nsl) == 0);
  TASK_CHECK(slices[1].num_lt == 1 && slices[1].lt_lsb[0] == 0);
  TASK_CHECK(slices[2].num_lt == 1 && slices[2].lt_lsb[0] == 1);
  TASK_CHECK(lt_full_poc(&sps, &slices[2], 0, 2) == 1);

  result_free(api, &r);
  ctx_close(api, &ctx);
  return 0;
}

static int case_dual_slot_alias(void)
{
  const kvz_api *api = kvz_api_get(8);
  TASK_CHECK(api);
  EncoderCtx ctx = {0};
  EncOpts opts = { .owf = 0, .threads = 1, .wpp = 0, .ref_frames = 2, .ltr_slots = 2 };
  TASK_CHECK(ctx_open(api, &opts, &ctx));
  EncodeResult r = {0};

  kvz_picture *p0 = make_picture(api, 0);
  p0->ltr_mark = 0;
  TASK_CHECK(send_picture(api, &ctx, p0, &r) == 0);
  TASK_CHECK(r.done == 1);

  struct kvz_encoder *ke = (struct kvz_encoder *)ctx.enc;
  TASK_CHECK(ke->ltr.slots[0].initialized);

  /* Create the otherwise impossible alias: both slot names own a ref to the
   * exact same production reconstruction/CU array/job. */
  ke->ltr.slots[1] = ke->ltr.slots[0];
  TASK_CHECK(kvz_image_copy_ref(ke->ltr.slots[1].pic) != NULL);
  TASK_CHECK(kvz_cu_array_copy_ref(ke->ltr.slots[1].cu) != NULL);
  TASK_CHECK(kvz_threadqueue_copy_ref(ke->ltr.slots[1].done_job) != NULL);
  ke->ltr.slots[1].initialized = 1;

  kvz_picture *p1 = make_picture(api, 1);
  p1->ltr_use = 1;
  TASK_CHECK(send_picture(api, &ctx, p1, &r) == 0);
  TASK_CHECK(r.done == 2);
  TASK_CHECK(r.info[1].ref_list_len[0] == 1 && r.info[1].ref_list[0][0] == 0);

  /* The main state used for POC1 still owns the DPB list used to code it. */
  encoder_state_t *st = &ke->states[0];
  TASK_CHECK(st->frame->ref->used_size == 1);
  TASK_CHECK(st->frame->ltr_lt_count == 1);
  TASK_CHECK(st->frame->ltr_lt[0].poc == 0 && st->frame->ltr_lt[0].used_by_curr == 1);

  SpsInfo sps;
  SliceInfo slices[8];
  int nsl = 0;
  TASK_CHECK(parse_stream(&r, &sps, slices, 8, &nsl) == 0);
  TASK_CHECK(slices[1].num_lt == 1 && slices[1].lt_used[0] == 1);

  result_free(api, &r);
  ctx_close(api, &ctx);
  return 0;
}

static int case_config_restrictions(void)
{
  const kvz_api *api = kvz_api_get(8);
  TASK_CHECK(api);

  struct BadCfg {
    int gop, intra, bipred, tmvp, ref;
    int chroma, scan, bitdepth;
  } bad[] = {
    {16, 0, 0, 0, 2, KVZ_CSP_420, 0, 8},
    {0,  32,0, 0, 2, KVZ_CSP_420, 0, 8},
    {0,  0, 1, 0, 2, KVZ_CSP_420, 0, 8},
    {0,  0, 0, 1, 2, KVZ_CSP_420, 0, 8},
    {0,  0, 0, 0, 0, KVZ_CSP_420, 0, 8},
    {0,  0, 0, 0, 5, KVZ_CSP_420, 0, 8},
    {0,  0, 0, 0, 2, KVZ_CSP_444, 0, 8},
    {0,  0, 0, 0, 2, KVZ_CSP_420, 1, 8},
    {0,  0, 0, 0, 2, KVZ_CSP_420, 0, 10},
  };

  for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
    kvz_config *c = api->config_alloc();
    TASK_CHECK(c && api->config_init(c));
    c->enable_logging_output = 0;
    TASK_CHECK(api->config_parse(c, "ltr-slots", "2"));
    c->width = 64; c->height = 64;
    c->gop_len = (int8_t)bad[i].gop;
    c->intra_period = bad[i].intra;
    c->bipred = bad[i].bipred;
    c->tmvp_enable = bad[i].tmvp;
    c->ref_frames = bad[i].ref;
    c->chroma_format = (enum kvz_chroma_format)bad[i].chroma;
    c->source_scan_type = (enum kvz_interlacing)bad[i].scan;
    c->input_bitdepth = bad[i].bitdepth;
    kvz_encoder *e = api->encoder_open(c);
    TASK_CHECK(e == NULL);
    api->config_destroy(c);
  }

  /* The promised WPP/OWF/thread combinations stay openable. */
  int owfs[2] = {0, 2};
  for (int i = 0; i < 2; ++i) {
    EncoderCtx ctx = {0};
    EncOpts o = { .owf = owfs[i], .threads = 2, .wpp = 1, .ref_frames = 3, .ltr_slots = 2 };
    TASK_CHECK(ctx_open(api, &o, &ctx));
    ctx_close(api, &ctx);
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/* syntax group                                                        */
/* ------------------------------------------------------------------ */

static int case_sps_syntax(void)
{
  const kvz_api *api = kvz_api_get(8);
  TASK_CHECK(api);
  EncodeResult r = {0};
  EncOpts o = { .owf = 0, .threads = 2, .wpp = 1, .ref_frames = 4, .ltr_slots = 2 };
  TASK_CHECK(run_standard(api, o, &r, NULL) == 0);

  SpsInfo sps;
  SliceInfo slices[TEST_FRAMES];
  int nsl = 0;
  TASK_CHECK(parse_stream(&r, &sps, slices, TEST_FRAMES, &nsl) == 0);
  TASK_CHECK(sps.valid && sps.poc_bits == 4);
  TASK_CHECK(sps.lt_present == 1 && sps.sps_lt_count == 0);
  /* Current + four short refs + two possible LTR slots. */
  TASK_CHECK(sps.max_dpb_minus1 + 1 >= 7);
  TASK_CHECK(r.bits.len > 1000);

  result_free(api, &r);
  return 0;
}

static int case_rps_retention_active_l0(void)
{
  const kvz_api *api = kvz_api_get(8);
  TASK_CHECK(api);
  EncodeResult r = {0};
  EncOpts o = { .owf = 0, .threads = 2, .wpp = 1, .ref_frames = 3, .ltr_slots = 2 };
  TASK_CHECK(run_standard(api, o, &r, NULL) == 0);

  SpsInfo sps;
  SliceInfo slices[TEST_FRAMES];
  int nsl = 0;
  TASK_CHECK(parse_stream(&r, &sps, slices, TEST_FRAMES, &nsl) == 0);

  for (int poc = 1; poc < TEST_FRAMES; ++poc) {
    const SliceInfo *s = &slices[poc];
    TASK_CHECK(s->valid);
    /* No LTR POC may be encoded as a negative short-term delta. */
    for (int i = 0; i < s->num_neg; ++i) {
      int short_poc = poc - s->neg_delta[i];
      TASK_CHECK(short_poc != 0 && short_poc != 4);
      TASK_CHECK(s->neg_used[i] == (r.info[poc].ref_list_len[0] == 1 ? 0 : 1));
    }

    if (poc == 24 || poc == 40) {
      TASK_CHECK(r.info[poc].ref_list_len[0] == 1);
      TASK_CHECK(r.info[poc].ref_list[0][0] == 0);
      TASK_CHECK(s->num_lt == 2);
      int used_count = 0, full0 = -1, full4 = -1;
      for (int i = 0; i < 2; ++i) {
        int full = lt_full_poc(&sps, s, i, poc);
        if (full == 0) full0 = full;
        if (full == 4) full4 = full;
        used_count += s->lt_used[i];
        if (s->lt_used[i]) TASK_CHECK(full == 0);
        else TASK_CHECK(full == 4);
      }
      TASK_CHECK(full0 == 0 && full4 == 4 && used_count == 1);
    } else if (poc == 32) {
      TASK_CHECK(r.info[32].ref_list[0][0] == 4);
      int used = 0;
      for (int i = 0; i < s->num_lt; ++i) {
        int full = lt_full_poc(&sps, s, i, poc);
        TASK_CHECK(full == 0 || full == 4);
        if (s->lt_used[i]) {
          ++used;
          TASK_CHECK(full == 4);
        }
      }
      TASK_CHECK(used == 1);
    } else if (poc < 5) {
      /* Slot 1 is established by POC4's mark and first appears in POC5. */
      TASK_CHECK(s->num_lt == 1);
      int full = lt_full_poc(&sps, s, 0, poc);
      TASK_CHECK(full == 0);
      int active = 0;
      for (int j = 0; j < r.info[poc].ref_list_len[0]; ++j) {
        if (r.info[poc].ref_list[0][j] == full) active = 1;
      }
      TASK_CHECK(s->lt_used[0] == active);
    } else {
      /* Both slots are retained. An overlapping sliding-window image is
       * active; every genuinely unused slot has used_by_curr_pic_lt_flag=0. */
      TASK_CHECK(s->num_lt == 2);
      for (int i = 0; i < s->num_lt; ++i) {
        int full = lt_full_poc(&sps, s, i, poc);
        int active = 0;
        for (int j = 0; j < r.info[poc].ref_list_len[0]; ++j) {
          if (r.info[poc].ref_list[0][j] == full) active = 1;
        }
        TASK_CHECK(s->lt_used[i] == active);
      }
    }
  }

  result_free(api, &r);
  return 0;
}

static int case_poc_wrap_explicit_msb(void)
{
  const kvz_api *api = kvz_api_get(8);
  TASK_CHECK(api);
  EncodeResult r = {0};
  EncOpts o = { .owf = 2, .threads = 2, .wpp = 1, .ref_frames = 2, .ltr_slots = 2 };
  TASK_CHECK(run_standard(api, o, &r, NULL) == 0);

  SpsInfo sps;
  SliceInfo slices[TEST_FRAMES];
  int nsl = 0;
  TASK_CHECK(parse_stream(&r, &sps, slices, TEST_FRAMES, &nsl) == 0);
  TASK_CHECK(sps.poc_bits == 4); /* POC LSB wraps at 16. */

  const SliceInfo *s40 = &slices[40];
  TASK_CHECK(s40->poc_lsb == 8);
  TASK_CHECK(s40->num_lt == 2);
  int found0 = 0;
  for (int i = 0; i < s40->num_lt; ++i) {
    TASK_CHECK(s40->lt_msb_present[i]);
    int full = lt_full_poc(&sps, s40, i, 40);
    if (full == 0) {
      found0 = 1;
      TASK_CHECK(s40->lt_used[i] == 1);
      /* POC40 and POC0 are two POC-LSB cycles apart. */
      TASK_CHECK(s40->lt_delta[i] == 2);
    }
  }
  TASK_CHECK(found0);

  /* POC32 crosses the wrap at 32 and identifies POC4 exactly. */
  const SliceInfo *s32 = &slices[32];
  int found4 = 0;
  for (int i = 0; i < s32->num_lt; ++i) {
    if (lt_full_poc(&sps, s32, i, 32) == 4) {
      found4 = 1;
      TASK_CHECK(s32->lt_delta[i] == 2); /* POC32 cycle2, POC4 cycle0 */
    }
  }
  TASK_CHECK(found4);

  result_free(api, &r);
  return 0;
}

/* ------------------------------------------------------------------ */
/* pipeline group                                                     */
/* ------------------------------------------------------------------ */

static int case_old_image_restoration(void)
{
  const kvz_api *api = kvz_api_get(8);
  TASK_CHECK(api);
  EncodeResult r = {0};
  EncOpts o = { .owf = 0, .threads = 2, .wpp = 1, .ref_frames = 4, .ltr_slots = 2 };
  EncoderCtx ctx = {0};
  TASK_CHECK(run_standard(api, o, &r, &ctx) == 0);

  TASK_CHECK(r.rec[0] && r.rec[23] && r.rec[24]);
  TASK_CHECK(r.info[24].ref_list_len[0] == 1);
  TASK_CHECK(r.info[24].ref_list[0][0] == 0);

  uint64_t to_old = luma_sad(r.rec[24], r.rec[0]);
  uint64_t to_wild = luma_sad(r.rec[24], r.rec[23]);
  TASK_CHECK(to_old < 4096);
  TASK_CHECK(to_wild > 8192);
  TASK_CHECK(to_old * 8 < to_wild);

  struct kvz_encoder *ke = (struct kvz_encoder *)ctx.enc;
  TASK_CHECK(ke->ltr.slots[0].poc == 0);
  TASK_CHECK(ke->ltr.slots[0].pic->y == r.rec[0]->y);
  int found_retained_poc0 = 0;
  encoder_state_t *st = &ke->states[0];
  for (unsigned i = 0; i < st->frame->ref->used_size; ++i) {
    if (st->frame->ref->pocs[i] == 0) {
      found_retained_poc0 = 1;
      TASK_CHECK(st->frame->ref->images[i]->y == r.rec[0]->y);
    }
  }
  TASK_CHECK(found_retained_poc0);

  result_free(api, &r);
  ctx_close(api, &ctx);
  return 0;
}

static int case_owf2_reuse_and_flush(void)
{
  const kvz_api *api = kvz_api_get(8);
  TASK_CHECK(api);
  EncodeResult r = {0};
  EncOpts o = { .owf = 2, .threads = 2, .wpp = 1, .ref_frames = 2, .ltr_slots = 2 };
  TASK_CHECK(run_standard(api, o, &r, NULL) == 0);
  TASK_CHECK(r.done == 48);

  for (int poc = 0; poc < 48; ++poc) {
    TASK_CHECK(r.rec[poc]);
    TASK_CHECK(r.info[poc].poc == poc);
  }
  TASK_CHECK(r.info[24].ref_list[0][0] == 0);
  TASK_CHECK(r.info[40].ref_list[0][0] == 0);
  TASK_CHECK(r.bits.len > 8000);

  SpsInfo sps;
  SliceInfo slices[TEST_FRAMES];
  int nsl = 0;
  TASK_CHECK(parse_stream(&r, &sps, slices, TEST_FRAMES, &nsl) == 0);
  result_free(api, &r);
  return 0;
}

static int case_same_pts_immediate_release(void)
{
  const kvz_api *api = kvz_api_get(8);
  TASK_CHECK(api);
  EncoderCtx ctx = {0};
  EncOpts o = { .owf = 2, .threads = 2, .wpp = 1, .ref_frames = 2, .ltr_slots = 2 };
  TASK_CHECK(ctx_open(api, &o, &ctx));
  EncodeResult r = {0};

  for (int n = 0; n < 28; ++n) {
    kvz_picture *p = make_picture(api, n);
    TASK_CHECK(p);
    p->pts = 12345; /* deliberately identical for every input */
    if (n == 0) p->ltr_mark = 0;
    if (n == 25) p->ltr_use = 0;
    TASK_CHECK(send_picture(api, &ctx, p, &r) == 0); /* frees p immediately */
  }
  for (int i = 0; i < 20 && r.done < 28; ++i) TASK_CHECK(call_null(api, &ctx, &r) == 0);
  TASK_CHECK(r.done == 28);
  TASK_CHECK(r.info[25].ref_list_len[0] == 1);
  TASK_CHECK(r.info[25].ref_list[0][0] == 0);

  result_free(api, &r);
  ctx_close(api, &ctx);
  return 0;
}

static int case_owf2_same_slot_chain(void)
{
  const kvz_api *api = kvz_api_get(8);
  TASK_CHECK(api);
  EncoderCtx ctx = {0};
  EncOpts o = { .owf = 2, .threads = 2, .wpp = 1, .ref_frames = 1, .ltr_slots = 2 };
  TASK_CHECK(ctx_open(api, &o, &ctx));
  EncodeResult r = {0};

  enum { N = 32 };
  for (int n = 0; n < N; ++n) {
    kvz_picture *p = make_picture(api, n);
    TASK_CHECK(p);
    p->ltr_mark = 0;
    if (n > 0) p->ltr_use = 0; /* use old version, then mark this frame */
    TASK_CHECK(send_picture(api, &ctx, p, &r) == 0);
  }
  for (int i = 0; i < 20 && r.done < N; ++i) TASK_CHECK(call_null(api, &ctx, &r) == 0);
  TASK_CHECK(r.done == N);
  for (int poc = 1; poc < N; ++poc) {
    TASK_CHECK(r.info[poc].ref_list_len[0] == 1);
    TASK_CHECK(r.info[poc].ref_list[0][0] == poc - 1);
  }

  SpsInfo sps;
  SliceInfo slices[N];
  int nsl = 0;
  TASK_CHECK(parse_stream(&r, &sps, slices, N, &nsl) == 0);
  for (int poc = 1; poc < N; ++poc) {
    TASK_CHECK(slices[poc].num_neg == 0);
    TASK_CHECK(slices[poc].num_lt == 1 && slices[poc].lt_used[0] == 1);
    TASK_CHECK(lt_full_poc(&sps, &slices[poc], 0, poc) == poc - 1);
  }

  result_free(api, &r);
  ctx_close(api, &ctx);
  return 0;
}

static int case_interleaved_instances(void)
{
  const kvz_api *api = kvz_api_get(8);
  TASK_CHECK(api);
  EncoderCtx a = {0}, b = {0};
  EncOpts oa = { .owf = 0, .threads = 2, .wpp = 1, .ref_frames = 1, .ltr_slots = 2 };
  EncOpts ob = { .owf = 2, .threads = 2, .wpp = 1, .ref_frames = 2, .ltr_slots = 2 };
  TASK_CHECK(ctx_open(api, &oa, &a));
  TASK_CHECK(ctx_open(api, &ob, &b));
  EncodeResult ra = {0}, rb = {0};

  for (int n = 0; n < 20; ++n) {
    kvz_picture *pa = make_picture(api, n);
    kvz_picture *pb = make_picture(api, n + 100);
    TASK_CHECK(pa && pb);
    if (n == 0) pa->ltr_mark = 0;
    if (n == 10) pa->ltr_use = 0;
    if (n == 2) pb->ltr_mark = 1;
    if (n == 12) pb->ltr_use = 1;
    TASK_CHECK(send_picture(api, &a, pa, &ra) == 0);
    TASK_CHECK(send_picture(api, &b, pb, &rb) == 0);
  }
  for (int i = 0; i < 16 && (ra.done < 20 || rb.done < 20); ++i) {
    TASK_CHECK(call_null(api, &a, &ra) == 0);
    TASK_CHECK(call_null(api, &b, &rb) == 0);
  }
  TASK_CHECK(ra.done == 20 && rb.done == 20);
  TASK_CHECK(ra.info[10].ref_list[0][0] == 0);
  TASK_CHECK(rb.info[12].ref_list[0][0] == 2);

  result_free(api, &ra);
  result_free(api, &rb);
  ctx_close(api, &a);
  ctx_close(api, &b);
  return 0;
}

/* ------------------------------------------------------------------ */
/* compat group                                                        */
/* ------------------------------------------------------------------ */

static int run_off_byte_stream(const kvz_api *api, int mutate_fields, EncodeResult *r)
{
  EncoderCtx ctx = {0};
  EncOpts o = { .owf = 0, .threads = 1, .wpp = 0, .ref_frames = 2, .ltr_slots = 0 };
  if (!ctx_open(api, &o, &ctx)) return -1;
  for (int n = 0; n < TEST_FRAMES; ++n) {
    kvz_picture *p = make_picture(api, n);
    if (!p) return -1;
    if (mutate_fields) {
      p->ltr_mark = (n % 3) - 1;       /* includes legal values */
      p->ltr_use = (n == 9) ? 44 : -7; /* deliberately illegal when off */
    }
    if (send_picture(api, &ctx, p, r)) return -1;
  }
  for (int i = 0; i < 12 && r->done < TEST_FRAMES; ++i) {
    if (call_null(api, &ctx, r)) return -1;
  }
  if (r->done != TEST_FRAMES) return -1;
  ctx_close(api, &ctx);
  return 0;
}

static int case_off_byte_compare(void)
{
  const kvz_api *api = kvz_api_get(8);
  TASK_CHECK(api);
  EncodeResult a = {0}, b = {0};
  TASK_CHECK(run_off_byte_stream(api, 0, &a) == 0);
  TASK_CHECK(run_off_byte_stream(api, 1, &b) == 0);
  TASK_CHECK(a.bits.len == b.bits.len);
  TASK_CHECK(a.bits.len > 2000);
  TASK_CHECK(memcmp(a.bits.data, b.bits.data, a.bits.len) == 0);

  result_free(api, &a);
  result_free(api, &b);
  return 0;
}

static int case_defaults_short_window(void)
{
  const kvz_api *api = kvz_api_get(8);
  TASK_CHECK(api);
  kvz_config *c = api->config_alloc();
  TASK_CHECK(c && api->config_init(c));
  TASK_CHECK(c->ltr_slots == 0);
  TASK_CHECK(api->config_parse(c, "ltr-slots", "0") && c->ltr_slots == 0);
  api->config_destroy(c);

  kvz_picture *p = api->picture_alloc(16, 16);
  TASK_CHECK(p && p->ltr_mark == -1 && p->ltr_use == -1);
  api->picture_free(p);

  EncodeResult r = {0};
  EncOpts o = { .owf = 0, .threads = 1, .wpp = 0, .ref_frames = 1, .ltr_slots = 0 };
  TASK_CHECK(run_standard(api, o, &r, NULL) == 0);
  for (int poc = 1; poc < 8; ++poc) {
    TASK_CHECK(r.info[poc].ref_list_len[0] == 1);
    TASK_CHECK(r.info[poc].ref_list[0][0] == poc - 1);
  }
  result_free(api, &r);
  return 0;
}

/* ------------------------------------------------------------------ */
/* Registration consumed by task_support/driver.c                     */
/* ------------------------------------------------------------------ */

const struct task_case task_cases[] = {
  { "slots", "validate_retry",        case_validate_retry },
  { "slots", "same_slot_overwrite",   case_same_slot_overwrite },
  { "slots", "dual_slot_alias",       case_dual_slot_alias },
  { "slots", "config_restrictions",   case_config_restrictions },

  { "syntax", "sps_flags_dpb",        case_sps_syntax },
  { "syntax", "rps_retention_l0",     case_rps_retention_active_l0 },
  { "syntax", "poc_wrap_msb",          case_poc_wrap_explicit_msb },

  { "pipeline", "old_restoration",    case_old_image_restoration },
  { "pipeline", "owf2_flush",          case_owf2_reuse_and_flush },
  { "pipeline", "same_pts_release",   case_same_pts_immediate_release },
  { "pipeline", "owf2_slot_chain",    case_owf2_same_slot_chain },
  { "pipeline", "interleaved",        case_interleaved_instances },

  { "compat", "off_byte_compare",     case_off_byte_compare },
  { "compat", "defaults_short_window", case_defaults_short_window },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
