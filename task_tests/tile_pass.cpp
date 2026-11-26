//============================================================================
// EmberGL tile pass task tests.
//
// Ordered tile passes interleaved with geometry dispatches, verified against
// the in-memory graphics device. The file only registers the task cases; the
// driver in task_support/driver.c provides main().
//============================================================================
#include "task_support/check.h"
#include "egl_device.h"
#include <cstring>

using namespace egl;

namespace
{
  const unsigned c_fb_width=70;
  const unsigned c_fb_height=50;
  const unsigned c_fb_pixels=c_fb_width*c_fb_height;
  typedef pixel<pixfmt_r8g8b8a8> Pixel;
  typedef graphics_device_mem<pixfmt_r8g8b8a8, c_fb_width, c_fb_height> Device;

  //--------------------------------------------------------------------------
  // device configurations
  //--------------------------------------------------------------------------
  template<e_depth_format DF, bool HIZ, int TORDER=tileorder_morton>
  struct TestCfg: rasterizer_memory_cfg_base
  {
    enum {tile_width=32, tile_height=32, max_dma_transfers=0};
    enum {depth_format=DF, depth_hiz=HIZ, rt1_fmt=pixfmt_r8g8b8a8};
    enum {tile_order=TORDER};
  };
  typedef TestCfg<depthfmt_float32, true> CfgF32Hz;
  typedef TestCfg<depthfmt_float32, false> CfgF32NoHz;
  typedef TestCfg<depthfmt_uint16, true> CfgU16Hz;
  typedef TestCfg<depthfmt_uint16, false> CfgU16NoHz;
  typedef TestCfg<depthfmt_uint8, true> CfgU8Hz;
  typedef TestCfg<depthfmt_uint8, false> CfgU8NoHz;
  typedef TestCfg<depthfmt_none, false> CfgNone;
  typedef TestCfg<depthfmt_float32, true, tileorder_linear> CfgLinear;

  template<class Cfg>
  struct Fixture
  {
    Pixel frame[c_fb_pixels];
    Device device;
    rasterizer_memory<Device, Cfg> memory;
    Fixture():device(frame)
    {
      std::memset(frame, 0, sizeof(frame));
      memory.init(device);
      device.clear_depth(cleardepth_max);
    }
  };

  //--------------------------------------------------------------------------
  // geometry: single full-ish triangle at a fixed depth with selectable state
  //--------------------------------------------------------------------------
  template<e_depth_format DF, int DFUNC=rstate_depthfunc_less, bool DWRITE=true>
  struct Tri: rasterizer_psc_base
  {
    enum {rt0_fmt=pixfmt_r8g8b8a8, depth_format=DF};
    enum {depth_func=DFUNC, depth_write=DWRITE};
    enum {tbfc_mode=rstate_tbfc_none, bci_mode=rstate_bcimode_noperspective};
    struct ptv_fmt {vec3f pos;};
    ptv_fmt vertices[3];
    Pixel color;
    vec3f minimum, maximum;
    Tri(float z, uint8_t r, uint8_t g, uint8_t b)
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

  // geometry whose binning box only reaches the top-left 32x32 tile
  struct SmallTri: Tri<depthfmt_float32>
  {
    SmallTri(float z, uint8_t r, uint8_t g, uint8_t b):Tri(z, r, g, b)
    {
      minimum.set(0.1f, 0.1f, z);
      maximum.set(0.34f, 0.5f, z);
    }
  };

  //--------------------------------------------------------------------------
  // depth helpers (normalized [0,1] regardless of storage format)
  //--------------------------------------------------------------------------
  float read_norm_depth(const rasterizer_tile_pass_context &ctx, uint16_t lx, uint16_t ly)
  {
    const size_t offs=lx+ly*ctx.pitch;
    switch(ctx.depth.format)
    {
      case depthfmt_uint8: return ((const uint8_t*)ctx.depth.data)[offs]/255.0f;
      case depthfmt_uint16: return ((const uint16_t*)ctx.depth.data)[offs]/65535.0f;
      case depthfmt_float32: return ((const float*)ctx.depth.data)[offs];
      default: return 1.0f;
    }
  }

