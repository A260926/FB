#pragma once
// Transient render pass: a tile-local, multi-subpass pipeline whose attachments live in
// on-tile RGBA32F memory instead of the RGBA8 framebuffer. Intermediate values keep full
// float range (negatives, values above 1) because nothing is quantized until the final
// attachment is resolved at end_pass().
//
// The plan is a value description plus a compiled side table (liveness intervals, logical-id
// to physical-slot mapping). Compilation validates the whole plan up front and throws
// std::invalid_argument without touching any renderer state; the renderer methods throw
// std::logic_error for out-of-order use.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "softgpu/math.hpp"
#include "softgpu/raster.hpp"
#include "softgpu/renderer.hpp"

namespace softgpu {

// One subpass: ordered input attachment IDs (read-only, must have been output by an earlier
// subpass) and ordered output attachment IDs (writable; cleared on the subpass that first
// outputs the attachment, preserved on later outputs). The two lists never overlap and hold
// no duplicates.
struct PassSubpass {
  std::vector<int> inputs;
  std::vector<int> outputs;
};

// Compiled side table attached to a plan. Physical slots are the on-tile RGBA32F stores;
// several logical attachments share one slot when their closed liveness intervals do not
// overlap (intervals touching at an endpoint still conflict).
struct PassPlanInfo {
  int min_slots = 0;                  // minimum physical slots the interval coloring needs
  int physical_slots = 0;             // slot budget the plan was compiled against
  std::map<int, int> slot_for;        // logical attachment ID -> physical slot
  std::map<int, int> first_output;    // logical ID -> subpass that first outputs it
  std::map<int, int> last_use;        // logical ID -> last subpass touching it
  bool compiled = false;
};

// A render-pass plan. Fill clear_values/subpasses/final_output/physical_slots, then call
// compile_render_pass(); the returned plan carries the populated PassPlanInfo.
struct RenderPassPlan {
  std::map<int, Vec4> clear_values;   // clear value of every logical attachment ID
  std::vector<PassSubpass> subpasses;
  int final_output = -1;              // attachment resolved into the color framebuffer
  int physical_slots = 0;             // available physical on-tile slots
  PassPlanInfo info;
};

// What a transient-pass fragment shader sees. Inputs are read-only values of the current
// pixel (there is deliberately no screen-space texture addressing); outputs are writable and
// ordered exactly like the subpass's outputs list. A pass fragment shader is any callable
// bool(const Fragment<N>&, PassFragmentIO&) that returns false to discard.
class PassFragmentIO {
 public:
  PassFragmentIO() = default;
  PassFragmentIO(const Vec4* const* in, int nin, Vec4* const* out, int nout)
      : in_(in), nin_(nin), out_(out), nout_(nout) {}

  int input_count() const { return nin_; }
  int output_count() const { return nout_; }
  const Vec4& in(int i) const { return *in_[i]; }
  Vec4& out(int i) { return *out_[i]; }

