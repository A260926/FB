//============================================================================
// EmberGL tile pass example (headless)
//
// Demonstrates an ordered tile pass between two geometry stages without any
// display hardware:
//
//   1. geometry stage draws a near rectangle into the tile buffer
//   2. a tile pass reduces the visible depth of each tile and, based on the
//      reduction, tints that tile's colour in place
//   3. geometry stage draws a far rectangle, which is then depth-tested
//      against the depth the pass left behind
//   4. the final tile shader converts the tile buffer to the frame buffer
//
// Build (from the repository root):
//   c++ -std=c++17 -O1 -Isrc -Itask_support examples/09_TilePass/main.cpp \
//       src/*.cpp -o tile_pass_example
//============================================================================

#include "egl_device.h"

#include <cstdio>
#include <cstring>

using namespace egl;

namespace
{
typedef pixel<pixfmt_r8g8b8a8> pixel_t;
typedef graphics_device_mem<pixfmt_r8g8b8a8, 160, 120> device_t;

// Frame memory for the intermediate tile buffers. The tile pass reuses these
// buffers, so no full-screen colour or depth target is allocated.
struct rasterizer_mem_cfg: rasterizer_memory_cfg_base
{
  enum {tile_width=32, tile_height=32};
  enum {depth_format=depthfmt_float32};
  enum {rt0_fmt=pixfmt_r8g8b8a8};
  enum {max_dma_transfers=0};
  enum {max_tile_passes=4};
};

//------------------------------------------------------------------------------
// A triangle PSO with a screen-space bounding box used for tile binning.
//------------------------------------------------------------------------------
struct colored_quad: rasterizer_psc_base
{
  enum {rt0_fmt=pixfmt_r8g8b8a8, depth_format=depthfmt_float32};
  enum {depth_func=rstate_depthfunc_less_equal};
  enum {tbfc_mode=rstate_tbfc_none, bci_mode=rstate_bcimode_noperspective};
  struct ptv_fmt {vec3f pos;};

  ptv_fmt vertices[4];
  pixel_t color;
  vec3f bbox_min, bbox_max;

  colored_quad() {}

  void set_rect(float x0_, float y0_, float x1_, float y1_, float z_, uint8_t r_, uint8_t g_, uint8_t b_)
  {
    float nx0=2.0f*x0_-1.0f, nx1=2.0f*x1_-1.0f;
    float ny0=1.0f-2.0f*y0_, ny1=1.0f-2.0f*y1_;
    vertices[0].pos.set(nx0, ny0, z_);
    vertices[1].pos.set(nx1, ny0, z_);
    vertices[2].pos.set(nx1, ny1, z_);
    vertices[3].pos.set(nx0, ny1, z_);
    bbox_min.set(x0_, y0_, z_);
    bbox_max.set(x1_, y1_, z_);
    color.set_rgba8(r_, g_, b_, 255);
  }

  rasterizer_local_cluster_index_t num_clusters() const {return 1;}
  const void *cluster(rasterizer_local_cluster_index_t) const {return this;}
  uint8_t num_cluster_vertices(const void*) const {return 4;}
  uint8_t num_cluster_triangles(const void*) const {return 2;}
  void bin_cluster(rasterizer_tiling &t_, const pso_state&, rasterizer_dispatch_index_t d_, rasterizer_local_cluster_index_t c_) const
  {
    t_.add_cluster(bbox_min, bbox_max, d_, c_);
  }
  void tform_cluster(ptv_fmt *out_, const pso_state&, const void*) const
  {
    std::memcpy(out_, vertices, sizeof(vertices));
  }
  void setup_primitive(const void*, uint8_t prim_, uint8_t idx_[3]) const
  {
    static const uint8_t indices[6]={0, 1, 2, 0, 2, 3};
    idx_[0]=indices[prim_*3+0];
    idx_[1]=indices[prim_*3+1];
    idx_[2]=indices[prim_*3+2];
  }
  template<class PSC>
  void shade_pixel(rasterizer_pixel_out<PSC> &out_, const pso_state&, const ptv_fmt&, const ptv_fmt&, const ptv_fmt&, const vec3f&, uint8_t) const
  {
    out_.export_rt0(color);
  }
};

//------------------------------------------------------------------------------
// The tile pass: reduce the visible depth of the tile and tint its colour.
//
// `writes_depth` is false, so the rasterizer keeps using Hi-Z to accelerate
// the geometry that follows. If the pass had modified depth it would set
// writes_depth to true and the rasterizer would recompute Hi-Z for the tile
// from the new depth values before testing any further geometry.
//------------------------------------------------------------------------------
struct tint_by_depth_pass
{
  enum {writes_depth=false};

