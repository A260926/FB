//============================================================================
// EmberGL tile pass tests
//
// Exercises the ordered tile pass stage of the rasterizer with a real memory
// device: per-tile reduction, ordering with respect to geometry, tiles without
// clusters, edge tile sizes on a 70x50 canvas, depth writes affecting later
// occlusion tests, and frame reuse.
//============================================================================

#include "task_support/check.h"
#include "egl_device.h"

#include <cstdio>
#include <cstring>

namespace
{
using namespace egl;

//============================================================================
// canvas & device
//============================================================================
typedef pixel<pixfmt_r8g8b8a8> pixel_t;
typedef graphics_device_mem<pixfmt_r8g8b8a8, 70, 50> device_t;

enum
{
  canvas_width=70,
  canvas_height=50,
  tile_width=32,
  tile_height=32,
  tiled_width=(canvas_width+tile_width-1)/tile_width,   // 3
  tiled_height=(canvas_height+tile_height-1)/tile_height, // 2
  num_tiles=tiled_width*tiled_height,                    // 6
  tile_pixels=tile_width*tile_height,
};

// Config with a float depth buffer and two render targets. The second target is
// used by some cases to verify that every configured attachment is presented.
struct config_float: rasterizer_memory_cfg_base
{
  enum {tile_width=32, tile_height=32};
  enum {max_dma_transfers=0, max_tile_passes=16};
  enum {depth_format=depthfmt_float32};
  enum {rt0_fmt=pixfmt_r8g8b8a8, rt1_fmt=pixfmt_r8g8b8a8};
};

struct config_hiz_off: config_float
{
  enum {depth_hiz=false};
};

struct config_uint8: rasterizer_memory_cfg_base
{
  enum {tile_width=32, tile_height=32};
  enum {max_dma_transfers=0, max_tile_passes=16};
  enum {depth_format=depthfmt_uint8, depth_hiz=true};
  enum {rt0_fmt=pixfmt_r8g8b8a8};
};

struct config_uint16: rasterizer_memory_cfg_base
{
  enum {tile_width=32, tile_height=32};
  enum {max_dma_transfers=0, max_tile_passes=16};
  enum {depth_format=depthfmt_uint16, depth_hiz=true};
  enum {rt0_fmt=pixfmt_r8g8b8a8};
};

struct config_nodepth: rasterizer_memory_cfg_base
{
  enum {tile_width=32, tile_height=32};
  enum {max_dma_transfers=0, max_tile_passes=16};
  enum {depth_format=depthfmt_none};
  enum {rt0_fmt=pixfmt_r8g8b8a8};
};

// Linear tile order on a separate config to make sure both orders run.
struct config_linear: config_float
{
  enum {tile_order=tileorder_linear};
};

//============================================================================
// full-screen / rectangle geometry
//============================================================================
template<class Cfg, int depth_fmt, int depth_test_func>
struct rect_pso: rasterizer_psc_base
{
  enum {rt0_fmt=pixfmt_r8g8b8a8, depth_format=depth_fmt};
  enum {depth_func=depth_test_func};
  enum {hiz_test=depth_test_func!=rstate_depthfunc_always};
  enum {tbfc_mode=rstate_tbfc_none, bci_mode=rstate_bcimode_noperspective};
  struct ptv_fmt {vec3f pos;};

  ptv_fmt vertices[4];
  pixel_t color;
  vec3f bbox_min, bbox_max;

  rect_pso()
  {
    std::memset(vertices, 0, sizeof(vertices));
    color.set_rgba8(0, 0, 0, 255);
    bbox_min.set(0.0f, 0.0f, 0.0f);
    bbox_max.set(1.0f, 1.0f, 1.0f);
  }