 private:
  const Vec4* const* in_ = nullptr;
  int nin_ = 0;
  Vec4* const* out_ = nullptr;
  int nout_ = 0;
};

namespace detail {

inline void pass_invalid(const std::string& why) {
  throw std::invalid_argument("invalid render pass plan: " + why);
}

inline bool has_dup(const std::vector<int>& v) {
  std::set<int> seen;
  for (int id : v)
    if (!seen.insert(id).second) return true;
  return false;
}

// Validates the description and builds liveness intervals plus the slot coloring.
inline PassPlanInfo compile_pass_info(const RenderPassPlan& plan) {
  PassPlanInfo info;
  if (plan.physical_slots < 1) pass_invalid("physical slot count must be positive");
  if (plan.subpasses.empty()) pass_invalid("render pass needs at least one subpass");
  const int nsp = static_cast<int>(plan.subpasses.size());

  // Every referenced ID must be declared with a clear value; declared-but-unused IDs are
  // legal and simply take no slot.
  auto declared = [&](int id) { return plan.clear_values.find(id) != plan.clear_values.end(); };
  if (plan.final_output < 0 || !declared(plan.final_output))
    pass_invalid("final output is undefined or has no clear value");

  std::set<int> defined;  // attachments output by some subpass already visited
  for (int s = 0; s < nsp; ++s) {
    const PassSubpass& sp = plan.subpasses[s];
    if (has_dup(sp.inputs)) pass_invalid("duplicate input attachment in subpass " + std::to_string(s));
    if (has_dup(sp.outputs)) pass_invalid("duplicate output attachment in subpass " + std::to_string(s));
    std::set<int> in_set(sp.inputs.begin(), sp.inputs.end());
    for (int id : sp.outputs)
      if (in_set.count(id))
        pass_invalid("feedback loop: attachment " + std::to_string(id) + " is both input and output in subpass " +
                     std::to_string(s));
    for (int id : sp.inputs) {
      if (id < 0 || !declared(id)) pass_invalid("input attachment " + std::to_string(id) + " is not declared");
      if (!defined.count(id))
        pass_invalid("subpass " + std::to_string(s) + " reads attachment " + std::to_string(id) +
                     " before any subpass outputs it");
    }
    for (int id : sp.outputs) {
      if (id < 0 || !declared(id)) pass_invalid("output attachment " + std::to_string(id) + " is not declared");
      if (!info.first_output.count(id)) info.first_output[id] = s;
      defined.insert(id);
    }
  }
  if (!defined.count(plan.final_output)) pass_invalid("final output attachment is never produced");

  // Closed liveness intervals: first output .. last input/output; the final output is live
  // until the pass ends so its value survives the whole resolve.
  for (const auto& kv : info.first_output) info.last_use[kv.first] = kv.second;
  for (int s = 0; s < nsp; ++s) {
    for (int id : plan.subpasses[s].inputs) info.last_use[id] = std::max(info.last_use[id], s);
    for (int id : plan.subpasses[s].outputs) info.last_use[id] = std::max(info.last_use[id], s);
  }
  info.last_use[plan.final_output] = nsp - 1;

  // Greedy interval coloring under closed-interval overlap: a slot can take [lo, hi] only
  // when the interval currently ending latest on it finishes strictly before lo (equal
  // endpoints conflict). Sweeping by start point and taking the lowest free slot is the
  // classic optimal coloring of an interval graph, so the number of slots used is the true
  // minimum (the maximum simultaneous liveness), never the logical attachment count.
  std::vector<int> ids;
  ids.reserve(info.first_output.size());
  for (const auto& kv : info.first_output) ids.push_back(kv.first);
  std::sort(ids.begin(), ids.end(), [&](int a, int b) {
    return info.first_output[a] != info.first_output[b] ? info.first_output[a] < info.first_output[b] : a < b;
  });
  std::vector<int> slot_end;  // end subpass of the latest interval assigned to each slot
  for (int id : ids) {
    const int lo = info.first_output[id], hi = info.last_use[id];
    int slot = 0;
    while (slot < static_cast<int>(slot_end.size()) && slot_end[slot] >= lo) ++slot;
    if (slot == static_cast<int>(slot_end.size()))
      slot_end.push_back(hi);
    else
      slot_end[slot] = hi;
    info.slot_for[id] = slot;
  }
  info.min_slots = static_cast<int>(slot_end.size());
  if (info.min_slots > plan.physical_slots)
    pass_invalid("not enough physical slots: plan needs " + std::to_string(info.min_slots) + ", has " +
                 std::to_string(plan.physical_slots));
  info.physical_slots = plan.physical_slots;
  info.compiled = true;
  return info;
}

// Everything a per-tile raster task reaches for: the resolved slot indices of one subpass,
// pointers into the worker's on-tile RGBA32F block, and the tile-local depth buffer.
struct PassTileContext {
  const RenderPassPlan* plan = nullptr;
  int subpass = 0;
  const std::vector<int>* in_slots = nullptr;
  const std::vector<int>* out_slots = nullptr;
  std::vector<Vec4*> slot_ptrs;   // physical slot -> pixel block for this tile
  int tile_stride = 0;            // pixels per row inside the tile block
  Rect rect;
  float* depth = nullptr;         // tile-local copy of the framebuffer depth
  RasterCounters* counters = nullptr;
  std::vector<const Vec4*> in_scratch;
  std::vector<Vec4*> out_scratch;
  std::vector<Vec4> saved_scratch;  // pre-discard output contents, reused per fragment
};

}  // namespace detail

// Validates `desc` and returns a compiled plan. Throws std::invalid_argument for illegal
// references, feedback loops, reads of undefined attachments, or an insufficient slot budget;
// the minimum slot count is computed from liveness, never assumed to equal the attachment
// count. Declared attachments that are never used take no slot.
inline std::shared_ptr<RenderPassPlan> compile_render_pass(const RenderPassPlan& desc) {
  auto plan = std::make_shared<RenderPassPlan>(desc);
  plan->info = detail::compile_pass_info(*plan);
  return plan;
}

// Type-erased subpass draw: one virtual per (triangle, tile), mirroring the ordinary Batch.
struct PassBatchBase {
  virtual ~PassBatchBase() = default;
  virtual void raster(const PassTriRef& ref, const Rect& clip, detail::PassTileContext& ctx) const = 0;
};

template <int N, class FS>
struct PassBatch : PassBatchBase {
  std::vector<std::vector<SetupTri<N>>> chunks;
  FS fs;
  DrawState st;
  PassBatch(const FS& f, const DrawState& s) : fs(f), st(s) {}

