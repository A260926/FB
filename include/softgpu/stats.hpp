#pragma once
#include <cstdint>
#include <cstdio>

namespace softgpu {

// Counters written from the per-tile loop; kept separate so each tile accumulates locally
// and merges once instead of touching shared atomics per fragment.
struct RasterCounters {
  uint64_t fragments_in = 0;       // samples covered by a triangle
  uint64_t early_z_rejected = 0;   // failed the depth test before shading
  uint64_t fragments_shaded = 0;   // fragment shader invocations
  uint64_t fragments_discarded = 0;
  uint64_t fragments_blended = 0;
  void add(const RasterCounters& o) {
    fragments_in += o.fragments_in;
    early_z_rejected += o.early_z_rejected;
    fragments_shaded += o.fragments_shaded;
    fragments_discarded += o.fragments_discarded;
    fragments_blended += o.fragments_blended;
  }
};

struct Stats {
  uint64_t vertices_shaded = 0;
  uint64_t triangles_in = 0;          // submitted
  uint64_t triangles_rejected = 0;    // entirely outside one frustum plane
  uint64_t triangles_clipped = 0;     // crossed at least one plane and went through Sutherland-Hodgman
  uint64_t triangles_culled = 0;      // back/front-face culled (after clipping, per sub-triangle)
  uint64_t triangles_degenerate = 0;  // zero area after snapping, or empty bbox
  uint64_t triangles_rasterized = 0;  // reached the rasterizer
  uint64_t tile_bins = 0;             // (triangle, tile) pairs
  RasterCounters raster;
  uint64_t vertex_ns = 0, setup_ns = 0, bin_ns = 0, raster_ns = 0, total_ns = 0;

  void reset() { *this = Stats{}; }

  void print(std::FILE* f) const {
    auto ms = [](uint64_t ns) { return ns / 1e6; };
    std::fprintf(f, "vertices shaded      %llu\n", static_cast<unsigned long long>(vertices_shaded));
    std::fprintf(f, "triangles submitted  %llu\n", static_cast<unsigned long long>(triangles_in));
    std::fprintf(f, "  rejected (outside) %llu\n", static_cast<unsigned long long>(triangles_rejected));
    std::fprintf(f, "  clipped            %llu\n", static_cast<unsigned long long>(triangles_clipped));
    std::fprintf(f, "  culled             %llu\n", static_cast<unsigned long long>(triangles_culled));
    std::fprintf(f, "  degenerate         %llu\n", static_cast<unsigned long long>(triangles_degenerate));
    std::fprintf(f, "  rasterized         %llu\n", static_cast<unsigned long long>(triangles_rasterized));
    std::fprintf(f, "  tile bins          %llu\n", static_cast<unsigned long long>(tile_bins));
    std::fprintf(f, "fragments covered    %llu\n", static_cast<unsigned long long>(raster.fragments_in));
    std::fprintf(f, "  early-z rejected   %llu\n", static_cast<unsigned long long>(raster.early_z_rejected));
    std::fprintf(f, "  shaded             %llu\n", static_cast<unsigned long long>(raster.fragments_shaded));
    std::fprintf(f, "  discarded          %llu\n", static_cast<unsigned long long>(raster.fragments_discarded));
    std::fprintf(f, "  blended            %llu\n", static_cast<unsigned long long>(raster.fragments_blended));
    std::fprintf(f, "time vertex %.2fms  clip+setup %.2fms  bin %.2fms  raster %.2fms  total %.2fms\n",
                 ms(vertex_ns), ms(setup_ns), ms(bin_ns), ms(raster_ns), ms(total_ns));
  }
};

}  // namespace softgpu
