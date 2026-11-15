#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "softgpu/clip.hpp"
#include "softgpu/clock.hpp"
#include "softgpu/framebuffer.hpp"
#include "softgpu/raster.hpp"
#include "softgpu/stats.hpp"
#include "softgpu/thread_pool.hpp"

namespace softgpu {

// What the fragment shader sees. attr are perspective-correct; ddx/ddy are their exact
// screen-space derivatives (a GPU would estimate them from a 2x2 quad).
template <int N>
struct Fragment {
  int x = 0, y = 0;
  float z = 0;
  float attr[attr_count<N>()] = {};
  float ddx[attr_count<N>()] = {};
  float ddy[attr_count<N>()] = {};
  bool front_facing = true;
};

struct DrawState {
  CullMode cull = CullMode::Back;
  bool depth_test = true;
  bool depth_write = true;
  bool blend = false;  // src-over: out = src * src.a + dst * (1 - src.a)
};

enum class Mode {
  Immediate,  // rasterize each triangle over its whole bbox as soon as it is set up
  Tiled,      // bin set-up triangles to screen tiles, rasterize tiles in parallel on finish()
};

struct Config {
  Mode mode = Mode::Tiled;
  int threads = 1;
  int tile_size = 32;
};

// A vertex shader is any callable VsOut<N>(const Vertex&); a fragment shader is any callable
// bool(const Fragment<N>&, Vec4& color) that returns false to discard.
class Renderer {
 public:
  Renderer(int width, int height, Config cfg = {})
      : fb_(width, height), cfg_(cfg), pool_(cfg.threads) {
    tiles_x_ = (width + cfg.tile_size - 1) / cfg.tile_size;
    tiles_y_ = (height + cfg.tile_size - 1) / cfg.tile_size;
    bins_.resize(static_cast<size_t>(tiles_x_) * tiles_y_);
    tile_counts_.assign(bins_.size(), 0u);
  }

  Framebuffer& fb() { return fb_; }
  const Framebuffer& fb() const { return fb_; }
  Stats& stats() { return stats_; }
  const Config& config() const { return cfg_; }
  int tiles_x() const { return tiles_x_; }
  int tiles_y() const { return tiles_y_; }
  // Triangles binned into a tile this frame (valid between the draws and finish()).
  uint32_t bin_count(int tx, int ty) const { return tile_counts_[static_cast<size_t>(ty) * tiles_x_ + tx]; }

  void clear(Vec4 color, float depth = 1.0f) { fb_.clear(color, depth); }

  // Debug overlays, drawn by finish() after the fill: triangle edges, and the tile grid tinted
  // by how many triangles were binned into each tile.
  void set_wireframe(bool on) { wireframe_ = on; }
  void set_show_tiles(bool on) { show_tiles_ = on; }

