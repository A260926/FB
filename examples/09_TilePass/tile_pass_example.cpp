//============================================================================
// EmberGL tile pass example (windowless / in-memory device)
//
// Demonstrates an ordered intermediate tile stage that runs *between* two
// geometry dispatches, operating directly on the on-tile color and depth
// buffers:
//
//   1. draw a near red triangle (writes color + depth)
//   2. tile pass: reduce the visible depth of the tile to its minimum, then
//      tint the tile color based on the result (no full-screen buffers)
//   3. draw a farther green triangle, which is depth-rejected where the first
//      triangle already wrote
//
// Build (from the repository root), together with every src/*.cpp:
//   c++ -std=c++17 -Isrc -iquote . examples/09_TilePass/tile_pass_example.cpp \
//       src/*.cpp -o tile_pass_example
//============================================================================
#include "egl_device.h"
#include <cstdio>
#include <cstring>

using namespace egl;

namespace
{
  const unsigned fb_width=70, fb_height=50;
  typedef pixel<pixfmt_r8g8b8a8> Pixel;
  typedef graphics_device_mem<pixfmt_r8g8b8a8, fb_width, fb_height> Device;

  struct Config: rasterizer_memory_cfg_base
  {
    enum {tile_width=32, tile_height=32, max_dma_transfers=0};
    enum {depth_format=depthfmt_float32, rt1_fmt=pixfmt_r8g8b8a8};
  };

  // simple single-triangle PSO with an explicit screen-space binning box
  struct Triangle: rasterizer_psc_base
  {
    enum {rt0_fmt=pixfmt_r8g8b8a8, depth_format=depthfmt_float32};
    enum {tbfc_mode=rstate_tbfc_none, bci_mode=rstate_bcimode_noperspective};
    struct ptv_fmt {vec3f pos;};
    ptv_fmt vertices[3];
    Pixel color;
    vec3f minimum, maximum;
    Triangle(float z, uint8_t r, uint8_t g, uint8_t b)
    {
      vertices[0].pos.set(-0.8f, -0.8f, z);
      vertices[1].pos.set(0.8f, -0.8f, z);
      vertices[2].pos.set(0.0f, 0.8f, z);
      color.set_rgba8(r, g, b, 255);
      minimum.set(0.1f, 0.1f, z);
      maximum.set(0.9f, 0.9f, z);
    }
    rasterizer_local_cluster_index_t num_clusters() const {return 1;}
    const void *cluster(rasterizer_local_cluster_index_t) const {return this;}
    uint8_t num_cluster_vertices(const void*) const {return 3;}
    uint8_t num_cluster_triangles(const void*) const {return 1;}
    void bin_cluster(rasterizer_tiling &t, const pso_state&, rasterizer_dispatch_index_t d, rasterizer_local_cluster_index_t c) const {t.add_cluster(minimum, maximum, d, c);}
    void tform_cluster(ptv_fmt *out, const pso_state&, const void*) const {std::memcpy(out, vertices, sizeof(vertices));}
    void setup_primitive(const void*, uint8_t, uint8_t ids[3]) const {ids[0]=0; ids[1]=1; ids[2]=2;}
    template<class PSC> void shade_pixel(rasterizer_pixel_out<PSC> &out, const pso_state&, const ptv_fmt&, const ptv_fmt&, const ptv_fmt&, const vec3f&, uint8_t) const {out.export_rt0(color);}
  };

  // the intermediate tile stage. trivially copyable, so it is value-copied into
  // the per-commit frame store at dispatch time.
  struct DepthReductionPass
  {
    // this pass only reads depth and adjusts color in place
    static constexpr bool writes_depth=false;
    float near_threshold;
    explicit DepthReductionPass(float t=0.5f):near_threshold(t) {}

    void exec(rasterizer_tile_pass_context &ctx) const
    {
      // 1) reduce the tile's visible depth to its minimum
      float min_depth=1.0f;
      if(ctx.depth.data)
      {
        const float *depth=(const float*)ctx.depth.data;
        for(uint16_t y=0; y<ctx.height; ++y)
          for(uint16_t x=0; x<ctx.width; ++x)
          {
            float z=depth[x+y*ctx.pitch];
            if(z<min_depth)
              min_depth=z;
          }
      }

      // 2) adjust the existing color attachment in place based on the result
      if(min_depth<near_threshold)
      {
        Pixel *rt=(Pixel*)ctx.rts[0];
        for(uint16_t y=0; y<ctx.height; ++y)
          for(uint16_t x=0; x<ctx.width; ++x)
            rt[x+y*ctx.pitch].c.b=200;
      }
    }
  };
}

int main()
{
  Pixel frame[fb_width*fb_height];
  std::memset(frame, 0, sizeof(frame));
  Device device(frame);
  rasterizer_memory<Device, Config> memory;
  memory.init(device);
  device.clear_depth(cleardepth_max);

  // geometry, then an intermediate tile pass, then more geometry
  device.dispatch_pso(Triangle(0.3f, 255, 0, 0));        // near, red
  device.dispatch_tile_pass(DepthReductionPass(0.5f));  // middle stage
  device.dispatch_pso(Triangle(0.8f, 0, 255, 0));        // far, green
  device.commit();

  const Pixel &center=frame[25*fb_width+35];
  std::printf("center pixel rgba=(%u,%u,%u,%u)\n", center.c.r, center.c.g, center.c.b, center.c.a);
  std::printf("expected          =(255,0,200,255) : red wins depth, pass tints blue\n");
  return (center.c.r==255 && center.c.g==0 && center.c.b==200)?0:1;
}
