#include <embree4/rtcore.h>
#include <cstdio>
#include <cmath>
#include <cstring>
static RTCGeometry triangle(RTCDevice d) {
 RTCGeometry g=rtcNewGeometry(d,RTC_GEOMETRY_TYPE_TRIANGLE);
 float* v=(float*)rtcSetNewGeometryBuffer(g,RTC_BUFFER_TYPE_VERTEX,0,RTC_FORMAT_FLOAT3,3*sizeof(float),3);
 const float vertices[]={-1,-1,0,1,-1,0,0,1,0};std::memcpy(v,vertices,sizeof(vertices));
 unsigned* i=(unsigned*)rtcSetNewGeometryBuffer(g,RTC_BUFFER_TYPE_INDEX,0,RTC_FORMAT_UINT3,3*sizeof(unsigned),1);i[0]=0;i[1]=1;i[2]=2;rtcCommitGeometry(g);return g;
}
static RTCRayHit ray(float x,float limit=100.f) {
 RTCRayHit r={};r.ray.org_x=x;r.ray.dir_z=2;r.ray.tfar=limit;r.ray.mask=~0u;r.hit.geomID=RTC_INVALID_GEOMETRY_ID;
 for(unsigned j=0;j<RTC_MAX_INSTANCE_LEVEL_COUNT;++j){r.hit.instID[j]=RTC_INVALID_GEOMETRY_ID;r.hit.instPrimID[j]=RTC_INVALID_GEOMETRY_ID;}
 return r;
}
int main() {
 RTCDevice d=rtcNewDevice("threads=1");if(!d)return 1;
 RTCScene mesh=rtcNewScene(d);RTCGeometry tri=triangle(d);rtcAttachGeometryByID(mesh,tri,7);rtcReleaseGeometry(tri);rtcCommitScene(mesh);
 RTCScene middle=rtcNewScene(d);RTCGeometry array=rtcNewGeometry(d,RTC_GEOMETRY_TYPE_INSTANCE_ARRAY);rtcSetGeometryInstancedScene(array,mesh);
 const float m[]={1,0,0,0,0,1,0,0,0,0,1,2, 1,0,0,3,0,1,0,0,0,0,1,2};
 float* transforms=(float*)rtcSetNewGeometryBuffer(array,RTC_BUFFER_TYPE_TRANSFORM,0,RTC_FORMAT_FLOAT3X4_ROW_MAJOR,12*sizeof(float),2);std::memcpy(transforms,m,sizeof(m));rtcCommitGeometry(array);rtcAttachGeometryByID(middle,array,11);rtcReleaseGeometry(array);rtcCommitScene(middle);
 RTCScene root=rtcNewScene(d);RTCGeometry inst=rtcNewGeometry(d,RTC_GEOMETRY_TYPE_INSTANCE);rtcSetGeometryInstancedScene(inst,middle);
 const float outer[]={1,0,0,0,0,1,0,0,0,0,1,4};rtcSetGeometryTransform(inst,0,RTC_FORMAT_FLOAT3X4_ROW_MAJOR,outer);rtcCommitGeometry(inst);rtcAttachGeometryByID(root,inst,13);rtcReleaseGeometry(inst);rtcCommitScene(root);
 RTCRayQueryContext ctx;rtcInitRayQueryContext(&ctx);RTCIntersectArguments ia;rtcInitIntersectArguments(&ia);ia.context=&ctx;
 RTCRayHit a=ray(0);rtcIntersect1(root,&a,&ia);
 RTCRayHit b=ray(3);rtcTraversableIntersect1(rtcGetSceneTraversable(root),&b,&ia);
 if(a.hit.geomID!=7||a.hit.instID[0]!=13||a.hit.instID[1]!=11||a.hit.instPrimID[1]!=0||std::abs(a.ray.tfar-3.f)>1e-5)return 2;
 if(b.hit.geomID!=7||b.hit.instPrimID[1]!=1||std::abs(b.ray.tfar-3.f)>1e-5)return 3;
 if(ctx.instStackSize!=0||ctx.instID[0]!=RTC_INVALID_GEOMETRY_ID)return 4;
 RTCOccludedArguments oa;rtcInitOccludedArguments(&oa);RTCRayHit shortRay=ray(0,2.9f),longRay=ray(0,3.1f);
 rtcOccluded1(root,&shortRay.ray,&oa);rtcTraversableOccluded1(rtcGetSceneTraversable(root),&longRay.ray,&oa);
 if(shortRay.ray.tfar<0||!(longRay.ray.tfar<0))return 5;
 if(rtcGetDeviceError(d)!=RTC_ERROR_NONE)return 6;
 std::printf("BASELINE nested_array t=%.6f path=13/11/7 element=0,1 finite_shadow=PASS context=RESTORED\n",a.ray.tfar);
 rtcReleaseScene(root);rtcReleaseScene(middle);rtcReleaseScene(mesh);rtcReleaseDevice(d);return 0;
}