  void write_norm_depth(const rasterizer_tile_pass_context &ctx, uint16_t lx, uint16_t ly, float z)
  {
    const size_t offs=lx+ly*ctx.pitch;
    switch(ctx.depth.format)
    {
      case depthfmt_uint8: ((uint8_t*)ctx.depth.data)[offs]=uint8_t(z*255.0f); break;
      case depthfmt_uint16: ((uint16_t*)ctx.depth.data)[offs]=uint16_t(z*65535.0f); break;
      case depthfmt_float32: ((float*)ctx.depth.data)[offs]=z; break;
      default: break;
    }
  }

  //--------------------------------------------------------------------------
  // tile passes. all are trivially copyable; external pointers stay owned by
  // the caller until commit() returns.
  //--------------------------------------------------------------------------
  // writes a fixed blue tag over the whole valid tile
  struct TagPass
  {
    static constexpr bool writes_depth=false;
    uint8_t tag;
    explicit TagPass(uint8_t tag_=77):tag(tag_) {}
    void exec(rasterizer_tile_pass_context &ctx) const
    {
      for(uint16_t y=0; y<ctx.height; ++y)
        for(uint16_t x=0; x<ctx.width; ++x)
          ((Pixel*)ctx.rts[0])[x+y*ctx.pitch].c.b=tag;
    }
  };

  // two consecutive passes: A writes a tag, B only reacts if it sees A's tag
  struct SeqPassA
  {
    static constexpr bool writes_depth=false;
    void exec(rasterizer_tile_pass_context &ctx) const
    {
      for(uint16_t y=0; y<ctx.height; ++y)
        for(uint16_t x=0; x<ctx.width; ++x)
          ((Pixel*)ctx.rts[0])[x+y*ctx.pitch].c.b=10;
    }
  };
  struct SeqPassB
  {
    static constexpr bool writes_depth=false;
    void exec(rasterizer_tile_pass_context &ctx) const
    {
      for(uint16_t y=0; y<ctx.height; ++y)
        for(uint16_t x=0; x<ctx.width; ++x)
        {
          Pixel &p=((Pixel*)ctx.rts[0])[x+y*ctx.pitch];
          if(p.c.b==10)
            p.c.g=15;
        }
    }
  };

  // in-tile reduction: minimum visible depth, then tint blue when below threshold
  struct ReduceMinDepthPass
  {
    static constexpr bool writes_depth=false;
    float threshold;
    explicit ReduceMinDepthPass(float t_=0.5f):threshold(t_) {}
    void exec(rasterizer_tile_pass_context &ctx) const
    {
      if(!ctx.depth.data)
        return;
      float min_z=1.0f;
      for(uint16_t y=0; y<ctx.height; ++y)
        for(uint16_t x=0; x<ctx.width; ++x)
        {
          float z=read_norm_depth(ctx, x, y);
          if(z<min_z)
            min_z=z;
        }
      if(min_z<threshold)
        for(uint16_t y=0; y<ctx.height; ++y)
          for(uint16_t x=0; x<ctx.width; ++x)
            ((Pixel*)ctx.rts[0])[x+y*ctx.pitch].c.b=200;
    }
  };

  // writes a constant normalized depth over the valid tile
  struct DepthSetPass
  {
    static constexpr bool writes_depth=true;
    float depth;
    explicit DepthSetPass(float d_):depth(d_) {}
    void exec(rasterizer_tile_pass_context &ctx) const
    {
      if(!ctx.depth.data)
        return;
      for(uint16_t y=0; y<ctx.height; ++y)
        for(uint16_t x=0; x<ctx.width; ++x)
          write_norm_depth(ctx, x, y, depth);
    }
  };

  // records the per-pixel normalized depth into a caller-owned screen buffer
  struct DepthCapturePass
  {
    static constexpr bool writes_depth=false;
    float *out;
    unsigned screen_pitch;
    DepthCapturePass(float *out_, unsigned pitch_):out(out_), screen_pitch(pitch_) {}
    void exec(rasterizer_tile_pass_context &ctx) const
    {
      for(uint16_t y=0; y<ctx.height; ++y)
        for(uint16_t x=0; x<ctx.width; ++x)
          out[(ctx.x+x)+(ctx.y+y)*screen_pitch]=read_norm_depth(ctx, x, y);
    }
  };

  // encodes the tile geometry into the local origin pixel
  struct DimPass
  {
    static constexpr bool writes_depth=false;
    void exec(rasterizer_tile_pass_context &ctx) const
    {
      Pixel &p=((Pixel*)ctx.rts[0])[0];
      p.c.r=uint8_t(ctx.width);
      p.c.g=uint8_t(ctx.height);
      p.c.b=uint8_t(ctx.pitch);
      p.c.a=255;
    }
  };

