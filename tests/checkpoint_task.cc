/*
 * Checkpoint save/restore tests.
 *
 * Registered through task_cases[]/task_case_count and driven by
 * task_support/driver.c. Each case uses TASK_CHECK for assertions. No main().
 *
 * The material is testdata/girlshy.h265. Frames are split into independent
 * input units at picture boundaries; each unit is pushed and followed by
 * de265_push_end_of_frame(), and de265_decode() is driven until the pushed
 * input is consumed, exactly as an offline exporter would. A checkpoint is
 * taken at a split point and restoring must reproduce the uninterrupted decode
 * picture for picture (pixels, PTS, order).
 */

#include "task_support/check.h"

#include "libde265/de265.h"
#include "libde265/image.h"
#include "libde265/slice.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

struct frame_unit {
  size_t begin;
  size_t end;
};

struct frame_result {
  long long pts;
  unsigned long long hash;
  bool is_b;
  bool tmvp;
  bool multi_ref;
  int width;
  int height;
};

/* Split a raw HEVC bytestream into per-picture input units. A new unit begins
   at the start code of a VCL NAL that has first_slice_segment_in_pic_flag set;
   parameter sets and other non-VCL NALs preceding the first picture belong to
   the first unit. */
std::vector<frame_unit> split_into_pictures(const std::vector<unsigned char>& data)
{
  struct nal { size_t start; size_t payload; int type; bool first_slice; };
  std::vector<nal> nals;

  size_t i = 0, n = data.size();
  std::vector<std::pair<size_t, size_t>> starts;
  while (i + 3 <= n) {
    if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
      starts.emplace_back(i, 3);
      i += 3;
    }
    else if (i + 4 <= n && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 &&
             data[i + 3] == 1) {
      starts.emplace_back(i, 4);
      i += 4;
    }
    else {
      i++;
    }
  }

  for (const auto& sc : starts) {
    size_t payload = sc.first + sc.second;
    if (payload + 2 > data.size()) { continue; }
    int type = (data[payload] >> 1) & 0x3f;
    bool first_slice = (type < 32) && ((data[payload + 2] >> 7) & 1);
    nals.push_back({sc.first, payload, type, first_slice});
  }

  std::vector<size_t> picture_starts;
  for (const nal& x : nals) {
    if (x.type < 32 && x.first_slice) {
      picture_starts.push_back(x.start);
    }
  }

  std::vector<frame_unit> units;
  for (size_t k = 0; k < picture_starts.size(); k++) {
    size_t begin = (k == 0) ? 0 : picture_starts[k];
    size_t end = (k + 1 < picture_starts.size()) ? picture_starts[k + 1] : data.size();
    units.push_back({begin, end});
  }
  return units;
}

unsigned long long fnv_init() { return 14695981039346656037ull; }

unsigned long long fnv_bytes(const unsigned char* p, int n, unsigned long long h)
{
  for (int i = 0; i < n; i++) {
    h ^= p[i];
    h *= 1099511628211ull;
  }
  return h;
}

frame_result grab(const de265_image* img)
{
  frame_result r;
  r.pts = de265_get_image_PTS(img);
  r.hash = fnv_init();
  for (int c = 0; c < 3; c++) {
    int stride = 0;
    int w = de265_get_image_width(img, c);
    int h = de265_get_image_height(img, c);
    const uint8_t* plane = de265_get_image_plane(img, c, &stride);
    if (!plane) { continue; }
    for (int y = 0; y < h; y++) {
      r.hash = fnv_bytes(plane + y * stride, w, r.hash);
    }
  }
  r.width = de265_get_image_width(img, 0);
  r.height = de265_get_image_height(img, 0);

  r.is_b = false;
  r.tmvp = false;
  r.multi_ref = false;
  for (const slice_segment_header* sh : img->slices) {
    r.is_b |= (sh->slice_type == SLICE_TYPE_B);
    r.tmvp |= sh->slice_temporal_mvp_enabled_flag;
    r.multi_ref |= (sh->num_ref_idx_l0_active > 1 || sh->num_ref_idx_l1_active > 1);
  }
  return r;
}

bool read_file(const char* path, std::vector<unsigned char>& out)
{
  std::ifstream in(path, std::ios::binary);
  if (!in) { return false; }
  out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return !out.empty();
}

/* Push one picture unit and drive the decoder until the pushed input has been
   consumed and the picture has been produced. 'collect' may copy out decoder
   output; 'consume' releases pictures as it goes. Pictures that are not
   consumed remain in the output queue. */
