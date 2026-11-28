// Fixed API consumer: checks identity data through the future production API.
#include <cal3d/volumeskin.h>
#include <cal3d/corebone.h>
#include <cal3d/coreskeleton.h>
#include <cal3d/skeleton.h>
#include <cal3d/submesh.h>
#include <cmath>
#include <cstdio>
int main(){
 auto cs=std::make_shared<CalCoreSkeleton>();cs->addCoreBone(std::make_shared<CalCoreBone>("root"));CalSkeleton sk(cs);sk.resetPose();sk.calculateAbsolutePose();
 auto mesh=std::make_shared<CalCoreSubmesh>(3,false,1);
 CalVolumeCage c;c.rest={CalVector(0,0,0),CalVector(1,0,0),CalVector(0,1,0),CalVector(0,0,1)};
 for(auto p:c.rest)c.weights.push_back({{0,1,true}});
 for(size_t i=0;i<3;i++){auto p=c.rest[i];mesh->addVertex({CalPoint4(p.x,p.y,p.z),CalVector4(0,0,1)},0xffffffff,{{0,1,true}});}
 mesh->addFace({0,1,2});c.cells.push_back({0,1,2,3});c.surface_cells={0,0,0};c.barycentric={{{1,0,0,0}},{{0,1,0,0}},{{0,0,1,0}}};
 CalVolumeBinding b;std::string error;if(!b.bind(mesh,1,c,error))return 31;CalSubmesh sm(mesh);CalVolumeOptions opts;CalVolumeReport report;
 alignas(16) float out[24]={};if(!CalPhysique::calculateVolumeVerticesAndNormals(sk,sm,b,opts,out,24,report,error))return 32;
 if(std::fabs(out[8]-1)>1e-5||std::fabs(out[17]-1)>1e-5||report.cage_positions.size()!=4||report.final_energy>1e-8)return 33;
 puts("VOLUME_API_IDENTITY_OK");
}
