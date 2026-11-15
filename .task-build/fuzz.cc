#include "TriMesh.h"
#include "TriMesh_algo.h"
#include <cstdio>
#include <cmath>
#include <random>
#include <vector>
#include <set>
#include <map>
#include <algorithm>
using namespace trimesh;
using namespace std;

static bool verify(TriMesh &in,const SheetSplitResult&r,const vector<point>&ov,const vector<TriMesh::Face>&of){
 if(r.vertex_source.size()!=in.vertices.size())return false;
 if(r.edge_partner.size()!=of.size())return false;
 if(in.faces.size()!=of.size())return false;
 for(size_t i=0;i<ov.size();i++){int s=r.vertex_source[i];if(s<0||s>=(int)ov.size())return false;
   if(fabs(in.vertices[i][0]-ov[s][0])>1e-6||fabs(in.vertices[i][1]-ov[s][1])>1e-6||fabs(in.vertices[i][2]-ov[s][2])>1e-6)return false;}
 for(size_t f=0;f<of.size();f++)for(int k=0;k<3;k++)if(r.vertex_source[of[f][k]]!=of[f][k]?false:false){}
 for(size_t f=0;f<of.size();f++)for(int k=0;k<3;k++)if(r.vertex_source[in.faces[f][k]]!=of[f][k])return false;
 // edge occurrences
 map<pair<int,int>,vector<pair<int,int>>> occ; // key -> (face,corner)
 for(size_t f=0;f<in.faces.size();f++)for(int k=0;k<3;k++){
   int u=in.faces[f][NEXT_MOD3(k)],w=in.faces[f][PREV_MOD3(k)];
   occ[{min(u,w),max(u,w)}].push_back({(int)f,k});
 }
 for(auto&kv:occ){
   auto&os=kv.second;
   if(os.size()==1){auto&o=os[0];if(r.edge_partner[o.first][o.second]!=-1)return false;}
   else if(os.size()==2){
     int f0=os[0].first,k0=os[0].second,f1=os[1].first,k1=os[1].second;
     int u0=in.faces[f0][NEXT_MOD3(k0)],w0=in.faces[f0][PREV_MOD3(k0)];
     int u1=in.faces[f1][NEXT_MOD3(k1)],w1=in.faces[f1][PREV_MOD3(k1)];
     if(r.edge_partner[f0][k0]!=-1||r.edge_partner[f1][k1]!=-1){
       if(!(u0==w1&&w0==u1))return false;
       if(r.edge_partner[f0][k0]!=f1||r.edge_partner[f1][k1]!=f0)return false;
     } else {
       // both boundary: must be same direction (opposite would have paired)
       if(u0!=w1)return false;
     }
   } else return false;
 }
 // manifold: each output vertex link single chain/cycle => across_edge recip and each vertex fan count 1
 in.need_across_edge();
 for(size_t f=0;f<in.faces.size();f++)for(int k=0;k<3;k++){
   int g=in.across_edge[f][k];if(g<0)continue;
   int u=in.faces[f][NEXT_MOD3(k)],w=in.faces[f][PREV_MOD3(k)];
   int kk=-1;for(int x=0;x<3;x++)if(in.faces[g][NEXT_MOD3(x)]==w&&in.faces[g][PREV_MOD3(x)]==u)kk=x;
   if(kk<0||in.across_edge[g][kk]!=(int)f)return false;
 }
 return true;
}

int main(){
 mt19937 rng(987654321);
 int nv=8;
 for(int trial=0;trial<20000;trial++){
  uniform_int_distribution<int> flip(0,1);
  TriMesh m;
  m.vertices={point(0,0,0),point(1,0,0),point(2,0,0),point(0,1,0),point(1,1,0.3f),point(2,1,0),point(0.5f,2,0.7f),point(1.5f,2,-0.4f)};
  // random triangles from vertex set, avoiding dup faces
  int nf=1+int(rng()%6);
  set<array<int,3>> seen;
  for(int f=0;f<nf;f++){
    int a=rng()%nv,b=rng()%nv,c=rng()%nv;
    if(a==b||b==c||c==a){f--;continue;}
    array<int,3> s={a,b,c};sort(s.begin(),s.end());
    if(seen.count(s)){f--;continue;}
    seen.insert(s);
    if(flip(rng))swap(b,c);
    // ensure nonzero area
    vec n=trinorm(m.vertices[a],m.vertices[b],m.vertices[c]);
    if(len(n)<1e-8){f--;continue;}
    m.faces.push_back(TriMesh::Face(a,b,c));
  }
  // optionally add duplicate faces in reversed winding? duplicates forbidden; skip
  TriMesh orig=m; vector<point> ov=m.vertices; vector<TriMesh::Face> of=m.faces;
  SheetSplitResult r;
  bool ok=split_nonmanifold_sheets(&m,r);
  if(!ok){
    // transactional
    if(m.vertices.size()!=ov.size()||m.faces.size()!=of.size()){printf("TXN FAIL trial %d\n",trial);return 1;}
    for(size_t i=0;i<ov.size();i++)if(memcmp(&m.vertices[i][0],&ov[i][0],12)){printf("TXN V FAIL\n");return 1;}
    continue;
  }
  if(!verify(m,r,ov,of)){
    printf("VERIFY FAIL trial %d\n",trial);
    for(auto&fc:of)printf(" (%d,%d,%d)",fc[0],fc[1],fc[2]);printf("\n");
    for(auto&fcz:m.faces)printf(" out (%d,%d,%d)\n",fcz[0],fcz[1],fcz[2]);
    return 1;
  }
 }
 printf("FUZZ OK\n");
}
