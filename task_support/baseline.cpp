#include "tests/gtest/gtest.h"
#include "lights/diffuse.h"
#include "shapes/triangle.h"
#include "interaction.h"
#include "sampling.h"
#include <cstdio>
using namespace pbrt;
TEST(EmitterBaseline, ConstantTriangle) {
    Transform id;
    int indices[] = {0,1,2};
    Point3f p[] = {{0,0,0},{2,0,0},{0,3,0}};
    Point2f uv[] = {{0,0},{1,0},{0,1}};
    auto shapes = CreateTriangleMesh(&id,&id,false,1,indices,3,p,nullptr,nullptr,uv,nullptr,nullptr);
    ASSERT_EQ(1u, shapes.size());
    DiffuseAreaLight light(id,MediumInterface(),Spectrum(2),1,shapes[0]);
    EXPECT_FLOAT_EQ(3,shapes[0]->Area());
    Interaction a,b;
    a.p=Point3f(.2f,.3f,0); a.n=Normal3f(0,0,1);
    b.p=Point3f(1.2f,.3f,0); b.n=a.n;
    EXPECT_FLOAT_EQ(2,light.L(a,Vector3f(0,0,1))[0]);
    EXPECT_FLOAT_EQ(2,light.L(b,Vector3f(0,0,1))[0]);
    EXPECT_FLOAT_EQ(0,light.L(a,Vector3f(0,0,-1))[0]);
    EXPECT_NEAR(6*Pi,light.Power()[0],1e-5);
    Ray ray; Normal3f n; Float pos,dir;
    auto Le=light.Sample_Le(Point2f(.36f,.25f),Point2f(.3f,.4f),0,&ray,&n,&pos,&dir);
    EXPECT_FLOAT_EQ(2,Le[0]); EXPECT_NEAR(1.f/3,pos,1e-6);
    Float qp,qd; light.Pdf_Le(ray,n,&qp,&qd);
    EXPECT_NEAR(pos,qp,1e-6); EXPECT_NEAR(dir,qd,1e-6);
    Interaction ref(Point3f(.3f,.3f,2),0,MediumInterface());
    Vector3f wi; VisibilityTester vis;
    auto Li=light.Sample_Li(ref,Point2f(.36f,.25f),&wi,&pos,&vis);
    EXPECT_FLOAT_EQ(2,Li[0]); EXPECT_GT(pos,0);
    EXPECT_NEAR(pos,light.Pdf_Li(ref,wi),pos*2e-5);
    std::printf("PBRT_EMITTER_BASELINE_OK area=3 L_a=2 L_b=2 uniform_pdf=0.333333\n");
}
