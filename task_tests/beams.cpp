#include "tutorials/photon_beams/renderer.h"

#include "task_support/check.h"

#include <embree4/rtcore.h>
#include <embree4/rtcore_photon_beam.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <random>
#include <thread>
#include <vector>

using photon_beams::Box;
using photon_beams::PerspectiveCamera;
using photon_beams::Renderer;
using photon_beams::Settings;
using photon_beams::Vec3;
using photon_beams::END_BOX_EXIT;
using photon_beams::END_MIRROR;
using photon_beams::END_VOLUME;
using photon_beams::EVENT_BOX_EXIT;
using photon_beams::EVENT_MIRROR;
using photon_beams::EVENT_VOLUME_SCATTER;

namespace
{
  bool nearlyEqual(double a, double b, double rel = 2.0e-7, double absTol = 1.0e-11)
  {
    if (std::isnan(a) || std::isnan(b) || std::isinf(a) || std::isinf(b))
      return false;
    const double tol = absTol + rel * std::max(std::fabs(a), std::fabs(b));
    return std::fabs(a - b) <= tol;
  }

  struct DeviceGuard
  {
    RTCDevice device;
    explicit DeviceGuard(const char* config = "threads=1") : device(rtcNewDevice(config)) {}
    ~DeviceGuard() { if (device) rtcReleaseDevice(device); }
  };

  struct SceneGuard
  {
    RTCScene scene;
    explicit SceneGuard(RTCDevice device) : scene(rtcNewScene(device)) {}
    ~SceneGuard() { if (scene) rtcReleaseScene(scene); }
    void commit() { rtcCommitScene(scene); }
  };

  RTCPhotonBeam beam(Vec3 from, Vec3 to, double radius, Vec3 flux)
  {
    RTCPhotonBeam b{};
    b.from[0] = from.x; b.from[1] = from.y; b.from[2] = from.z;
    b.to[0] = to.x;     b.to[1] = to.y;     b.to[2] = to.z;
    b.radius = radius;
    b.flux[0] = flux.x; b.flux[1] = flux.y; b.flux[2] = flux.z;
    return b;
  }

  RTCBeamQuery query(Vec3 origin, Vec3 direction, double tnear, double tfar,
                     double sigmaT, double sigmaS)
  {
    RTCBeamQuery q{};
    q.origin[0] = origin.x; q.origin[1] = origin.y; q.origin[2] = origin.z;
    q.direction[0] = direction.x; q.direction[1] = direction.y; q.direction[2] = direction.z;
    q.tnear = tnear;
    q.tfar = tfar;
    q.sigma_t = sigmaT;
    q.sigma_s = sigmaS;
    return q;
  }

  namespace reference_math
  {
    using LD = long double;

    LD dot(const LD* a, const LD* b) { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }

    bool interval(const RTCPhotonBeam& b, const RTCBeamQuery& q, LD& loOut, LD& hiOut)
    {
      LD f[3] = {b.from[0], b.from[1], b.from[2]};
      LD t[3] = {b.to[0], b.to[1], b.to[2]};
      LD delta[3] = {t[0]-f[0], t[1]-f[1], t[2]-f[2]};
      LD length = std::sqrt(dot(delta, delta));
      if (!(length > 0.0L)) return false;
      LD w[3] = {delta[0]/length, delta[1]/length, delta[2]/length};

      LD o[3] = {q.origin[0], q.origin[1], q.origin[2]};
      LD d[3] = {q.direction[0], q.direction[1], q.direction[2]};
      LD ro[3] = {o[0]-f[0], o[1]-f[1], o[2]-f[2]};

      const LD inf = std::numeric_limits<LD>::infinity();
      LD lo = -inf, hi = inf;
      LD s0 = dot(ro, w);
      LD sv = dot(d, w);
      if (sv != 0.0L)
      {
        LD a = -s0 / sv;
        LD c = (length - s0) / sv;
        lo = std::min(a, c);
        hi = std::max(a, c);
      }
      else if (s0 < 0.0L || s0 > length) return false;

      /* Independent radial formulation: project ro and d perpendicular to w. */
      LD wd = dot(d, w);
      LD dv[3] = {d[0]-w[0]*wd, d[1]-w[1]*wd, d[2]-w[2]*wd};
      LD ws = dot(ro, w);
      LD p0[3] = {ro[0]-w[0]*ws, ro[1]-w[1]*ws, ro[2]-w[2]*ws};
      LD A = dot(dv, dv);
      LD B = dot(p0, dv);
      LD C = dot(p0, p0) - LD(b.radius) * LD(b.radius);

      if (A > 0.0L)
      {
        LD D = B*B - A*C;
        if (!(D > 0.0L)) return false;
        LD root = std::sqrt(D);
        LD r0 = (-B-root)/A;
        LD r1 = (-B+root)/A;
        lo = std::max(lo, std::min(r0,r1));
        hi = std::min(hi, std::max(r0,r1));
      }
      else if (!(C < 0.0L))
      {
        return false;
      }

      lo = std::max(lo, LD(q.tnear));
      hi = std::min(hi, LD(q.tfar));
      if (!(hi > lo)) return false;
      loOut = lo; hiOut = hi;
      return true;
    }

    template <class F>
    LD adaptiveSimpson(F& f, LD a, LD b, LD eps, LD whole, int depth)
    {
      LD c = (a+b)/2;
      LD h = b-a;
      LD left = (f(a) + 4*f((a+c)/2) + f(c)) * h / 12;
      LD right = (f(c) + 4*f((c+b)/2) + f(b)) * h / 12;
      LD refined = left + right;
      if (depth <= 0 || std::fabsl(refined-whole) <= 15*eps)
        return refined + (refined-whole)/15;
      return adaptiveSimpson(f,a,c,eps/2,left,depth-1) +
             adaptiveSimpson(f,c,b,eps/2,right,depth-1);
    }

    template <class F>
    LD integrate(F f, LD a, LD b)
    {
      LD c=(a+b)/2, h=b-a;
      LD whole = (f(a)+4*f(c)+f(b))*h/6;
      return adaptiveSimpson(f,a,b,1e-14L,whole,28);
    }