  void raster(const PassTriRef& ref, const Rect& clip, detail::PassTileContext& ctx) const override {
    const SetupTri<N>& t = chunks[ref.chunk][ref.tri];
    rasterize<N>(t, clip.intersect(t.bbox), [&](int px, int py, float l0, float l1, float l2) {
      ++ctx.counters->fragments_in;
      const int lx = px - ctx.rect.x0, ly = py - ctx.rect.y0;
      const size_t local = static_cast<size_t>(ly) * ctx.tile_stride + lx;
      const float z = l0 * t.z[0] + l1 * t.z[1] + l2 * t.z[2];
      if (st.depth_test && !(z < ctx.depth[local])) {
        ++ctx.counters->early_z_rejected;
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

      // Outputs start every invocation at zero; remember the previous contents so a discard
      // leaves the attachment (which may be a re-output preserving earlier data) untouched.
      ctx.saved_scratch.resize(ctx.out_slots->size());
      for (size_t i = 0; i < ctx.out_slots->size(); ++i) {
        Vec4& dst = ctx.slot_ptrs[(*ctx.out_slots)[i]][local];
        ctx.saved_scratch[i] = dst;
        dst = Vec4{};
      }
      ctx.in_scratch.resize(ctx.in_slots->size());
      ctx.out_scratch.resize(ctx.out_slots->size());
      for (size_t i = 0; i < ctx.in_slots->size(); ++i)
        ctx.in_scratch[i] = &ctx.slot_ptrs[(*ctx.in_slots)[i]][local];
      for (size_t i = 0; i < ctx.out_slots->size(); ++i)
        ctx.out_scratch[i] = &ctx.slot_ptrs[(*ctx.out_slots)[i]][local];
      PassFragmentIO io(ctx.in_scratch.data(), static_cast<int>(ctx.in_scratch.size()), ctx.out_scratch.data(),
                        static_cast<int>(ctx.out_scratch.size()));
      if (!fs(f, io)) {
        for (size_t i = 0; i < ctx.out_slots->size(); ++i)
          ctx.slot_ptrs[(*ctx.out_slots)[i]][local] = ctx.saved_scratch[i];
        ++ctx.counters->fragments_discarded;
        return;
      }
      ++ctx.counters->fragments_shaded;
      if (st.depth_write) ctx.depth[local] = z;
    });
  }
};

inline void Renderer::begin_pass(const RenderPassPlan& plan) {
  if (pass_active_) throw std::logic_error("begin_pass() while a render pass is already active");
  if (!batches_.empty()) throw std::logic_error("begin_pass() before finish() of ordinary tiled draws");
  if (!plan.info.compiled) throw std::invalid_argument("render pass plan has not been compiled");
  // All validation happens above: nothing is mutated before this point.
  const int nsp = static_cast<int>(plan.subpasses.size());
  pass_plan_ = &plan;
  pass_subpass_ = 0;
  pass_batches_.clear();
  pass_batches_.resize(nsp);
  pass_bins_.assign(nsp, std::vector<std::vector<PassTriRef>>(bins_.size()));
  pass_peak_pixels_ = 0;
  pass_active_ = true;
}

template <int N, class Vertex, class VS, class FS>
void Renderer::subpass_draw(const Vertex* verts, size_t nverts, const uint32_t* idx, size_t nidx, const VS& vs,
                            const FS& fs, const DrawState& st) {
  if (!pass_active_) throw std::logic_error("subpass_draw() outside an active render pass");
  if (st.blend) throw std::invalid_argument("blending is not supported in a transient render pass");
  const RenderPassPlan& plan = *pass_plan_;
  if (plan.subpasses[pass_subpass_].outputs.empty())
    throw std::logic_error("a subpass with no output attachments cannot accept draws");

  const uint64_t t0 = now_ns();
  auto batch = std::make_unique<PassBatch<N, FS>>(fs, st);

  // Vertex stage: identical to an ordinary draw.
  std::vector<VsOut<N>> out(nverts);
  const size_t vchunk = 4096;
  pool_.run((nverts + vchunk - 1) / vchunk, [&](size_t c) {
    const size_t end = std::min(nverts, (c + 1) * vchunk);
    for (size_t i = c * vchunk; i < end; ++i) out[i] = vs(verts[i]);
  });
  stats_.vertices_shaded += nverts;

  // Primitive assembly, six-plane clipping, perspective divide, viewport and setup: the same
  // pipeline as Renderer::draw, only the bin destination is the current subpass.
  const size_t ntris = nidx / 3, tchunk = 2048;
  const size_t nchunks = (ntris + tchunk - 1) / tchunk;
  batch->chunks.resize(nchunks);
  std::vector<ChunkStats> cstats(nchunks);
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
        if (r == Setup::Culled)
          ++cs.culled;
        else if (r != Setup::Ok)
          ++cs.degenerate;
        else
          tris.push_back(setup);
      }
    }
  });
  for (const ChunkStats& cs : cstats) {
    stats_.triangles_in += cs.in;
    stats_.triangles_rejected += cs.rejected;
    stats_.triangles_clipped += cs.clipped;
    stats_.triangles_culled += cs.culled;
    stats_.triangles_degenerate += cs.degenerate;
  }
  for (const auto& ch : batch->chunks) stats_.triangles_rasterized += ch.size();

  // Bin into the current subpass's per-tile lists; chunks are walked in order, so submission
  // order (including consecutive draws with different N and FS) is preserved inside a tile.
  auto& sub_bins = pass_bins_[pass_subpass_];
  const uint32_t draw_index = static_cast<uint32_t>(pass_batches_[pass_subpass_].size());
  for (size_t c = 0; c < nchunks; ++c) {
    for (uint32_t t = 0; t < batch->chunks[c].size(); ++t) {
      const SetupTri<N>& tri = batch->chunks[c][t];
      for_each_tile(tri.bbox, [&](size_t tile) {
        sub_bins[tile].push_back(PassTriRef{draw_index, static_cast<uint32_t>(c), t});
        ++stats_.tile_bins;
      });
    }
  }
  pass_batches_[pass_subpass_].push_back(std::move(batch));
  const uint64_t t1 = now_ns();
  stats_.setup_ns += t1 - t0;
  stats_.total_ns += t1 - t0;
}