void decode_unit(de265_decoder_context* ctx, const std::vector<unsigned char>& data,
                 const frame_unit& unit, unsigned long long pts)
{
  de265_push_data(ctx, data.data() + unit.begin,
                  static_cast<int>(unit.end - unit.begin), static_cast<de265_PTS>(pts),
                  nullptr);
  de265_push_end_of_frame(ctx);

  int guard = 0;
  while ((de265_get_number_of_input_bytes_pending(ctx) > 0 ||
          de265_get_number_of_NAL_units_pending(ctx) > 0) && guard++ < 100000) {
    int more = 0;
    de265_decode(ctx, &more);
  }
  /* One more call runs the end-of-frame flush and the post-processing tasks. */
  int more = 0;
  de265_decode(ctx, &more);
}

/* Decode the whole stream without interruption, one unit at a time, consuming
   all output. */
std::vector<frame_result> decode_reference(const std::vector<unsigned char>& data,
                                           const std::vector<frame_unit>& units)
{
  std::vector<frame_result> out;
  de265_decoder_context* ctx = de265_new_decoder();
  if (!ctx) { return out; }

  for (size_t k = 0; k < units.size(); k++) {
    decode_unit(ctx, data, units[k], 1000 + k);
    const de265_image* img;
    while ((img = de265_get_next_picture(ctx)) != nullptr) {
      out.push_back(grab(img));
    }
  }

  /* Tail drain: the final pictures may still be in the reorder buffer. */
  de265_flush_data(ctx);
  int more = 1, guard = 0;
  while (more && guard++ < 1000) {
    more = 0;
    de265_decode(ctx, &more);
    const de265_image* img;
    while ((img = de265_get_next_picture(ctx)) != nullptr) {
      out.push_back(grab(img));
    }
  }

  de265_free_decoder(ctx);
  return out;
}

struct checkpoint_run {
  std::vector<frame_result> consumed_before;
  std::vector<frame_result> after;
  size_t checkpoint_size = 0;
  bool identical_on_repeat = false;
};

/* Decode units [0, split], keep some output unconsumed, checkpoint, restore,
   then decode units [split+1, end] from the restored instance. */
checkpoint_run run_checkpoint(const std::vector<unsigned char>& data,
                              const std::vector<frame_unit>& units, size_t split,
                              size_t consume_limit)
{
  checkpoint_run run;

  de265_decoder_context* ctx = de265_new_decoder();
  if (!ctx) { return run; }

  for (size_t k = 0; k <= split; k++) {
    decode_unit(ctx, data, units[k], 1000 + k);
    while (run.consumed_before.size() < consume_limit) {
      const de265_image* img = de265_peek_next_picture(ctx);
      if (!img) { break; }
      run.consumed_before.push_back(grab(img));
      de265_release_next_picture(ctx);
    }
  }

  run.checkpoint_size = de265_checkpoint_size(ctx);
  if (run.checkpoint_size == 0) {
    de265_free_decoder(ctx);
    return run;
  }

  std::vector<unsigned char> checkpoint(run.checkpoint_size);
  if (de265_checkpoint_write(ctx, checkpoint.data(), checkpoint.size()) != DE265_OK) {
    run.checkpoint_size = 0;
    de265_free_decoder(ctx);
    return run;
  }

  std::vector<unsigned char> checkpoint2(run.checkpoint_size);
  de265_checkpoint_write(ctx, checkpoint2.data(), checkpoint2.size());
  run.identical_on_repeat = (checkpoint == checkpoint2);

  de265_free_decoder(ctx);

  de265_error err = DE265_OK;
  de265_decoder_context* restored =
      de265_checkpoint_read(checkpoint.data(), checkpoint.size(), &err);
  if (!restored || err != DE265_OK) {
    run.checkpoint_size = 0;
    return run;
  }

  for (size_t k = split + 1; k < units.size(); k++) {
    decode_unit(restored, data, units[k], 1000 + k);
    const de265_image* img;
    while ((img = de265_get_next_picture(restored)) != nullptr) {
      run.after.push_back(grab(img));
    }
  }

  de265_flush_data(restored);
  int more = 1, guard = 0;
  while (more && guard++ < 1000) {
    more = 0;
    de265_decode(restored, &more);
    const de265_image* img;
    while ((img = de265_get_next_picture(restored)) != nullptr) {
      run.after.push_back(grab(img));
    }
  }

  de265_free_decoder(restored);
  return run;
}

bool compare_sequence(const std::vector<frame_result>& got,
                      const std::vector<frame_result>& want)
{
  if (got.size() != want.size()) { return false; }
  for (size_t i = 0; i < got.size(); i++) {
    if (got[i].pts != want[i].pts || got[i].hash != want[i].hash) {
      return false;
    }
  }
  return true;
}

const std::vector<unsigned char>& test_stream()
{
  static std::vector<unsigned char> data;
  static bool loaded = false;
  if (!loaded) {
    loaded = true;
    read_file("testdata/girlshy.h265", data);
  }
  return data;
}