    void addBeam(const RTCPhotonBeam& b, const RTCBeamQuery& q, LD rgb[3],
                 std::uint64_t* intervalCount = nullptr)
    {
      LD a,bp;
      if (!interval(b,q,a,bp)) return;
      if (intervalCount) ++*intervalCount;

      LD d[3] = {q.direction[0], q.direction[1], q.direction[2]};
      LD dn = std::sqrt(dot(d,d));
      LD k = LD(q.sigma_t) * dn;
      auto eye = [&](LD t) {
        return dn * std::expl(-k*(t-LD(q.tnear)));
      };
      LD eyeIntegral = integrate(eye,a,bp);
      LD pi2 = std::acos(-1.0L); pi2 *= pi2;
      for (int c=0;c<3;++c)
        rgb[c] += LD(b.flux[c]) * LD(q.sigma_s) /
                  (4*pi2*LD(b.radius)*LD(b.radius)) * eyeIntegral;
    }

    std::array<double,3> rgb(const std::vector<RTCPhotonBeam>& beams,
                             const RTCBeamQuery& q,
                             std::uint64_t* intervals = nullptr)
    {
      LD sum[3] = {0,0,0};
      std::uint64_t n = 0;
      for (const auto& b : beams) reference_math::addBeam(b,q,sum,&n);
      if (intervals) *intervals = n;
      return {(double)sum[0],(double)sum[1],(double)sum[2]};
    }
  }

  std::array<double,3> apiRGB(RTCPhotonBeamMap map, const RTCBeamQuery& q,
                              RTCBeamStats* stats = nullptr)
  {
    double rgb[3] = {0,0,0};
    RTCBeamStats local{};
    bool ok = rtcIntegratePhotonBeams(map,&q,rgb,&local);
    if (!ok) return {std::numeric_limits<double>::quiet_NaN(),rgb[1],rgb[2]};
    if (stats) *stats = local;
    return {rgb[0],rgb[1],rgb[2]};
  }

  RTCScene makeMirrorScene(RTCDevice device)
  {
    RTCScene scene = rtcNewScene(device);
    RTCGeometry geom = rtcNewGeometry(device,RTC_GEOMETRY_TYPE_TRIANGLE);
    struct Vertex { float x,y,z,w; };
    Vertex* v = (Vertex*)rtcSetNewGeometryBuffer(
      geom,RTC_BUFFER_TYPE_VERTEX,0,RTC_FORMAT_FLOAT3,sizeof(Vertex),4);
    unsigned* idx = (unsigned*)rtcSetNewGeometryBuffer(
      geom,RTC_BUFFER_TYPE_INDEX,0,RTC_FORMAT_UINT3,3*sizeof(unsigned),2);
    v[0] = {-100,-100,1.5f,0};
    v[1] = { 100,-100,1.5f,0};
    v[2] = { 100, 100,1.5f,0};
    v[3] = {-100, 100,1.5f,0};
    unsigned ids[6] = {0,1,2,0,2,3};
    for (int i=0;i<6;++i) idx[i]=ids[i];
    rtcCommitGeometry(geom);
    rtcAttachGeometry(scene,geom);
    rtcReleaseGeometry(geom);
    rtcCommitScene(scene);
    return scene;
  }
}

/* ------------------------------ integral ------------------------------ */

static int integral_analytic_cases(void)
{
  DeviceGuard guard;
  TASK_CHECK(guard.device != nullptr);

  struct Case { RTCPhotonBeam b; RTCBeamQuery q; };
  std::vector<Case> cases;
  cases.push_back({
    beam({0,0,0},{0,0,1},0.10,{1.0,0.5,0.25}),
    query({0.05,0,-0.5},{0,0,2},0,1,0.40,0.20)});
  cases.push_back({
    beam({0,0,0},{0,0,1},0.10,{0.7,1.3,0.9}),
    query({0,0,-0.2},{0.05,0,1},0,2,0.13,0.11)});
  cases.push_back({
    beam({1,2,-0.4},{1,2,1.7},0.075,{2,0.4,1.1}),
    query({1.02,2.0,-0.8},{1.0e-8,0,1},0,4,0.07,0.03)});
  cases.push_back({
    beam({-2,0.3,1},{-0.4,1.2,2.5},0.13,{0.2,2.2,1.4}),
    query({-1.71,0.56,2.15},{0.7,0.2,-1.1},0.1,2.0,0.21,0.19)});
  cases.push_back({
    beam({0,0,0},{0,0,1},0.10,{1,1,1}),
    query({0.05,0,-0.5},{0,0,2},0.4999999999,0.5000000001,1.0e-8,1.0e-8)});

  std::vector<RTCPhotonBeam> beams;
  for (auto& c : cases) beams.push_back(c.b);
  RTCPhotonBeamMap map = rtcNewPhotonBeamMap(guard.device,beams.data(),beams.size());
  TASK_CHECK(map != nullptr);

  for (size_t i=0;i<cases.size();++i)
  {
    long double expected[3] = {0,0,0};
    std::uint64_t expectedIntervals = 0;
    reference_math::addBeam(cases[i].b,cases[i].q,expected,&expectedIntervals);

    RTCBeamStats stats{};
    double got[3]{};
    TASK_CHECK(rtcIntegratePhotonBeams(map,&cases[i].q,got,&stats));
    TASK_CHECK(stats.supportIntervals >= 1);
    (void)expectedIntervals;
    /* The shared map sums all beams; compare against the complete reference. */
    long double allExpected[3] = {0,0,0};
    std::uint64_t allIntervals = 0;
    for (const auto& b : beams)
      reference_math::addBeam(b,cases[i].q,allExpected,&allIntervals);
    TASK_CHECK(stats.supportIntervals == allIntervals);
    for (int c=0;c<3;++c)
      TASK_CHECK(nearlyEqual(got[c],(double)allExpected[c],3.0e-7,1.0e-13));
  }

  /* Exact tangency is zero measure and must not contribute. */
  RTCPhotonBeam tangent = beam({0,0,0},{0,0,1},0.1,{10,10,10});
  RTCPhotonBeamMap tangentMap = rtcNewPhotonBeamMap(guard.device,&tangent,1);
  RTCBeamQuery tq = query({0.1,0,-1},{0,0,1},0,3,0.2,0.1);
  double tangentRGB[3] = {9,9,9};
  RTCBeamStats ts{};
  TASK_CHECK(rtcIntegratePhotonBeams(tangentMap,&tq,tangentRGB,&ts));
  TASK_CHECK(ts.supportIntervals == 0);
  TASK_CHECK(ts.beamTests == 1);
  TASK_CHECK(nearlyEqual(tangentRGB[0],0,0,1e-14));
  TASK_CHECK(nearlyEqual(tangentRGB[1],0,0,1e-14));
  TASK_CHECK(nearlyEqual(tangentRGB[2],0,0,1e-14));
  rtcReleasePhotonBeamMap(tangentMap);

  /* Overlapping beams accumulate rather than clipping one another. */
  RTCPhotonBeam overlap[2] = {
    beam({0,0,0},{0,0,1},0.1,{1,0,0}),
    beam({0,0,0},{0,0,1},0.1,{0,2,0})
  };
  RTCPhotonBeamMap overlapMap = rtcNewPhotonBeamMap(guard.device,overlap,2);
  RTCBeamQuery oq = query({0.02,0,-0.2},{0,0,1},0,2,0.3,0.3);
  double orgb[3]{};
  TASK_CHECK(rtcIntegratePhotonBeams(overlapMap,&oq,orgb,nullptr));
  long double oe[3]{};
  reference_math::addBeam(overlap[0],oq,oe);
  reference_math::addBeam(overlap[1],oq,oe);
  TASK_CHECK(nearlyEqual(orgb[0],(double)oe[0],1e-7));
  TASK_CHECK(nearlyEqual(orgb[1],(double)oe[1],1e-7));
  TASK_CHECK(orgb[2] == 0.0);
  rtcReleasePhotonBeamMap(overlapMap);

  rtcReleasePhotonBeamMap(map);
  return 0;
}

