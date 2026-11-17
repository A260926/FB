#include "scene.h"
int main() {
  scene::Fixture f;
  scene::Triangle front(0.2f,255,0,0),back(0.8f,0,255,0);
  f.device.dispatch_pso(front);
  f.device.dispatch_pso(back);
  f.device.commit();
  unsigned red=0,green=0;
  for(const auto &p:f.frame) {red+=p.c.r!=0;green+=p.c.g!=0;}
  if(red==0 || green!=0 || f.frame[25*70+35].c.r!=255) return 1;
  // The next commit really reuses the same tile backing storage.
  f.device.dispatch_pso(back);
  f.device.commit();
  if(f.frame[25*70+35].c.g!=255) return 2;
  std::printf("BASELINE_OK red=%u green=%u frames=2 tiles=6\n",red,green);
  std::puts("CURRENT_CAPABILITY geometry_dispatch_and_final_tile_shader_only");
  return 0;
}