  template <int N, class Vertex, class VS, class FS>
  void draw(const Vertex* verts, size_t nverts, const uint32_t* idx, size_t nidx, const VS& vs, const FS& fs,
            const DrawState& st = DrawState{}) {
    const uint64_t t0 = now_ns();
    auto batch = std::make_unique<Batch<N, FS>>(fs, st);

    // Vertex stage: pure per-vertex, so it splits into chunks across the pool.
    std::vector<VsOut<N>> out(nverts);
    const size_t vchunk = 4096;
    pool_.run((nverts + vchunk - 1) / vchunk, [&](size_t c) {
      const size_t end = std::min(nverts, (c + 1) * vchunk);
      for (size_t i = c * vchunk; i < end; ++i) out[i] = vs(verts[i]);
    });
    stats_.vertices_shaded += nverts;
    const uint64_t t1 = now_ns();
    stats_.vertex_ns += t1 - t0;

    // Primitive assembly, clipping, perspective divide, viewport and setup are independent per
    // triangle, so they run per chunk in parallel; each chunk owns its output so nothing is
    // copied afterwards. Binning walks the chunks in order, which keeps submission order.
    const size_t ntris = nidx / 3, tchunk = 2048;
    const size_t nchunks = (ntris + tchunk - 1) / tchunk;
    batch->chunks.resize(nchunks);
    std::vector<ChunkStats> cstats(nchunks);
    std::vector<std::vector<Line>> cedges(wireframe_ ? nchunks : 0);
    pool_.run(nchunks, [&](size_t c) {
      std::vector<SetupTri<N>>& tris = batch->chunks[c];
      tris.reserve(tchunk);
      ChunkStats& cs = cstats[c];
      VsOut<N> poly[kMaxClipVerts];
      ScreenVertex<N> sv[kMaxClipVerts];
      const size_t end = std::min(ntris, (c + 1) * tchunk);
      for (size_t t = c * tchunk; t < end; ++t) {
        const size_t i = t * 3;
        ++cs.in;
        const VsOut<N> tri[3] = {out[idx[i]], out[idx[i + 1]], out[idx[i + 2]]};
        bool clipped = false;
        const int n = clip_triangle<N>(tri, poly, &clipped);
        if (n == 0) {
          ++cs.rejected;
          continue;
        }
        if (clipped) ++cs.clipped;
        for (int k = 0; k < n; ++k) sv[k] = to_screen<N>(poly[k]);
        for (int k = 1; k + 1 < n; ++k) {
          const ScreenVertex<N> fan[3] = {sv[0], sv[k], sv[k + 1]};
          SetupTri<N> setup;
          const Setup r = setup_triangle<N>(fan, st.cull, fb_.width, fb_.height, setup);
          if (r != Setup::Ok) {
            if (r == Setup::Culled)
              ++cs.culled;
            else
              ++cs.degenerate;
            continue;
          }
          tris.push_back(setup);
          if (wireframe_)
            for (int e = 0; e < 3; ++e)
              cedges[c].push_back({fan[e].x, fan[e].y, fan[(e + 1) % 3].x, fan[(e + 1) % 3].y});
        }
      }
    });
    const uint64_t t_setup = now_ns();
    const uint32_t batch_index = static_cast<uint32_t>(batches_.size());
    for (size_t c = 0; c < nchunks; ++c) {
      stats_.triangles_in += cstats[c].in;
      stats_.triangles_rejected += cstats[c].rejected;
      stats_.triangles_clipped += cstats[c].clipped;
      stats_.triangles_culled += cstats[c].culled;
      stats_.triangles_degenerate += cstats[c].degenerate;
      stats_.triangles_rasterized += batch->chunks[c].size();
      if (wireframe_) edges_.insert(edges_.end(), cedges[c].begin(), cedges[c].end());
    }

    // Binning in two parallel passes, the way hardware tilers do it: count (triangle, tile)
    // pairs per chunk, prefix-sum per tile so every chunk knows its slot range, then scatter.
    // Chunks land in submission order inside each tile, and no two threads share a slot.
    const size_t ntiles = bins_.size();
    counts_.assign(nchunks * ntiles, 0u);
    pool_.run(nchunks, [&](size_t c) {
      uint32_t* cnt = &counts_[c * ntiles];
      for (const SetupTri<N>& tri : batch->chunks[c]) for_each_tile(tri.bbox, [&](size_t tile) { ++cnt[tile]; });
    });
    for (size_t tile = 0; tile < ntiles; ++tile) {
      uint32_t run = 0;
      for (size_t c = 0; c < nchunks; ++c) {
        const uint32_t n = counts_[c * ntiles + tile];
        counts_[c * ntiles + tile] = static_cast<uint32_t>(bins_[tile].size()) + run;
        run += n;
      }
      tile_counts_[tile] += run;
      stats_.tile_bins += run;
      if (cfg_.mode == Mode::Tiled) bins_[tile].resize(bins_[tile].size() + run);
    }
    if (cfg_.mode == Mode::Tiled) {
      pool_.run(nchunks, [&](size_t c) {
        uint32_t* slot = &counts_[c * ntiles];
        for (uint32_t t = 0; t < batch->chunks[c].size(); ++t)
          for_each_tile(batch->chunks[c][t].bbox, [&](size_t tile) {
            bins_[tile][slot[tile]++] = TriRef{batch_index, static_cast<uint32_t>(c), t};
          });
      });
    }
    const uint64_t t_bin = now_ns();

    // Immediate reference path: rasterize each triangle over its whole bbox, in order, here.
    if (cfg_.mode == Mode::Immediate)
      for (size_t c = 0; c < nchunks; ++c)
        for (const SetupTri<N>& tri : batch->chunks[c]) batch->raster(tri, tri.bbox, fb_, stats_.raster);
    const uint64_t t2 = now_ns();
    stats_.setup_ns += t_setup - t1;
    stats_.bin_ns += t_bin - t_setup;
    stats_.raster_ns += t2 - t_bin;
    stats_.total_ns += t2 - t0;
    if (cfg_.mode == Mode::Tiled) batches_.push_back(std::move(batch));
  }