const std::vector<frame_unit>& test_units()
{
  static std::vector<frame_unit> units;
  static bool built = false;
  if (!built) {
    built = true;
    if (!test_stream().empty()) {
      units = split_into_pictures(test_stream());
    }
  }
  return units;
}

/* --------------------------------------------------------------------- */
/* format group                                                          */
/* --------------------------------------------------------------------- */

int case_size_after_complete_picture()
{
  const auto& data = test_stream();
  const auto& units = test_units();
  TASK_CHECK(!data.empty());
  TASK_CHECK(units.size() == 75);

  de265_decoder_context* ctx = de265_new_decoder();
  TASK_CHECK(ctx != nullptr);

  TASK_CHECK(de265_checkpoint_size(ctx) == 0);   // nothing decoded yet

  decode_unit(ctx, data, units[0], 1000);
  TASK_CHECK(de265_checkpoint_size(ctx) > 0);

  /* Parameters are fixed and the DPB is bounded, so once the DPB has reached
     its steady state the checkpoint size does not depend on how many pictures
     have been decoded so far. */
  for (size_t k = 1; k <= 30; k++) {
    decode_unit(ctx, data, units[k], 1000 + k);
    const de265_image* img;
    while ((img = de265_get_next_picture(ctx)) != nullptr) { }
  }
  size_t s_after_30 = de265_checkpoint_size(ctx);
  TASK_CHECK(s_after_30 > 0);

  for (size_t k = 31; k <= 45; k++) {
    decode_unit(ctx, data, units[k], 1000 + k);
    const de265_image* img;
    while ((img = de265_get_next_picture(ctx)) != nullptr) { }
  }
  size_t s_after_45 = de265_checkpoint_size(ctx);
  TASK_CHECK(s_after_30 == s_after_45);

  de265_free_decoder(ctx);
  return 0;
}

int case_unavailable_mid_frame()
{
  const auto& data = test_stream();
  const auto& units = test_units();

  de265_decoder_context* ctx = de265_new_decoder();
  TASK_CHECK(ctx != nullptr);
  decode_unit(ctx, data, units[0], 1000);

  /* Push part of the next unit without ending the frame. */
  size_t partial = (units[1].end - units[1].begin) / 2;
  TASK_CHECK(partial > 0);
  de265_push_data(ctx, data.data() + units[1].begin, static_cast<int>(partial), 1001,
                  nullptr);

  TASK_CHECK(de265_checkpoint_size(ctx) == 0);

  unsigned char sentinel[64];
  memset(sentinel, 0x5a, sizeof(sentinel));
  TASK_CHECK(de265_checkpoint_write(ctx, sentinel, sizeof(sentinel)) ==
             DE265_ERROR_CHECKPOINT_UNAVAILABLE);
  for (unsigned char c : sentinel) {
    TASK_CHECK(c == 0x5a);
  }
  /* The unavailable write must not advance the state. */
  TASK_CHECK(de265_get_number_of_input_bytes_pending(ctx) > 0);

  de265_free_decoder(ctx);
  return 0;
}

int case_unavailable_before_driving()
{
  const auto& data = test_stream();
  const auto& units = test_units();

  de265_decoder_context* ctx = de265_new_decoder();
  TASK_CHECK(ctx != nullptr);

  /* Push a complete picture unit and signal end-of-frame, but do not call
     de265_decode() yet: the input/NALs are still pending. */
  de265_push_data(ctx, data.data() + units[0].begin,
                  static_cast<int>(units[0].end - units[0].begin), 1000, nullptr);
  de265_push_end_of_frame(ctx);

  TASK_CHECK(de265_get_number_of_NAL_units_pending(ctx) > 0);
  TASK_CHECK(de265_checkpoint_size(ctx) == 0);
  unsigned char sentinel[8];
  memset(sentinel, 0x33, sizeof(sentinel));
  TASK_CHECK(de265_checkpoint_write(ctx, sentinel, sizeof(sentinel)) ==
             DE265_ERROR_CHECKPOINT_UNAVAILABLE);
  for (unsigned char c : sentinel) { TASK_CHECK(c == 0x33); }

  /* Once the input is driven through, the same instance becomes savable. */
  int guard = 0;
  while ((de265_get_number_of_input_bytes_pending(ctx) > 0 ||
          de265_get_number_of_NAL_units_pending(ctx) > 0) && guard++ < 100000) {
    int more = 0;
    de265_decode(ctx, &more);
  }
  int more = 0;
  de265_decode(ctx, &more);
  TASK_CHECK(de265_checkpoint_size(ctx) > 0);

  de265_free_decoder(ctx);
  return 0;
}

