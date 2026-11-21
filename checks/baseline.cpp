#include "MaskedOcclusionCulling.h"
#include "CullingThreadpool.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
static void need(bool b) { if (!b) std::exit(2); }
int main() {
 using M=MaskedOcclusionCulling;
 M *m=M::Create(M::SSE41), *reference=M::Create(M::SSE41);
 m->SetResolution(128,64);reference->SetResolution(128,64);
 m->SetNearClipPlane(.1f);reference->SetNearClipPlane(.1f);
 m->ClearBuffer();reference->ClearBuffer();
 float vertices[]={-1,-1,0,1, 1,-1,0,1, 1,1,0,1, -1,1,0,1};
 unsigned indices[]={0,1,2,0,2,3};
 {
  CullingThreadpool pool(2,2,2,4);pool.SetBuffer(m);pool.WakeThreads();
  need(pool.TestRect(-.25f,-.25f,.25f,.25f,2)==M::VISIBLE);
  pool.RenderTriangles(vertices,indices,2,M::BACKFACE_NONE);pool.Flush();
  need(pool.TestRect(-.25f,-.25f,.25f,.25f,2)==M::OCCLUDED);
  need(pool.TestRect(-.25f,-.25f,.25f,.25f,1)==M::VISIBLE);
  reference->RenderTriangles(vertices,indices,2,nullptr,M::BACKFACE_NONE);
  std::vector<float> a(128*64),b(a.size());pool.ComputePixelDepthBuffer(a.data(),false);
  reference->ComputePixelDepthBuffer(b.data(),false);need(a==b);
  pool.ClearBuffer();need(pool.TestRect(-.25f,-.25f,.25f,.25f,2)==M::VISIBLE);
  pool.SuspendThreads();
 }
 M::Destroy(m);M::Destroy(reference);
 std::puts("MOC_BASELINE_OK bins=4 workers=2 prefix=visible,occluded equal=visible depth_match=8192 clear=visible");
}
