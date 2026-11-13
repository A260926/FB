#include "octree.h"
#include <cassert>
#include <cstdio>
int main(){auto*r=BuildOctree(ivec3(-16,-4,-16),32,-1.f);VertexBuffer v;IndexBuffer i;GenerateMeshFromOctree(r,v,i);assert(!v.empty()&&!i.empty()&&i.size()%3==0);for(int k:i)assert(k>=0&&size_t(k)<v.size());printf("DUAL vertices=%zu triangles=%zu\n",v.size(),i.size()/3);DestroyOctree(r);}
