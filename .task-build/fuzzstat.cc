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
 mt19937 rng(987654321);int nv=8;int okc=0,rejc=0;
 for(int trial=0;trial<20000;trial++){
  TriMesh m;m.vertices={point(0,0,0),point(1,0,0),point(2,0,0),point(0,1,0),point(1,1,0.3f),point(2,1,0),point(0.5f,2,0.7f),point(1.5f,2,-0.4f)};
  int nf=1+int(rng()%6);set<array<int,3>> seen;
  for(int f=0;f<nf;f++){int a=rng()%nv,b=rng()%nv,c=rng()%nv;if(a==b||b==c||c==a){f--;continue;}
   array<int,3> s={a,b,c};sort(s.begin(),s.end());if(seen.count(s)){f--;continue;}seen.insert(s);
   if(rng()%2)swap(b,c);vec n=trinorm(m.vertices[a],m.vertices[b],m.vertices[c]);if(len(n)<1e-8){f--;continue;}
   m.faces.push_back(TriMesh::Face(a,b,c));}
  SheetSplitResult r; if(split_nonmanifold_sheets(&m,r))okc++;else rejc++;
 }
 printf("ok=%d rejected=%d\n",okc,rejc);
}