  // Tiled mode: rasterize every bin. Each tile is owned by exactly one thread for the whole
  // pass, and walks its triangles in submission order, so depth test and blending see the
  // same fragment order as the immediate path.
  void finish() {
    const uint64_t t0 = now_ns();
    if (cfg_.mode == Mode::Tiled && !batches_.empty()) {
      pool_.run(bins_.size(), [&](size_t tile) {
        const auto& bin = bins_[tile];
        if (bin.empty()) return;
        const Rect r = tile_rect(static_cast<int>(tile));
        RasterCounters local;
        for (const TriRef& ref : bin) batches_[ref.batch]->raster(ref, r, fb_, local);
        std::lock_guard<std::mutex> lk(mu_);
        stats_.raster.add(local);
      });
    }
    for (auto& b : bins_) b.clear();
    batches_.clear();
    if (show_tiles_) draw_tile_overlay();
    if (wireframe_) draw_edges();
    edges_.clear();
    std::fill(tile_counts_.begin(), tile_counts_.end(), 0u);
    const uint64_t dt = now_ns() - t0;
    stats_.raster_ns += dt;
    stats_.total_ns += dt;
  }

  Rect tile_rect(int tile) const {
    const int tx = tile % tiles_x_, ty = tile / tiles_x_;
    return Rect{tx * cfg_.tile_size, ty * cfg_.tile_size, std::min(fb_.width, (tx + 1) * cfg_.tile_size) - 1,
                std::min(fb_.height, (ty + 1) * cfg_.tile_size) - 1};
  }

 private:
  struct TriRef {
    uint32_t batch, chunk, tri;
  };

  struct ChunkStats {
    uint64_t in = 0, rejected = 0, clipped = 0, culled = 0, degenerate = 0;
  };

  struct BatchBase {
    virtual ~BatchBase() = default;
    virtual void raster(const TriRef& ref, const Rect& clip, Framebuffer& fb, RasterCounters& c) const = 0;
  };

  template <int N, class FS>
  struct Batch : BatchBase {
    std::vector<std::vector<SetupTri<N>>> chunks;
    FS fs;
    DrawState st;
    Batch(const FS& f, const DrawState& s) : fs(f), st(s) {}

    void raster(const TriRef& ref, const Rect& clip, Framebuffer& fb, RasterCounters& c) const override {
      const SetupTri<N>& t = chunks[ref.chunk][ref.tri];
      raster(t, clip.intersect(t.bbox), fb, c);
    }

    void raster(const SetupTri<N>& t, const Rect& clip, Framebuffer& fb, RasterCounters& c) const {
      uint8_t* color = fb.color.rgba.data();
      float* depth = fb.depth.data();
      const int width = fb.width;
      rasterize<N>(t, clip, [&](int px, int py, float l0, float l1, float l2) {
        ++c.fragments_in;
        const size_t pix = static_cast<size_t>(py) * width + px;
        const float z = l0 * t.z[0] + l1 * t.z[1] + l2 * t.z[2];
        if (st.depth_test && !(z < depth[pix])) {
          ++c.early_z_rejected;
          return;
        }
        Fragment<N> f;
        f.x = px;
        f.y = py;
        f.z = z;
        f.front_facing = t.front_facing;
        const float den = l0 * t.invw[0] + l1 * t.invw[1] + l2 * t.invw[2];
        const float inv = 1.0f / den;
        const float inv2 = inv * inv;
        for (int k = 0; k < N; ++k) {
          const float num = l0 * t.attr[0][k] + l1 * t.attr[1][k] + l2 * t.attr[2][k];
          f.attr[k] = num * inv;
          f.ddx[k] = (t.dattr_dx[k] * den - num * t.dinvw_dx) * inv2;
          f.ddy[k] = (t.dattr_dy[k] * den - num * t.dinvw_dy) * inv2;
        }
        Vec4 col;
        if (!fs(f, col)) {
          ++c.fragments_discarded;
          return;
        }
        ++c.fragments_shaded;
        uint8_t* dst = color + pix * 4;
        if (st.blend) {
          ++c.fragments_blended;
          const float a = clampf(col.w, 0.0f, 1.0f), ia = 1.0f - a;
          const float s = 1.0f / 255.0f;
          col.x = col.x * a + dst[0] * s * ia;
          col.y = col.y * a + dst[1] * s * ia;
          col.z = col.z * a + dst[2] * s * ia;
          col.w = a + dst[3] * s * ia;
        }
        dst[0] = to_u8(col.x);
        dst[1] = to_u8(col.y);
        dst[2] = to_u8(col.z);
        dst[3] = to_u8(col.w);
        if (st.depth_write) depth[pix] = z;
      });
    }
  };

