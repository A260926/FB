#include "draco/mesh/triangle_soup_mesh_builder.h"
#include "draco/compression/encode.h"
#include "draco/compression/decode.h"
#include <cmath>
#include <iostream>
#include <memory>
static std::unique_ptr<draco::Mesh> fixture(){
  draco::TriangleSoupMeshBuilder b;b.Start(8);
  int pa=b.AddAttribute(draco::GeometryAttribute::POSITION,3,draco::DT_FLOAT32);
  int uv=b.AddAttribute(draco::GeometryAttribute::TEX_COORD,2,draco::DT_FLOAT32);
  float v[8][3]={{0,0,0},{.01f,0,0},{0,.01f,0},{0,0,.01f},
    {100,100,100},{1000,100,100},{100,1000,100},{100,100,1000}};
  int f[4][3]={{0,2,1},{0,1,3},{1,2,3},{2,0,3}};
  for(int k=0;k<8;++k){int o=k<4?0:4;auto fi=draco::FaceIndex(k);
    b.SetAttributeValuesForFace(pa,fi,v[o+f[k%4][0]],v[o+f[k%4][1]],v[o+f[k%4][2]]);
    float t[3][2]={{float(k),0},{float(k)+1,0},{float(k),1}};
    b.SetAttributeValuesForFace(uv,fi,t[0],t[1],t[2]);
  }return b.Finalize();
}
static int degenerates(const draco::Mesh& m){
  auto a=m.GetNamedAttribute(draco::GeometryAttribute::POSITION);if(!a)return -1;
  int count=0;
  for(draco::FaceIndex fi(0);fi<m.num_faces();++fi){
    const auto& f=m.face(fi);float p[3][3];
    for(int j=0;j<3;++j)if(!a->ConvertValue<float>(a->mapped_index(f[j]),3,p[j]))return -1;
    double x[3],y[3];for(int j=0;j<3;++j){x[j]=double(p[1][j])-p[0][j];y[j]=double(p[2][j])-p[0][j];}
    double nx=x[1]*y[2]-x[2]*y[1],ny=x[2]*y[0]-x[0]*y[2],nz=x[0]*y[1]-x[1]*y[0];
    if(nx*nx+ny*ny+nz*nz==0)++count;
  }return count;
}
int main(){
  auto m=fixture();if(!m||m->num_faces()!=8||degenerates(*m)!=0)return 1;
  for(int method:{int(draco::MESH_SEQUENTIAL_ENCODING),int(draco::MESH_EDGEBREAKER_ENCODING)}){
    for(int bits:{0,8}){
      draco::Encoder e;e.SetEncodingMethod(method);e.SetSpeedOptions(5,5);
      e.SetAttributeQuantization(draco::GeometryAttribute::POSITION,bits);
      draco::EncoderBuffer out;auto st=e.EncodeMeshToBuffer(*m,&out);if(!st.ok()||out.size()==0)return 2;
      draco::Decoder d;draco::DecoderBuffer in;in.Init(out.data(),out.size());
      auto decoded=d.DecodeMeshFromBuffer(&in);if(!decoded.ok())return 3;
      auto result=std::move(decoded).value();if(result->num_faces()!=8)return 4;
      int n=degenerates(*result);if(n<0||(bits==0&&n!=0))return 5;
      std::cout<<"ROUNDTRIP method="<<method<<" bits="<<bits<<" faces="<<result->num_faces()<<" degenerate="<<n<<" bytes="<<out.size()<<"\n";
    }
  }std::cout<<"DRACO_BASELINE_COMPLETE\n";
}