static int integral_lifetime_and_errors(void)
{
  DeviceGuard guard;
  TASK_CHECK(guard.device != nullptr);

  RTCError before = rtcGetDeviceError(guard.device);
  TASK_CHECK(before == RTC_ERROR_NONE);
  TASK_CHECK(rtcNewPhotonBeamMap(guard.device,nullptr,1) == nullptr);
  TASK_CHECK(rtcGetDeviceError(guard.device) == RTC_ERROR_INVALID_ARGUMENT);

  RTCPhotonBeam bad = beam({0,0,0},{0,0,0},1,{1,1,1});
  TASK_CHECK(rtcNewPhotonBeamMap(guard.device,&bad,1) == nullptr);
  TASK_CHECK(rtcGetDeviceError(guard.device) == RTC_ERROR_INVALID_ARGUMENT);

  bad = beam({0,0,0},{0,0,1},0,{1,1,1});
  TASK_CHECK(rtcNewPhotonBeamMap(guard.device,&bad,1) == nullptr);
  TASK_CHECK(rtcGetDeviceError(guard.device) == RTC_ERROR_INVALID_ARGUMENT);

  bad = beam({0,0,0},{0,0,1},1,{-1,0,0});
  TASK_CHECK(rtcNewPhotonBeamMap(guard.device,&bad,1) == nullptr);
  TASK_CHECK(rtcGetDeviceError(guard.device) == RTC_ERROR_INVALID_ARGUMENT);

  /* Empty maps are legal and answer zero. */
  RTCPhotonBeamMap empty = rtcNewPhotonBeamMap(guard.device,nullptr,0);
  TASK_CHECK(empty != nullptr);
  RTCBeamQuery q = query({0,0,-1},{0,0,1},0,3,.1,.05);
  double rgb[3] = {1,2,3};
  RTCBeamStats st{9,9,9};
  TASK_CHECK(rtcIntegratePhotonBeams(empty,&q,rgb,&st));
  TASK_CHECK(rgb[0] == 0 && rgb[1] == 0 && rgb[2] == 0);
  TASK_CHECK(st.nodeVisits == 0 && st.beamTests == 0 && st.supportIntervals == 0);

  RTCBeamQuery badQ = q;
  badQ.direction[0] = badQ.direction[1] = badQ.direction[2] = 0;
  double untouched[3] = {4.0,5.0,6.0};
  RTCBeamStats cleared{7,8,9};
  TASK_CHECK(!rtcIntegratePhotonBeams(empty,&badQ,untouched,&cleared));
  TASK_CHECK(untouched[0] == 4 && untouched[1] == 5 && untouched[2] == 6);
  TASK_CHECK(cleared.nodeVisits == 0 && cleared.beamTests == 0 && cleared.supportIntervals == 0);
  TASK_CHECK(rtcGetDeviceError(guard.device) == RTC_ERROR_INVALID_ARGUMENT);

  badQ = q; badQ.tnear = 2; badQ.tfar = 1;
  TASK_CHECK(!rtcIntegratePhotonBeams(empty,&badQ,untouched,nullptr));
  TASK_CHECK(rtcGetDeviceError(guard.device) == RTC_ERROR_INVALID_ARGUMENT);
  badQ = q; badQ.sigma_s = 2; badQ.sigma_t = 1;
  TASK_CHECK(!rtcIntegratePhotonBeams(empty,&badQ,untouched,nullptr));
  TASK_CHECK(rtcGetDeviceError(guard.device) == RTC_ERROR_INVALID_ARGUMENT);
  rtcReleasePhotonBeamMap(empty);

  /* Deep copy: releasing and mutating caller memory cannot affect the map. */
  RTCPhotonBeam* owned = new RTCPhotonBeam[1];
  RTCPhotonBeam original = beam({0,0,0},{0,0,1},.12,{1.2,.8,.4});
  owned[0] = original;
  RTCPhotonBeamMap releasedInputMap = rtcNewPhotonBeamMap(guard.device,owned,1);
  TASK_CHECK(releasedInputMap != nullptr);
  delete[] owned;
  RTCPhotonBeam mutableBeam = original;
  RTCPhotonBeamMap mutableMap = rtcNewPhotonBeamMap(guard.device,&mutableBeam,1);
  TASK_CHECK(mutableMap != nullptr);
  mutableBeam.radius = 1000.0;
  mutableBeam.from[0] = 999.0;
  RTCBeamQuery mq = query({.03,0,-.2},{0,0,1},0,2,.2,.1);
  double a[3]{}, b[3]{};
  TASK_CHECK(rtcIntegratePhotonBeams(releasedInputMap,&mq,a,nullptr));
  TASK_CHECK(rtcIntegratePhotonBeams(mutableMap,&mq,b,nullptr));
  for (int c=0;c<3;++c) TASK_CHECK(nearlyEqual(a[c],b[c],1e-12,1e-14));
  rtcReleasePhotonBeamMap(releasedInputMap);
  rtcReleasePhotonBeamMap(mutableMap);
  return 0;
}