  // counts exec() calls into caller-owned storage, proving when callbacks run
  struct CountPass
  {
    static constexpr bool writes_depth=false;
    unsigned *counter;
    explicit CountPass(unsigned *counter_):counter(counter_) {}
    void exec(rasterizer_tile_pass_context&) const {++*counter;}
  };

  // verifies the attachment layout and records it externally
  struct AttachmentPass
  {
    static constexpr bool writes_depth=false;
    unsigned *ok;
    explicit AttachmentPass(unsigned *ok_):ok(ok_) {}
    void exec(rasterizer_tile_pass_context &ctx) const
    {
      if(ctx.rts[0] && ctx.rt_pixel_sizes[0]==sizeof(Pixel) &&
         ctx.rts[1] && ctx.rt_pixel_sizes[1]==sizeof(Pixel) &&
         (!ctx.rts[2]) && (!ctx.rts[3]))
        ++*ok;
      ((Pixel*)ctx.rts[1])[0].c.r=99;
    }
  };

  // runs with no depth configured and confirms the attachment is null
  struct NoDepthPass
  {
    static constexpr bool writes_depth=false;
    unsigned *ok;
    explicit NoDepthPass(unsigned *ok_):ok(ok_) {}
    void exec(rasterizer_tile_pass_context &ctx) const
    {
      if(!ctx.depth.data && ctx.depth.format==depthfmt_none)
        ++*ok;
      ((Pixel*)ctx.rts[0])[0].c.g=55;
    }
  };

  //--------------------------------------------------------------------------
  // helpers
  //--------------------------------------------------------------------------
  bool frames_equal(const Pixel *a, const Pixel *b)
  {
    for(unsigned i=0; i<c_fb_pixels; ++i)
      if(a[i].v!=b[i].v)
        return false;
    return true;
  }

  bool depths_equal(const float *a, const float *b)
  {
    for(unsigned i=0; i<c_fb_pixels; ++i)
      if(a[i]!=b[i])
        return false;
    return true;
  }

  enum channel_t {ch_r, ch_g, ch_b, ch_a};
  uint8_t get_channel(const Pixel &p, channel_t ch)
  {
    switch(ch)
    {
      case ch_r: return p.c.r;
      case ch_g: return p.c.g;
      case ch_b: return p.c.b;
      case ch_a: return p.c.a;
    }
    return 0;
  }

  unsigned count_channel(const Pixel *frame, channel_t ch, uint8_t value)
  {
    unsigned n=0;
    for(unsigned i=0; i<c_fb_pixels; ++i)
      if(get_channel(frame[i], ch)==value)
        ++n;
    return n;
  }

  //--------------------------------------------------------------------------
  // ordering group
  //--------------------------------------------------------------------------
  int order_geometry_pass_geometry()
  {
    Fixture<CfgF32Hz> f;
    // red geometry, then a reduction/tint pass, then green geometry behind it
    f.device.dispatch_pso(Tri<depthfmt_float32>(0.3f, 255, 0, 0));
    f.device.dispatch_tile_pass(ReduceMinDepthPass(0.5f));
    f.device.dispatch_pso(Tri<depthfmt_float32>(0.8f, 0, 255, 0));
    f.device.commit();

    // center pixel is inside both triangles: red wins depth, pass tinted blue
    const Pixel &center=f.frame[25*c_fb_width+35];
    TASK_CHECK(center.c.r==255);
    TASK_CHECK(center.c.g==0);
    TASK_CHECK(center.c.b==200);
    // the reduction only tints tiles that contain geometry: the right column
    // tiles have no clusters and stay at clear depth, so they are not tinted
    unsigned tinted=count_channel(f.frame, ch_b, 200);
    TASK_CHECK(tinted>0);
    TASK_CHECK(tinted<c_fb_pixels);
    TASK_CHECK(f.frame[25*c_fb_width+67].c.b==0);
    return 0;
  }

  int order_consecutive_passes()
  {
    Fixture<CfgF32Hz> f;
    f.device.dispatch_pso(Tri<depthfmt_float32>(0.3f, 255, 0, 0));
    f.device.dispatch_tile_pass(SeqPassA());
    f.device.dispatch_tile_pass(SeqPassB());
    f.device.commit();
    // both passes ran in order: A's blue tag and B's green reaction everywhere
    TASK_CHECK(count_channel(f.frame, ch_b, 10)==c_fb_pixels);
    TASK_CHECK(count_channel(f.frame, ch_g, 15)==c_fb_pixels);
    return 0;
  }