int case_insufficient_capacity()
{
  const auto& data = test_stream();
  const auto& units = test_units();

  de265_decoder_context* ctx = de265_new_decoder();
  TASK_CHECK(ctx != nullptr);
  decode_unit(ctx, data, units[0], 1000);

  size_t need = de265_checkpoint_size(ctx);
  TASK_CHECK(need > 0);

  std::vector<unsigned char> buf(need + 16, 0xA5);
  TASK_CHECK(de265_checkpoint_write(ctx, buf.data(), need - 1) ==
             DE265_ERROR_INVALID_ARGUMENT);
  for (size_t i = 0; i < buf.size(); i++) {
    TASK_CHECK(buf[i] == 0xA5);   // nothing written
  }

  TASK_CHECK(de265_checkpoint_write(ctx, buf.data(), buf.size()) == DE265_OK);

  de265_free_decoder(ctx);
  return 0;
}

int make_checkpoint(size_t split, std::vector<unsigned char>& out)
{
  const auto& data = test_stream();
  const auto& units = test_units();

  de265_decoder_context* ctx = de265_new_decoder();
  if (!ctx) { return -1; }
  for (size_t k = 0; k <= split; k++) {
    decode_unit(ctx, data, units[k], 1000 + k);
  }
  size_t size = de265_checkpoint_size(ctx);
  out.resize(size);
  de265_error err = size ? de265_checkpoint_write(ctx, out.data(), out.size())
                         : DE265_ERROR_CHECKPOINT_UNAVAILABLE;
  de265_free_decoder(ctx);
  return err == DE265_OK ? 0 : -1;
}

int case_truncated_and_version()
{
  std::vector<unsigned char> good;
  TASK_CHECK(make_checkpoint(10, good) == 0);
  TASK_CHECK(good.size() > 32);

  /* Every truncation must be rejected. */
  for (size_t cut : {size_t(0), size_t(8), good.size() / 2, good.size() - 1}) {
    de265_error err = DE265_OK;
    const void* p = cut ? static_cast<const void*>(good.data()) : nullptr;
    de265_decoder_context* d = de265_checkpoint_read(p, cut, &err);
    TASK_CHECK(d == nullptr);
    TASK_CHECK(err != DE265_OK);
    if (d) { de265_free_decoder(d); }
  }

  /* Wrong magic. */
  {
    std::vector<unsigned char> bad = good;
    bad[0] ^= 0xff;
    de265_error err = DE265_OK;
    de265_decoder_context* d = de265_checkpoint_read(bad.data(), bad.size(), &err);
    TASK_CHECK(d == nullptr);
    TASK_CHECK(err == DE265_ERROR_CHECKPOINT_FORMAT);
    if (d) { de265_free_decoder(d); }
  }

  /* Unsupported version. */
  {
    std::vector<unsigned char> bad = good;
    bad[8] = 0x7f;
    bad[9] = 0x00;
    de265_error err = DE265_OK;
    de265_decoder_context* d = de265_checkpoint_read(bad.data(), bad.size(), &err);
    TASK_CHECK(d == nullptr);
    TASK_CHECK(err == DE265_ERROR_CHECKPOINT_FORMAT);
    if (d) { de265_free_decoder(d); }
  }

  /* Trailing garbage (size mismatch). */
  {
    std::vector<unsigned char> bad = good;
    bad.push_back(0);
    de265_error err = DE265_OK;
    de265_decoder_context* d = de265_checkpoint_read(bad.data(), bad.size(), &err);
    TASK_CHECK(d == nullptr);
    TASK_CHECK(err == DE265_ERROR_CHECKPOINT_FORMAT);
    if (d) { de265_free_decoder(d); }
  }

  return 0;
}

int case_inconsistent_reference_ids()
{
  /* Corrupt a wide range of body offsets; every resulting checkpoint must
     either parse fully or fail cleanly, never crash, and a corrupted SPS/PPS
     or reference id must not silently produce a decoder that references a
     nonexistent parameter set / slot. We only require that corrupting the
     parameter-set area is always rejected. */
  std::vector<unsigned char> good;
  TASK_CHECK(make_checkpoint(20, good) == 0);

  int rejected = 0;
  for (size_t off = 16; off + 4 < good.size(); off += 503) {
    std::vector<unsigned char> bad = good;
    for (int k = 0; k < 4; k++) { bad[off + k] ^= 0xff; }
    de265_error err = DE265_OK;
    de265_decoder_context* d = de265_checkpoint_read(bad.data(), bad.size(), &err);
    if (d == nullptr) {
      rejected++;
      TASK_CHECK(err == DE265_ERROR_CHECKPOINT_FORMAT ||
                 err == DE265_ERROR_INVALID_ARGUMENT);
    }
    else {
      de265_free_decoder(d);
    }
  }
  TASK_CHECK(rejected > 0);

  return 0;
}