static int integral_concurrent_maps(void)
{
  DeviceGuard guard;
  TASK_CHECK(guard.device != nullptr);

  constexpr size_t countA = 73, countB = 41;
  std::vector<RTCPhotonBeam> a(countA), b(countB);
  std::mt19937_64 rng(0xabcdef123456ULL);
  std::uniform_real_distribution<double> pos(-4,4), dir(-1,1), rad(.02,.12), flux(0,2);
  for (size_t i=0;i<countA;++i)
  {
    Vec3 f{pos(rng),pos(rng),pos(rng)};
    Vec3 d = photon_beams::normalize({dir(rng),dir(rng),dir(rng)});
    a[i] = beam(f,f+d*(.5+std::fabs(dir(rng))*2),rad(rng),{flux(rng),flux(rng),flux(rng)});
  }
  for (size_t i=0;i<countB;++i)
  {
    Vec3 f{pos(rng)+20,pos(rng),pos(rng)};
    Vec3 d = photon_beams::normalize({dir(rng),dir(rng),dir(rng)});
    b[i] = beam(f,f+d*(.5+std::fabs(dir(rng))*2),rad(rng),{flux(rng),flux(rng),flux(rng)});
  }

  RTCPhotonBeamMap mapA = rtcNewPhotonBeamMap(guard.device,a.data(),a.size());
  RTCPhotonBeamMap mapB = rtcNewPhotonBeamMap(guard.device,b.data(),b.size());
  TASK_CHECK(mapA && mapB);

  std::vector<RTCBeamQuery> queries;
  for (int i=0;i<24;++i)
    queries.push_back(query({pos(rng),pos(rng),pos(rng)},{dir(rng),dir(rng),dir(rng)},
                            0,2+std::fabs(dir(rng))*5,.40+(i%5)*.10,.01+(i%4)*.08));

  std::array<std::array<double,3>,24> expectedA{}, expectedB{};
  for (size_t i=0;i<queries.size();++i)
  {
    long double ea[3]{}, eb[3]{};
    for (const auto& beam : a) reference_math::addBeam(beam,queries[i],ea);
    for (const auto& beam : b) reference_math::addBeam(beam,queries[i],eb);
    for (int c=0;c<3;++c) { expectedA[i][c]=(double)ea[c]; expectedB[i][c]=(double)eb[c]; }
  }

  std::atomic<int> failures{0};
  auto worker = [&](int id) {
    rtcRetainPhotonBeamMap(mapA);
    rtcRetainPhotonBeamMap(mapB);
    for (int rep=0;rep<200;++rep)
    {
      size_t qi = (rep + id) % queries.size();
      RTCPhotonBeamMap first = (rep & 1) ? mapA : mapB;
      double rgb[3]{};
      if (!rtcIntegratePhotonBeams(first,&queries[qi],rgb,nullptr)) { ++failures; break; }
      const auto& expected = (rep & 1) ? expectedA[qi] : expectedB[qi];
      for (int c=0;c<3;++c)
        if (!nearlyEqual(rgb[c],expected[c],3e-7,1e-12)) { ++failures; break; }
    }
    rtcReleasePhotonBeamMap(mapA);
    rtcReleasePhotonBeamMap(mapB);
  };

  std::vector<std::thread> threads;
  for (int t=0;t<8;++t) threads.emplace_back(worker,t);
  for (auto& th : threads) th.join();
  TASK_CHECK(failures.load() == 0);

  rtcReleasePhotonBeamMap(mapA);
  rtcReleasePhotonBeamMap(mapB);
  return 0;
}

/* -------------------------------- index -------------------------------- */

static int index_exhaustive_random(void)
{
  DeviceGuard guard;
  TASK_CHECK(guard.device != nullptr);

  constexpr size_t n = 260;
  std::vector<RTCPhotonBeam> beams(n);
  std::mt19937_64 rng(0x13579bdf2468aceULL);
  std::uniform_real_distribution<double> pos(-5,5), dir(-1,1), len(.25,2.5), rad(.02,.18), flux(0,3);
  for (size_t i=0;i<n;++i)
  {
    Vec3 f{pos(rng),pos(rng),pos(rng)};
    Vec3 d = photon_beams::normalize({dir(rng),dir(rng),dir(rng)});
    beams[i] = beam(f,f+d*len(rng),rad(rng),{flux(rng),flux(rng),flux(rng)});
  }
  RTCPhotonBeamMap map = rtcNewPhotonBeamMap(guard.device,beams.data(),beams.size());
  TASK_CHECK(map != nullptr);

  for (int qi=0;qi<140;++qi)
  {
    RTCBeamQuery q = query({pos(rng)*1.2,pos(rng)*1.2,pos(rng)*1.2},
                           {dir(rng),dir(rng),dir(rng)},
                           0,1+std::fabs(dir(rng))*8,
                           std::fabs(dir(rng))*1.5,
                           std::fabs(dir(rng))*0.7);
    q.sigma_s = std::min(q.sigma_s,q.sigma_t);
    if (q.direction[0]==0 && q.direction[1]==0 && q.direction[2]==0) q.direction[0]=1;

    long double expectedLD[3]{};
    std::uint64_t expectedIntervals = 0;
    for (const auto& b : beams)
      reference_math::addBeam(b,q,expectedLD,&expectedIntervals);

    RTCBeamStats stats{};
    double got[3]{};
    TASK_CHECK(rtcIntegratePhotonBeams(map,&q,got,&stats));
    TASK_CHECK(stats.supportIntervals == expectedIntervals);
    TASK_CHECK(stats.beamTests >= stats.supportIntervals);
    if (expectedIntervals) TASK_CHECK(stats.nodeVisits > 0);
    for (int c=0;c<3;++c)
      TASK_CHECK(nearlyEqual(got[c],(double)expectedLD[c],4e-7,2e-11));
  }

  rtcReleasePhotonBeamMap(map);
  return 0;
}

