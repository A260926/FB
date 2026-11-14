#include "../test/unit/morph_frames_data.h"
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <cstdio>
#include <cstring>
int main(){
  Assimp::Importer imp;
  const aiScene* s=imp.ReadFileFromMemory(kScenePreservation,std::strlen(kScenePreservation),0,"gltf");
  aiMesh* m=s->mMeshes[0];
  printf("verts=%u faces=%u\n",m->mNumVertices,m->mNumFaces);
  for(unsigned f=0;f<m->mNumFaces;++f){
    printf("face%u n=%d:",f,m->mFaces[f].mNumIndices);
    for(unsigned k=0;k<m->mFaces[f].mNumIndices;++k)printf(" %u",m->mFaces[f].mIndices[k]);
    printf("\n");
  }
  for(unsigned v=0;v<m->mNumVertices;++v)
    printf("v%u P=(%.1f,%.1f,%.1f) N=(%.2f,%.2f,%.2f) T=(%.2f,%.2f,%.2f)\n",v,
      m->mVertices[v].x,m->mVertices[v].y,m->mVertices[v].z,
      m->mNormals[v].x,m->mNormals[v].y,m->mNormals[v].z,
      m->mTangents[v].x,m->mTangents[v].y,m->mTangents[v].z);
  return 0;
}
