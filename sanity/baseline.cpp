#include "src.cmd/Simplify.h"
#include <cstdio>
#include <set>
#include <array>
#include <algorithm>
int main(){
 using namespace Simplify;
 const vec3f p[]={{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
 for(auto a:p){Vertex v={};v.p=a;vertices.push_back(v);}
 const int f[][3]={{0,2,4},{2,1,4},{1,3,4},{3,0,4},{2,0,5},{1,2,5},{3,1,5},{0,3,5}};
 for(auto a:f){Triangle t={};for(int j=0;j<3;j++)t.v[j]=a[j];triangles.push_back(t);}
 simplify_mesh(2,7,false);
 std::set<std::array<int,3>> unique;
 for(const auto&t:triangles){std::array<int,3>a={{t.v[0],t.v[1],t.v[2]}};std::sort(a.begin(),a.end());unique.insert(a);}
 printf("octahedron input_faces=8 output_faces=%zu unique_unoriented=%zu vertices=%zu\n",triangles.size(),unique.size(),vertices.size());
 if(triangles.empty()||triangles.size()>8)return 1;
 puts("FQMS_BASELINE_OK simplification-executed");
}
