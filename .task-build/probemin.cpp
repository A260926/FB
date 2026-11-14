#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <fstream>
#include <sstream>
#include <cstdio>
int main(){
  std::ifstream f(".task-build/min_tan.gltf");std::stringstream ss;ss<<f.rdbuf();std::string j=ss.str();
  Assimp::Importer imp;
  const aiScene* s=imp.ReadFileFromMemory(j.data(),j.size(),0,"gltf");
  if(!s){printf("ERR\n");return 1;}
  aiMesh* m=s->mMeshes[0];
  for(unsigned v=0;v<m->mNumVertices;++v)
    printf("v%u T=(%.2f,%.2f,%.2f)\n",v,m->mTangents[v].x,m->mTangents[v].y,m->mTangents[v].z);
  return 0;
}
