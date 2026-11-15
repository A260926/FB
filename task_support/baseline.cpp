#include <assimp/Importer.hpp>
#include <assimp/Exporter.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <cmath>
#include <iostream>
static bool point(const aiVector3D& p, float x, float y) {
 return std::fabs(p.x-x)<1e-6f && std::fabs(p.y-y)<1e-6f && std::fabs(p.z)<1e-6f;
}
static unsigned faces(const aiScene* s) {
 unsigned result=0;for(unsigned j=0;j<s->mNumMeshes;++j)result+=s->mMeshes[j]->mNumFaces;return result;
}
static bool hasLongEdge(const aiScene* s) {
 for(unsigned j=0;j<s->mNumMeshes;++j){auto m=s->mMeshes[j];
 for(unsigned f=0;f<m->mNumFaces;++f){auto& a=m->mFaces[f];
 for(unsigned k=0;k<a.mNumIndices;++k){auto p=m->mVertices[a.mIndices[k]],q=m->mVertices[a.mIndices[(k+1)%a.mNumIndices]];
 if((point(p,0,0)&&point(q,2,0))||(point(p,2,0)&&point(q,0,0)))return true;}}}return false;
}
int main() {
 Assimp::Importer in;
 const aiScene* s=in.ReadFile("task_support/tjunction.obj",aiProcess_Triangulate|aiProcess_JoinIdenticalVertices);
 if(!s){std::cerr<<in.GetErrorString()<<'\n';return 1;}
 if(faces(s)!=3 || !hasLongEdge(s)) return 2;
 for(unsigned i=0;i<s->mNumMeshes;++i)if(!s->mMeshes[i]->HasTextureCoords(0)||!s->mMeshes[i]->HasNormals())return 3;
 Assimp::Exporter out;auto blob=out.ExportToBlob(s,"obj");if(!blob || !blob->size)return 4;
 Assimp::Importer again;const aiScene* copy=again.ReadFileFromMemory(blob->data,blob->size,aiProcess_Triangulate,"obj");
 if(!copy || faces(copy)!=3 || !hasLongEdge(copy))return 5;
 std::cout<<"DEFAULT_TRIANGULATE faces="<<faces(s)<<" long_edge=1\nOBJ_ROUNDTRIP faces="<<faces(copy)<<"\nASSIMP_TJ_BASELINE_OK\n";
}
