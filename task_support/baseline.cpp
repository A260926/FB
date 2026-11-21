#include "PostProcessing/ScaleProcess.h"
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/config.h>
#include <assimp/postprocess.h>
#include <cstdio>
#include <cmath>
#include <cstring>
static int fail(const char*s){std::fprintf(stderr,"BASELINE_FAIL %s\n",s);return 1;}
int main(int argc,char**argv){
 bool gap=argc==2 && std::strcmp(argv[1],"--gap")==0;
 aiScene scene;scene.mRootNode=new aiNode("root");
 scene.mNumMeshes=1;scene.mMeshes=new aiMesh*[1]{new aiMesh};
 aiMesh& mesh=*scene.mMeshes[0];mesh.mNumVertices=3;mesh.mVertices=new aiVector3D[3]{aiVector3D(1,0,0),aiVector3D(0,1,0),aiVector3D(0,0,1)};
 mesh.mPrimitiveTypes=aiPrimitiveType_TRIANGLE;mesh.mNumFaces=1;mesh.mFaces=new aiFace[1];mesh.mFaces[0].mNumIndices=3;mesh.mFaces[0].mIndices=new unsigned[3]{0,1,2};
 scene.mRootNode->mNumMeshes=1;scene.mRootNode->mMeshes=new unsigned[1]{0};
 scene.mRootNode->mTransformation.a4=3;
 mesh.mNumBones=1;mesh.mBones=new aiBone*[1]{new aiBone};mesh.mBones[0]->mName.Set("root");mesh.mBones[0]->mOffsetMatrix.b4=4;
 Assimp::Importer properties;properties.SetPropertyFloat(AI_CONFIG_GLOBAL_SCALE_FACTOR_KEY,2.f);properties.SetPropertyFloat(AI_CONFIG_APP_SCALE_KEY,1.5f);
 Assimp::ScaleProcess proc;proc.SetupProperties(&properties);proc.Execute(&scene);
 if(mesh.mVertices[0].x!=3 || scene.mRootNode->mTransformation.a4!=9 || mesh.mBones[0]->mOffsetMatrix.b4!=12)return fail("production scalar mesh/node/bone scaling");
 std::printf("LEGACY scale=3 vertex_x=3 root_x=9 bone_y=12 PASS\n");
 const char* obj="v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
 Assimp::Importer importer;importer.SetPropertyFloat(AI_CONFIG_GLOBAL_SCALE_FACTOR_KEY,2.f);
 const aiScene* s=importer.ReadFileFromMemory(obj,std::strlen(obj),aiProcess_Triangulate|aiProcess_GlobalScale,"obj");
 if(!s || s->mNumMeshes!=1 || s->mMeshes[0]->mNumFaces!=1 || s->mMeshes[0]->mNumVertices!=3)return fail("OBJ import");
 bool two=false;for(unsigned i=0;i<3;i++)if(s->mMeshes[0]->mVertices[i].x==2)two=true;
 if(!two)return fail("Importer scale setting");
 std::printf("LEGACY OBJ vertices=3 faces=1 scaled_x=2 PASS\n");
 if(gap){
  Assimp::Importer ignored;ignored.SetPropertyBool("GLOBAL_SCALE_BASIS_ENABLED",true);
  aiMatrix4x4 u;u.a2=0.5f;u.a4=7.f;ignored.SetPropertyMatrix("GLOBAL_SCALE_BASIS",u);
  Assimp::ScaleProcess old;old.SetupProperties(&ignored);old.Execute(&scene);
  if(mesh.mVertices[0].x!=3 || scene.mRootNode->mTransformation.a4!=9)return fail("starting gap observation");
  std::printf("STARTING_GAP arbitrary_basis_property_ignored vertex_x=3 PASS\n");
 }
 return 0;
}