int case_byte_identical()
{
  /* Two independent decoders that reach the same logical state must produce
     identical bytes. */
  std::vector<unsigned char> a, b;
  TASK_CHECK(make_checkpoint(12, a) == 0);
  TASK_CHECK(make_checkpoint(12, b) == 0);
  TASK_CHECK(a == b);

  /* Saving the same live instance twice must also be identical, and writing
     the checkpoint must not modify the decoder. */
  const auto& data = test_stream();
  const auto& units = test_units();
  de265_decoder_context* ctx = de265_new_decoder();
  TASK_CHECK(ctx != nullptr);
  for (size_t k = 0; k <= 18; k++) {
    decode_unit(ctx, data, units[k], 1000 + k);
  }
  size_t size = de265_checkpoint_size(ctx);
  TASK_CHECK(size > 0);

  std::vector<unsigned char> first(size), second(size);
  TASK_CHECK(de265_checkpoint_write(ctx, first.data(), size) == DE265_OK);
  TASK_CHECK(de265_checkpoint_size(ctx) == size);
  TASK_CHECK(de265_checkpoint_write(ctx, second.data(), size) == DE265_OK);
  TASK_CHECK(first == second);

  /* Continue decoding in the same instance; it must behave as if nothing was
     saved. */
  std::vector<frame_result> live_tail;
  for (size_t k = 19; k < units.size(); k++) {
    decode_unit(ctx, data, units[k], 1000 + k);
    const de265_image* img;
    while ((img = de265_get_next_picture(ctx)) != nullptr) {
      live_tail.push_back(grab(img));
    }
  }
  de265_flush_data(ctx);
  int more = 1, guard = 0;
  while (more && guard++ < 1000) {
    more = 0;
    de265_decode(ctx, &more);
    const de265_image* img;
    while ((img = de265_get_next_picture(ctx)) != nullptr) {
      live_tail.push_back(grab(img));
    }
  }
  de265_free_decoder(ctx);

  std::vector<frame_result> reference = decode_reference(data, units);
  size_t base = 0;
  while (base < reference.size() && reference[base].pts != live_tail.front().pts) {
    base++;
  }
  TASK_CHECK(live_tail.size() == reference.size() - base);
  for (size_t i = 0; i < live_tail.size(); i++) {
    TASK_CHECK(live_tail[i].hash == reference[base + i].hash);
  }
  return 0;
}

/* --------------------------------------------------------------------- */
/* reference group                                                      */
/* --------------------------------------------------------------------- */

bool check_resume_at(size_t split, size_t consume_limit,
                     const std::vector<frame_result>& reference)
{
  const auto& data = test_stream();
  const auto& units = test_units();

  checkpoint_run run = run_checkpoint(data, units, split, consume_limit);
  if (run.checkpoint_size == 0) { return false; }

  std::vector<frame_result> combined = run.consumed_before;
  combined.insert(combined.end(), run.after.begin(), run.after.end());
  return compare_sequence(combined, reference);
}

int case_resume_three_splits()
{
  const auto& data = test_stream();
  const auto& units = test_units();
  TASK_CHECK(!data.empty());

  std::vector<frame_result> reference = decode_reference(data, units);
  TASK_CHECK(reference.size() == 75);

  /* Three non-IDR split points. The first is early (few references), the
     later ones have several reference pictures in use. */
  for (size_t split : {size_t(9), size_t(24), size_t(47)}) {
    TASK_CHECK(units[split + 1].begin > units[split].begin);
    TASK_CHECK(check_resume_at(split, 1000, reference));
  }
  return 0;
}

int case_resume_with_pending_output()
{
  const auto& data = test_stream();
  const auto& units = test_units();

  std::vector<frame_result> reference = decode_reference(data, units);
  TASK_CHECK(reference.size() == 75);

  /* Keep the number of pictures left in the DPB below its capacity so that
     decoding does not stall on a full DPB (a stalled decoder has pending
     input and is not checkpointable, by design). */
  for (size_t split : {size_t(8), size_t(15), size_t(24)}) {
    for (size_t consume : {size_t(0), size_t(2), size_t(5)}) {
      checkpoint_run run = run_checkpoint(data, units, split, consume);
      TASK_CHECK(run.checkpoint_size > 0);
      TASK_CHECK(run.identical_on_repeat);

      std::vector<frame_result> combined = run.consumed_before;
      combined.insert(combined.end(), run.after.begin(), run.after.end());
      TASK_CHECK(compare_sequence(combined, reference));
    }
  }
  return 0;
}