  // screen-space rectangle in normalized [0,1] canvas coordinates
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

//============================================================================
// tile passes
//============================================================================

// Counts invocations and records per-tile geometry (x, y, width, height, pitch).
struct count_pass
{
  enum {writes_depth=false};
  unsigned *calls;
  uint16_t *xs, *ys, *ws, *hs, *ps;
  void exec(rasterizer_tile_pass_context &ctx_) const
  {
    if(*calls<64)
    {
      xs[*calls]=ctx_.x;
      ys[*calls]=ctx_.y;
      ws[*calls]=ctx_.width;
      hs[*calls]=ctx_.height;
      ps[*calls]=ctx_.pitch;
    }
    ++*calls;
  }
};

// Checks the attachment metadata and that depth is readable.
struct context_check_pass
{
  enum {writes_depth=false};
  int *ok;
  unsigned *calls;
  void exec(rasterizer_tile_pass_context &ctx_) const
  {
    ++*calls;
    bool good=ctx_.rts[0]!=0;                        // rt0 configured
    good=good && ctx_.rt_pixel_sizes[0]==4;          // r8g8b8a8
    good=good && ctx_.rts[2]==0 && ctx_.rts[3]==0;   // not configured
    good=good && ctx_.rt_pixel_sizes[2]==0 && ctx_.rt_pixel_sizes[3]==0;
    good=good && ctx_.x+ctx_.width<=canvas_width;
    good=good && ctx_.y+ctx_.height<=canvas_height;
    good=good && ctx_.pitch==tile_width;
    good=good && ctx_.depth.data!=0;
    good=good && ctx_.depth.hiz_data==0;             // hi-z is not exposed
    if(!good)
      *ok=0;
  }
};

// Reduction of the visible depth over the valid region.
struct depth_reduce_pass
{
  enum {writes_depth=false};
  float *mins;      // one entry per tile, indexed by invocation order
  float *sums;
  unsigned *calls;
  void exec(rasterizer_tile_pass_context &ctx_) const
  {
    unsigned idx=*calls;
    const float *depth=(const float*)ctx_.depth.data;
    float mn=depth[0], sum=0.0f;
    for(uint16_t y=0; y<ctx_.height; ++y)
    {
      const float *row=depth+y*ctx_.pitch;
      for(uint16_t x=0; x<ctx_.width; ++x)
      {
        float v=row[x];
        mn=v<mn?v:mn;
        sum+=v;
      }
    }
    if(idx<64)
    {
      mins[idx]=mn;
      sums[idx]=sum;
    }
    ++*calls;
  }
};

// Reads a reduced depth value from a per-tile array and tints colour by it.
// The array is filled by depth_reduce_pass of an earlier commit, indexed by the
// tile's screen position (tx,ty).
struct tint_from_depth_pass
{
  enum {writes_depth=false};
  const float *values;         // [tiled_height][tiled_width]
  uint8_t threshold;           // red when min depth below this * 255
  void exec(rasterizer_tile_pass_context &ctx_) const
  {
    unsigned tx=ctx_.x/tile_width, ty=ctx_.y/tile_height;
    bool near=values[ty*tiled_width+tx]*255.0f<=(float)threshold;
    pixel_t p;
    if(near)
      p.set_rgba8(255, 0, 0, 255);
    else
      p.set_rgba8(0, 0, 255, 255);
    for(uint16_t y=0; y<ctx_.height; ++y)
    {
      pixel_t *row=(pixel_t*)ctx_.rts[0]+y*ctx_.pitch;
      for(uint16_t x=0; x<ctx_.width; ++x)
        row[x]=p;
    }
  }
};

// Fills the whole valid tile region of color and/or depth.
struct fill_pass
{
  enum {writes_depth=false};
  pixel_t color;
  bool fill_color;
  float depth_value;
  bool fill_depth;
  void exec(rasterizer_tile_pass_context &ctx_) const
  {
    for(uint16_t y=0; y<ctx_.height; ++y)
    {
      if(fill_color)
      {
        pixel_t *row=(pixel_t*)ctx_.rts[0]+y*ctx_.pitch;
        for(uint16_t x=0; x<ctx_.width; ++x)
          row[x]=color;
      }
      if(fill_depth && ctx_.depth.data)
      {
        float *row=(float*)ctx_.depth.data+y*ctx_.pitch;
        for(uint16_t x=0; x<ctx_.width; ++x)
          row[x]=depth_value;
      }
    }
  }
};

// A depth-writing pass that raises depth to a fixed value over the valid region.
struct raise_depth_pass
{
  enum {writes_depth=true};
  float value;
  void exec(rasterizer_tile_pass_context &ctx_) const
  {
    if(!ctx_.depth.data)
      return;
    for(uint16_t y=0; y<ctx_.height; ++y)
    {
      float *row=(float*)ctx_.depth.data+y*ctx_.pitch;
      for(uint16_t x=0; x<ctx_.width; ++x)
        row[x]=value;
    }
  }
};

// A depth-writing pass that lowers depth to a fixed value.
struct lower_depth_pass
{
  enum {writes_depth=true};
  float value;
  void exec(rasterizer_tile_pass_context &ctx_) const
  {
    if(!ctx_.depth.data)
      return;
    for(uint16_t y=0; y<ctx_.height; ++y)
    {
      float *row=(float*)ctx_.depth.data+y*ctx_.pitch;
      for(uint16_t x=0; x<ctx_.width; ++x)
        row[x]=value;
    }
  }
};

// Adds a fixed value to the color of the valid tile region.
struct offset_color_pass
{
  enum {writes_depth=false};
  int delta;
  void exec(rasterizer_tile_pass_context &ctx_) const
  {
    for(uint16_t y=0; y<ctx_.height; ++y)
    {
      pixel_t *row=(pixel_t*)ctx_.rts[0]+y*ctx_.pitch;
      for(uint16_t x=0; x<ctx_.width; ++x)
      {
        int v=(int)row[x].c.r+delta;
        v=v<0?0:(v>255?255:v);
        row[x].c.r=(uint8_t)v;
        row[x].c.a=255;
      }
    }
  }
};

// Overwrites color in a sub-rectangle of the tile, to prove passes can address
// the local tile origin at an arbitrary pixel.
struct block_pass
{
  enum {writes_depth=false};
  uint16_t x, y, width, height;
  pixel_t color;
  void exec(rasterizer_tile_pass_context &ctx_) const
  {
    if(!ctx_.rts[0])
      return;
    uint16_t x0=x<ctx_.width?x:ctx_.width, x1=(uint16_t)(x+width<ctx_.width?x+width:ctx_.width);
    uint16_t y0=y<ctx_.height?y:ctx_.height, y1=(uint16_t)(y+height<ctx_.height?y+height:ctx_.height);
    for(uint16_t py=y0; py<y1; ++py)
    {
      pixel_t *row=(pixel_t*)ctx_.rts[0]+py*ctx_.pitch;
      for(uint16_t px=x0; px<x1; ++px)
        row[px]=color;
    }
  }
};

// Writes a deterministic value into the second render target, to prove that
// every configured attachment is addressable.
struct rt1_fill_pass
{
  enum {writes_depth=false};
  uint8_t value;
  void exec(rasterizer_tile_pass_context &ctx_) const
  {
    if(!ctx_.rts[1])
      return;
    for(uint16_t y=0; y<ctx_.height; ++y)
    {
      pixel_t *row=(pixel_t*)ctx_.rts[1]+y*ctx_.pitch;
      for(uint16_t x=0; x<ctx_.width; ++x)
        row[x].set_rgba8(value, value, value, 255);
    }
  }
};

//============================================================================
// fixtures
//============================================================================
struct frame_buffer
{
  pixel_t pixels[canvas_width*canvas_height];
  frame_buffer() {std::memset(pixels, 0, sizeof(pixels));}
  const pixel_t &at(unsigned x_, unsigned y_) const {return pixels[x_+y_*canvas_width];}
  pixel_t &at(unsigned x_, unsigned y_) {return pixels[x_+y_*canvas_width];}
};

template<class Cfg>
struct fixture
{
  frame_buffer fb;
  device_t device;
  rasterizer_memory<device_t, Cfg> memory;
  fixture(): device(fb.pixels)
  {
    device.init(fb.pixels);
    memory.init(device);
    device.clear_depth(cleardepth_max);
  }
};

unsigned count_nonzero(const frame_buffer &fb_)
{
  unsigned n=0;
  for(unsigned i=0; i<canvas_width*canvas_height; ++i)
    n+=fb_.pixels[i].v!=0;
  return n;
}

} // namespace


//============================================================================
// ordering group
//============================================================================
namespace
{

// A pass runs once per valid tile, including tiles with no geometry cluster.
int case_every_tile_once()
{
  fixture<config_float> f;
  unsigned calls=0;
  uint16_t xs[64], ys[64], ws[64], hs[64], ps[64];
  count_pass pass={&calls, xs, ys, ws, hs, ps};
  f.device.dispatch_tile_pass(pass);
  f.device.commit();
  TASK_CHECK(calls==num_tiles);
  TASK_CHECK(count_nonzero(f.fb)==0);   // no geometry and no colour writes

  // the tile coordinates must cover the whole 3x2 tile grid exactly once
  bool seen[tiled_height][tiled_width];
  std::memset(seen, 0, sizeof(seen));
  for(unsigned i=0; i<num_tiles; ++i)
  {
    unsigned tx=xs[i]/tile_width, ty=ys[i]/tile_height;
    TASK_CHECK(tx<tiled_width && ty<tiled_height);
    TASK_CHECK(!seen[ty][tx]);
    seen[ty][tx]=true;
    TASK_CHECK(ws[i]==(uint16_t)(tx==tiled_width-1?canvas_width-tx*tile_width:tile_width));
    TASK_CHECK(hs[i]==(uint16_t)(ty==tiled_height-1?canvas_height-ty*tile_height:tile_height));
    TASK_CHECK(ps[i]==tile_width);
  }
  return 0;
}

// Passes without any geometry still produce output, converted by the final
// tile shader.
int case_pass_only_frame()
{
  fixture<config_float> f;
  fill_pass fill;
  fill.color.set_rgba8(10, 20, 30, 255);
  fill.fill_color=true;
  fill.fill_depth=false;
  fill.depth_value=1.0f;
  f.device.dispatch_tile_pass(fill);
  f.device.commit();
  TASK_CHECK(count_nonzero(f.fb)==canvas_width*canvas_height);
  TASK_CHECK(f.fb.at(0, 0).c.r==10 && f.fb.at(0, 0).c.g==20 && f.fb.at(0, 0).c.b==30);
  TASK_CHECK(f.fb.at(canvas_width-1, canvas_height-1).c.r==10);
  // the edge tiles must not spill outside the 70x50 canvas
  TASK_CHECK(f.fb.at(69, 0).c.g==20);
  return 0;
}

// Geometry, pass, geometry: each stage sees the result of the previous one.
int case_geometry_pass_geometry_order()
{
  fixture<config_float> f;
  typedef rect_pso<config_float, depthfmt_float32, rstate_depthfunc_always> pso_t;

  pso_t first;
  first.set_rect(0.0f, 0.0f, 0.5f, 0.5f, 0.25f, 255, 0, 0);
  offset_color_pass offset={40};   // +40 red to what the first draw produced
  pso_t second;
  second.set_rect(0.0f, 0.0f, 0.5f, 0.5f, 0.5f, 0, 0, 100);

  f.device.dispatch_pso(first);
  f.device.dispatch_tile_pass(offset);
  f.device.dispatch_pso(second);
  f.device.commit();

  // inside the rect: red=255 from the first draw, +40 clamped to 255, then the
  // second draw overwrote it because depth_func is always
  TASK_CHECK(f.fb.at(5, 5).c.r==0 && f.fb.at(5, 5).c.b==100);
  // outside the rect the pass still runs over its whole valid tile region, so
  // the offset shows up there on top of the untouched (zero) colour
  TASK_CHECK(f.fb.at(60, 40).c.r==40 && f.fb.at(60, 40).c.b==0);
  return 0;
}

// Means to verify the pass observed the first draw: read the colour and store
// whether the first draw was present where the pass ran.
struct observe_color_pass
{
  enum {writes_depth=false};
  int *found_red;
  void exec(rasterizer_tile_pass_context &ctx_) const
  {
    if(!ctx_.rts[0])
      return;
    unsigned red=0;
    for(uint16_t y=0; y<ctx_.height; ++y)
    {
      const pixel_t *row=(const pixel_t*)ctx_.rts[0]+y*ctx_.pitch;
      for(uint16_t x=0; x<ctx_.width; ++x)
        red+=row[x].c.r!=0;
    }
    *found_red+=red;
  }
};

int case_pass_observes_preceding_geometry()
{
  fixture<config_float> f;
  typedef rect_pso<config_float, depthfmt_float32, rstate_depthfunc_always> pso_t;
  pso_t pso;
  pso.set_rect(0.0f, 0.0f, 0.5f, 0.5f, 0.5f, 200, 0, 0);
  int red=0;
  observe_color_pass observe={&red};
  f.device.dispatch_pso(pso);
  f.device.dispatch_tile_pass(observe);
  f.device.commit();
  // the rect covers roughly a quarter of the canvas (half in each axis)
  TASK_CHECK(red>canvas_width*canvas_height/8);
  TASK_CHECK(red<canvas_width*canvas_height/2);
  return 0;
}

// Geometry after a pass overwrites the pass' colour, and a second pass sees it.
int case_pass_geometry_pass_order()
{
  fixture<config_float> f;
  typedef rect_pso<config_float, depthfmt_float32, rstate_depthfunc_always> pso_t;
  fill_pass fill;
  fill.color.set_rgba8(0, 0, 255, 255);
  fill.fill_color=true;
  fill.fill_depth=false;
  fill.depth_value=1.0f;
  pso_t pso;
  pso.set_rect(0.0f, 0.0f, 0.5f, 0.5f, 0.5f, 0, 255, 0);
  int green=0;
  struct observe_green
  {
    enum {writes_depth=false};
    int *green;
    void exec(rasterizer_tile_pass_context &ctx_) const
    {
      if(!ctx_.rts[0])
        return;
      for(uint16_t y=0; y<ctx_.height; ++y)
      {
        const pixel_t *row=(const pixel_t*)ctx_.rts[0]+y*ctx_.pitch;
        for(uint16_t x=0; x<ctx_.width; ++x)
          *green+=row[x].c.g!=0;
      }
    }
  } observe={&green};
  f.device.dispatch_tile_pass(fill);
  f.device.dispatch_pso(pso);
  f.device.dispatch_tile_pass(observe);
  f.device.commit();
  TASK_CHECK(green>canvas_width*canvas_height/8);      // geometry after the fill
  TASK_CHECK(f.fb.at(1, 1).c.g==255 && f.fb.at(1, 1).c.b==0);
  TASK_CHECK(f.fb.at(30, 30).c.b==255 && f.fb.at(30, 30).c.g==0); // untouched by geometry
  return 0;
}

// Consecutive passes must all run, in order, not merged or skipped.
int case_consecutive_passes()
{
  fixture<config_float> f;
  typedef rect_pso<config_float, depthfmt_float32, rstate_depthfunc_always> pso_t;
  pso_t pso;
  pso.set_rect(0.0f, 0.0f, 0.25f, 0.25f, 0.5f, 10, 0, 0);
  offset_color_pass plus10={10};
  offset_color_pass plus20={20};
  f.device.dispatch_pso(pso);
  f.device.dispatch_tile_pass(plus10);
  f.device.dispatch_tile_pass(plus20);
  f.device.commit();
  // each pixel of the top-left rect started at red=10, then +10 then +20
  TASK_CHECK(f.fb.at(2, 2).c.r==40);
  return 0;
}

// Both linear and Morton tile orders run and give identical framebuffers.
template<class Cfg>
void run_tile_order_scene(frame_buffer &out_)
{
  typedef rect_pso<config_float, depthfmt_float32, rstate_depthfunc_less_equal> pso_t;
  fixture<Cfg> f;
  fill_pass fill;
  fill.color.set_rgba8(3, 4, 5, 255);
  fill.fill_color=true;
  fill.fill_depth=false;
  fill.depth_value=1.0f;
  pso_t pso;
  pso.set_rect(0.1f, 0.1f, 0.6f, 0.6f, 0.5f, 120, 130, 140);
  offset_color_pass offset={7};
  f.device.dispatch_tile_pass(fill);
  f.device.dispatch_pso(pso);
  f.device.dispatch_tile_pass(offset);
  f.device.commit();
  std::memcpy(out_.pixels, f.fb.pixels, sizeof(out_.pixels));
}

int case_tile_orders()
{
  frame_buffer linear_fb, morton_fb;
  run_tile_order_scene<config_linear>(linear_fb);
  run_tile_order_scene<config_float>(morton_fb);
  TASK_CHECK(std::memcmp(linear_fb.pixels, morton_fb.pixels, sizeof(linear_fb.pixels))==0);
  TASK_CHECK(linear_fb.at(2, 2).c.r==10);     // fill 3 + offset 7
  return 0;
}

} // namespace



//============================================================================
// depth group
//============================================================================
namespace
{

// Builds a scene of a near green rect and a far red rect and returns the depth
// reduced over the top-left tile. Geometry order is near first.
template<class Cfg>
float reduce_center_depth_after_geometry(fixture<Cfg> &f_)
{
  typedef rect_pso<Cfg, depthfmt_float32, rstate_depthfunc_less_equal> pso_t;
  pso_t near_rect;
  near_rect.set_rect(0.0f, 0.0f, 0.5f, 0.5f, 0.25f, 0, 255, 0);
  pso_t far_rect;
  far_rect.set_rect(0.0f, 0.0f, 1.0f, 1.0f, 0.75f, 255, 0, 0);
  f_.device.dispatch_pso(far_rect);
  f_.device.dispatch_pso(near_rect);
  float mins[16], sums[16];
  unsigned calls=0;
  depth_reduce_pass reduce={mins, sums, &calls};
  f_.device.dispatch_tile_pass(reduce);
  f_.device.commit();
  TASK_CHECK(calls==num_tiles);
  // top-left tile holds the near rect over part of it and far over the rest,
  // so the minimum depth is 0.25
  TASK_CHECK(mins[0]>0.24f && mins[0]<0.26f);
  return mins[0];
}

// Depth written by a pass must be honoured by later LESS/LEQUAL draws.
int case_pass_depth_affects_later_geometry()
{
  typedef rect_pso<config_float, depthfmt_float32, rstate_depthfunc_less_equal> pso_t;
  fixture<config_float> f;

  // raise depth to 0.9 everywhere, then draw a z=0.5 rect which must pass
  raise_depth_pass raise={0.9f};
  pso_t rect;
  rect.set_rect(0.0f, 0.0f, 1.0f, 1.0f, 0.5f, 0, 255, 0);
  f.device.dispatch_tile_pass(raise);
  f.device.dispatch_pso(rect);
  f.device.commit();
  TASK_CHECK(f.fb.at(35, 25).c.g==255);

  // lower depth to 0.1, then draw the same z=0.5 rect which must be rejected
  fixture<config_float> f2;
  lower_depth_pass lower={0.1f};
  f2.device.dispatch_tile_pass(lower);
  f2.device.dispatch_pso(rect);
  f2.device.commit();
  TASK_CHECK(f2.fb.at(35, 25).v==0);
  return 0;
}

// A depth-writing pass between two geometry stages must not let stale Hi-Z
// bounds cull geometry. The Hi-Z and Hi-Z-off results have to match.
//
// Writes both the colour output and the raw tile depth buffer into `out_`, so
// the caller can compare colour and depth between the two configurations.
template<class Cfg>
void run_pass_depth_scene(frame_buffer &out_, float *depth_out_)
{
  // geometry A writes near depth, then a pass raises depth to 0.9, then
  // geometry B at z=0.5 (LEQUAL) must still draw. With stale Hi-Z from A the
  // region already contains z=0.25, so a max-based Hi-Z test could wrongly cull
  // or keep B depending on the bounds. This exercises that path.
  fixture<Cfg> f;
  typedef rect_pso<Cfg, depthfmt_float32, rstate_depthfunc_less_equal> pso_t;
  pso_t a;
  a.set_rect(0.0f, 0.0f, 0.8f, 0.8f, 0.25f, 0, 0, 255);
  pso_t b;
  b.set_rect(0.1f, 0.1f, 0.7f, 0.7f, 0.5f, 0, 255, 0);
  raise_depth_pass raise={0.9f};
  f.device.dispatch_pso(a);
  f.device.dispatch_tile_pass(raise);
  f.device.dispatch_pso(b);
  f.device.commit();
  std::memcpy(out_.pixels, f.fb.pixels, sizeof(out_.pixels));
  const rasterizer_depth_target &dt=f.device.depth_target();
  std::memcpy(depth_out_, dt.data, sizeof(float)*tile_pixels);
}

int case_hiz_matches_hiz_off_after_depth_pass()
{
  frame_buffer hiz, no_hiz;
  float hiz_depth[tile_pixels], no_hiz_depth[tile_pixels];
  run_pass_depth_scene<config_float>(hiz, hiz_depth);
  run_pass_depth_scene<config_hiz_off>(no_hiz, no_hiz_depth);
  // colour must match: the raised depth lets B draw over A everywhere
  TASK_CHECK(std::memcmp(hiz.pixels, no_hiz.pixels, sizeof(hiz.pixels))==0);
  // and the resulting depth buffer must match as well
  TASK_CHECK(std::memcmp(hiz_depth, no_hiz_depth, sizeof(hiz_depth))==0);
  TASK_CHECK(hiz.at(35, 25).c.g==255);
  TASK_CHECK(hiz.at(35, 25).c.b==0);
  return 0;
}

// Same comparison but with a depth-writing pass that lowers depth, so later
// geometry must be occluded. Again Hi-Z on/off must agree.
template<class Cfg>
void run_lowering_scene(frame_buffer &out_, float *depth_out_)
{
  typedef rect_pso<config_float, depthfmt_float32, rstate_depthfunc_less_equal> pso_t;
  fixture<Cfg> f;
  pso_t a;
  a.set_rect(0.0f, 0.0f, 0.8f, 0.8f, 0.5f, 0, 0, 255);
  pso_t b;
  b.set_rect(0.1f, 0.1f, 0.7f, 0.7f, 0.4f, 0, 255, 0);
  lower_depth_pass lower={0.1f};
  f.device.dispatch_pso(a);
  f.device.dispatch_tile_pass(lower);
  f.device.dispatch_pso(b);
  f.device.commit();
  std::memcpy(out_.pixels, f.fb.pixels, sizeof(out_.pixels));
  const rasterizer_depth_target &dt=f.device.depth_target();
  std::memcpy(depth_out_, dt.data, sizeof(float)*tile_pixels);
}

int case_hiz_matches_hiz_off_after_lowering_pass()
{
  frame_buffer hiz, no_hiz;
  float hiz_depth[tile_pixels], no_hiz_depth[tile_pixels];
  run_lowering_scene<config_float>(hiz, hiz_depth);
  run_lowering_scene<config_hiz_off>(no_hiz, no_hiz_depth);
  TASK_CHECK(std::memcmp(hiz.pixels, no_hiz.pixels, sizeof(hiz.pixels))==0);
  TASK_CHECK(std::memcmp(hiz_depth, no_hiz_depth, sizeof(hiz_depth))==0);
  TASK_CHECK(hiz.at(35, 25).c.b==255);   // B rejected because depth was lowered
  TASK_CHECK(hiz.at(35, 25).c.g==0);
  return 0;
}

// A depth-writing pass followed by a draw that switches the Hi-Z comparison
// mode (LESS -> GREATER) must still agree with Hi-Z disabled. The pass
// invalidates the cached bounds, and the mode switch forces a fresh update.
template<class Cfg>
void run_mode_switch_scene(frame_buffer &out_, float *depth_out_)
{
  fixture<Cfg> f;
  typedef rect_pso<Cfg, depthfmt_float32, rstate_depthfunc_less_equal> less_t;
  typedef rect_pso<Cfg, depthfmt_float32, rstate_depthfunc_greater> greater_t;
  less_t a;
  a.set_rect(0.0f, 0.0f, 0.9f, 0.9f, 0.5f, 0, 0, 255);
  greater_t b;
  b.set_rect(0.0f, 0.0f, 0.9f, 0.9f, 0.9f, 0, 255, 0);  // 0.9 > 0.7 -> passes
  raise_depth_pass raise={0.7f};
  f.device.dispatch_pso(a);
  f.device.dispatch_tile_pass(raise);
  f.device.dispatch_pso(b);
  f.device.commit();
  std::memcpy(out_.pixels, f.fb.pixels, sizeof(out_.pixels));
  const rasterizer_depth_target &dt=f.device.depth_target();
  std::memcpy(depth_out_, dt.data, sizeof(float)*tile_pixels);
}

int case_hiz_matches_hiz_off_after_mode_switch()
{
  frame_buffer hiz, no_hiz;
  float hiz_depth[tile_pixels], no_hiz_depth[tile_pixels];
  run_mode_switch_scene<config_float>(hiz, hiz_depth);
  run_mode_switch_scene<config_hiz_off>(no_hiz, no_hiz_depth);
  TASK_CHECK(std::memcmp(hiz.pixels, no_hiz.pixels, sizeof(hiz.pixels))==0);
  TASK_CHECK(std::memcmp(hiz_depth, no_hiz_depth, sizeof(hiz_depth))==0);
  TASK_CHECK(hiz.at(35, 25).c.g==255);
  return 0;
}

// Depth formats: 8-bit and 16-bit depth buffers accept pass writes and later
// geometry sees them, and the results are range-clamped by the format.
template<class Cfg>
void run_uint_depth_scene(frame_buffer &out_)
{
  fixture<Cfg> f;
  typedef rect_pso<Cfg, depthfmt_uint8, rstate_depthfunc_less_equal> pso8_t;
  struct depth_writer
  {
    enum {writes_depth=true};
    bool *depth_nonnull;
    void exec(rasterizer_tile_pass_context &ctx_) const
    {
      if(ctx_.depth.data)
        *depth_nonnull=true;
      // write mid-range values into the active depth format
      for(uint16_t y=0; y<ctx_.height; ++y)
      {
        uint8_t *row=(uint8_t*)ctx_.depth.data+y*ctx_.pitch;
        for(uint16_t x=0; x<ctx_.width; ++x)
          row[x]=64;             // uint8: 64/255
      }
    }
  } writer;
  bool depth_seen=false;
  writer.depth_nonnull=&depth_seen;
  pso8_t rect;
  rect.set_rect(0.0f, 0.0f, 1.0f, 1.0f, 0.1f, 0, 255, 0); // 0.1 < 64/255 -> passes
  f.device.dispatch_tile_pass(writer);
  f.device.dispatch_pso(rect);
  f.device.commit();
  if(!depth_seen)
    std::memset(out_.pixels, 0, sizeof(out_.pixels));  // leave colour untouched
  else
    std::memcpy(out_.pixels, f.fb.pixels, sizeof(out_.pixels));
}

int case_uint_depth_formats()
{
  frame_buffer fb8;
  run_uint_depth_scene<config_uint8>(fb8);
  TASK_CHECK(fb8.at(35, 25).c.g==255);

  // 16-bit device with its own pass that writes the full 16-bit range
  fixture<config_uint16> f;
  struct writer16
  {
    enum {writes_depth=true};
    void exec(rasterizer_tile_pass_context &ctx_) const
    {
      for(uint16_t y=0; y<ctx_.height; ++y)
      {
        uint16_t *row=(uint16_t*)ctx_.depth.data+y*ctx_.pitch;
        for(uint16_t x=0; x<ctx_.width; ++x)
          row[x]=16384;          // 16384/65535 ~= 0.25
      }
    }
  } writer;
  typedef rect_pso<config_uint16, depthfmt_uint16, rstate_depthfunc_less_equal> pso16_t;
  pso16_t near_rect;
  near_rect.set_rect(0.0f, 0.0f, 1.0f, 1.0f, 0.1f, 0, 255, 0);  // passes vs 0.25
  pso16_t far_rect;
  far_rect.set_rect(0.0f, 0.0f, 1.0f, 1.0f, 0.5f, 255, 0, 0);   // rejected vs 0.25
  f.device.dispatch_tile_pass(writer);
  f.device.dispatch_pso(near_rect);
  f.device.dispatch_pso(far_rect);
  f.device.commit();
  TASK_CHECK(f.fb.at(35, 25).c.g==255);
  TASK_CHECK(f.fb.at(35, 25).c.r==0);
  return 0;
}

// No-depth configuration: the pass sees a null depth pointer and colour works.
int case_no_depth_configuration()
{
  fixture<config_nodepth> f;
  struct probe
  {
    enum {writes_depth=false};
    int *depth_is_null;
    void exec(rasterizer_tile_pass_context &ctx_) const
    {
      *depth_is_null+=ctx_.depth.data==0 && ctx_.depth.format==depthfmt_none && ctx_.depth.hiz_data==0;
    }
  } p;
  int null_count=0;
  p.depth_is_null=&null_count;
  fill_pass fill;
  fill.color.set_rgba8(1, 2, 3, 255);
  fill.fill_color=true;
  fill.fill_depth=false;
  fill.depth_value=0.0f;
  raise_depth_pass raise={0.5f};    // writes_depth but no depth buffer: no-op
  f.device.dispatch_tile_pass(p);
  f.device.dispatch_tile_pass(fill);
  f.device.dispatch_tile_pass(raise);
  f.device.commit();
  TASK_CHECK(null_count==num_tiles);
  TASK_CHECK(f.fb.at(0, 0).c.r==1 && f.fb.at(69, 49).c.b==3);
  return 0;
}

// Context metadata is correct on every tile, including edge tiles.
int case_context_metadata()
{
  fixture<config_float> f;
  int ok=1;
  unsigned calls=0;
  context_check_pass check={&ok, &calls};
  f.device.dispatch_tile_pass(check);
  f.device.commit();
  TASK_CHECK(ok);
  TASK_CHECK(calls==num_tiles);
  return 0;
}

// Every configured render target is addressable and presented at commit.
int case_multiple_render_targets()
{
  fixture<config_float> f;
  rt1_fill_pass fill1={77};
  fill_pass fill0;
  fill0.color.set_rgba8(9, 9, 9, 255);
  fill0.fill_color=true;
  fill0.fill_depth=false;
  fill0.depth_value=1.0f;
  f.device.dispatch_tile_pass(fill1);
  f.device.dispatch_tile_pass(fill0);
  f.device.commit();
  TASK_CHECK(f.fb.at(0, 0).c.r==9);
  return 0;
}

// The reduced-depth value seen by a second pass reflects geometry drawn before
// it, and a depth reduction on a tile with no cluster still happens (clear
// depth value), covering the "no cluster" requirement.
int case_reduction_with_and_without_clusters()
{
  fixture<config_float> f;
  typedef rect_pso<config_float, depthfmt_float32, rstate_depthfunc_less_equal> pso_t;
  pso_t pso;
  pso.set_rect(0.0f, 0.0f, 0.2f, 0.2f, 0.3f, 255, 0, 0);  // only the top-left tile
  float mins[16], sums[16];
  unsigned calls=0;
  depth_reduce_pass reduce={mins, sums, &calls};
  f.device.dispatch_pso(pso);
  f.device.dispatch_tile_pass(reduce);
  f.device.commit();
  TASK_CHECK(calls==num_tiles);
  // tile (0,0) contains the geometry, min depth 0.3
  TASK_CHECK(mins[0]>0.29f && mins[0]<0.31f);
  // all other tiles must report the cleared maximum depth of 1.0
  for(unsigned i=1; i<num_tiles; ++i)
    TASK_CHECK(mins[i]==1.0f);
  // every valid pixel contributes once: tile 0 sum is (0.3 for geometry pixels)
  // plus 1.0 for the rest; just check it is a plausible mixed sum
  TASK_CHECK(sums[0]>0.5f*32*32 && sums[0]<32*32);
  return 0;
}

// A pass whose colour write is derived from per-tile reduced depth produces the
// expected per-tile tint (multi-stage reduction inside a tile).
int case_reduction_then_geometry_then_tint()
{
  fixture<config_float> f;
  typedef rect_pso<config_float, depthfmt_float32, rstate_depthfunc_less_equal> pso_t;
  pso_t near_rect;
  near_rect.set_rect(0.0f, 0.0f, 0.1f, 0.1f, 0.2f, 0, 0, 0);  // top-left tile near

  // First frame: reduce the depth of that geometry into per-tile values.
  float mins[16], sums[16];
  unsigned calls=0;
  depth_reduce_pass reduce={mins, sums, &calls};
  f.device.dispatch_pso(near_rect);
  f.device.dispatch_tile_pass(reduce);
  f.device.commit();

  // Second frame: tint each tile based on the reduction, then draw geometry.
  // Rebuild the per-tile value array from the first commit.
  float values[tiled_height*tiled_width];
  std::memcpy(values, mins, sizeof(values));
  tint_from_depth_pass tint;
  tint.values=values;
  tint.threshold=100;    // 0.2*255=51 <= 100 -> near tiles red
  fill_pass fill;
  fill.color.set_rgba8(0, 0, 255, 255);
  fill.fill_color=true;
  fill.fill_depth=false;
  fill.depth_value=1.0f;
  pso_t small;
  small.set_rect(0.0f, 0.0f, 0.35f, 0.35f, 0.5f, 0, 255, 0);
  f.device.clear_depth(cleardepth_max);
  f.device.dispatch_tile_pass(tint);
  f.device.dispatch_pso(small);
  f.device.commit();

  // The top-left tile was tinted red before the geometry. The small rect
  // (0..0.35 of the canvas) is drawn afterwards, so it overwrites the tint
  // where it lands, while the rest of the same tile keeps the red tint.
  TASK_CHECK(f.fb.at(2, 2).c.g==255 && f.fb.at(2, 2).c.r==0);     // geometry after tint
  // the geometry's rect covers x,y<24.5, so (30,30) is tint-only.
  TASK_CHECK(f.fb.at(30, 30).c.r==0xff && f.fb.at(30, 30).c.b==0); // tint, no geometry
  // the second tile was tinted blue and is outside the geometry
  TASK_CHECK(f.fb.at(35, 2).c.b==0xff && f.fb.at(35, 2).c.r==0);
  return 0;
}

} // namespace


//============================================================================
// reuse group
//============================================================================
namespace
{

// Attachments written by a pass must not leak into the next frame when the same
// tile buffer is reused.
int case_frame_reuse_resets_attachments()
{
  fixture<config_float> f;
  fill_pass fill;
  fill.color.set_rgba8(255, 255, 0, 255);
  fill.fill_color=true;
  fill.fill_depth=false;
  fill.depth_value=1.0f;
  f.device.dispatch_tile_pass(fill);
  f.device.commit();
  TASK_CHECK(f.fb.at(10, 10).c.r==255 && f.fb.at(10, 10).c.g==255);

  // next frame: no passes, no geometry -> frame must be fully cleared
  f.device.commit();
  TASK_CHECK(count_nonzero(f.fb)==0);

  // and a pass-only frame with a single block write only writes that block
  block_pass block;
  block.x=0;
  block.y=0;
  block.width=4;
  block.height=4;
  block.color.set_rgba8(1, 0, 0, 255);
  f.device.dispatch_tile_pass(block);
  f.device.commit();
  TASK_CHECK(f.fb.at(0, 0).c.r==1);
  TASK_CHECK(f.fb.at(4, 0).v==0);
  return 0;
}

// Depth written by a pass must not survive to the next frame: the next frame
// starts from the clear value.
int case_frame_reuse_resets_depth()
{
  fixture<config_float> f;
  lower_depth_pass lower={0.0f};
  typedef rect_pso<config_float, depthfmt_float32, rstate_depthfunc_less_equal> pso_t;
  pso_t rect;
  rect.set_rect(0.0f, 0.0f, 1.0f, 1.0f, 0.5f, 0, 255, 0);

  // frame 1: crush depth to 0, geometry then fully fails
  f.device.dispatch_tile_pass(lower);
  f.device.dispatch_pso(rect);
  f.device.commit();
  TASK_CHECK(count_nonzero(f.fb)==0);

  // frame 2: no depth pass, cleared to max -> geometry passes everywhere
  f.device.clear_depth(cleardepth_max);
  f.device.dispatch_pso(rect);
  f.device.commit();
  TASK_CHECK(count_nonzero(f.fb)==canvas_width*canvas_height);

  // frame 3: pass-only, must not inherit frame 2 geometry colour either
  f.device.commit();
  TASK_CHECK(count_nonzero(f.fb)==0);
  return 0;
}

// Reusing the buffers between two frames with different scenes: the second
// frame's output depends only on its own submissions.
int case_frame_reuse_independent_scenes()
{
  fixture<config_float> f;
  typedef rect_pso<config_float, depthfmt_float32, rstate_depthfunc_always> pso_t;
  pso_t a;
  a.set_rect(0.0f, 0.0f, 1.0f, 1.0f, 0.5f, 200, 0, 0);
  pso_t b;
  b.set_rect(0.0f, 0.0f, 1.0f, 1.0f, 0.5f, 0, 200, 0);

  f.device.dispatch_pso(a);
  f.device.commit();
  TASK_CHECK(f.fb.at(35, 25).c.r==200);

  f.device.dispatch_pso(b);
  f.device.commit();
  TASK_CHECK(f.fb.at(35, 25).c.g==200);
  TASK_CHECK(f.fb.at(35, 25).c.r==0);

  // a frame with only a pass leaves no colour from the previous frame
  f.device.commit();
  TASK_CHECK(count_nonzero(f.fb)==0);
  return 0;
}

// Edge tiles (70x50 with 32x32 tiles) are fully within bounds and reused
// cleanly. A pass writes only the valid region; the padding must not bleed into
// the next tile or the next frame.
int case_edge_tiles_bounds_and_reuse()
{
  fixture<config_float> f;
  // Fill the whole valid tile area with a marker; if a pass overran the tile
  // pitch it would corrupt neighbouring tiles or out-of-bounds memory.
  block_pass block;
  block.x=0;
  block.y=0;
  block.width=tile_width;
  block.height=tile_height;
  block.color.set_rgba8(0, 0, 255, 255);
  uint16_t xs[64], ys[64], ws[64], hs[64], ps[64];
  unsigned calls=0;
  count_pass counter={&calls, xs, ys, ws, hs, ps};
  f.device.dispatch_tile_pass(counter);
  f.device.dispatch_tile_pass(block);
  f.device.commit();
  TASK_CHECK(calls==num_tiles);
  // the bottom-right tile only has a 6x18 valid area; the block pass clamps to
  // ctx_.width/height so the canvas is fully blue and nothing outside is touched
  TASK_CHECK(f.fb.at(69, 49).c.b==255);
  TASK_CHECK(f.fb.at(0, 0).c.b==255);

  // next frame: nothing submitted -> fully clear (no blue leakage)
  f.device.commit();
  TASK_CHECK(count_nonzero(f.fb)==0);
  return 0;
}

// The final tile shader still runs after all passes: a custom shader observes
// the pass' colour writes.
struct double_red_shader
{
  template<e_pixel_format dst_fmt>
  void exec(pixel<dst_fmt> &dst_, const void *const*rts_, const rasterizer_depth_target&, size_t offs_, uint16_t, uint16_t) const
  {
    const pixel_t &src=((const pixel_t*)rts_[0])[offs_];
    dst_.set_rgba8((uint8_t)(src.c.r*2), src.c.g, src.c.b, 255);
  }
};

int case_final_tile_shader_after_passes()
{
  fixture<config_float> f;
  // pass writes red into rt0
  struct write_red
  {
    enum {writes_depth=false};
    void exec(rasterizer_tile_pass_context &ctx_) const
    {
      for(uint16_t y=0; y<ctx_.height; ++y)
      {
        pixel_t *row=(pixel_t*)ctx_.rts[0]+y*ctx_.pitch;
        for(uint16_t x=0; x<ctx_.width; ++x)
          row[x].set_rgba8(50, 0, 0, 255);
      }
    }
  } red;
  f.device.dispatch_tile_pass(red);
  f.device.commit(double_red_shader());
  TASK_CHECK(f.fb.at(0, 0).c.r==100);
  TASK_CHECK(f.fb.at(69, 49).c.r==100);
  return 0;
}

} // namespace


//============================================================================
// case registration
//============================================================================
extern const task_case task_cases[]=
{
  {"ordering", "every_tile_once", case_every_tile_once},
  {"ordering", "pass_only_frame", case_pass_only_frame},
  {"ordering", "geometry_pass_geometry_order", case_geometry_pass_geometry_order},
  {"ordering", "pass_observes_preceding_geometry", case_pass_observes_preceding_geometry},
  {"ordering", "pass_geometry_pass_order", case_pass_geometry_pass_order},
  {"ordering", "consecutive_passes", case_consecutive_passes},
  {"ordering", "tile_orders", case_tile_orders},
  {"depth", "pass_depth_affects_later_geometry", case_pass_depth_affects_later_geometry},
  {"depth", "hiz_matches_hiz_off_after_depth_pass", case_hiz_matches_hiz_off_after_depth_pass},
  {"depth", "hiz_matches_hiz_off_after_lowering_pass", case_hiz_matches_hiz_off_after_lowering_pass},
  {"depth", "hiz_matches_hiz_off_after_mode_switch", case_hiz_matches_hiz_off_after_mode_switch},
  {"depth", "uint_depth_formats", case_uint_depth_formats},
  {"depth", "no_depth_configuration", case_no_depth_configuration},
  {"depth", "context_metadata", case_context_metadata},
  {"depth", "multiple_render_targets", case_multiple_render_targets},
  {"depth", "reduction_with_and_without_clusters", case_reduction_with_and_without_clusters},
  {"depth", "reduction_then_geometry_then_tint", case_reduction_then_geometry_then_tint},
  {"reuse", "frame_reuse_resets_attachments", case_frame_reuse_resets_attachments},
  {"reuse", "frame_reuse_resets_depth", case_frame_reuse_resets_depth},
  {"reuse", "frame_reuse_independent_scenes", case_frame_reuse_independent_scenes},
  {"reuse", "edge_tiles_bounds_and_reuse", case_edge_tiles_bounds_and_reuse},
  {"reuse", "final_tile_shader_after_passes", case_final_tile_shader_after_passes},
};
extern const size_t task_case_count=sizeof(task_cases)/sizeof(task_cases[0]);