static int index_sparse_budget_and_identity(void)
{
  DeviceGuard guard;
  TASK_CHECK(guard.device != nullptr);

  constexpr size_t n = 4096;
  constexpr double spacing = 10.0;
  std::vector<RTCPhotonBeam> beams(n);
  for (size_t i=0;i<n;++i)
  {
    double x = (double(i) - double(n-1)/2.0) * spacing;
    double identityFlux = 1.0 + double(i) * 1.0e-7;
    beams[i] = beam({x,0,0},{x,0,1},0.05,{identityFlux,0.7*identityFlux,1.3*identityFlux});
  }

  RTCPhotonBeamMap map = rtcNewPhotonBeamMap(guard.device,beams.data(),beams.size());
  TASK_CHECK(map != nullptr);

  std::uint64_t totalBeamTests = 0, totalNodeVisits = 0, totalIntervals = 0;
  std::uint64_t maxBeamTests = 0;
  for (size_t i=0;i<n;++i)
  {
    double x = (double(i) - double(n-1)/2.0) * spacing;
    RTCBeamQuery q = query({x+0.012,0,-1},{0,0,1},0,3,.17,.11);
    RTCBeamStats stats{};
    double got[3]{};
    TASK_CHECK(rtcIntegratePhotonBeams(map,&q,got,&stats));
    TASK_CHECK(stats.supportIntervals == 1);
    TASK_CHECK(stats.beamTests >= 1 && stats.beamTests <= 32);
    maxBeamTests = std::max(maxBeamTests,stats.beamTests);
    totalBeamTests += stats.beamTests;
    totalNodeVisits += stats.nodeVisits;
    totalIntervals += stats.supportIntervals;

    long double expected[3]{};
    reference_math::addBeam(beams[i],q,expected);
    for (int c=0;c<3;++c)
      TASK_CHECK(nearlyEqual(got[c],(double)expected[c],3e-7,1e-13));
  }

  const std::uint64_t budget = 32ULL * n;
  std::printf("INDEX_SPARSE beams=%zu queries=%zu nodeVisits=%llu beamTests=%llu "
              "supportIntervals=%llu maxBeamTestsPerQuery=%llu budget=%llu\n",
              n,n,(unsigned long long)totalNodeVisits,
              (unsigned long long)totalBeamTests,
              (unsigned long long)totalIntervals,
              (unsigned long long)maxBeamTests,
              (unsigned long long)budget);
  TASK_CHECK(totalBeamTests <= budget);
  TASK_CHECK(totalIntervals == n);
  rtcReleasePhotonBeamMap(map);
  return 0;
}

/* ------------------------------ transport ------------------------------ */

static int transport_vacuum_diagnostics(void)
{
  DeviceGuard guard;
  SceneGuard scene(guard.device);
  scene.commit();

  constexpr size_t n = 128;
  Renderer renderer(guard.device,scene.scene,Box{{-1,-1,-1},{1,1,1}},0,0,
                    {0,0,0},{2,3,4},
                    PerspectiveCamera{{0,0,3},{0,0,0},{0,1,0},45},
                    4,3,Settings{1234,1,.05});
  TASK_CHECK(renderer.step(n));
  TASK_CHECK(renderer.emittedPhotons == n);
  TASK_CHECK(renderer.photons.size() == n);
  TASK_CHECK(renderer.currentBeams.empty());
  TASK_CHECK(renderer.currentSegments.size() == n);

  int checkedExits = 0;
  for (size_t i=0;i<n;++i)
  {
    const auto& p = renderer.photons[i];
    TASK_CHECK(p.index == i);
    TASK_CHECK(p.initialFlux.x == 2 && p.initialFlux.y == 3 && p.initialFlux.z == 4);
    TASK_CHECK(p.events.size() >= 2);
    TASK_CHECK(p.segmentIndices.size() == 1);
    const auto& seg = renderer.currentSegments[p.segmentIndices[0]];
    TASK_CHECK(!seg.stored);
    TASK_CHECK(seg.endType == END_BOX_EXIT);

    /* Independent slab implementation for the expected vacuum exit point. */
    double exitLo = -std::numeric_limits<double>::infinity();
    double exitHi = std::numeric_limits<double>::infinity();
    bool boxHit = true;
    const double po[3] = {renderer.lightPosition.x, renderer.lightPosition.y, renderer.lightPosition.z};
    const double pd[3] = {p.initialDirection.x, p.initialDirection.y, p.initialDirection.z};
    const double blo[3] = {renderer.mediumBox.lower.x, renderer.mediumBox.lower.y, renderer.mediumBox.lower.z};
    const double bhi[3] = {renderer.mediumBox.upper.x, renderer.mediumBox.upper.y, renderer.mediumBox.upper.z};
    for (int axis=0; axis<3; ++axis)
    {
      if (pd[axis] == 0.0)
      {
        if (po[axis] < blo[axis] || po[axis] > bhi[axis]) boxHit = false;
      }
      else
      {
        double ta=(blo[axis]-po[axis])/pd[axis];
        double tb=(bhi[axis]-po[axis])/pd[axis];
        if (ta>tb) std::swap(ta,tb);
        exitLo=std::max(exitLo,ta);
        exitHi=std::min(exitHi,tb);
      }
    }
    TASK_CHECK(boxHit && exitHi >= 0.0);
    Vec3 expectedTo = renderer.lightPosition + p.initialDirection * exitHi;
    TASK_CHECK(nearlyEqual(seg.beam.from[0],renderer.lightPosition.x,1e-12));
    TASK_CHECK(nearlyEqual(seg.beam.to[0],expectedTo.x,1e-8));
    TASK_CHECK(nearlyEqual(seg.beam.to[1],expectedTo.y,1e-8));
    TASK_CHECK(nearlyEqual(seg.beam.to[2],expectedTo.z,1e-8));
    TASK_CHECK(nearlyEqual(seg.beam.flux[1],3,0,0));

    const photon_beams::PhotonEvent& exit = p.events.back();
    TASK_CHECK(exit.type == EVENT_BOX_EXIT);
    TASK_CHECK(nearlyEqual(exit.fluxBefore.x,2,0,0));
    TASK_CHECK(nearlyEqual(exit.fluxAfter.z,4,0,0));
    ++checkedExits;
  }
  TASK_CHECK(checkedExits == int(n));
  TASK_CHECK(nearlyEqual(renderer.pixel(0,0).x,0,0,1e-18));
  return 0;
}

