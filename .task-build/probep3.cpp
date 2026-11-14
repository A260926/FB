#include "../test/unit/morph_frames_data.h"
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <assimp/config.h>
#include <cstdio>
#include <cstring>
static void dump(const aiScene* s,const char* tag){
  aiMesh* m=s->mMeshes[0];
  aiAnimMesh* a=m->mAnimMeshes[0];
  printf("%s N0=(%.4f,%.4f,%.4f) T0=(%.4f,%.4f,%.4f) B0=(%.4f,%.4f,%.4f) tptr=%p bptr=%p\n",tag,
    a->mNormals[0].x,a->mNormals[0].y,a->mNormals[0].z,
    a->mTangents[0].x,a->mTangents[0].y,a->mTangents[0].z,
    a->mBitangents[0].x,a->mBitangents[0].y,a->mBitangents[0].z,(void*)a->mTangents,(void*)a->mBitangents);
}
int main(){
  Assimp::Importer imp;
  imp.SetPropertyBool(AI_CONFIG_PP_MORPH_FRAME_COMPLETION,true);
  const aiScene* s=imp.ReadFileFromMemory(kScenePreservation,std::strlen(kScenePreservation),0,"gltf");
  dump(s,"import      ");
  s=imp.ApplyPostProcessing(aiProcess_GenSmoothNormals);
  dump(s,"after normals");
  s=imp.ApplyPostProcessing(aiProcess_CalcTangentSpace);
  dump(s,"after tang  ");
  return 0;
}
