#include "storage.h"
#include "raytracing.h"
#include <array>
#include <cstdio>
#include <cmath>
using namespace Cubiquity;
template<class T> concept TranslatedCombine = requires(T& dst,const T& src,const std::array<i64,3>& p,const MaterialCombiner& f) {dst.combineTranslated(src,p,p,p,f);};
int main(){
 Volume dst,src;dst.setVoxel(2,3,4,3);src.setVoxel(2,3,4,5);src.setVoxel(5,3,4,7);
 dst.combine(src,[](MaterialId a,MaterialId b){return b?b:a;});
 if(dst.voxel(2,3,4)!=5||dst.voxel(5,3,4)!=7||dst.voxel(2,3,5)!=0||src.voxel(2,3,4)!=5)return 2;
 auto sub=findSubDAGs(dst.mNodeStore,dst.rootNodeIndex());
 auto hit=intersectVolume(dst,sub,5.25f,3.25f,0.25f,0,0,1,true);
 if(!hit.hit||hit.material!=7||std::abs(hit.distance-3.25)>1e-5){std::fprintf(stderr,"ray hit=%d mat=%u distance=%g\n",hit.hit,hit.material,hit.distance);return 3;}
 std::printf("ALIGNED_COMBINE_OK materials=5,7 source=5\nRAY_OK material=%u distance=%.2f\n",hit.material,hit.distance);
 std::printf("TRANSLATED_COMBINE_AVAILABLE=%d\n",int(TranslatedCombine<Volume>));
 return 0;
}
