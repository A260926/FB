#include "../test/unit/morph_frames_data.h"
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <cstdio>
#include <cstring>
int main(){
  Assimp::Importer imp;
  const aiScene* s=imp.ReadFileFromMemory(kScenePreservation,std::strlen(kScenePreservation),0,"gltf");
  aiMesh* m=s->mMeshes[0];
  printf("verts=%u\n",m->mNumVertices);
  for(unsigned v=0;v<m->mNumVertices;++v)
    printf("base v%u T=(%.4f,%.4f,%.4f)\n",v,m->mTangents[v].x,m->mTangents[v].y,m->mTangents[v].z);
  aiAnimMesh* a=m->mAnimMeshes[0];
  for(unsigned v=0;v<m->mNumVertices;++v)
    printf("target0 v%u T=(%.4f,%.4f,%.4f)\n",v,a->mTangents[v].x,a->mTangents[v].y,a->mTangents[v].z);
  return 0;
}
