#include "SkeletalAnimation/SkeletalModel.h"
#include <cstdio>
#include <cmath>
using namespace SA;
static bool close(const glm::vec3&a,const glm::vec3&b){return glm::length(a-b)<2e-5f;}
int main(){
 SkeletalModel m; sSkeleton& s=m.GetSkeleton(); s.Bones.resize(2);
 for(unsigned i=0;i<2;++i){sBone& b=s.Bones[i];b.Name=i?"child":"root";b.NodeTransform=glm::mat4(1);b.OffsetMatrix=glm::mat4(1);b.FinalTransformation=glm::mat4(1);b.NumChildren=0;b.pChildren=NULL;b.NumWeights=0;b.pWeights=NULL;}
 s.Bones[0].NumChildren=1;s.Bones[0].pChildren=new unsigned[1]{1};
 s.Bones[1].NodeTransform=glm::translate(glm::vec3(0,2,0));
 s.Bones[1].OffsetMatrix=glm::translate(glm::vec3(0,-2,0));
 s.Bones[1].NumWeights=1;s.Bones[1].pWeights=new sWeight[1]{{0,1}};
 const glm::vec3 root(1,0,0),p(2,1,0),n(0,1,0);
 sAnimatedMesh mesh={};mesh.NumVertices=1;mesh.pVertices=new glm::vec3[1]{p};mesh.pNormals=new glm::vec3[1]{n};mesh.pTransformedVertices=new glm::vec3[1];mesh.pTransformedNormals=new glm::vec3[1];m.AddMesh(mesh);
 sAnimation& a=m.GetAnimation();a.Duration=2;a.TicksPerSecond=1;
 sNodeAnimation channel;channel.Name="root";channel.PositionKeys.push_back({root,0});channel.PositionKeys.push_back({root+glm::vec3(2,0,0),2});
 channel.RotationKeys.push_back({glm::quat(1,0,0,0),0});channel.RotationKeys.push_back({glm::angleAxis(90.0f,glm::vec3(0,0,1)),2});a.NodeAnimations.push_back(channel);
 m.SetGlobalInverseTransform(glm::translate(glm::vec3(-.5f,0,0)));
 m.Update(.5f);
 glm::mat4 expected=m.GetGlobalInverseTransform()*glm::translate(glm::vec3(1.5f,0,0))*glm::toMat4(glm::angleAxis(22.5f,glm::vec3(0,0,1)));
 const glm::vec3 want=glm::vec3(expected*glm::vec4(p,1));
 if(!close(m.GetMesh(0).pTransformedVertices[0],want)||!close(m.GetMesh(0).pTransformedNormals[0],glm::normalize(glm::mat3(expected)*n)))return 2;
 m.Update(0);if(!close(m.GetMesh(0).pTransformedVertices[0],want))return 3;
 std::printf("BASELINE_FK_SKIN_OK joints=2 animated_parent=1 inverse_bind=1 zero_step=1\n");
 m.Clear();if(m.GetNumMeshes()!=0||!m.GetSkeleton().Bones.empty())return 4;
 std::puts("BASELINE_LIFECYCLE_OK");
}
