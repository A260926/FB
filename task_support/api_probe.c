#include <string.h>
#include "mini3d.h"
#include "reyes.h"
int main(void){device_t d;device_init(&d,64,64,0);d.background=0;d.render_state=RENDER_STATE_COLOR;device_clear(&d,0);
 reyes_patch p;memset(&p,0,sizeof(p));for(int j=0;j<4;j++)for(int i=0;i<4;i++){p.cp[4*j+i][0]=-1+2.0*i/3;p.cp[4*j+i][1]=1-2.0*j/3;p.cp[4*j+i][2]=3;}p.color[0]=.8;p.color[1]=.2;p.color[2]=.1;p.ku=p.kv=1;
 reyes_options o;memset(&o,0,sizeof(o));o.max_grid_vertices=81;o.max_depth=10;o.max_edge_pixels=2;o.near_z=1;o.far_z=500;o.displacement_axis[2]=1;o.light_dir[2]=-1;o.ambient=.2;
 reyes_stats s;memset(&s,0,sizeof(s));int r=reyes_render(&d,&p,1,&o,&s);
 int ok=r==1&&s.emitted_triangles>0&&s.shaded_vertices>0&&s.peak_grid_vertices<=81&&s.peak_live_vertices<=162&&(d.framebuffer[32][32]&0xff0000)>0x400000&&d.zbuffer[32][32]>0;
 device_destroy(&d);return ok?0:5;}
