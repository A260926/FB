#include "../test/unit/morph_frames_data.h"
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <assimp/config.h>
#include <cstdio>
#include <cstring>
int main(){
  Assimp::Importer imp;
  imp.SetPropertyBool(AI_CONFIG_PP_MORPH_FRAME_COMPLETION,true);
  const aiScene* s=imp.ReadFileFromMemory(kScenePreservation,std::strlen(kScenePreservation),0,"gltf");
  s=imp.ApplyPostProcessing(aiProcess_GenSmoothNormals|aiProcess_CalcTangentSpace);
  aiMesh* m=s->mMeshes[0];
  for(unsigned t=0;t<m->mNumAnimMeshes;++t){
    aiAnimMesh* a=m->mAnimMeshes[t];
    printf("t%u n=%d t=%d b=%d N0=(%.6f,%.6f,%.6f) T0=(%.6f,%.6f,%.6f) B0=(%.6f,%.6f,%.6f)\n",t,
      (int)a->HasNormals(),(int)(a->mTangents!=nullptr),(int)(a->mBitangents!=nullptr),
      a->mNormals?a->mNormals[0].x:0,a->mNormals?a->mNormals[0].y:0,a->mNormals?a->mNormals[0].z:0,
      a->mTangents?a->mTangents[0].x:0,a->mTangents?a->mTangents[0].y:0,a->mTangents?a->mTangents[0].z:0,
      a->mBitangents?a->mBitangents[0].x:0,a->mBitangents?a->mBitangents[0].y:0,a->mBitangents?a->mBitangents[0].z:0);
  }
  return 0;
}
