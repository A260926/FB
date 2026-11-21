#include <assimp/scene.h>
#include <assimp/anim.h>
#include <assimp/mesh.h>
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>
// replicate rich fixture minimal checks
int main(){
  aiScene s;
  aiNode*root=new aiNode("Root");
  aiNode*hip=new aiNode("Hip");
  s.mRootNode=root;
  root->mNumChildren=1; root->mChildren=new aiNode*[1]{hip}; hip->mParent=root;
  s.mNumMeshes=1; s.mMeshes=new aiMesh*[1]{new aiMesh()};
  aiMesh*m=s.mMeshes[0];
  m->mNumVertices=1; m->mVertices=new aiVector3D[1]{aiVector3D(0,0,0)};
  m->mNumBones=1; m->mBones=new aiBone*[1]{new aiBone()};
  m->mBones[0]->mName="Hip";
  m->mBones[0]->mNumWeights=1; m->mBones[0]->mWeights=new aiVertexWeight[1];
  m->mBones[0]->mWeights[0].mVertexId=0; m->mBones[0]->mWeights[0].mWeight=1.0f;
  s.mNumAnimations=1; s.mAnimations=new aiAnimation*[1]{new aiAnimation()};
  s.mAnimations[0]->mNumChannels=1; s.mAnimations[0]->mChannels=new aiNodeAnim*[1]{new aiNodeAnim()};
  aiNodeAnim&c=*s.mAnimations[0]->mChannels[0]; c.mNodeName="Hip";
  c.mNumPositionKeys=1; c.mPositionKeys=new aiVectorKey[1]{aiVectorKey(0,aiVector3D(0,0,0))};
  c.mNumRotationKeys=1; c.mRotationKeys=new aiQuatKey[1]{aiQuatKey(0,aiQuaternion())};
  c.mNumScalingKeys=1; c.mScalingKeys=new aiVectorKey[1]{aiVectorKey(0,aiVector3D(1,1,1))};
  std::printf("mesh counts %u %u morph %u bones %u\n", m->mNumVertices, m->mNumFaces, m->mNumAnimMeshes, m->mNumBones);
  std::printf("bone name %s weights %u vid %u w %f\n", m->mBones[0]->mName.C_Str(), m->mBones[0]->mNumWeights, m->mBones[0]->mWeights[0].mVertexId, m->mBones[0]->mWeights[0].mWeight);
  return 0;
}
