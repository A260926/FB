/*
 * Checkpoint demo: save a decoder state between two pictures and resume it in
 * a new process.
 *
 *   checkpoint-demo save   <input.h265> <state.bin> <cursor.txt> <suffix.h265> <expected.txt>
 *   checkpoint-demo resume <state.bin> <cursor.txt> <suffix.h265> <actual.txt>
 *   checkpoint-demo verify <expected.txt> <actual.txt>
 *
 * "save" decodes a prefix of the input up to a split point, writes the decoder
 * checkpoint, a cursor file with the byte offset of the first input byte after
 * the split, the remaining (suffix) input starting at that offset, and the
 * expected picture list of an uninterrupted decode.
 *
 * "resume" reads only the checkpoint, the cursor and the suffix file. It does
 * not read any VCL byte from before the split point. It decodes the suffix and
 * writes the resulting picture list.
 *
 * "verify" compares the two picture lists (pixels, PTS and order) and returns
 * non-zero on any difference.
 */

#include "libde265/de265.h"
#include "libde265/image.h"
#include "libde265/slice.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

const size_t SPLIT_PICTURE = 30;   /* split after this many complete pictures */

struct frame_unit {
  size_t begin;
  size_t end;
};

struct frame_result {
  long long pts;
  unsigned long long hash;
};

unsigned long long fnv_init() { return 14695981039346656037ull; }

unsigned long long fnv_bytes(const unsigned char* p, int n, unsigned long long h)
{
  for (int i = 0; i < n; i++) {
    h ^= p[i];
    h *= 1099511628211ull;
  }
  return h;
}

bool read_file(const std::string& path, std::vector<unsigned char>& out)
{
  std::ifstream in(path, std::ios::binary);
  if (!in) { return false; }
  out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return !out.empty();
}

bool write_file(const std::string& path, const void* data, size_t size)
{
  std::ofstream out(path, std::ios::binary);
  if (!out) { return false; }
  out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
  return out.good();
}

std::vector<frame_unit> split_into_pictures(const std::vector<unsigned char>& data)
{
  struct nal { size_t start; int type; bool first_slice; };
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
    nals.push_back({sc.first, type, first_slice});
  }
  std::vector<size_t> picture_starts;
  for (const nal& x : nals) {
    if (x.type < 32 && x.first_slice) { picture_starts.push_back(x.start); }
  }
  std::vector<frame_unit> units;
  for (size_t k = 0; k < picture_starts.size(); k++) {
    size_t begin = (k == 0) ? 0 : picture_starts[k];
    size_t end = (k + 1 < picture_starts.size()) ? picture_starts[k + 1] : data.size();
    units.push_back({begin, end});
  }
  return units;
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
  return r;
}

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
  int more = 0;
  de265_decode(ctx, &more);
}

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

bool write_results(const std::string& path, const std::vector<frame_result>& frames)
{
  std::string text;
  for (const frame_result& f : frames) {
    char line[64];
    snprintf(line, sizeof(line), "%lld %016llx\n", f.pts, f.hash);
    text += line;
  }
  return write_file(path, text.data(), text.size());
}

int do_save(const char* input, const char* state_path, const char* cursor_path,
            const char* suffix_path, const char* expected_path)
{
  std::vector<unsigned char> data;
  if (!read_file(input, data)) {
    fprintf(stderr, "cannot read %s\n", input);
    return 1;
  }
  std::vector<frame_unit> units = split_into_pictures(data);
  if (units.size() <= SPLIT_PICTURE + 1) {
    fprintf(stderr, "stream too short (%zu pictures)\n", units.size());
    return 1;
  }

  /* Expected output of an uninterrupted decode. */
  std::vector<frame_result> expected = decode_reference(data, units);
  if (expected.empty()) {
    fprintf(stderr, "reference decode produced no pictures\n");
    return 1;
  }

  /* Decode the prefix, leaving a couple of pictures in the output queue so
     that the checkpoint exercises both consumed and unconsumed output. */
  de265_decoder_context* ctx = de265_new_decoder();
  if (!ctx) { return 1; }

  size_t consumed = 0;
  for (size_t k = 0; k <= SPLIT_PICTURE; k++) {
    decode_unit(ctx, data, units[k], 1000 + k);
    /* Consume pictures as they become available, but stop early so that some
       output is still queued at the split point. The pictures left in the
       queue, plus everything decoded from the suffix, are what the resumed
       process is expected to output. */
    while (de265_peek_next_picture(ctx) != nullptr && k + 1 < SPLIT_PICTURE) {
      de265_release_next_picture(ctx);
      consumed++;
    }
  }

  size_t checkpoint_size = de265_checkpoint_size(ctx);
  if (checkpoint_size == 0) {
    fprintf(stderr, "decoder state is not checkpointable at split %zu\n", SPLIT_PICTURE);
    de265_free_decoder(ctx);
    return 1;
  }

  std::vector<unsigned char> checkpoint(checkpoint_size);
  de265_error err = de265_checkpoint_write(ctx, checkpoint.data(), checkpoint.size());
  if (err != DE265_OK) {
    fprintf(stderr, "checkpoint_write failed: %d\n", err);
    de265_free_decoder(ctx);
    return 1;
  }
  de265_free_decoder(ctx);

  size_t cursor = units[SPLIT_PICTURE + 1].begin;

  if (!write_file(state_path, checkpoint.data(), checkpoint.size())) {
    fprintf(stderr, "cannot write %s\n", state_path);
    return 1;
  }
  {
    /* Cursor file:
         line 1: byte offset of the first input byte after the split
         line 2: global index of the first picture in the suffix
         line 3: PTS of the first picture in the suffix
       The suffix file begins at the recorded offset; resume never needs the
       bytes before it. */
    char text[128];
    snprintf(text, sizeof(text), "%zu\n%zu\n%llu\n", cursor,
             SPLIT_PICTURE + 1,
             static_cast<unsigned long long>(1000 + SPLIT_PICTURE + 1));
    if (!write_file(cursor_path, text, strlen(text))) {
      fprintf(stderr, "cannot write %s\n", cursor_path);
      return 1;
    }
  }
  if (!write_file(suffix_path, data.data() + cursor, data.size() - cursor)) {
    fprintf(stderr, "cannot write %s\n", suffix_path);
    return 1;
  }
  /* The resumed process starts from the queue contents of the checkpoint and
     then decodes the suffix, so its output is the reference tail after the
     pictures this process already consumed. */
  if (consumed > expected.size()) { consumed = expected.size(); }
  std::vector<frame_result> expected_tail(expected.begin() + consumed, expected.end());
  if (!write_results(expected_path, expected_tail)) {
    fprintf(stderr, "cannot write %s\n", expected_path);
    return 1;
  }

  return 0;
}