static int transport_mirror_diagnostics(void)
{
  DeviceGuard guard;
  RTCScene scene = makeMirrorScene(guard.device);
  TASK_CHECK(scene != nullptr);

  constexpr size_t n = 256;
  Renderer renderer(guard.device,scene,Box{{-2,-2,-2},{2,2,2}},0,0,
                    {0,0,0},{1,1,1},
                    PerspectiveCamera{{0,0,4},{0,0,0},{0,1,0},45},
                    2,2,Settings{5511,1,.04});
  renderer.mirrorReflectance = {0.25,0.5,0.75};
  TASK_CHECK(renderer.step(n));

  int checkedMirrors = 0;
  for (size_t i=0;i<n;++i)
  {
    const auto& p = renderer.photons[i];
    for (const auto& e : p.events)
    {
      if (e.type != EVENT_MIRROR) continue;
      Vec3 expectedNormal{0,0,-1};
      double t = 1.5 / p.initialDirection.z;
      if (!(p.initialDirection.z > 0) || !(t > 0)) continue;
      Vec3 hit = renderer.lightPosition + p.initialDirection * t;
      if (std::fabs(hit.x) > 99 || std::fabs(hit.y) > 99) continue;

      TASK_CHECK(nearlyEqual(e.position.x,hit.x,2e-5));
      TASK_CHECK(nearlyEqual(e.position.y,hit.y,2e-5));
      TASK_CHECK(nearlyEqual(e.position.z,1.5,2e-5));
      TASK_CHECK(nearlyEqual(e.fluxBefore.x,1,0,1e-12));
      TASK_CHECK(nearlyEqual(e.fluxAfter.x,.25,0,1e-12));
      TASK_CHECK(nearlyEqual(e.fluxAfter.y,.5,0,1e-12));
      TASK_CHECK(nearlyEqual(e.fluxAfter.z,.75,0,1e-12));
      Vec3 expectedDirection = photon_beams::normalize(
        p.initialDirection - expectedNormal * (2*dot(p.initialDirection,expectedNormal)));
      TASK_CHECK(nearlyEqual(e.directionAfter.x,expectedDirection.x,2e-6));
      TASK_CHECK(nearlyEqual(e.directionAfter.y,expectedDirection.y,2e-6));
      TASK_CHECK(nearlyEqual(e.directionAfter.z,expectedDirection.z,2e-6));
      ++checkedMirrors;
    }
  }
  std::printf("TRANSPORT_MIRROR checked=%d\n",checkedMirrors);
  TASK_CHECK(checkedMirrors > 80);
  rtcReleaseScene(scene);
  return 0;
}

static int transport_volume_diagnostics(void)
{
  DeviceGuard guard;
  SceneGuard scene(guard.device);
  scene.commit();

  constexpr size_t n = 512;
  Renderer renderer(guard.device,scene.scene,Box{{-2,-2,-2},{2,2,2}},6.0,3.0,
                    {0.1,-0.1,0.05},{1,2,3},
                    PerspectiveCamera{{0,0,5},{0,0,0},{0,1,0},45},
                    2,2,Settings{9917,1,.04});
  TASK_CHECK(renderer.step(n));
  TASK_CHECK(renderer.emittedPhotons == n);

  int collisions = 0;
  for (size_t i=0;i<n;++i)
  {
    const auto& p = renderer.photons[i];
    for (const auto& e : p.events)
    {
      if (e.type != EVENT_VOLUME_SCATTER) continue;
      ++collisions;
      TASK_CHECK(e.randomValue > 0 && e.randomValue < 1);
      double expectedFree = -std::log(1.0-e.randomValue)/6.0;
      TASK_CHECK(nearlyEqual(e.sampledDistance,expectedFree,1e-12,1e-12));
      TASK_CHECK(nearlyEqual(length(e.position-renderer.lightPosition),expectedFree,1e-9));
      TASK_CHECK(renderer.mediumBox.contains(e.position));
      TASK_CHECK(nearlyEqual(e.fluxBefore.x,1,0,1e-12));
      TASK_CHECK(nearlyEqual(e.fluxBefore.y,2,0,1e-12));
      TASK_CHECK(nearlyEqual(e.fluxAfter.x,.5,0,1e-12));
      TASK_CHECK(nearlyEqual(e.fluxAfter.y,1.0,0,1e-12));
      TASK_CHECK(nearlyEqual(e.fluxAfter.z,1.5,0,1e-12));
      TASK_CHECK(nearlyEqual(length(e.directionAfter),1,1e-12));

      TASK_CHECK(e.segmentIndex != static_cast<size_t>(-1));
      const auto& seg = renderer.currentSegments[e.segmentIndex];
      TASK_CHECK(seg.stored);
      TASK_CHECK(seg.endType == END_VOLUME);
      TASK_CHECK(nearlyEqual(seg.beam.to[0],e.position.x,1e-10));
      TASK_CHECK(nearlyEqual(seg.beam.flux[2],3,0,1e-12));
    }
  }
  std::printf("TRANSPORT_VOLUME collisions=%d emissions=%zu\n",
              collisions,renderer.emittedPhotons);
  TASK_CHECK(collisions > 350);
  return 0;
}

