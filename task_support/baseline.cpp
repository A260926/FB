#include "minipbrt.h"
#include <cstdio>
#include <fstream>
#include <cstring>
#include <cmath>
static int fail(const char* message){std::fprintf(stderr,"BASELINE_FAIL %s\n",message);return 1;}
int main(int argc,char**argv){
 const bool gap=argc==2 && std::strcmp(argv[1],"--gap")==0;
 const char* file=".task-build/single.pbrt";
 {std::ofstream f(file);f<<"TransformTimes 0 1\nWorldBegin\nObjectBegin \"tri\"\nShape \"trianglemesh\" \"integer indices\" [0 1 2] \"point P\" [0 0 0 1 0 0 0 1 0]\nObjectEnd\nTranslate 3 4 5\nObjectInstance \"tri\"\nWorldEnd\n";if(!f)return fail("fixture write");}
 minipbrt::Loader loader;
 if(!loader.load(file))return fail("single layer load");
 minipbrt::Scene* s=loader.borrow_scene();
 if(!s || s->objects.size()!=1 || s->instances.size()!=1 || s->shapes.size()!=1)return fail("shape/object/instance counts");
 if(s->shapes[0]->type()!=minipbrt::ShapeType::TriangleMesh)return fail("mesh type");
 const minipbrt::TriangleMesh* m=static_cast<const minipbrt::TriangleMesh*>(s->shapes[0]);
 if(m->num_vertices!=3 || m->num_indices!=3 || m->indices[2]!=2 || m->P[3]!=1)return fail("production mesh arrays");
 if(s->objects[0]->firstShape!=0 || s->objects[0]->numShapes!=1 || s->instances[0]->object!=0 || s->shapes[0]->object!=0)return fail("identity");
 const minipbrt::Transform& t=s->instances[0]->instanceToWorld;
 if(t.start[0][3]!=3 || t.start[1][3]!=4 || t.start[2][3]!=5 || t.end[2][3]!=5)return fail("instance placement");
 // A world-space vertex observation on the legacy public data path.
 double x=t.start[0][0]*m->P[3]+t.start[0][1]*m->P[4]+t.start[0][2]*m->P[5]+t.start[0][3];
 if(std::fabs(x-4)>1e-6)return fail("world vertex");
 std::printf("LEGACY mesh_vertices=3 objects=1 instances=1 world_vertex_x=%.1f PASS\n",x);
 if(gap){
  const char* nested=".task-build/nested.pbrt";
  {std::ofstream f(nested);f<<"WorldBegin\nObjectBegin \"leaf\"\nShape \"trianglemesh\" \"integer indices\" [0 1 2] \"point P\" [0 0 0 1 0 0 0 1 0]\nObjectEnd\nObjectBegin \"parent\"\nObjectInstance \"leaf\"\nObjectEnd\nObjectInstance \"parent\"\nWorldEnd\n";if(!f)return fail("nested fixture write");}
  minipbrt::Loader n;
  if(n.load(nested) || !n.error())return fail("expected starting capability gap");
  if(!std::strstr(n.error()->message(),"Nested instances are not allowed"))return fail("unexpected parse failure");
  std::printf("STARTING_GAP nested_instance_rejected line=%lld message=%s\n",(long long)n.error()->line(),n.error()->message());
 }
 return 0;
}