int do_resume(const char* state_path, const char* cursor_path,
              const char* suffix_path, const char* actual_path)
{
  std::vector<unsigned char> checkpoint;
  if (!read_file(state_path, checkpoint)) {
    fprintf(stderr, "cannot read %s\n", state_path);
    return 1;
  }

  std::vector<unsigned char> cursor_text;
  if (!read_file(cursor_path, cursor_text)) {
    fprintf(stderr, "cannot read %s\n", cursor_path);
    return 1;
  }
  /* Parse the three cursor lines. */
  cursor_text.push_back(0);
  const char* p = reinterpret_cast<const char*>(cursor_text.data());
  char* endp = nullptr;
  size_t cursor = static_cast<size_t>(strtoull(p, &endp, 10));
  size_t first_picture_index = 0;
  unsigned long long first_pts = 1000;
  if (endp) {
    first_picture_index = static_cast<size_t>(strtoull(endp + 1, &endp, 10));
    if (endp) {
      first_pts = strtoull(endp + 1, &endp, 10);
    }
  }
  (void)cursor;   /* informational: the suffix file starts here */

  std::vector<unsigned char> suffix;
  if (!read_file(suffix_path, suffix)) {
    fprintf(stderr, "cannot read %s\n", suffix_path);
    return 1;
  }

  de265_error err = DE265_OK;
  de265_decoder_context* ctx =
      de265_checkpoint_read(checkpoint.data(), checkpoint.size(), &err);
  if (!ctx || err != DE265_OK) {
    fprintf(stderr, "checkpoint_read failed: %d\n", err);
    if (ctx) { de265_free_decoder(ctx); }
    return 1;
  }

  /* Decode the suffix, splitting it into pictures the same way save did. The
     PTS of the first suffix picture and its global index come from the cursor;
     later pictures follow consecutively, matching the save-side numbering. */
  std::vector<frame_unit> units = split_into_pictures(suffix);
  std::vector<frame_result> results;
  (void)first_picture_index;

  for (size_t j = 0; j < units.size(); j++) {
    unsigned long long pts = first_pts + j;
    decode_unit(ctx, suffix, units[j], pts);
    const de265_image* img;
    while ((img = de265_get_next_picture(ctx)) != nullptr) {
      results.push_back(grab(img));
    }
  }
  de265_flush_data(ctx);
  int more = 1, guard = 0;
  while (more && guard++ < 1000) {
    more = 0;
    de265_decode(ctx, &more);
    const de265_image* img;
    while ((img = de265_get_next_picture(ctx)) != nullptr) {
      results.push_back(grab(img));
    }
  }
  de265_free_decoder(ctx);

  if (!write_results(actual_path, results)) {
    fprintf(stderr, "cannot write %s\n", actual_path);
    return 1;
  }
  return 0;
}

int do_verify(const char* expected_path, const char* actual_path)
{
  std::vector<unsigned char> a, b;
  if (!read_file(expected_path, a) || !read_file(actual_path, b)) {
    fprintf(stderr, "cannot read result files\n");
    return 1;
  }
  if (a != b) {
    fprintf(stderr, "picture sequences differ\n");
    return 1;
  }
  printf("verify: %zu bytes identical\n", a.size());
  return 0;
}

} // anonymous namespace

int main(int argc, char** argv)
{
  if (argc < 2) {
    fprintf(stderr,
            "usage:\n"
            "  %s save   <input.h265> <state.bin> <cursor.txt> <suffix.h265> <expected.txt>\n"
            "  %s resume <state.bin> <cursor.txt> <suffix.h265> <actual.txt>\n"
            "  %s verify <expected.txt> <actual.txt>\n",
            argv[0], argv[0], argv[0]);
    return 2;
  }

  std::string cmd = argv[1];
  if (cmd == "save") {
    if (argc != 7) { return 2; }
    return do_save(argv[2], argv[3], argv[4], argv[5], argv[6]);
  }
  if (cmd == "resume") {
    if (argc != 6) { return 2; }
    return do_resume(argv[2], argv[3], argv[4], argv[5]);
  }
  if (cmd == "verify") {
    if (argc != 4) { return 2; }
    return do_verify(argv[2], argv[3]);
  }

  fprintf(stderr, "unknown command: %s\n", cmd.c_str());
  return 2;
}
