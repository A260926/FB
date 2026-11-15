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
int main(){
 int n=5;
 // generic positions
 std::vector<point> vs={point(0,0,0),point(1,0,0),point(0.13f,1.07f,0.21f),
   point(-0.31f,0.22f,1.13f),point(0.47f,-0.63f,0.58f)};
 // candidate oriented triangles, canonical orientation by a hash
 std::vector<TriMesh::Face> cand;
 for(int a=0;a<n;a++)for(int b=a+1;b<n;b++)for(int c=b+1;c<n;c++){
   cand.push_back(TriMesh::Face(a,b,c)); cand.push_back(TriMesh::Face(a,c,b));
 }
 // force edge (0,1) shared by several faces with various dirs
 int m=cand.size();
 // enumerate subsets containing (0,1,x) faces
 for(int t1=0;t1<m;t1++){
  TriMesh::Face f1=cand[t1];
  // require edge 0-1 in f1
  auto has01=[](TriMesh::Face f){for(int k=0;k<3;k++){int u=f[k],w=f[(k+1)%3]; if((u==0&&w==1)||(u==1&&w==0))return true;}return false;};
  if(!has01(f1))continue;
  for(int t2=t1+1;t2<m;t2++){ TriMesh::Face f2=cand[t2]; if(!has01(f2))continue;
   for(int t3=t2+1;t3<m;t3++){ TriMesh::Face f3=cand[t3]; if(!has01(f3))continue;
    // add up to two more non-01 faces
    for(int t4=t3+1;t4<=m;t4++){
     for(int t5=t4+1;t5<=m;t5++){
      std::vector<TriMesh::Face> fs={f1,f2,f3};
      if(t4<m)fs.push_back(cand[t4]); if(t5<m)fs.push_back(cand[t5]);
      if(!valid(fs))continue;
      TriMesh mesh; mesh.vertices=vs; mesh.faces=fs;
      SheetSplitResult r;
      if(!split_nonmanifold_sheets(&mesh,r)){
        printf("CONFLICT faces:");
        for(auto&f:fs)printf(" (%d,%d,%d)",f[0],f[1],f[2]);
        printf("\n");
        return 0;
      }
     }
    }
   }
  }
 }
 printf("none\n");
}