static int transport_single_scattering_reference(void)
{
  DeviceGuard guard;
  SceneGuard scene(guard.device);
  scene.commit();

  const Vec3 light{0.3,0.2,0.0};
  const double sigma = 0.8;
  const double radius = 0.02;
  const int samplesPerSeed = 100000;
  const std::uint64_t seeds[4] = {11,29,47,83};
  constexpr double kPerSeedTolerance = 0.12;
  constexpr double kPooledTolerance = 0.05;

  /* Camera at z=4; box [-2,2]^3 gives t in [2,6] along the central ray. */
  auto reference = [&](double t) {
    Vec3 x{0.0,0.0,4.0-t};
    double r = length(x-light);
    return std::exp(-sigma*(t-2.0)) * std::exp(-sigma*r) / (r*r);
  };
  const int integrationSamples = 200000;
  const double a=2.0,b=6.0,h=(b-a)/integrationSamples;
  double sum = reference(a)+reference(b);
  for (int i=1;i<integrationSamples;++i)
    sum += (i&1) ? 4.0*reference(a+i*h) : 2.0*reference(a+i*h);
  const double referenceValue = sigma/(16.0*std::acos(-1.0)*std::acos(-1.0))*sum*h/3.0;

  double pooled = 0.0;
  for (int s=0;s<4;++s)
  {
    Renderer renderer(guard.device,scene.scene,Box{{-2,-2,-2},{2,2,2}},sigma,sigma,
                      light,{1,1,1},
                      PerspectiveCamera{{0,0,4},{0,0,0},{0,1,0},45},
                      1,1,Settings{seeds[s],1,radius});
    TASK_CHECK(renderer.step(samplesPerSeed));
    double value = renderer.pixel(0,0).x;
    double bias = (value-referenceValue)/referenceValue;
    pooled += value;
    std::printf("SINGLE_SCATTER seed=%llu samples=%d value=%.8g reference=%.8g "
                "bias=%+.4f tolerance=%.2f\n",
                (unsigned long long)seeds[s],samplesPerSeed,value,referenceValue,
                bias,kPerSeedTolerance);
    TASK_CHECK(std::fabs(bias) <= kPerSeedTolerance);
    TASK_CHECK(value > 0.5*referenceValue);
  }
  pooled /= 4.0;
  double pooledBias = (pooled-referenceValue)/referenceValue;
  std::printf("SINGLE_SCATTER pooled_samples=%d pooled=%.8g bias=%+.5f "
              "pooled_tolerance=%.3f\n",
              4*samplesPerSeed,pooled,pooledBias,kPooledTolerance);
  TASK_CHECK(std::fabs(pooledBias) <= kPooledTolerance);
  return 0;
}

/* ----------------------------- progressive ----------------------------- */

