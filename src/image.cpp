#include "softgpu/image.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace softgpu {

namespace {

struct File {
  std::FILE* f;
  explicit File(const std::string& p, const char* mode) : f(std::fopen(p.c_str(), mode)) {}
  ~File() {
    if (f) std::fclose(f);
  }
};

uint32_t crc32(const uint8_t* p, size_t n, uint32_t crc = 0) {
  static uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
    init = true;
  }
  crc = ~crc;
  for (size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

void put_be32(std::vector<uint8_t>& v, uint32_t x) {
  v.push_back(static_cast<uint8_t>(x >> 24));
  v.push_back(static_cast<uint8_t>(x >> 16));
  v.push_back(static_cast<uint8_t>(x >> 8));
  v.push_back(static_cast<uint8_t>(x));
}

void put_chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
  put_be32(out, static_cast<uint32_t>(data.size()));
  std::vector<uint8_t> td(type, type + 4);
  td.insert(td.end(), data.begin(), data.end());
  out.insert(out.end(), td.begin(), td.end());
  put_be32(out, crc32(td.data(), td.size()));
}

}  // namespace

bool write_ppm(const std::string& path, const Image& img) {
  File f(path, "wb");
  if (!f.f) return false;
  std::fprintf(f.f, "P6\n%d %d\n255\n", img.width, img.height);
  std::vector<uint8_t> row(static_cast<size_t>(img.width) * 3);
  for (int y = 0; y < img.height; ++y) {
    for (int x = 0; x < img.width; ++x) std::memcpy(&row[x * 3], img.at(x, y), 3);
    if (std::fwrite(row.data(), 1, row.size(), f.f) != row.size()) return false;
  }
  return true;
}

bool write_pgm(const std::string& path, int w, int h, const float* v, float lo, float hi) {
  File f(path, "wb");
  if (!f.f) return false;
  std::fprintf(f.f, "P5\n%d %d\n255\n", w, h);
  std::vector<uint8_t> row(static_cast<size_t>(w));
  const float scale = hi > lo ? 1.0f / (hi - lo) : 1.0f;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) row[x] = to_u8((v[static_cast<size_t>(y) * w + x] - lo) * scale);
    if (std::fwrite(row.data(), 1, row.size(), f.f) != row.size()) return false;
  }
  return true;
}

bool write_png(const std::string& path, const Image& img) {
  // Raw scanlines: filter byte 0 + RGBA.
  const size_t stride = static_cast<size_t>(img.width) * 4 + 1;
  std::vector<uint8_t> raw(stride * img.height);
  for (int y = 0; y < img.height; ++y) {
    raw[y * stride] = 0;
    std::memcpy(&raw[y * stride + 1], img.at(0, y), static_cast<size_t>(img.width) * 4);
  }
  // zlib stream of stored blocks (max 65535 bytes each) + adler32.
  std::vector<uint8_t> z;
  z.push_back(0x78);
  z.push_back(0x01);
  size_t pos = 0;
  while (pos < raw.size() || raw.empty()) {
    const size_t n = std::min<size_t>(65535, raw.size() - pos);
    const bool last = pos + n == raw.size();
    z.push_back(last ? 1 : 0);
    z.push_back(static_cast<uint8_t>(n));
    z.push_back(static_cast<uint8_t>(n >> 8));
    z.push_back(static_cast<uint8_t>(~n));
    z.push_back(static_cast<uint8_t>(~n >> 8));
    z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
    pos += n;
    if (raw.empty()) break;
  }
  uint32_t a = 1, b = 0;
  for (uint8_t c : raw) {
    a = (a + c) % 65521;
    b = (b + a) % 65521;
  }
  put_be32(z, (b << 16) | a);

  std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  std::vector<uint8_t> ihdr;
  put_be32(ihdr, static_cast<uint32_t>(img.width));
  put_be32(ihdr, static_cast<uint32_t>(img.height));
  ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});  // 8-bit RGBA, deflate, no filter, no interlace
  put_chunk(out, "IHDR", ihdr);
  put_chunk(out, "IDAT", z);
  put_chunk(out, "IEND", {});
  File f(path, "wb");
  if (!f.f) return false;
  return std::fwrite(out.data(), 1, out.size(), f.f) == out.size();
}

bool write_image(const std::string& path, const Image& img) {
  const bool png = path.size() > 4 && path.compare(path.size() - 4, 4, ".png") == 0;
  return png ? write_png(path, img) : write_ppm(path, img);
}

bool read_ppm(const std::string& path, Image& img) {
  File f(path, "rb");
  if (!f.f) return false;
  int w = 0, h = 0, maxv = 0;
  if (std::fscanf(f.f, "P6 %d %d %d", &w, &h, &maxv) != 3 || maxv != 255) return false;
  std::fgetc(f.f);  // single whitespace after header
  img = Image(w, h);
  std::vector<uint8_t> row(static_cast<size_t>(w) * 3);
  for (int y = 0; y < h; ++y) {
    if (std::fread(row.data(), 1, row.size(), f.f) != row.size()) return false;
    for (int x = 0; x < w; ++x) img.set(x, y, row[x * 3], row[x * 3 + 1], row[x * 3 + 2]);
  }
  return true;
}

}  // namespace softgpu
