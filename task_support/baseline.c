#include <float.h>
#include <math.h>
#include <stdio.h>
#include <v.h>
#include <datatypes/mesh.h>
#include <datatypes/bbox.h>
#include <datatypes/hitrecord.h>
#include <accelerators/bvh.h>
int main(void){
 struct mesh m={0};
 struct vector points[]={{-1,-1,3},{1,-1,3},{0,1,3},{-1,-1,5},{1,-1,5},{0,1,5}};
 for(size_t i=0;i<6;++i)v_arr_add(m.vbuf.vertices,points[i]);
 struct vector normals[]={{-.5f,-.5f,1},{.5f,-.5f,1},{0,.5f,1}};
 for(size_t i=0;i<3;++i)v_arr_add(m.vbuf.normals,normals[i]);
 struct poly front={.vertexIndex={0,1,2},.normalIndex={0,1,2},.textureIndex={-1,-1,-1},.materialIndex=7,.hasNormals=true};
 struct poly back={.vertexIndex={3,4,5},.normalIndex={0,1,2},.textureIndex={-1,-1,-1},.materialIndex=11,.hasNormals=true};
 v_arr_add(m.polygons,front);v_arr_add(m.polygons,back);
 m.bvh=build_mesh_bvh(&m);if(!m.bvh)return 2;
 struct lightRay ray={.start={0,0,0},.direction={0,0,2},.type=rt_camera};
 struct hitRecord h={.distance=FLT_MAX,.instIndex=-1};
 if(!traverse_bottom_level_bvh(&m,&ray,&h,NULL))return 3;
 if(h.polygon!=m.polygons || h.polygon->materialIndex!=7 || fabsf(h.distance-1.5f)>1e-5f || fabsf(h.hitPoint.z-3)>1e-5f)return 4;
 struct hitRecord finite={.distance=1.4f,.instIndex=-1};
 if(traverse_bottom_level_bvh(&m,&ray,&finite,NULL) || finite.distance!=1.4f)return 5;
 struct boundingBox b=get_root_bbox(m.bvh);if(b.min.z!=3 || b.max.z!=5)return 6;
 printf("BASELINE flat_mesh faces=2 nearest_material=7 t=1.5 finite_miss=1 root_z=[3,5]\n");
 mesh_free(&m);return 0;
}