inline void Renderer::next_subpass() {
  if (!pass_active_) throw std::logic_error("next_subpass() outside an active render pass");
  const int nsp = static_cast<int>(pass_plan_->subpasses.size());
  if (pass_subpass_ + 1 >= nsp) throw std::logic_error("next_subpass() past the last subpass");
  ++pass_subpass_;
}

inline void Renderer::end_pass() {
  if (!pass_active_) throw std::logic_error("end_pass() outside an active render pass");
  const uint64_t t0 = now_ns();
  const RenderPassPlan& plan = *pass_plan_;
  const PassPlanInfo& info = plan.info;
  const int nsp = static_cast<int>(plan.subpasses.size());
  const int nslots = info.min_slots;
  const int cap = cfg_.tile_size * cfg_.tile_size;
  const int ntiles = tiles_x_ * tiles_y_;

  // Resolve logical IDs to physical slots once for the whole pass, and collect the clear
  // (slot, value) each subpass has to perform for the attachments it is the first to output.
  std::vector<std::vector<int>> in_slots(nsp), out_slots(nsp);
  std::vector<std::vector<std::pair<int, Vec4>>> clears(nsp);
  for (int s = 0; s < nsp; ++s) {
    for (int id : plan.subpasses[s].inputs) in_slots[s].push_back(info.slot_for.at(id));
    for (int id : plan.subpasses[s].outputs) out_slots[s].push_back(info.slot_for.at(id));
  }
  for (const auto& kv : info.first_output)
    clears[kv.second].emplace_back(info.slot_for.at(kv.first), plan.clear_values.at(kv.first));
  const int final_slot = info.slot_for.at(plan.final_output);

  // One RGBA32F block per worker, claimed and released per tile. Immediate mode runs tiles
  // serially and therefore only ever holds one block; tiled mode holds up to pool_.size().
  // There is never a full-screen float image: each block is min_slots * tile_size^2 Vec4s.
  struct Block {
    std::vector<Vec4> px;
    bool busy = false;
  };
  const int workers = cfg_.mode == Mode::Immediate ? 1 : pool_.size();
  std::vector<Block> blocks(workers);
  std::mutex block_mu;
  std::mutex stats_mu;
  RasterCounters total_counters;

  auto run_tile = [&](size_t tile_id) {
    Block* block;
    {
      std::lock_guard<std::mutex> lk(block_mu);
      block = nullptr;
      for (Block& b : blocks)
        if (!b.busy) {
          block = &b;
          break;
        }
      if (block->px.empty()) block->px.resize(static_cast<size_t>(nslots) * cap);
      block->busy = true;
      size_t held = 0;  // Vec4 pixels simultaneously held across all workers right now
      for (Block& b : blocks)
        if (b.busy) held += b.px.size();
      pass_peak_pixels_ = std::max(pass_peak_pixels_, held);
    }

    const Rect r = tile_rect(static_cast<int>(tile_id));
    const int tw = r.x1 - r.x0 + 1, th = r.y1 - r.y0 + 1, npix = tw * th;

    // The shared depth buffer is loaded for the tile and carried through every subpass.
    std::vector<float> depth_local(npix);
    for (int y = 0; y < th; ++y)
      for (int x = 0; x < tw; ++x)
        depth_local[static_cast<size_t>(y) * tw + x] =
            fb_.depth[static_cast<size_t>(r.y0 + y) * fb_.width + (r.x0 + x)];

    detail::PassTileContext ctx;
    ctx.plan = &plan;
    ctx.tile_stride = tw;
    ctx.rect = r;
    ctx.depth = depth_local.data();
    ctx.slot_ptrs.resize(nslots);
    for (int s = 0; s < nslots; ++s) ctx.slot_ptrs[s] = block->px.data() + static_cast<size_t>(s) * cap;
    RasterCounters local;
    ctx.counters = &local;

    for (int s = 0; s < nsp; ++s) {
      // First-output clears cover the valid tile area even when the subpass has no geometry.
      for (const auto& cl : clears[s])
        for (int i = 0; i < npix; ++i) ctx.slot_ptrs[cl.first][i] = cl.second;
      ctx.subpass = s;
      ctx.in_slots = &in_slots[s];
      ctx.out_slots = &out_slots[s];
      for (const PassTriRef& ref : pass_bins_[s][tile_id])
        pass_batches_[s][ref.draw]->raster(ref, r, ctx);
    }

    // Resolve the final attachment through the existing quantizer and write depth back;
    // every other attachment has no full-screen storage and dies with the tile block.
    const Vec4* final_px = ctx.slot_ptrs[final_slot];
    for (int y = 0; y < th; ++y)
      for (int x = 0; x < tw; ++x) {
        const size_t local = static_cast<size_t>(y) * tw + x;
        const Vec4 v = final_px[local];
        uint8_t* dst = fb_.color.at(r.x0 + x, r.y0 + y);
        dst[0] = to_u8(v.x);
        dst[1] = to_u8(v.y);
        dst[2] = to_u8(v.z);
        dst[3] = to_u8(v.w);
        fb_.depth[static_cast<size_t>(r.y0 + y) * fb_.width + (r.x0 + x)] = depth_local[local];
      }

    {
      std::lock_guard<std::mutex> lk(stats_mu);
      total_counters.add(local);
    }
    std::lock_guard<std::mutex> lk(block_mu);
    block->busy = false;
  };

  if (cfg_.mode == Mode::Immediate) {
    for (size_t t = 0; t < static_cast<size_t>(ntiles); ++t) run_tile(t);
  } else {
    pool_.run(static_cast<size_t>(ntiles), run_tile);
  }
  stats_.raster.add(total_counters);

  pass_batches_.clear();
  pass_bins_.clear();
  pass_plan_ = nullptr;
  pass_subpass_ = -1;
  pass_active_ = false;
  const uint64_t dt = now_ns() - t0;
  stats_.raster_ns += dt;
  stats_.total_ns += dt;
}

}  // namespace softgpu
