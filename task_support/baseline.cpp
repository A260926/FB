#include "SkeletalAnimation/SkeletalModel.h"
#include "SkeletalAnimation/SkeletalModelSerializer.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
static void require(bool ok){if(!ok)std::abort();}
int main(int argc,char** argv){
 SA::SkeletalModel m;
 SA::sBone b={}; b.Name="root";b.NodeTransform=glm::translate(glm::vec3(2,0,0));b.OffsetMatrix=glm::mat4(1);b.NumWeights=1;b.pWeights=new SA::sWeight[1];b.pWeights[0]={0,1};m.GetSkeleton().Bones.push_back(b);
 SA::sAnimatedMesh mesh={};mesh.NumVertices=1;mesh.pVertices=new glm::vec3[1]{glm::vec3(1,0,0)};mesh.pNormals=new glm::vec3[1]{glm::vec3(0,1,0)};mesh.pTransformedVertices=new glm::vec3[1];mesh.pTransformedNormals=new glm::vec3[1];m.AddMesh(mesh);
 m.GetAnimation().TicksPerSecond=1;m.GetAnimation().Duration=2;m.Update(0.25f);
 const auto& out=m.GetMesh(0);require(std::abs(out.pTransformedVertices[0].x-3)<1e-5f);require(std::abs(out.pTransformedNormals[0].y-1)<1e-5f);
 std::string blob=SA::ModelToData(m);require(blob.size()>40);
 if(argc==2){FILE* f=std::fopen(argv[1],"wb");require(f!=nullptr);require(std::fwrite(blob.data(),1,blob.size(),f)==blob.size());std::fclose(f);}
 std::printf("LEGACY_SKIN_OK x=%.3f normal_y=%.3f bytes=%zu\n",out.pTransformedVertices[0].x,out.pTransformedNormals[0].y,blob.size());
}