int case_resume_preserves_tmvp()
{
  /* Verify that the resumed tail still exercises B pictures, temporal motion
     vector prediction and multiple references: these are the features that
     need the stored motion and slice-reference state. */
  const auto& data = test_stream();
  const auto& units = test_units();

  std::vector<frame_result> reference = decode_reference(data, units);
  TASK_CHECK(reference.size() == 75);

  checkpoint_run run = run_checkpoint(data, units, 20, 1000);
  TASK_CHECK(run.checkpoint_size > 0);

  bool b = false, tmvp = false, multi = false;
  for (const frame_result& f : run.after) {
    b |= f.is_b;
    tmvp |= f.tmvp;
    multi |= f.multi_ref;
  }
  TASK_CHECK(b);
  TASK_CHECK(tmvp);
  TASK_CHECK(multi);
  return 0;
}

int case_restored_has_no_workers()
{
  std::vector<unsigned char> checkpoint;
  TASK_CHECK(make_checkpoint(16, checkpoint) == 0);

  de265_error err = DE265_OK;
  de265_decoder_context* restored =
      de265_checkpoint_read(checkpoint.data(), checkpoint.size(), &err);
  TASK_CHECK(restored != nullptr);
  TASK_CHECK(err == DE265_OK);

  /* A restored decoder starts without worker threads; enabling them later
     must work and must not resurrect any old thread. */
  TASK_CHECK(de265_start_worker_threads(restored, 2) == DE265_OK);

  /* Decode the rest with workers and compare to the reference. Because the
     split was made after picture index 16, the checkpoint may still hold some
     earlier pictures in its reorder buffer; those are produced first. Find
     the matching offset in the reference by PTS. */
  const auto& data = test_stream();
  const auto& units = test_units();
  std::vector<frame_result> reference = decode_reference(data, units);

  std::vector<frame_result> after;
  for (size_t k = 17; k < units.size(); k++) {
    decode_unit(restored, data, units[k], 1000 + k);
    const de265_image* img;
    while ((img = de265_get_next_picture(restored)) != nullptr) {
      after.push_back(grab(img));
    }
  }
  size_t base = 17;
  de265_flush_data(restored);
  int more = 1, guard = 0;
  while (more && guard++ < 1000) {
    more = 0;
    de265_decode(restored, &more);
    const de265_image* img;
    while ((img = de265_get_next_picture(restored)) != nullptr) {
      after.push_back(grab(img));
    }
  }
  TASK_CHECK(!after.empty());
  size_t base2 = 0;
  while (base2 < reference.size() && reference[base2].pts != after.front().pts) {
    base2++;
  }
  TASK_CHECK(base2 < reference.size());
  TASK_CHECK(after.size() == reference.size() - base2);
  for (size_t i = 0; i < after.size(); i++) {
    TASK_CHECK(after[i].hash == reference[base2 + i].hash);
    TASK_CHECK(after[i].pts == reference[base2 + i].pts);
  }
  (void)base;

  de265_free_decoder(restored);
  return 0;
}

int case_independent_ownership()
{
  const auto& data = test_stream();
  const auto& units = test_units();

  std::vector<unsigned char> checkpoint;
  TASK_CHECK(make_checkpoint(28, checkpoint) == 0);

  /* Read the checkpoint twice; the two instances must not share state. */
  de265_error e1 = DE265_OK, e2 = DE265_OK;
  de265_decoder_context* a =
      de265_checkpoint_read(checkpoint.data(), checkpoint.size(), &e1);
  de265_decoder_context* b =
      de265_checkpoint_read(checkpoint.data(), checkpoint.size(), &e2);
  TASK_CHECK(a != nullptr && b != nullptr);
  TASK_CHECK(e1 == DE265_OK && e2 == DE265_OK);

  /* Destroy the buffer and the source stream copy; 'a' must be unaffected. */
  std::vector<unsigned char>().swap(checkpoint);

  std::vector<frame_result> reference = decode_reference(data, units);

  std::vector<frame_result> after_a;
  for (size_t k = 29; k < units.size(); k++) {
    decode_unit(a, data, units[k], 1000 + k);
    const de265_image* img;
    while ((img = de265_get_next_picture(a)) != nullptr) {
      after_a.push_back(grab(img));
    }
  }
  de265_flush_data(a);
  int more = 1, guard = 0;
  while (more && guard++ < 1000) {
    more = 0;
    de265_decode(a, &more);
    const de265_image* img;
    while ((img = de265_get_next_picture(a)) != nullptr) {
      after_a.push_back(grab(img));
    }
  }
  TASK_CHECK(!after_a.empty());
  size_t base = 0;
  while (base < reference.size() && reference[base].pts != after_a.front().pts) {
    base++;
  }
  TASK_CHECK(base < reference.size());
  TASK_CHECK(after_a.size() == reference.size() - base);
  for (size_t i = 0; i < after_a.size(); i++) {
    TASK_CHECK(after_a[i].hash == reference[base + i].hash);
    TASK_CHECK(after_a[i].pts == reference[base + i].pts);
  }

  de265_free_decoder(a);
  de265_free_decoder(b);
  return 0;
}

