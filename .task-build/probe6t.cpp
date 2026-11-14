#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <fstream>
#include <sstream>
#include <cstdio>
int main(){
  std::ifstream f(".task-build/min6t.gltf");std::stringstream ss;ss<<f.rdbuf();std::string j=ss.str();
  Assimp::Importer imp;
  const aiScene* s=imp.ReadFileFromMemory(j.data(),j.size(),0,"gltf");
  if(!s){printf("ERR %s\n",imp.GetErrorString());return 1;}
  aiMesh* m=s->mMeshes[0];
  printf("verts=%u anim=%u\n",m->mNumVertices,m->mNumAnimMeshes);
  for(unsigned v=0;v<m->mNumVertices;++v)
    printf("base v%u T=(%.2f,%.2f,%.2f)\n",v,m->mTangents[v].x,m->mTangents[v].y,m->mTangents[v].z);
  aiAnimMesh* a=m->mAnimMeshes[0];
  for(unsigned v=0;v<m->mNumVertices;++v)
    printf("targ v%u T=(%.2f,%.2f,%.2f)\n",v,a->mTangents[v].x,a->mTangents[v].y,a->mTangents[v].z);
  return 0;
}
