#include <assimp/Importer.hpp>
#include <assimp/Exporter.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <cmath>
#include <cstdio>
#include <cstring>
static const char input[] = R"gltf({"asset":{"version":"2.0"},"buffers":[{"byteLength":96,"uri":"data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAAAAAAAAAAAAAAAAAAAAAAIA/AAAAAAAAAAAAAAAA"}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":24},{"buffer":0,"byteOffset":60,"byteLength":36}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},{"bufferView":1,"componentType":5126,"count":3,"type":"VEC2"},{"bufferView":2,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[0,0,1]}],"meshes":[{"primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":1},"mode":4,"targets":[{"POSITION":2}]}],"weights":[0]}],"nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0})gltf";
int main() {
 Assimp::Importer importer;
 const aiScene *s=importer.ReadFileFromMemory(input,strlen(input),0,"gltf");
 if(!s||s->mNumMeshes!=1){std::fprintf(stderr,"import: %s\n",importer.GetErrorString());return 1;}
 s=importer.ApplyPostProcessing(aiProcess_GenSmoothNormals|aiProcess_CalcTangentSpace);
 if(!s){std::fprintf(stderr,"postprocess: %s\n",importer.GetErrorString());return 2;}
 const aiMesh *m=s->mMeshes[0];
 if(!m->HasNormals()||!m->HasTangentsAndBitangents()||m->mNumAnimMeshes!=1)return 3;
 if(std::abs(m->mNormals[0].z-1)>1e-5f||std::abs(m->mTangents[0].x-1)>1e-5f)return 4;
 auto *a=m->mAnimMeshes[0];
 if(!a->HasPositions()||a->mNumVertices!=3||std::abs(a->mVertices[1].z-1)>1e-6f)return 5;
 // The new option remains unset: legacy omission is the current supported behavior.
 if(a->mNormals||a->mTangents||a->mBitangents)return 6;
 Assimp::Exporter exporter;
 if(exporter.Export(s,"gltf2",".task-build/baseline.gltf")!=aiReturn_SUCCESS){std::fprintf(stderr,"export: %s\n",exporter.GetErrorString());return 7;}
 Assimp::Importer again;const aiScene *r=again.ReadFile(".task-build/baseline.gltf",0);
 if(!r||!r->mMeshes[0]->HasNormals()||!r->mMeshes[0]->HasTangentsAndBitangents()||r->mMeshes[0]->mNumAnimMeshes!=1)return 8;
 std::printf("BASELINE_OK base_normal_z=%.6f base_tangent_x=%.6f morph_normal=0 morph_tangent=0 gltf_roundtrip=1\n",m->mNormals[0].z,m->mTangents[0].x);
 return 0;
}