  int order_pass_only_has_output()
  {
    Fixture<CfgF32Hz> f;
    f.device.dispatch_tile_pass(TagPass(33));
    f.device.commit();
    // a frame containing only passes still produces output on every pixel
    TASK_CHECK(count_channel(f.frame, ch_b, 33)==c_fb_pixels);
    return 0;
  }

  int order_tiles_without_clusters()
  {
    Fixture<CfgF32Hz> f;
    // triangle binning only reaches the top-left tile region; the pass must
    // still run for all 6 tiles
    f.device.dispatch_pso(Tri<depthfmt_float32>(0.3f, 255, 0, 0));
    f.device.dispatch_tile_pass(TagPass(44));
    f.device.commit();
    TASK_CHECK(count_channel(f.frame, ch_b, 44)==c_fb_pixels);
    // geometry only colored some pixels
    TASK_CHECK(count_channel(f.frame, ch_r, 255)>0);
    TASK_CHECK(count_channel(f.frame, ch_r, 255)<c_fb_pixels);
    return 0;
  }

  int order_pass_between_disjoint_geometry()
  {
    Fixture<CfgF32Hz> f;
    // geometry 1 only bins to the top-left tile, then a pass, then geometry 2
    // which reaches every tile. tiles not touched by geometry 1 must still run
    // the pass before geometry 2 (no pass may be dropped across the gap).
    f.device.dispatch_pso(SmallTri(0.3f, 255, 0, 0));
    f.device.dispatch_tile_pass(TagPass(77));
    f.device.dispatch_pso(Tri<depthfmt_float32>(0.8f, 0, 255, 0));
    f.device.commit();
    // every tile executed the pass
    TASK_CHECK(count_channel(f.frame, ch_b, 77)>0);
    // the red small triangle only lands in the top-left tile it was binned to
    unsigned red_topleft=0, red_center=0;
    for(unsigned y=0; y<c_fb_height; ++y)
      for(unsigned x=0; x<c_fb_width; ++x)
        if(f.frame[y*c_fb_width+x].c.r==255)
        {
          if(x<32 && y<32) ++red_topleft;
          if(x>=32 && x<64 && y<32) ++red_center;
        }
    TASK_CHECK(red_topleft>0);
    TASK_CHECK(red_center==0);
    // the center tile never received the red cluster: only pass then green
    TASK_CHECK(f.frame[25*c_fb_width+35].c.r==0);
    TASK_CHECK(f.frame[25*c_fb_width+35].c.g==255);
    return 0;
  }

  int order_edge_tile_dims()
  {
    Fixture<CfgF32Hz> f;
    f.device.dispatch_tile_pass(DimPass());
    f.device.commit();
    // 70x50 with 32px tiles => 3x2 tiles. check every tile origin geometry.
    const struct {uint16_t ox, oy, w, h;} expect[]=
    {
      {0, 0, 32, 32}, {32, 0, 32, 32}, {64, 0, 6, 32},
      {0, 32, 32, 18}, {32, 32, 32, 18}, {64, 32, 6, 18},
    };
    for(const auto &e: expect)
    {
      const Pixel &p=f.frame[e.oy*c_fb_width+e.ox];
      TASK_CHECK(p.c.r==e.w);
      TASK_CHECK(p.c.g==e.h);
      TASK_CHECK(p.c.b==32); // pitch is the full allocated tile stride
    }
    return 0;
  }

  int order_callback_timing_and_count()
  {
    Fixture<CfgF32Hz> f;
    unsigned calls=0;
    CountPass pass(&calls);
    f.device.dispatch_tile_pass(pass);
    // submission must not execute the callback
    TASK_CHECK(calls==0);
    f.device.commit();
    // exactly once per valid tile (3x2)
    TASK_CHECK(calls==6);
    return 0;
  }

  int order_linear_tile_order()
  {
    Fixture<CfgLinear> f;
    f.device.dispatch_pso(Tri<depthfmt_float32>(0.3f, 255, 0, 0));
    f.device.dispatch_tile_pass(TagPass(55));
    f.device.dispatch_pso(Tri<depthfmt_float32>(0.8f, 0, 255, 0));
    f.device.commit();
    TASK_CHECK(count_channel(f.frame, ch_b, 55)==c_fb_pixels);
    const Pixel &center=f.frame[25*c_fb_width+35];
    TASK_CHECK(center.c.r==255 && center.c.g==0);
    return 0;
  }