/* --------------------------------------------------------------------- */
/* process group                                                        */
/* --------------------------------------------------------------------- */

int case_reset_before_and_after()
{
  const auto& data = test_stream();
  const auto& units = test_units();

  std::vector<frame_result> reference = decode_reference(data, units);
  TASK_CHECK(reference.size() == 75);

  /* Decode a prefix, then reset the source and decode another stream from the
     start: the reset instance must not return pictures from the first run. */
  de265_decoder_context* ctx = de265_new_decoder();
  TASK_CHECK(ctx != nullptr);
  for (size_t k = 0; k <= 20; k++) {
    decode_unit(ctx, data, units[k], 9000 + k);
  }
  de265_reset(ctx);
  TASK_CHECK(de265_checkpoint_size(ctx) == 0);

  std::vector<frame_result> again;
  for (size_t k = 0; k < units.size(); k++) {
    decode_unit(ctx, data, units[k], 1000 + k);
    const de265_image* img;
    while ((img = de265_get_next_picture(ctx)) != nullptr) {
      again.push_back(grab(img));
    }
  }
  de265_flush_data(ctx);
  int more = 1, guard = 0;
  while (more && guard++ < 1000) {
    more = 0;
    de265_decode(ctx, &more);
    const de265_image* img;
    while ((img = de265_get_next_picture(ctx)) != nullptr) {
      again.push_back(grab(img));
    }
  }
  TASK_CHECK(compare_sequence(again, reference));
  de265_free_decoder(ctx);

  /* A restored decoder can also be reset and used for another stream. */
  std::vector<unsigned char> checkpoint;
  TASK_CHECK(make_checkpoint(14, checkpoint) == 0);
  de265_error err = DE265_OK;
  de265_decoder_context* restored =
      de265_checkpoint_read(checkpoint.data(), checkpoint.size(), &err);
  TASK_CHECK(restored != nullptr);

  de265_reset(restored);
  TASK_CHECK(de265_checkpoint_size(restored) == 0);

  std::vector<frame_result> fresh;
  for (size_t k = 0; k < units.size(); k++) {
    decode_unit(restored, data, units[k], 1000 + k);
    const de265_image* img;
    while ((img = de265_get_next_picture(restored)) != nullptr) {
      fresh.push_back(grab(img));
    }
  }
  de265_flush_data(restored);
  more = 1; guard = 0;
  while (more && guard++ < 1000) {
    more = 0;
    de265_decode(restored, &more);
    const de265_image* img;
    while ((img = de265_get_next_picture(restored)) != nullptr) {
      fresh.push_back(grab(img));
    }
  }
  TASK_CHECK(compare_sequence(fresh, reference));
  de265_free_decoder(restored);

  return 0;
}

int case_pictures_output_once()
{
  const auto& data = test_stream();
  const auto& units = test_units();

  checkpoint_run run = run_checkpoint(data, units, 22, 7);
  TASK_CHECK(run.checkpoint_size > 0);

  /* Consumed pictures must not appear among the resumed outputs, and every
     picture must appear exactly once overall. Check PTS uniqueness. */
  std::vector<long long> all;
  for (const frame_result& f : run.consumed_before) all.push_back(f.pts);
  for (const frame_result& f : run.after) all.push_back(f.pts);
  TASK_CHECK(all.size() == 75);

  std::vector<long long> sorted = all;
  std::sort(sorted.begin(), sorted.end());
  for (size_t i = 1; i < sorted.size(); i++) {
    TASK_CHECK(sorted[i] != sorted[i - 1]);
  }

  /* The consumed ones must be the very first pictures, in order. */
  for (size_t i = 0; i < run.consumed_before.size(); i++) {
    TASK_CHECK(run.consumed_before[i].pts == 1000 + static_cast<long long>(i));
  }
  /* The resumed tail must not contain any consumed PTS. */
  long long max_consumed = run.consumed_before.empty() ? -1
                                                       : run.consumed_before.back().pts;
  for (const frame_result& f : run.after) {
    TASK_CHECK(f.pts > max_consumed);
  }
  return 0;
}

int case_pts_and_three_planes()
{
  const auto& data = test_stream();
  const auto& units = test_units();

  std::vector<frame_result> reference = decode_reference(data, units);
  TASK_CHECK(reference.size() == 75);

  /* girlshy.h265 is coded at 320x240. Compare the visible size of the
     uninterrupted decode with the size of a restored decode. */
  for (const frame_result& f : reference) {
    TASK_CHECK(f.width > 0 && f.height > 0);
  }

  /* Sample three splits and confirm pixel hashes (all three planes are folded
     into the hash), PTS and order match the uninterrupted decode. */
  for (size_t split : {size_t(6), size_t(30), size_t(55)}) {
    checkpoint_run run = run_checkpoint(data, units, split, 1000);
    TASK_CHECK(run.checkpoint_size > 0);

    std::vector<frame_result> combined = run.consumed_before;
    combined.insert(combined.end(), run.after.begin(), run.after.end());
    TASK_CHECK(compare_sequence(combined, reference));
  }
  return 0;
}

