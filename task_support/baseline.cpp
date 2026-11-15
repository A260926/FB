#include <libnurbs/Surface/Surface.hpp>
#include <libnurbs/Curve/Curve.hpp>
#include <cmath>
#include <iostream>
#include <sstream>
#include <stdexcept>
using namespace libnurbs;
static void need(bool b) { if (!b) throw std::runtime_error("baseline assertion"); }
int main() {
    Surface s;
    s.DegreeU=2; s.DegreeV=1;
    s.KnotsU=KnotVector{{0,0,0,1,1,1}};
    s.KnotsV=KnotVector{{0,0,1,1}};
    s.ControlPoints=ControlPointGrid(3,2);
    s.ControlPoints.Values={Vec4(1,0,0,1),Vec4(1,1,0,1),Vec4(0,1,0,2),
                            Vec4(1,0,1,1),Vec4(1,1,1,1),Vec4(0,1,1,2)};
    need((s.Evaluate(.5,.5)-Vec3(.6,.8,.5)).norm()<1e-12);
    auto d=s.EvaluateAll(.37,.46,1,1);
    need(d.Get(1,0).norm()>0 && std::abs(d.Get(0,1).z()-1)<1e-12);
    auto e=s.ElevateDegreeU().ElevateDegreeV().InsertKnotU(.25).InsertKnotV(.6);
    double edit_error=(e.Evaluate(.37,.46)-s.Evaluate(.37,.46)).norm();
    need(edit_error<1e-11);
    for(bool bin : {false,true}) {
        std::stringstream io(std::ios::in|std::ios::out|std::ios::binary);
        e.SaveToFile(io,bin); io.seekg(0); Surface back; back.LoadFromFile(io);
        need((back.Evaluate(.37,.46)-e.Evaluate(.37,.46)).norm()<1e-10);
    }
    Curve c; c.Degree=1; c.Knots=KnotVector{{0,0,1,1}};
    c.ControlPoints={Vec4(0,0,0,1),Vec4(2,0,0,1)};
    need((c.Evaluate(.3)-Vec3(.6,0,0)).norm()<1e-12);
    need(std::abs(c.BinarySearchParameter(Vec3(.6,0,0),1e-9)-.3)<1e-8);
    std::cout<<"UNIT_SURFACE point=0.6,0.8,0.5 derivative_ok=1\n";
    std::cout<<"EDIT_ROUNDTRIP_OK text=1 binary=1 error="<<edit_error<<"\n";
    std::cout<<"CURVE_INVERSE_OK\n";
    bool accepted=true;
    try { KnotVector original_domain{{-3,-3,-3,5,5,5}}; }
    catch(const std::invalid_argument&) { accepted=false; }
    // Observe the capability gap; target accepts the future supported result too.
    std::cout<<"ORIGINAL_DOMAIN_CONSTRUCTOR supported="<<accepted<<"\n";
    std::cout<<"BASELINE_COMPLETE\n";
}