  int order_attachment_layout()
  {
    Fixture<CfgF32Hz> f;
    unsigned ok=0;
    f.device.dispatch_tile_pass(AttachmentPass(&ok));
    f.device.commit();
    TASK_CHECK(ok==6); // once per tile, rt0/rt1 set, rt2/rt3 null
    return 0;
  }

  //--------------------------------------------------------------------------
  // depth group: hi-z enabled vs disabled must match in color and depth
  //--------------------------------------------------------------------------
  template<class CfgHz, class CfgNoHz, e_depth_format DF>
  int depth_parity_common()
  {
    float depth_hz[c_fb_pixels], depth_nohz[c_fb_pixels];
    Pixel color_hz[c_fb_pixels], color_nohz[c_fb_pixels];

    {
      Fixture<CfgHz> f;
      f.device.dispatch_pso(Tri<DF>(0.5f, 255, 0, 0));
      f.device.dispatch_tile_pass(DepthSetPass(0.9f));  // raise depth
      f.device.dispatch_pso(Tri<DF, rstate_depthfunc_less>(0.7f, 0, 255, 0));
      f.device.dispatch_tile_pass(DepthSetPass(0.2f));  // lower depth
      f.device.dispatch_pso(Tri<DF, rstate_depthfunc_less>(0.4f, 0, 0, 255));
      f.device.dispatch_tile_pass(DepthCapturePass(depth_hz, c_fb_width));
      f.device.commit();
      std::memcpy(color_hz, f.frame, sizeof(color_hz));
    }
    {
      Fixture<CfgNoHz> f;
      f.device.dispatch_pso(Tri<DF>(0.5f, 255, 0, 0));
      f.device.dispatch_tile_pass(DepthSetPass(0.9f));
      f.device.dispatch_pso(Tri<DF, rstate_depthfunc_less>(0.7f, 0, 255, 0));
      f.device.dispatch_tile_pass(DepthSetPass(0.2f));
      f.device.dispatch_pso(Tri<DF, rstate_depthfunc_less>(0.4f, 0, 0, 255));
      f.device.dispatch_tile_pass(DepthCapturePass(depth_nohz, c_fb_width));
      f.device.commit();
      std::memcpy(color_nohz, f.frame, sizeof(color_nohz));
    }

    TASK_CHECK(frames_equal(color_hz, color_nohz));
    TASK_CHECK(depths_equal(depth_hz, depth_nohz));

    // sanity: after lowering depth to 0.2 the final blue (0.4, LESS) is fully
    // rejected, while the green (0.7) drawn after the 0.9 raise survives
    const Pixel &center=color_hz[25*c_fb_width+35];
    TASK_CHECK(center.c.b==0);
    TASK_CHECK(center.c.g==255);
    return 0;
  }

  int depth_float32_parity() {return depth_parity_common<CfgF32Hz, CfgF32NoHz, depthfmt_float32>();}
  int depth_uint16_parity() {return depth_parity_common<CfgU16Hz, CfgU16NoHz, depthfmt_uint16>();}
  int depth_uint8_parity() {return depth_parity_common<CfgU8Hz, CfgU8NoHz, depthfmt_uint8>();}

  int depth_lequal_after_pass()
  {
    float depth_hz[c_fb_pixels], depth_nohz[c_fb_pixels];
    Pixel color_hz[c_fb_pixels], color_nohz[c_fb_pixels];
    {
      Fixture<CfgF32Hz> f;
      f.device.dispatch_pso(Tri<depthfmt_float32, rstate_depthfunc_less_equal>(0.5f, 255, 0, 0));
      f.device.dispatch_tile_pass(DepthSetPass(0.5f)); // equal to geometry depth
      f.device.dispatch_pso(Tri<depthfmt_float32, rstate_depthfunc_less_equal>(0.5f, 0, 255, 0));
      f.device.dispatch_tile_pass(DepthCapturePass(depth_hz, c_fb_width));
      f.device.commit();
      std::memcpy(color_hz, f.frame, sizeof(color_hz));
    }
    {
      Fixture<CfgF32NoHz> f;
      f.device.dispatch_pso(Tri<depthfmt_float32, rstate_depthfunc_less_equal>(0.5f, 255, 0, 0));
      f.device.dispatch_tile_pass(DepthSetPass(0.5f));
      f.device.dispatch_pso(Tri<depthfmt_float32, rstate_depthfunc_less_equal>(0.5f, 0, 255, 0));
      f.device.dispatch_tile_pass(DepthCapturePass(depth_nohz, c_fb_width));
      f.device.commit();
      std::memcpy(color_nohz, f.frame, sizeof(color_nohz));
    }
    TASK_CHECK(frames_equal(color_hz, color_nohz));
    TASK_CHECK(depths_equal(depth_hz, depth_nohz));
    // LEQUAL at equal depth passes and replaces red with green
    TASK_CHECK(color_hz[25*c_fb_width+35].c.g==255);
    return 0;
  }

