#include "TriMesh.h"
#include "TriMesh_algo.h"
#include <cstdio>
#include <cmath>
#include <random>
#include <vector>
#include <set>
#include <map>
#include <algorithm>
#include <array>
using namespace trimesh;
using namespace std;
int main(){
 mt19937 rng(555);
 for(int trial=0;trial<5000;trial++){
  int nv=10; TriMesh m;
  for(int i=0;i<nv;i++)m.vertices.push_back(point((float)(rng()%200-100)/50.0f,(float)(rng()%200-100)/50.0f,(float)(rng()%200-100)/50.0f));
  for(int i=0;i<nv;i++){m.colors.push_back(Color(float(i)/nv,1.0f-float(i)/nv,0.5f));m.confidences.push_back(float(i));m.flags.push_back(unsigned(7*i));}
  m.flag_curr=13;
  int nf=1+int(rng()%10);set<array<int,3>>seen;
  for(int f=0;f<nf;f++){int a=rng()%nv,b=rng()%nv,c=rng()%nv;if(a==b||b==c||c==a){f--;continue;}
   array<int,3>s={a,b,c};sort(s.begin(),s.end());if(seen.count(s)){f--;continue;}
   vec n=trinorm(m.vertices[a],m.vertices[b],m.vertices[c]);if(len(n)*0.5f<1e-8){f--;continue;}
   seen.insert(s); if(rng()%2)swap(b,c); m.faces.push_back(TriMesh::Face(a,b,c));}
  TriMesh orig=m;SheetSplitResult r;
  if(!split_nonmanifold_sheets(&m,r))continue;
  for(size_t i=0;i<m.vertices.size();i++){
    int s=r.vertex_source[i];
    if(m.colors[i]!=orig.colors[s]||m.confidences[i]!=orig.confidences[s]||m.flags[i]!=orig.flags[s]){printf("ATTR FAIL\n");return 1;}
  }
  if(m.flag_curr!=13){printf("FLAG_CURR FAIL\n");return 1;}
  if(!m.normals.empty()||!m.pointareas.empty()||!m.neighbors.empty()||!m.adjacentfaces.empty()||!m.across_edge.empty()){printf("CLEAR FAIL\n");return 1;}
  m.need_normals();m.need_across_edge();m.need_pointareas();m.need_neighbors();m.need_adjacentfaces();
  if(m.normals.size()!=m.vertices.size()||m.across_edge.size()!=m.faces.size()){printf("REBUILD FAIL\n");return 1;}
  // all finite normals
  for(auto&n:m.normals)if(!std::isfinite(n[0])||!std::isfinite(n[1])||!std::isfinite(n[2])){printf("NAN NORMAL\n");return 1;}
 }
 printf("FUZZ2 OK\n");
}
