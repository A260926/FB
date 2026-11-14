#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <fstream>
#include <sstream>
#include <cstdio>
int main(){
  std::ifstream f(".task-build/min6.gltf");std::stringstream ss;ss<<f.rdbuf();std::string j=ss.str();
  Assimp::Importer imp;
  const aiScene* s=imp.ReadFileFromMemory(j.data(),j.size(),0,"gltf");
  if(!s){printf("ERR %s\n",imp.GetErrorString());return 1;}
  aiMesh* m=s->mMeshes[0];
  printf("verts=%u faces=%u\n",m->mNumVertices,m->mNumFaces);
  for(unsigned v=0;v<m->mNumVertices;++v)
    printf("v%u P=(%.0f,%.0f,%.0f) T=(%.2f,%.2f,%.2f)\n",v,m->mVertices[v].x,m->mVertices[v].y,m->mVertices[v].z,
      m->mTangents[v].x,m->mTangents[v].y,m->mTangents[v].z);
  return 0;
}