static int progressive_film_rounds(void)
{
  DeviceGuard guard;
  SceneGuard scene(guard.device);
  scene.commit();

  const int w=5,h=4;
  Renderer renderer(guard.device,scene.scene,Box{{-1,-1,-1},{1,1,1}},.45,.20,
                    {0,.1,0},{1,.8,.6},
                    PerspectiveCamera{{0,0,3},{0,0,0},{0,1,0},50},
                    w,h,Settings{20251125,4,.09});

  TASK_CHECK(!renderer.step(0));
  TASK_CHECK(renderer.completedRounds == 0 && renderer.emittedPhotons == 0);

  TASK_CHECK(renderer.step(37));
  TASK_CHECK(renderer.completedRounds == 1);
  TASK_CHECK(renderer.emittedPhotons == 37);
  TASK_CHECK(nearlyEqual(renderer.currentRadius,.09,1e-14));

  std::vector<Vec3> independentTotal(w*h,Vec3{});
  auto queryCurrentBeams = [&](std::vector<Vec3>& film) {
    RTCPhotonBeamMap independentMap =
      rtcNewPhotonBeamMap(guard.device,
                          renderer.currentBeams.empty()?nullptr:renderer.currentBeams.data(),
                          renderer.currentBeams.size());
    TASK_CHECK(independentMap != nullptr);
    for (int y=0;y<h;++y) for (int x=0;x<w;++x)
    {
      RTCBeamQuery q{};
      if (!renderer.makePixelQuery(x,y,q)) continue;
      double rgb[3]{};
      RTCBeamStats stats{};
      TASK_CHECK(rtcIntegratePhotonBeams(independentMap,&q,rgb,&stats));
      film[renderer.filmIndex(x,y)] += Vec3(rgb[0],rgb[1],rgb[2]);
    }
    rtcReleasePhotonBeamMap(independentMap);
    return 0;
  };

  TASK_CHECK(queryCurrentBeams(independentTotal) == 0);
  for (size_t i=0;i<independentTotal.size();++i)
    for (int c=0;c<3;++c)
      TASK_CHECK(nearlyEqual((&renderer.filmSum[i].x)[c],(&independentTotal[i].x)[c],1e-14,1e-18));

  std::vector<Vec3> firstFilm = renderer.filmSum;
  std::vector<RTCPhotonBeam> firstBeams = renderer.currentBeams;

  TASK_CHECK(renderer.step(53));
  TASK_CHECK(renderer.completedRounds == 2);
  TASK_CHECK(renderer.emittedPhotons == 90);
  double expectedRadius2 = .09 * std::pow(2.0,-1.0/6.0);
  TASK_CHECK(nearlyEqual(renderer.currentRadius,expectedRadius2,1e-14));
  TASK_CHECK(renderer.currentRadius < .09);

  /* The film increment must be exactly this round's new pool. */
  std::vector<Vec3> secondOnly(w*h,Vec3{});
  TASK_CHECK(queryCurrentBeams(secondOnly) == 0);
  for (size_t i=0;i<secondOnly.size();++i)
  {
    Vec3 increment = renderer.filmSum[i] - firstFilm[i];
    TASK_CHECK(nearlyEqual(increment.x,secondOnly[i].x,1e-13,1e-18));
    TASK_CHECK(nearlyEqual(increment.y,secondOnly[i].y,1e-13,1e-18));
    TASK_CHECK(nearlyEqual(increment.z,secondOnly[i].z,1e-13,1e-18));
    independentTotal[i] += secondOnly[i];
  }

  /* The saved first pool still reproduces round one only. */
  RTCPhotonBeamMap firstMap = rtcNewPhotonBeamMap(
    guard.device,firstBeams.empty()?nullptr:firstBeams.data(),firstBeams.size());
  TASK_CHECK(firstMap != nullptr);
  for (int y=0;y<h;++y) for (int x=0;x<w;++x)
  {
    RTCBeamQuery q{};
    if (!renderer.makePixelQuery(x,y,q)) continue;
    /* makePixelQuery uses current medium constants only, so it remains valid. */
    double rgb[3]{};
    TASK_CHECK(rtcIntegratePhotonBeams(firstMap,&q,rgb,nullptr));
    size_t id=renderer.filmIndex(x,y);
    TASK_CHECK(nearlyEqual(rgb[0],firstFilm[id].x,1e-13,1e-18));
  }
  rtcReleasePhotonBeamMap(firstMap);

  for (int y=0;y<h;++y) for (int x=0;x<w;++x)
  {
    size_t id=renderer.filmIndex(x,y);
    Vec3 p=renderer.pixel(x,y);
    TASK_CHECK(nearlyEqual(p.x,independentTotal[id].x/90.0,1e-13));
    TASK_CHECK(nearlyEqual(p.y,independentTotal[id].y/90.0,1e-13));
  }

  std::vector<Vec3> beforeZero = renderer.filmSum;
  TASK_CHECK(!renderer.step(0));
  TASK_CHECK(renderer.completedRounds == 2 && renderer.emittedPhotons == 90);
  TASK_CHECK(renderer.filmSum.size() == beforeZero.size());
  for (size_t i=0;i<beforeZero.size();++i)
    TASK_CHECK(renderer.filmSum[i].x == beforeZero[i].x);

  TASK_CHECK(renderer.step(17));
  TASK_CHECK(renderer.emittedPhotons == 107);
  TASK_CHECK(renderer.currentRadius < expectedRadius2);
  return 0;
}

static int progressive_interleaving(void)
{
  DeviceGuard guard;
  SceneGuard scene(guard.device);
  scene.commit();

  auto makeRenderer = [&]() {
    return std::make_unique<Renderer>(
      guard.device,scene.scene,Box{{-1.5,-1.5,-1.5},{1.5,1.5,1.5}},.35,.15,
      Vec3{.2,.1,0},Vec3{1,.9,.8},
      PerspectiveCamera{{0,0,4},{0,0,0},{0,1,0},42},
      4,4,Settings{8675309,3,.07});
  };

  auto aInter = makeRenderer();
  auto bInter = makeRenderer();
  auto aCont = makeRenderer();
  auto bCont = makeRenderer();

  TASK_CHECK(aInter->step(40));
  TASK_CHECK(bInter->step(25));
  TASK_CHECK(aInter->step(30));
  TASK_CHECK(bInter->step(55));

  TASK_CHECK(aCont->step(40));
  TASK_CHECK(aCont->step(30));
  TASK_CHECK(bCont->step(25));
  TASK_CHECK(bCont->step(55));

  TASK_CHECK(aInter->emittedPhotons == 70 && bInter->emittedPhotons == 80);
  TASK_CHECK(aCont->emittedPhotons == 70 && bCont->emittedPhotons == 80);

  for (int y=0;y<4;++y) for (int x=0;x<4;++x)
  {
    Vec3 p1=aInter->pixel(x,y), p2=aCont->pixel(x,y);
    Vec3 q1=bInter->pixel(x,y), q2=bCont->pixel(x,y);
    TASK_CHECK(nearlyEqual(p1.x,p2.x,1e-15,1e-18));
    TASK_CHECK(nearlyEqual(p1.y,p2.y,1e-15,1e-18));
    TASK_CHECK(nearlyEqual(q1.z,q2.z,1e-15,1e-18));
  }

  /* Identities are keyed by (seed, round, photon index), not call timing. */
  TASK_CHECK(aInter->identityFor(0,7) == aCont->identityFor(0,7));
  TASK_CHECK(aInter->identityFor(1,19) == aCont->identityFor(1,19));
  TASK_CHECK(aInter->identityFor(2,3) != aInter->identityFor(1,3));
  return 0;
}

const struct task_case task_cases[] = {
  {"integral", "analytic_hand_beams", integral_analytic_cases},
  {"integral", "validation_release", integral_lifetime_and_errors},
  {"integral", "concurrent_two_maps", integral_concurrent_maps},
  {"index", "exhaustive_random", index_exhaustive_random},
  {"index", "sparse_4096_budget", index_sparse_budget_and_identity},
  {"transport", "vacuum_diagnostics", transport_vacuum_diagnostics},
  {"transport", "mirror_diagnostics", transport_mirror_diagnostics},
  {"transport", "volume_diagnostics", transport_volume_diagnostics},
  {"transport", "single_scattering_reference", transport_single_scattering_reference},
  {"progressive", "film_round_accumulation", progressive_film_rounds},
  {"progressive", "interleaved_determinism", progressive_interleaving},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
