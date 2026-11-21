#include "CatmullClark.h"
#include <map>
#include <utility>
#include <cmath>
#include <cstdio>
int main(){
 cc_Mesh*m=ccm_Create(4,0,12,6,4); if(!m)return 1;
 const int f[4][3]={{0,2,1},{0,1,3},{1,2,3},{2,0,3}};
 const float v[4][3]={{0,0,0},{2,0,0},{0,3,0},{0,0,4}};
 std::map<std::pair<int,int>,int> directed;int e=0;
 for(int i=0;i<4;++i){for(int k=0;k<3;++k)m->vertexPoints[i].array[k]=v[i][k];m->vertexToHalfedgeIDs[i]=-1;}
 for(int i=0;i<4;++i){m->faceToHalfedgeIDs[i]=3*i;for(int j=0;j<3;++j){int h=3*i+j,a=f[i][j],b=f[i][(j+1)%3];auto &x=m->halfedges[h];x.vertexID=a;x.nextID=3*i+(j+1)%3;x.prevID=3*i+(j+2)%3;x.faceID=i;x.uvID=-1;x.twinID=-1;x.edgeID=-1;m->vertexToHalfedgeIDs[a]=h;directed[{a,b}]=h;}}
 for(auto kv:directed){int h=kv.second;if(m->halfedges[h].edgeID>=0)continue;auto it=directed.find({kv.first.second,kv.first.first});if(it==directed.end())return 2;int t=it->second;m->halfedges[h].twinID=t;m->halfedges[t].twinID=h;m->halfedges[h].edgeID=m->halfedges[t].edgeID=e;m->edgeToHalfedgeIDs[e]=h;m->creases[e]={e,e,0};++e;}
 cc_Subd*s=ccs_Create(m,1);if(!s)return 3;ccs_Refine_NoCreases_Gather(s);
 if(ccm_VertexCountAtDepth(m,1)!=14||ccm_FaceCountAtDepth(m,1)!=12)return 4;
 auto fp=ccs_VertexPoint(s,4,1);double err=std::abs(fp.x-2.0/3)+std::abs(fp.y-1)+std::abs(fp.z);
 if(err>1e-6)return 5;
 for(int h=0;h<48;++h){int t=ccs_HalfedgeTwinID(s,h,1);if(t<0||ccs_HalfedgeTwinID(s,t,1)!=h)return 6;}
 auto old=ccs_VertexPoint(s,0,1);double moved=std::sqrt(old.x*old.x+old.y*old.y+old.z*old.z);if(moved<.1)return 7;
 if(!ccm_Save(m,".task-build/baseline.ccm"))return 8;cc_Mesh*copy=ccm_Load(".task-build/baseline.ccm");if(!copy||copy->vertexCount!=4||copy->halfedgeCount!=12)return 9;
 std::printf("FORWARD_PRODUCTION_OK V=14 F=12 H=48 facepoint_error=%.9g oldpoint_moved=%.9g save_load=1\n",err,moved);
 ccm_Release(copy);ccs_Release(s);ccm_Release(m);return 0;
}