  int depth_consecutive_writes_parity()
  {
    // two adjacent depth-writing passes (raise then lower) must leave hi-z in
    // a state consistent with hi-z disabled for the following LESS geometry
    float depth_hz[c_fb_pixels], depth_nohz[c_fb_pixels];
    Pixel color_hz[c_fb_pixels], color_nohz[c_fb_pixels];
    const auto sequence=[](Device &device, float *depths)
    {
      device.dispatch_tile_pass(DepthSetPass(0.95f));
      device.dispatch_tile_pass(DepthSetPass(0.25f));
      device.dispatch_pso(Tri<depthfmt_float32, rstate_depthfunc_less>(0.5f, 255, 0, 0));
      device.dispatch_tile_pass(DepthCapturePass(depths, c_fb_width));
      device.commit();
    };
    {
      Fixture<CfgF32Hz> f;
      sequence(f.device, depth_hz);
      std::memcpy(color_hz, f.frame, sizeof(color_hz));
    }
    {
      Fixture<CfgF32NoHz> f;
      sequence(f.device, depth_nohz);
      std::memcpy(color_nohz, f.frame, sizeof(color_nohz));
    }
    TASK_CHECK(frames_equal(color_hz, color_nohz));
    TASK_CHECK(depths_equal(depth_hz, depth_nohz));
    // depth lowered to 0.25 rejects the 0.5 LESS triangle on covered pixels
    TASK_CHECK(color_hz[25*c_fb_width+35].c.r==0);
    return 0;
  }

  int depth_no_depth_config()
  {
    Fixture<CfgNone> f;
    unsigned ok=0;
    f.device.dispatch_pso(Tri<depthfmt_none>(0.3f, 255, 0, 0));
    f.device.dispatch_tile_pass(NoDepthPass(&ok));
    f.device.commit();
    TASK_CHECK(ok==6); // null depth reported on every tile
    TASK_CHECK(f.frame[25*c_fb_width+35].c.r==255);
    return 0;
  }

  int depth_read_only_pass_preserves_path()
  {
    // a read-only pass must not disturb geometry occlusion: identical output
    // to the same geometry sequence without any pass
    Pixel with_pass[c_fb_pixels], without_pass[c_fb_pixels];
    {
      Fixture<CfgF32Hz> f;
      f.device.dispatch_pso(Tri<depthfmt_float32>(0.3f, 255, 0, 0));
      f.device.dispatch_tile_pass(ReduceMinDepthPass(0.5f));
      f.device.dispatch_pso(Tri<depthfmt_float32>(0.8f, 0, 255, 0));
      f.device.commit();
      std::memcpy(with_pass, f.frame, sizeof(with_pass));
    }
    {
      Fixture<CfgF32Hz> f;
      f.device.dispatch_pso(Tri<depthfmt_float32>(0.3f, 255, 0, 0));
      f.device.dispatch_pso(Tri<depthfmt_float32>(0.8f, 0, 255, 0));
      f.device.commit();
      std::memcpy(without_pass, f.frame, sizeof(without_pass));
    }
    // the read-only pass only adds the blue tint; rgb geometry result matches
    for(unsigned i=0; i<c_fb_pixels; ++i)
    {
      TASK_CHECK(with_pass[i].c.r==without_pass[i].c.r);
      TASK_CHECK(with_pass[i].c.g==without_pass[i].c.g);
    }
    return 0;
  }

