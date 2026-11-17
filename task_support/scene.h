#ifndef TILE_SCENE_H
#define TILE_SCENE_H
#include "egl_device.h"
#include <cstdio>
#include <cstring>
namespace scene {
using namespace egl;
typedef pixel<pixfmt_r8g8b8a8> Pixel;
typedef graphics_device_mem<pixfmt_r8g8b8a8,70,50> Device;
struct Config: rasterizer_memory_cfg_base {
  enum {tile_width=32,tile_height=32,max_dma_transfers=0};
  enum {depth_format=depthfmt_float32,rt1_fmt=pixfmt_r8g8b8a8};
};
struct Fixture {
  Pixel frame[70*50];
  Device device;
  rasterizer_memory<Device,Config> memory;
  Fixture():device(frame) {
    std::memset(frame,0,sizeof(frame));
    memory.init(device);
    device.clear_depth(cleardepth_max);
  }
};
// A real PSC with an exact screen-space box for binning. No new tile-pass API.
struct Triangle: rasterizer_psc_base {
  enum {rt0_fmt=pixfmt_r8g8b8a8,depth_format=depthfmt_float32};
  enum {tbfc_mode=rstate_tbfc_none,bci_mode=rstate_bcimode_noperspective};
  struct ptv_fmt {vec3f pos;};
  ptv_fmt vertices[3];
  Pixel color;
  vec3f minimum,maximum;
  Triangle(float z,uint8_t r,uint8_t g,uint8_t b) {
    vertices[0].pos.set(-0.8f,-0.8f,z);
    vertices[1].pos.set(0.8f,-0.8f,z);
    vertices[2].pos.set(0.0f,0.8f,z);
    color.set_rgba8(r,g,b,255);
    minimum.set(0.1f,0.1f,z);
    maximum.set(0.9f,0.9f,z);
  }
  rasterizer_local_cluster_index_t num_clusters() const {return 1;}
  const void *cluster(rasterizer_local_cluster_index_t) const {return this;}
  uint8_t num_cluster_vertices(const void*) const {return 3;}
  uint8_t num_cluster_triangles(const void*) const {return 1;}
  void bin_cluster(rasterizer_tiling &t,const pso_state&,rasterizer_dispatch_index_t d,rasterizer_local_cluster_index_t c) const {t.add_cluster(minimum,maximum,d,c);}
  void tform_cluster(ptv_fmt *out,const pso_state&,const void*) const {std::memcpy(out,vertices,sizeof(vertices));}
  void setup_primitive(const void*,uint8_t,uint8_t ids[3]) const {ids[0]=0;ids[1]=1;ids[2]=2;}
  template<class PSC> void shade_pixel(rasterizer_pixel_out<PSC> &out,const pso_state&,const ptv_fmt&,const ptv_fmt&,const ptv_fmt&,const vec3f&,uint8_t) const {out.export_rt0(color);}
};
}
#endif
