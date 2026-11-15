#include "TriMesh.h"
#include "TriMesh_algo.h"
#include <cstdio>
#include <cmath>
#include <vector>
#include <algorithm>
using namespace trimesh;
static double normal_cost(const TriMesh&m,int f,int g){
  point p0=m.vertices[m.faces[f][0]],p1=m.vertices[m.faces[f][1]],p2=m.vertices[m.faces[f][2]];
  double ax=p1[0]-p0[0],ay=p1[1]-p0[1],az=p1[2]-p0[2],bx=p2[0]-p0[0],by=p2[1]-p0[1],bz=p2[2]-p0[2];
  double nx=ay*bz-az*by,ny=az*bx-ax*bz,nz=ax*by-ay*bx;double l=sqrt(nx*nx+ny*ny+nz*nz);nx/=l;ny/=l;nz/=l;
  p0=m.vertices[m.faces[g][0]];p1=m.vertices[m.faces[g][1]];p2=m.vertices[m.faces[g][2]];
  ax=p1[0]-p0[0];ay=p1[1]-p0[1];az=p1[2]-p0[2];bx=p2[0]-p0[0];by=p2[1]-p0[1];bz=p2[2]-p0[2];
  double mx=ay*bz-az*by,my=az*bx-ax*bz,mz=ax*by-ay*bx;l=sqrt(mx*mx+my*my+mz*mz);mx/=l;my/=l;mz/=l;
  double d=nx*mx+ny*my+nz*mz;if(d>1)d=1;if(d<-1)d=-1;return 1-d;
}
int main(){
 // replicate three_by_three fixture
 TriMesh m;m.vertices={point(0,0,0),point(1,0,0)};
 const float tops[3][3]={{0,1,0},{0,1,0.2f},{0,1,0.4f}};
 const float bots[3][3]={{0,-1,0.4f},{0,-1,0},{0,-1,0.2f}};
 for(int i=0;i<3;i++){m.vertices.push_back(point(0.5f*i+0.1f,tops[i][1],tops[i][2]));m.vertices.push_back(point(0.5f*i+0.1f,bots[i][1],bots[i][2]));}
 for(int i=0;i<3;i++){m.faces.push_back(TriMesh::Face(0,1,2+2*i));m.faces.push_back(TriMesh::Face(1,0,3+2*i));}
 // fwd faces 0,2,4 ; bwd 1,3,5
 for(int r=0;r<3;r++){printf("C%d:",r);for(int c=0;c<3;c++)printf(" %.6f",normal_cost(m,2*r,2*c+1));printf("\n");}
 double C[3][3];for(int r=0;r<3;r++)for(int c=0;c<3;c++)C[r][c]=normal_cost(m,2*r,2*c+1);
 int used[3]={0,0,0};double g=0;for(int r=0;r<3;r++){int bc=-1;double bv=1e9;for(int c=0;c<3;c++)if(!used[c]&&C[r][c]<bv){bv=C[r][c];bc=c;}used[bc]=1;g+=bv;}
 printf("greedy=%.6f\n",g);
}
