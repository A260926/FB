#include "TriMesh.h"
#include "TriMesh_algo.h"
#include <cstdio>
#include <vector>
#include <algorithm>
using namespace trimesh;
static bool valid(const std::vector<TriMesh::Face>&fs){
 for(size_t i=0;i<fs.size();++i){
  int a=fs[i][0],b=fs[i][1],c=fs[i][2]; if(a==b||b==c||c==a)return false;
  int s[3]={a,b,c}; std::sort(s,s+3);
  for(size_t j=i+1;j<fs.size();++j){int t[3]={fs[j][0],fs[j][1],fs[j][2]};std::sort(t,t+3);if(s[0]==t[0]&&s[1]==t[1]&&s[2]==t[2])return false;}
 }
 return true;
}
static bool has01(TriMesh::Face f){for(int k=0;k<3;k++){int u=f[k],w=f[(k+1)%3]; if((u==0&&w==1)||(u==1&&w==0))return true;}return false;}
int main(){
 int n=4;
 std::vector<point> vs={point(0,0,0),point(1,0,0),point(0.13f,1.07f,0.21f),
   point(-0.31f,0.22f,1.13f)};
 std::vector<TriMesh::Face> cand;
 for(int a=0;a<n;a++)for(int b=a+1;b<n;b++)for(int c=b+1;c<n;c++){cand.push_back(TriMesh::Face(a,b,c));cand.push_back(TriMesh::Face(a,c,b));}
 int m=cand.size();
 for(int t1=0;t1<m;t1++){if(!has01(cand[t1]))continue;
  for(int t2=t1+1;t2<m;t2++){if(!has01(cand[t2]))continue;
   for(int t3=t2+1;t3<m;t3++){if(!has01(cand[t3]))continue;
    for(int t4=t3+1;t4<m;t4++){ std::vector<TriMesh::Face> fs={cand[t1],cand[t2],cand[t3],cand[t4]};
     if(!valid(fs))continue; TriMesh mesh;mesh.vertices=vs;mesh.faces=fs;SheetSplitResult r;
     if(!split_nonmanifold_sheets(&mesh,r)){printf("4CONFLICT:");for(auto&f:fs)printf(" (%d,%d,%d)",f[0],f[1],f[2]);printf("\n");return 0;}
    }}}}
 printf("none4\n");
}
