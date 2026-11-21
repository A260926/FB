#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <cstdio>
#include <cstring>
int main(){
const char* obj="o Tri\nv 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nf 1//1 2//1 3//1\n";
Assimp::Importer imp; const aiScene*s=imp.ReadFileFromMemory(obj,strlen(obj),aiProcess_Triangulate|aiProcess_GlobalScale,"obj");
if(!s){printf("null %s\n",imp.GetErrorString());return 1;}
aiMesh*m=s->mMeshes[0];
printf("verts %u normals=%p faces %u n0=(%f %f %f)\n",m->mNumVertices,(void*)m->mNormals,m->mNumFaces, m->mNormals?m->mNormals[0].x:0,m->mNormals?m->mNormals[0].y:0,m->mNormals?m->mNormals[0].z:0);
return 0;}