  void exec(rasterizer_tile_pass_context &ctx_) const
  {
    if(!ctx_.depth.data || !ctx_.rts[0])
      return;

    // reduce the depth of the visible tile region
    const float *depth=(const float*)ctx_.depth.data;
    float min_depth=depth[0];
    for(uint16_t y=0; y<ctx_.height; ++y)
    {
      const float *row=depth+y*ctx_.pitch;
      for(uint16_t x=0; x<ctx_.width; ++x)
        min_depth=row[x]<min_depth?row[x]:min_depth;
    }

    // tint the tile in place according to the reduction: red when the tile has
    // something near, a dim blue otherwise
    pixel_t *rt=(pixel_t*)ctx_.rts[0];
    for(uint16_t y=0; y<ctx_.height; ++y)
    {
      pixel_t *row=rt+y*ctx_.pitch;
      for(uint16_t x=0; x<ctx_.width; ++x)
      {
        color_rgbaf c=row[x].get_colorf();
        if(min_depth<0.4f)
        {
          c.r=1.0f;
          c.g=min(1.0f, c.g*0.25f);
        }
        else
          c.b=min(1.0f, c.b*0.5f+0.25f);
        row[x]=c;
      }
    }
  }
};

} // namespace


int main()
{
  // allocate frame and rasterizer memory
  static pixel_t frame_buffer[160*120];
  static device_t device(frame_buffer);
  static rasterizer_memory<device_t, rasterizer_mem_cfg> rasterizer_mem;
  std::memset(frame_buffer, 0, sizeof(frame_buffer));
  rasterizer_mem.init(device);
  device.clear_depth(cleardepth_max);

  // stage 1: draw a near rectangle covering the whole canvas
  colored_quad near_rect;
  near_rect.set_rect(0.0f, 0.0f, 1.0f, 1.0f, 0.25f, 60, 60, 200);
  // stage 3: draw a far rectangle that depth tests against the pass output
  colored_quad far_rect;
  far_rect.set_rect(0.2f, 0.2f, 0.8f, 0.8f, 0.6f, 0, 220, 0);

  device.dispatch_pso(near_rect);
  device.dispatch_tile_pass(tint_by_depth_pass());
  device.dispatch_pso(far_rect);
  device.commit();

  // report a couple of pixels: the near rect is tinted red by the pass and the
  // far rect is rejected by the depth test over the near geometry
  const pixel_t &corner=frame_buffer[0];
  const pixel_t &center=frame_buffer[30*160+80];
  std::printf("corner   = r%u g%u b%u (tinted near tile)\n", corner.c.r, corner.c.g, corner.c.b);
  std::printf("center   = r%u g%u b%u (near geometry kept)\n", center.c.r, center.c.g, center.c.b);

  // Run a second frame that only uses a pass; the previous frame's colour and
  // depth must not leak through the reused tile buffers.
  device.clear_depth(cleardepth_max);
  device.commit();
  unsigned non_zero=0;
  for(unsigned i=0; i<160*120; ++i)
    non_zero+=frame_buffer[i].v!=0;
  std::printf("empty frame non-zero pixels = %u\n", non_zero);
  return non_zero?1:0;
}