  template <int N>
  ScreenVertex<N> to_screen(const VsOut<N>& v) const {
    ScreenVertex<N> s;
    const float invw = 1.0f / v.pos.w;
    s.x = (v.pos.x * invw * 0.5f + 0.5f) * static_cast<float>(fb_.width);
    s.y = (0.5f - v.pos.y * invw * 0.5f) * static_cast<float>(fb_.height);
    s.z = v.pos.z * invw * 0.5f + 0.5f;
    s.invw = invw;
    for (int k = 0; k < N; ++k) s.attr[k] = v.attr[k] * invw;
    return s;
  }

  // Bounding-box binning: every tile the bbox touches.
  template <class F>
  void for_each_tile(const Rect& bbox, F&& f) const {
    const int tx0 = bbox.x0 / cfg_.tile_size, tx1 = bbox.x1 / cfg_.tile_size;
    const int ty0 = bbox.y0 / cfg_.tile_size, ty1 = bbox.y1 / cfg_.tile_size;
    for (int ty = ty0; ty <= ty1; ++ty)
      for (int tx = tx0; tx <= tx1; ++tx) f(static_cast<size_t>(ty) * tiles_x_ + tx);
  }

  struct Line {
    float x0, y0, x1, y1;
  };

  void draw_line(const Line& l, uint8_t r, uint8_t g, uint8_t b) {
    const float dx = l.x1 - l.x0, dy = l.y1 - l.y0;
    const int steps = static_cast<int>(std::max(std::fabs(dx), std::fabs(dy))) + 1;
    for (int i = 0; i <= steps; ++i) {
      const float t = static_cast<float>(i) / steps;
      const int x = static_cast<int>(l.x0 + dx * t), y = static_cast<int>(l.y0 + dy * t);
      if (x < 0 || y < 0 || x >= fb_.width || y >= fb_.height) continue;
      fb_.color.set(x, y, r, g, b);
    }
  }

  void draw_edges() {
    for (const Line& l : edges_) draw_line(l, 255, 255, 255);
  }

  void draw_tile_overlay() {
    uint32_t max_count = 1;
    for (uint32_t c : tile_counts_) max_count = std::max(max_count, c);
    for (int tile = 0; tile < tiles_x_ * tiles_y_; ++tile) {
      const Rect r = tile_rect(tile);
      const float heat = static_cast<float>(tile_counts_[tile]) / static_cast<float>(max_count);
      const float a = tile_counts_[tile] ? 0.15f + 0.45f * heat : 0.0f;
      for (int y = r.y0; y <= r.y1; ++y)
        for (int x = r.x0; x <= r.x1; ++x) {
          uint8_t* p = fb_.color.at(x, y);
          const bool grid = x == r.x0 || y == r.y0;
          if (grid) {
            p[0] = p[1] = p[2] = 40;
            continue;
          }
          p[0] = static_cast<uint8_t>(p[0] * (1 - a) + 255 * a);
          p[1] = static_cast<uint8_t>(p[1] * (1 - a) + 200 * (1 - heat) * a);
          p[2] = static_cast<uint8_t>(p[2] * (1 - a));
        }
    }
  }

  Framebuffer fb_;
  Config cfg_;
  ThreadPool pool_;
  Stats stats_;
  int tiles_x_ = 0, tiles_y_ = 0;
  std::vector<std::vector<TriRef>> bins_;
  std::vector<uint32_t> tile_counts_;
  std::vector<uint32_t> counts_;  // per (chunk, tile) scratch for the binning passes
  std::vector<std::unique_ptr<BatchBase>> batches_;
  std::vector<Line> edges_;
  std::mutex mu_;
  bool wireframe_ = false, show_tiles_ = false;
};

}  // namespace softgpu