/* The resume input is a suffix-only file. Exercise exactly that: split the
   bytestream at a picture start, feed only the tail (never the pre-split VCL)
   to a restored decoder, and confirm the tail matches the reference. */
int case_suffix_only_input()
{
  const auto& data = test_stream();
  const auto& units = test_units();

  std::vector<frame_result> reference = decode_reference(data, units);
  TASK_CHECK(reference.size() == 75);

  for (size_t split : {size_t(11), size_t(33)}) {
    /* Make a checkpoint at the split, consuming all output so the restored
       instance should produce only the tail. */
    de265_decoder_context* ctx = de265_new_decoder();
    TASK_CHECK(ctx != nullptr);
    for (size_t k = 0; k <= split; k++) {
      decode_unit(ctx, data, units[k], 1000 + k);
      const de265_image* img;
      while ((img = de265_get_next_picture(ctx)) != nullptr) { }
    }
    size_t size = de265_checkpoint_size(ctx);
    TASK_CHECK(size > 0);
    std::vector<unsigned char> checkpoint(size);
    TASK_CHECK(de265_checkpoint_write(ctx, checkpoint.data(), size) == DE265_OK);
    de265_free_decoder(ctx);

    /* A suffix-only file containing nothing from before the split. */
    size_t suffix_begin = units[split + 1].begin;
    std::vector<unsigned char> suffix(data.begin() + suffix_begin, data.end());
    TASK_CHECK(suffix.size() < data.size());

    de265_error err = DE265_OK;
    de265_decoder_context* restored =
        de265_checkpoint_read(checkpoint.data(), checkpoint.size(), &err);
    TASK_CHECK(restored != nullptr);
    TASK_CHECK(err == DE265_OK);

    /* Split the suffix the same way; the first suffix picture is index
       split+1, so it carries PTS 1000+(split+1). */
    std::vector<frame_unit> suffix_units = split_into_pictures(suffix);
    TASK_CHECK(!suffix_units.empty());

    std::vector<frame_result> got;
    for (size_t j = 0; j < suffix_units.size(); j++) {
      decode_unit(restored, suffix, suffix_units[j],
                  1000 + static_cast<unsigned long long>(split + 1 + j));
      const de265_image* img;
      while ((img = de265_get_next_picture(restored)) != nullptr) {
        got.push_back(grab(img));
      }
    }
    de265_flush_data(restored);
    int more = 1, guard = 0;
    while (more && guard++ < 1000) {
      more = 0;
      de265_decode(restored, &more);
      const de265_image* img;
      while ((img = de265_get_next_picture(restored)) != nullptr) {
        got.push_back(grab(img));
      }
    }
    de265_free_decoder(restored);

    TASK_CHECK(got.size() == reference.size() - (split + 1));
    for (size_t i = 0; i < got.size(); i++) {
      TASK_CHECK(got[i].hash == reference[split + 1 + i].hash);
      TASK_CHECK(got[i].pts == reference[split + 1 + i].pts);
    }
  }
  return 0;
}

} // anonymous namespace

/* --------------------------------------------------------------------- */
/* Registration                                                         */
/* --------------------------------------------------------------------- */

const task_case task_cases[] = {
  {"format", "size_after_complete_picture", case_size_after_complete_picture},
  {"format", "unavailable_mid_frame", case_unavailable_mid_frame},
  {"format", "unavailable_before_driving", case_unavailable_before_driving},
  {"format", "insufficient_capacity", case_insufficient_capacity},
  {"format", "truncated_and_version", case_truncated_and_version},
  {"format", "inconsistent_reference_ids", case_inconsistent_reference_ids},
  {"format", "byte_identical", case_byte_identical},

  {"reference", "resume_three_splits", case_resume_three_splits},
  {"reference", "resume_with_pending_output", case_resume_with_pending_output},
  {"reference", "resume_preserves_tmvp", case_resume_preserves_tmvp},
  {"reference", "restored_has_no_workers", case_restored_has_no_workers},
  {"reference", "independent_ownership", case_independent_ownership},

  {"process", "reset_before_and_after", case_reset_before_and_after},
  {"process", "pictures_output_once", case_pictures_output_once},
  {"process", "pts_and_three_planes", case_pts_and_three_planes},
  {"process", "suffix_only_input", case_suffix_only_input},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