  //--------------------------------------------------------------------------
  // reuse group: tile buffers must be clean for the next commit
  //--------------------------------------------------------------------------
  int reuse_color_buffers_cleared()
  {
    Fixture<CfgF32Hz> f;
    // frame 1: pass tags the whole screen, geometry then overwrites part of it
    f.device.dispatch_tile_pass(TagPass(66));
    f.device.dispatch_pso(Tri<depthfmt_float32>(0.3f, 255, 0, 0));
    f.device.commit();
    TASK_CHECK(count_channel(f.frame, ch_b, 66)>0);
    TASK_CHECK(count_channel(f.frame, ch_b, 66)<c_fb_pixels);

    // frame 2: green geometry only. no blue tag residue may remain.
    std::memset(f.frame, 0, sizeof(f.frame));
    f.device.dispatch_pso(Tri<depthfmt_float32>(0.3f, 0, 255, 0));
    f.device.commit();
    TASK_CHECK(count_channel(f.frame, ch_b, 66)==0);
    TASK_CHECK(f.frame[25*c_fb_width+35].c.g==255);
    TASK_CHECK(f.frame[25*c_fb_width+35].c.r==0);
    return 0;
  }

  int reuse_depth_buffer_cleared()
  {
    Fixture<CfgF32Hz> f;
    // frame 1: force every depth pixel near the camera
    f.device.dispatch_tile_pass(DepthSetPass(0.1f));
    f.device.commit();

    // frame 2: LESS geometry at 0.5 must draw, proving depth was reset to 1.0
    f.device.dispatch_pso(Tri<depthfmt_float32>(0.5f, 255, 0, 0));
    f.device.commit();
    TASK_CHECK(f.frame[25*c_fb_width+35].c.r==255);
    return 0;
  }

  int reuse_matches_fresh_fixture()
  {
    Fixture<CfgF32Hz> f;
    // dirty the buffers with a pass-heavy first frame
    f.device.dispatch_tile_pass(TagPass(99));
    f.device.dispatch_tile_pass(DepthSetPass(0.15f));
    f.device.dispatch_pso(Tri<depthfmt_float32>(0.3f, 255, 0, 0));
    f.device.commit();

    // second frame on the reused backing storage
    std::memset(f.frame, 0, sizeof(f.frame));
    f.device.dispatch_pso(Tri<depthfmt_float32>(0.4f, 10, 20, 30));
    f.device.dispatch_pso(Tri<depthfmt_float32>(0.9f, 40, 50, 60));
    f.device.commit();

    // identical sequence on a brand new fixture
    Pixel reference[c_fb_pixels];
    {
      Fixture<CfgF32Hz> g;
      g.device.dispatch_pso(Tri<depthfmt_float32>(0.4f, 10, 20, 30));
      g.device.dispatch_pso(Tri<depthfmt_float32>(0.9f, 40, 50, 60));
      g.device.commit();
      std::memcpy(reference, g.frame, sizeof(reference));
    }
    TASK_CHECK(frames_equal(f.frame, reference));
    return 0;
  }
} // namespace

//----------------------------------------------------------------------------
// task registration
//----------------------------------------------------------------------------
const task_case task_cases[]=
{
  {"ordering", "geometry_pass_geometry", order_geometry_pass_geometry},
  {"ordering", "pass_between_disjoint_geometry", order_pass_between_disjoint_geometry},
  {"ordering", "consecutive_passes", order_consecutive_passes},
  {"ordering", "pass_only_has_output", order_pass_only_has_output},
  {"ordering", "tiles_without_clusters", order_tiles_without_clusters},
  {"ordering", "edge_tile_dims_70x50", order_edge_tile_dims},
  {"ordering", "callback_timing_and_count", order_callback_timing_and_count},
  {"ordering", "linear_tile_order", order_linear_tile_order},
  {"ordering", "attachment_layout", order_attachment_layout},

  {"depth", "float32_hiz_parity", depth_float32_parity},
  {"depth", "uint16_hiz_parity", depth_uint16_parity},
  {"depth", "uint8_hiz_parity", depth_uint8_parity},
  {"depth", "lequal_after_depth_pass", depth_lequal_after_pass},
  {"depth", "consecutive_writes_parity", depth_consecutive_writes_parity},
  {"depth", "no_depth_config", depth_no_depth_config},
  {"depth", "read_only_pass_preserves_path", depth_read_only_pass_preserves_path},

  {"reuse", "color_buffers_cleared", reuse_color_buffers_cleared},
  {"reuse", "depth_buffer_cleared", reuse_depth_buffer_cleared},
  {"reuse", "matches_fresh_fixture", reuse_matches_fresh_fixture},
};
const size_t task_case_count=sizeof(task_cases)/sizeof(task_cases[0]);
