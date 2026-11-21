// Copyright 2009-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

// Task tests for the native photon beam map and the progressive
// photon_beams::Renderer. No main() is defined here; task_support/driver.c
// drives the registered cases.

#include "task_support/check.h"

#include <embree4/rtcore.h>
#include <embree4/rtcore_photon_beam.h>
#include "tutorials/photon_beams/renderer.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <limits>
#include <memory>
#include <thread>
#include <vector>

using namespace photon_beams;

namespace
{
  // --------------------------------------------------------------------
  // Shared helpers
  // --------------------------------------------------------------------

  constexpr double PI = 3.14159265358979323846;

  struct D3 { double x, y, z; };
  D3 operator+(D3 a, D3 b) { return {a.x+b.x, a.y+b.y, a.z+b.z}; }
  D3 operator-(D3 a, D3 b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
  D3 operator*(D3 a, double s) { return {a.x*s, a.y*s, a.z*s}; }
  double dot(D3 a, D3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
  D3 cross(D3 a, D3 b)
  {
    return { a.y*b.z - a.z*b.y,
             a.z*b.x - a.x*b.z,
             a.x*b.y - a.y*b.x };
  }
  double norm(D3 a) { return std::sqrt(dot(a,a)); }
  D3 unit(D3 a) { return a * (1.0 / norm(a)); }

  D3 beamFrom(const RTCPhotonBeam& b) { return {b.from[0], b.from[1], b.from[2]}; }
  D3 beamTo(const RTCPhotonBeam& b) { return {b.to[0], b.to[1], b.to[2]}; }

  RTCPhotonBeam makeBeam(D3 from, D3 to, double radius, D3 flux)
  {
    RTCPhotonBeam b;
    b.from[0] = from.x; b.from[1] = from.y; b.from[2] = from.z;
    b.to[0] = to.x;   b.to[1] = to.y;   b.to[2] = to.z;
    b.radius = radius;
    b.flux[0] = flux.x; b.flux[1] = flux.y; b.flux[2] = flux.z;
    return b;
  }

  RTCBeamQuery makeQuery(D3 origin, D3 dir, double tnear, double tfar,
                         double sigmaT, double sigmaS)
  {
    RTCBeamQuery q;
    q.origin[0] = origin.x; q.origin[1] = origin.y; q.origin[2] = origin.z;
    q.direction[0] = dir.x; q.direction[1] = dir.y; q.direction[2] = dir.z;
    q.tnear = tnear; q.tfar = tfar;
    q.sigma_t = sigmaT; q.sigma_s = sigmaS;
    return q;
  }

  // Independent support/ray interval, written separately from the
  // kernel implementation. Returns the ray parameter range inside the
  // flat-ended cylinder support.
  bool refInterval(const RTCPhotonBeam& beam, D3 O, D3 d,
                   double tnear, double tfar, double& t0, double& t1)
  {
    const D3 A = beamFrom(beam);
    const D3 B = beamTo(beam);
    const D3 axis = B - A;
    const double L = norm(axis);
    const D3 w = axis * (1.0 / L);
    const D3 rel = O - A;
    const double dw = dot(d, w);
    const double rw = dot(rel, w);

    double lo = tnear, hi = tfar;

    // End cap slabs 0 <= rw + dw*t <= L.
    if (dw != 0.0)
    {
      double a = -rw / dw;
      double b = (L - rw) / dw;
      if (a > b) std::swap(a, b);
      lo = std::max(lo, a);
      hi = std::min(hi, b);
      if (hi <= lo) return false;
    }
    else if (rw < 0.0 || rw > L)
    {
      return false;
    }

    // Distance to the axis line via cross products; subtraction-free
    // construction stays accurate for near-parallel rays.
    const D3 u = cross(rel, w);   // (O-A) x w
    const D3 v = cross(d, w);     // d x w
    const double qa = dot(v, v);
    const double qb = 2.0 * dot(u, v);
    const double qc = dot(u, u) - beam.radius * beam.radius;

    if (qa == 0.0)
    {
      if (qc >= 0.0) return false;
    }
    else
    {
      const double disc = qb*qb - 4.0*qa*qc;
      if (disc <= 0.0) return false; // tangent has zero measure
      const double s = std::sqrt(disc);
      const double q = -0.5 * (qb + (qb >= 0.0 ? s : -s));
      double ra = q / qa;
      double rb = qc / q;
      if (ra > rb) std::swap(ra, rb);
      lo = std::max(lo, ra);
      hi = std::min(hi, rb);
      if (hi <= lo) return false;
    }

    t0 = lo; t1 = hi;
    return true;
  }

  // Adaptive Simpson of a smooth scalar function.
  double simpsonRec(const std::function<double(double)>& f,
                    double a, double b, double fa, double fm, double fb,
                    double whole, double eps, int depth)
  {
    const double m = 0.5 * (a + b);
    const double lm = 0.5 * (a + m);
    const double rm = 0.5 * (m + b);
    const double flm = f(lm), frm = f(rm);
    const double left = (b - a) / 12.0 * (fa + 4.0*flm + fm);
    const double right = (b - a) / 12.0 * (fm + 4.0*frm + fb);
    const double refined = left + right;
    if (depth <= 0 || std::fabs(refined - whole) <= 15.0 * eps)
      return refined + (refined - whole) / 15.0;
    return simpsonRec(f, a, m, fa, flm, fm, left, 0.5*eps, depth-1)
         + simpsonRec(f, m, b, fm, frm, fb, right, 0.5*eps, depth-1);
  }

  double adaptiveSimpson(const std::function<double(double)>& f,
                         double a, double b, double eps = 1e-12)
  {
    const double m = 0.5 * (a + b);
    const double fa = f(a), fm = f(m), fb = f(b);
    const double whole = (b - a) / 6.0 * (fa + 4.0*fm + fb);
    return simpsonRec(f, a, b, fa, fm, fb, whole, eps, 24);
  }

  // High-precision independent reference: per-beam analytic interval
  // plus numerical integration of the exponential weight.
  void referenceIntegrate(const std::vector<RTCPhotonBeam>& beams,
                          const RTCBeamQuery& q, double rgb[3])
  {
    const D3 O{q.origin[0], q.origin[1], q.origin[2]};
    const D3 D{q.direction[0], q.direction[1], q.direction[2]};
    const double dlen = norm(D);
    const double k = q.sigma_t * dlen;

    for (const RTCPhotonBeam& beam : beams)
    {
      double t0, t1;
      if (!refInterval(beam, O, D, q.tnear, q.tfar, t0, t1))
        continue;
      const double integral = adaptiveSimpson([&](double t)
      {
        return std::exp(-k * (t - q.tnear)) * dlen;
      }, t0, t1);
      const double c = q.sigma_s / (4.0 * PI * PI * beam.radius * beam.radius);
      for (int ch = 0; ch < 3; ++ch)
        rgb[ch] += beam.flux[ch] * c * integral;
    }
  }

  bool close(double a, double b, double rel = 1e-9, double absTol = 1e-12)
  {
    return std::fabs(a - b) <= std::max(absTol, rel * std::max(std::fabs(a), std::fabs(b)));
  }

  bool closeVec(const double a[3], const double b[3],
                double rel = 1e-9, double absTol = 1e-12)
  {
    for (int i = 0; i < 3; ++i)
      if (!close(a[i], b[i], rel, absTol)) return false;
    return true;
  }

  // --------------------------------------------------------------------
  // Scene construction helpers
  // --------------------------------------------------------------------

  RTCScene emptyScene(RTCDevice device)
  {
    RTCScene scene = rtcNewScene(device);
    rtcCommitScene(scene);
    return scene;
  }

  RTCScene triangleScene(RTCDevice device, D3 v0, D3 v1, D3 v2)
  {
    RTCScene scene = rtcNewScene(device);
    RTCGeometry geom = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_TRIANGLE);
    auto* v = (float*) rtcSetNewGeometryBuffer(geom, RTC_BUFFER_TYPE_VERTEX, 0,
                                               RTC_FORMAT_FLOAT3, 3*sizeof(float), 3);
    auto* ix = (unsigned*) rtcSetNewGeometryBuffer(geom, RTC_BUFFER_TYPE_INDEX, 0,
                                                   RTC_FORMAT_UINT3, 3*sizeof(unsigned), 1);
    v[0]=float(v0.x); v[1]=float(v0.y); v[2]=float(v0.z);
    v[3]=float(v1.x); v[4]=float(v1.y); v[5]=float(v1.z);
    v[6]=float(v2.x); v[7]=float(v2.y); v[8]=float(v2.z);
    ix[0]=0; ix[1]=1; ix[2]=2;
    rtcCommitGeometry(geom);
    rtcAttachGeometry(scene, geom);
    rtcReleaseGeometry(geom);
    rtcCommitScene(scene);
    return scene;
  }

  // Closed triangle box with walls just inside the medium box.
  RTCScene closedMirrorBox(RTCDevice device, const MediumBox& box)
  {
    RTCScene scene = rtcNewScene(device);
    RTCGeometry geom = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_TRIANGLE);
    const double pad = 0.02;
    const double l[3] = { box.lower[0]+pad, box.lower[1]+pad, box.lower[2]+pad };
    const double u[3] = { box.upper[0]-pad, box.upper[1]-pad, box.upper[2]-pad };
    const float corners[8][3] =
    {
      {float(l[0]),float(l[1]),float(l[2])}, {float(l[0]),float(l[1]),float(u[2])},
      {float(l[0]),float(u[1]),float(l[2])}, {float(l[0]),float(u[1]),float(u[2])},
      {float(u[0]),float(l[1]),float(l[2])}, {float(u[0]),float(l[1]),float(u[2])},
      {float(u[0]),float(u[1]),float(l[2])}, {float(u[0]),float(u[1]),float(u[2])}
    };
    // Outward-wound triangles of the six axis-aligned walls.
    static const unsigned faces[12][3] =
    {
      {0,1,3},{0,3,2}, // -x
      {4,7,5},{4,6,7}, // +x
      {0,5,1},{0,4,5}, // -y
      {2,3,7},{2,7,6}, // +y
      {0,6,2},{0,4,6}, // -z
      {1,5,7},{1,7,3}  // +z
    };
    auto* v = (float*) rtcSetNewGeometryBuffer(geom, RTC_BUFFER_TYPE_VERTEX, 0,
                                               RTC_FORMAT_FLOAT3, 3*sizeof(float), 8);
    auto* ix = (unsigned*) rtcSetNewGeometryBuffer(geom, RTC_BUFFER_TYPE_INDEX, 0,
                                                   RTC_FORMAT_UINT3, 3*sizeof(unsigned), 12);
    for (int i = 0; i < 8; ++i)
      for (int j = 0; j < 3; ++j) v[3*i+j] = corners[i][j];
    for (int i = 0; i < 12; ++i)
      for (int j = 0; j < 3; ++j) ix[3*i+j] = faces[i][j];
    rtcCommitGeometry(geom);
    rtcAttachGeometry(scene, geom);
    rtcReleaseGeometry(geom);
    rtcCommitScene(scene);
    return scene;
  }

  PerspectiveCamera makeCamera(D3 origin, D3 dir, D3 up, double fov = 0.5)
  {
    PerspectiveCamera c;
    c.origin[0]=origin.x; c.origin[1]=origin.y; c.origin[2]=origin.z;
    c.direction[0]=dir.x; c.direction[1]=dir.y; c.direction[2]=dir.z;
    c.up[0]=up.x; c.up[1]=up.y; c.up[2]=up.z;
    c.fovY = fov;
    return c;
  }

  // ====================================================================
  // integral group
  // ====================================================================

  int integralBasic()
  {
    RTCDevice device = rtcNewDevice("threads=1");
    std::vector<RTCPhotonBeam> beams;
    beams.push_back(makeBeam({0,-2,0},{0,2,0}, 0.5, {1.0,2.0,3.0}));
    RTCPhotonBeamMap map = rtcNewPhotonBeamMap(device, beams.data(), beams.size());
    TASK_CHECK(map != nullptr);

    RTCBeamQuery q = makeQuery({0,0,-3},{0,0,1}, 0.0, 6.0, 0.7, 0.4);
    double rgb[3] = {0,0,0}, ref[3] = {0,0,0};
    RTCBeamStats stats;
    TASK_CHECK(rtcIntegratePhotonBeams(map, &q, rgb, &stats));
    referenceIntegrate(beams, q, ref);
    TASK_CHECK(closeVec(rgb, ref, 1e-10, 1e-12));
    TASK_CHECK(stats.supportIntervals == 1);
    TASK_CHECK(stats.beamTests >= 1);
    rtcReleasePhotonBeamMap(map);
    rtcReleaseDevice(device);
    return 0;
  }

  int integralNonUnitAndCaps()
  {
    RTCDevice device = rtcNewDevice("threads=1");
    std::vector<RTCPhotonBeam> beams;
    beams.push_back(makeBeam({1,0,0},{4,0,0}, 0.25, {1.5,0.5,2.5}));
    beams.push_back(makeBeam({2,-1,0},{2,1,0}, 0.2, {0.5,1.5,0.25}));
    RTCPhotonBeamMap map = rtcNewPhotonBeamMap(device, beams.data(), beams.size());
    TASK_CHECK(map != nullptr);

    // Non-unit direction, and tnear/tfar cutting through the supports.
    RTCBeamQuery q = makeQuery({0,0.2,-1},{1.3,0.0,1.7}, 0.5, 2.2, 0.33, 0.21);
    double rgb[3] = {0,0,0}, ref[3] = {0,0,0};
    TASK_CHECK(rtcIntegratePhotonBeams(map, &q, rgb, nullptr));
    referenceIntegrate(beams, q, ref);
    TASK_CHECK(closeVec(rgb, ref, 1e-10, 1e-12));

    // Near-zero extinction: exercises the stable expm1-based path.
    RTCBeamQuery q0 = q;
    q0.sigma_t = 1e-12; q0.sigma_s = 5e-13;
    double rgb0[3] = {0,0,0}, ref0[3] = {0,0,0};
    TASK_CHECK(rtcIntegratePhotonBeams(map, &q0, rgb0, nullptr));
    referenceIntegrate(beams, q0, ref0);
    TASK_CHECK(closeVec(rgb0, ref0, 1e-10, 1e-12));
    rtcReleasePhotonBeamMap(map);
    rtcReleaseDevice(device);
    return 0;
  }

  int integralParallel()
  {
    RTCDevice device = rtcNewDevice("threads=1");
    std::vector<RTCPhotonBeam> beams;
    // Beam along x; three eye rays: near parallel, parallel inside,
    // parallel outside.
    beams.push_back(makeBeam({-3,0,0},{3,0,0}, 0.4, {1,1,1}));
    RTCPhotonBeamMap map = rtcNewPhotonBeamMap(device, beams.data(), beams.size());

    RTCBeamQuery nearP = makeQuery({0,0.39,-2},{0.001,0,1}, 0, 4, 0.5, 0.25);
    double rgb[3] = {0,0,0}, ref[3] = {0,0,0};
    TASK_CHECK(rtcIntegratePhotonBeams(map, &nearP, rgb, nullptr));
    referenceIntegrate(beams, nearP, ref);
    TASK_CHECK(closeVec(rgb, ref, 1e-9, 1e-11));

    RTCBeamQuery parIn = makeQuery({0,0.1,0},{1,0,0}, 0, 5.5, 0.5, 0.25);
    double rgbIn[3] = {0,0,0}, refIn[3] = {0,0,0};
    TASK_CHECK(rtcIntegratePhotonBeams(map, &parIn, rgbIn, nullptr));
    referenceIntegrate(beams, parIn, refIn);
    TASK_CHECK(closeVec(rgbIn, refIn, 1e-10, 1e-12));
    TASK_CHECK(rgbIn[0] > 0.0);

    RTCBeamQuery parOut = makeQuery({0,5,0},{1,0,0}, 0, 2, 0.5, 0.25);
    double rgbOut[3] = {0,0,0};
    TASK_CHECK(rtcIntegratePhotonBeams(map, &parOut, rgbOut, nullptr));
    TASK_CHECK(rgbOut[0] == 0.0 && rgbOut[1] == 0.0 && rgbOut[2] == 0.0);
    rtcReleasePhotonBeamMap(map);
    rtcReleaseDevice(device);
    return 0;
  }

  int integralOverlapTangentThin()
  {
    RTCDevice device = rtcNewDevice("threads=1");
    std::vector<RTCPhotonBeam> beams;
    beams.push_back(makeBeam({-2,0,0},{2,0,0}, 0.6, {1,2,3}));
    beams.push_back(makeBeam({-2,0,0},{2,0,0}, 0.3, {4,0.5,1})); // nested overlap
    RTCPhotonBeamMap map = rtcNewPhotonBeamMap(device, beams.data(), beams.size());

    RTCBeamQuery q = makeQuery({0,0,-2},{0,0.2,1}, 0, 4, 0.9, 0.6);
    double rgb[3] = {0,0,0}, ref[3] = {0,0,0};
    TASK_CHECK(rtcIntegratePhotonBeams(map, &q, rgb, nullptr));
    referenceIntegrate(beams, q, ref);
    TASK_CHECK(closeVec(rgb, ref, 1e-9, 1e-11));

    // Tangent ray: distance to the axis equals the radius.
    RTCBeamQuery tangent = makeQuery({-2,0.6,0},{1,0,0}, 0, 4, 0.9, 0.6);
    double rgbT[3] = {7,7,7};
    TASK_CHECK(rtcIntegratePhotonBeams(map, &tangent, rgbT, nullptr));
    TASK_CHECK(rgbT[0] == 7.0); // untouched: zero-measure tangent

    // Very thin non-empty interval and near-zero extinction.
    RTCBeamQuery thin = makeQuery({0,0.5999999,-2},{0,0,1}, 0, 4, 1e-12, 1e-13);
    double rgbThin[3] = {0,0,0}, refThin[3] = {0,0,0};
    TASK_CHECK(rtcIntegratePhotonBeams(map, &thin, rgbThin, nullptr));
    referenceIntegrate(beams, thin, refThin);
    TASK_CHECK(closeVec(rgbThin, refThin, 1e-8, 1e-14));
    TASK_CHECK(rgbThin[0] > 0.0);
    rtcReleasePhotonBeamMap(map);
    rtcReleaseDevice(device);
    return 0;
  }

  int integralValidation()
  {
    RTCDevice device = rtcNewDevice("threads=1");

    RTCPhotonBeam badRadius = makeBeam({0,0,0},{1,0,0}, 0.0, {1,1,1});
    TASK_CHECK(rtcNewPhotonBeamMap(device, &badRadius, 1) == nullptr);
    TASK_CHECK(rtcGetDeviceError(device) == RTC_ERROR_INVALID_ARGUMENT);

    RTCPhotonBeam badLen = makeBeam({1,1,1},{1,1,1}, 0.1, {1,1,1});
    TASK_CHECK(rtcNewPhotonBeamMap(device, &badLen, 1) == nullptr);
    TASK_CHECK(rtcGetDeviceError(device) == RTC_ERROR_INVALID_ARGUMENT);

    RTCPhotonBeam badFlux = makeBeam({0,0,0},{1,0,0}, 0.1, {-1,1,1});
    TASK_CHECK(rtcNewPhotonBeamMap(device, &badFlux, 1) == nullptr);
    TASK_CHECK(rtcGetDeviceError(device) == RTC_ERROR_INVALID_ARGUMENT);

    RTCPhotonBeam nanBeam = makeBeam({0,0,0},{1,0,0}, 0.1, {1,1,1});
    nanBeam.to[1] = std::numeric_limits<double>::quiet_NaN();
    TASK_CHECK(rtcNewPhotonBeamMap(device, &nanBeam, 1) == nullptr);
    TASK_CHECK(rtcGetDeviceError(device) == RTC_ERROR_INVALID_ARGUMENT);

    TASK_CHECK(rtcNewPhotonBeamMap(device, nullptr, 3) == nullptr);
    TASK_CHECK(rtcGetDeviceError(device) == RTC_ERROR_INVALID_ARGUMENT);

    // Empty map is legal and integrates to no-op.
    RTCPhotonBeamMap empty = rtcNewPhotonBeamMap(device, nullptr, 0);
    TASK_CHECK(empty != nullptr);
    TASK_CHECK(rtcGetDeviceError(device) == RTC_ERROR_NONE);
    RTCBeamQuery q = makeQuery({0,0,0},{0,0,1}, 0, 1, 0.5, 0.25);
    double rgb[3] = {3,3,3};
    TASK_CHECK(rtcIntegratePhotonBeams(empty, &q, rgb, nullptr));
    TASK_CHECK(rgb[0] == 3.0 && rgb[1] == 3.0 && rgb[2] == 3.0);

    // Invalid queries: false, rgb untouched, error reported.
    RTCPhotonBeam good = makeBeam({-1,0,0},{1,0,0}, 0.2, {1,1,1});
    RTCPhotonBeamMap map = rtcNewPhotonBeamMap(device, &good, 1);
    TASK_CHECK(map != nullptr);
    RTCBeamQuery bad = q;
    bad.direction[0] = bad.direction[1] = bad.direction[2] = 0.0;
    double rgb2[3] = {5,5,5};
    TASK_CHECK(!rtcIntegratePhotonBeams(map, &bad, rgb2, nullptr));
    TASK_CHECK(rgb2[0] == 5.0);
    TASK_CHECK(rtcGetDeviceError(device) == RTC_ERROR_INVALID_ARGUMENT);

    RTCBeamQuery bad2 = q;
    bad2.sigma_s = bad2.sigma_t + 1.0;
    TASK_CHECK(!rtcIntegratePhotonBeams(map, &bad2, rgb2, nullptr));
    TASK_CHECK(rtcGetDeviceError(device) == RTC_ERROR_INVALID_ARGUMENT);

    RTCBeamQuery bad3 = q;
    bad3.tfar = -1.0;
    TASK_CHECK(!rtcIntegratePhotonBeams(map, &bad3, rgb2, nullptr));
    TASK_CHECK(rtcGetDeviceError(device) == RTC_ERROR_INVALID_ARGUMENT);

    rtcReleasePhotonBeamMap(map);
    rtcReleasePhotonBeamMap(empty);
    rtcReleaseDevice(device);
    return 0;
  }

  int integralLifetimeAndConcurrency()
  {
    RTCDevice device = rtcNewDevice("threads=2");

    // Deep copy: source data dies before any query.
    RTCPhotonBeamMap map;
    {
      std::vector<RTCPhotonBeam> local;
      for (size_t i = 0; i < 16; ++i)
        local.push_back(makeBeam({double(i)*2,0,0},{double(i)*2+1,0,0},
                                 0.2, {double(i+1),1,2}));
      map = rtcNewPhotonBeamMap(device, local.data(), local.size());
      TASK_CHECK(map != nullptr);
    }

    rtcRetainPhotonBeamMap(map);
    rtcReleasePhotonBeamMap(map); // map must stay alive

    // A second, independently built map queried concurrently as well.
    RTCPhotonBeamMap second = nullptr;
    double expectedSecond[3] = {0,0,0};
    {
      std::vector<RTCPhotonBeam> other;
      for (size_t i = 0; i < 12; ++i)
        other.push_back(makeBeam({double(i)*2.0+0.5, 1.0, 0.0},
                                 {double(i)*2.0+1.5, 1.0, 0.0},
                                 0.25, {2.0, double(i), 0.5}));
      second = rtcNewPhotonBeamMap(device, other.data(), other.size());
      TASK_CHECK(second != nullptr);
      const RTCBeamQuery q2 = makeQuery({1,1,-1},{0,0,1}, 0, 40, 0.4, 0.3);
      referenceIntegrate(other, q2, expectedSecond);
    }

    const RTCBeamQuery q = makeQuery({0,0,-1},{0,0,1}, 0, 40, 0.4, 0.3);
    const RTCBeamQuery q2 = makeQuery({1,1,-1},{0,0,1}, 0, 40, 0.4, 0.3);
    double expected[3] = {0,0,0};
    {
      std::vector<RTCPhotonBeam> rebuilt;
      for (size_t i = 0; i < 16; ++i)
        rebuilt.push_back(makeBeam({double(i)*2,0,0},{double(i)*2+1,0,0},
                                   0.2, {double(i+1),1,2}));
      referenceIntegrate(rebuilt, q, expected);
    }

    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
    {
      threads.emplace_back([&]()
      {
        for (int rep = 0; rep < 200; ++rep)
        {
          double rgb[3] = {0,0,0};
          RTCBeamStats stats;
          if (!rtcIntegratePhotonBeams(map, &q, rgb, &stats)) { failures++; continue; }
          if (!closeVec(rgb, expected, 1e-10, 1e-12)) failures++;
          if (stats.nodeVisits == 0 || stats.beamTests == 0) failures++;

          double rgb2[3] = {0,0,0};
          RTCBeamStats stats2;
          if (!rtcIntegratePhotonBeams(second, &q2, rgb2, &stats2)) { failures++; continue; }
          if (!closeVec(rgb2, expectedSecond, 1e-10, 1e-12)) failures++;
          if (stats2.nodeVisits == 0 || stats2.beamTests == 0) failures++;
        }
      });
    }
    for (auto& th : threads) th.join();
    TASK_CHECK(failures.load() == 0);

    rtcReleasePhotonBeamMap(second);
    rtcReleasePhotonBeamMap(map); // final release
    rtcReleaseDevice(device);
    return 0;
  }

  // ====================================================================
  // index group
  // ====================================================================

  int indexExhaustive()
  {
    RTCDevice device = rtcNewDevice("threads=1");
    std::vector<RTCPhotonBeam> beams;
    for (int i = 0; i < 300; ++i)
    {
      const double x = double(i) * 3.0;
      beams.push_back(makeBeam({x,0,0},{x+1,0,0}, 0.15,
                               {double(i+1), 0.1*double(i), 1.0}));
    }
    RTCPhotonBeamMap map = rtcNewPhotonBeamMap(device, beams.data(), beams.size());
    TASK_CHECK(map != nullptr);

    // Deterministic scattered rays; native result must match the
    // independent exhaustive enumeration for every one.
    uint64_t state = 0xABCDEF123456789ull;
    auto rnd = [&]()
    {
      state += 0x9E3779B97F4A7C15ull;
      uint64_t z = state;
      z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
      z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
      return double(z >> 11) / 9007199254740992.0;
    };

    for (int ray = 0; ray < 400; ++ray)
    {
      D3 O{900.0*(rnd()-0.5), 4.0*(rnd()-0.5), 4.0*(rnd()-0.5)};
      D3 d{rnd()-0.5, rnd()-0.5, rnd()-0.5};
      if (norm(d) < 1e-6) d = {1,0,0};
      RTCBeamQuery q = makeQuery(O, d, 0.0, 2000.0, 0.35, 0.2);
      double rgb[3] = {0,0,0}, ref[3] = {0,0,0};
      RTCBeamStats stats;
      TASK_CHECK(rtcIntegratePhotonBeams(map, &q, rgb, &stats));
      referenceIntegrate(beams, q, ref);
      TASK_CHECK(closeVec(rgb, ref, 1e-9, 1e-11));
      TASK_CHECK(stats.beamTests <= beams.size());
    }
    rtcReleasePhotonBeamMap(map);
    rtcReleaseDevice(device);
    return 0;
  }

  int indexLargeSampleCount()
  {
    constexpr size_t N = 4096;
    RTCDevice device = rtcNewDevice("threads=1");
    std::vector<RTCPhotonBeam> beams;
    beams.reserve(N);
    for (size_t i = 0; i < N; ++i)
    {
      const double x = double(i) * 4.0;
      beams.push_back(makeBeam({x,0,0},{x+1,0,0}, 0.1, {1,1,1}));
    }
    RTCPhotonBeamMap map = rtcNewPhotonBeamMap(device, beams.data(), beams.size());
    TASK_CHECK(map != nullptr);

    uint64_t totalTests = 0, totalIntervals = 0, totalVisits = 0;
    for (size_t i = 0; i < N; ++i)
    {
      const double cx = double(i) * 4.0 + 0.5;
      RTCBeamQuery q = makeQuery({cx,-5,0},{0,1,0}, 0.0, 10.0, 0.25, 0.1);
      double rgb[3] = {0,0,0}, ref[3] = {0,0,0};
      RTCBeamStats stats;
      TASK_CHECK(rtcIntegratePhotonBeams(map, &q, rgb, &stats));
      referenceIntegrate(beams, q, ref);
      TASK_CHECK(closeVec(rgb, ref, 1e-10, 1e-12));
      TASK_CHECK(stats.supportIntervals == 1);
      totalTests += stats.beamTests;
      totalIntervals += stats.supportIntervals;
      totalVisits += stats.nodeVisits;
    }
    std::printf("large_sample beams=%zu total_beam_tests=%llu budget=%llu "
                "intervals=%llu node_visits=%llu\n",
                N, (unsigned long long)totalTests,
                (unsigned long long)(32ull*N),
                (unsigned long long)totalIntervals,
                (unsigned long long)totalVisits);
    TASK_CHECK(totalTests <= 32ull * N);
    TASK_CHECK(totalIntervals == N);
    TASK_CHECK(totalVisits > 0);

    // Counters reset on every query.
    RTCBeamQuery miss = makeQuery({0,100,0},{1,0,0}, 0, 5, 0.25, 0.1);
    double rgbMiss[3] = {0,0,0};
    RTCBeamStats statsMiss;
    TASK_CHECK(rtcIntegratePhotonBeams(map, &miss, rgbMiss, &statsMiss));
    TASK_CHECK(statsMiss.supportIntervals == 0);
    TASK_CHECK(statsMiss.beamTests < N); // no hidden full scan
    rtcReleasePhotonBeamMap(map);
    rtcReleaseDevice(device);
    return 0;
  }

  int indexIdentityAndStats()
  {
    RTCDevice device = rtcNewDevice("threads=1");
    // Distinct per-channel flux makes leaf reordering detectable.
    std::vector<RTCPhotonBeam> beams;
    for (int i = 0; i < 64; ++i)
    {
      const double x = double(i) * 1.5;
      beams.push_back(makeBeam({x,0,0},{x+0.8,0,0}, 0.3,
                               {0.3*double(i+1), 0.05*double(i+1), 1.0}));
    }
    RTCPhotonBeamMap map = rtcNewPhotonBeamMap(device, beams.data(), beams.size());

    const RTCBeamQuery q = makeQuery({0,0.1,-1},{1,0,0}, 0, 120, 0.2, 0.15);
    double rgb[3] = {0,0,0}, ref[3] = {0,0,0};
    RTCBeamStats s1, s2;
    TASK_CHECK(rtcIntegratePhotonBeams(map, &q, rgb, &s1));
    referenceIntegrate(beams, q, ref);
    TASK_CHECK(closeVec(rgb, ref, 1e-10, 1e-12));

    // Repeated identical queries give identical counters; a second map
    // built from the same data gives the same answer.
    double rgbAgain[3] = {0,0,0};
    TASK_CHECK(rtcIntegratePhotonBeams(map, &q, rgbAgain, &s2));
    TASK_CHECK(closeVec(rgb, rgbAgain));
    TASK_CHECK(s1.nodeVisits == s2.nodeVisits);
    TASK_CHECK(s1.beamTests == s2.beamTests);
    TASK_CHECK(s1.supportIntervals == s2.supportIntervals);

    RTCPhotonBeamMap map2 = rtcNewPhotonBeamMap(device, beams.data(), beams.size());
    double rgb2[3] = {0,0,0};
    TASK_CHECK(rtcIntegratePhotonBeams(map2, &q, rgb2, nullptr));
    TASK_CHECK(closeVec(rgb, rgb2));
    rtcReleasePhotonBeamMap(map2);
    rtcReleasePhotonBeamMap(map);
    rtcReleaseDevice(device);
    return 0;
  }

  // ====================================================================
  // transport group
  // ====================================================================

  Renderer makeBasicRenderer(RTCDevice device, RTCScene scene,
                             const MediumBox& box, D3 lightPos,
                             double sigmaT, double sigmaS,
                             double reflectance, double radius,
                             unsigned maxEvents, uint64_t seed,
                             size_t w = 5, size_t h = 5)
  {
    PointLight light;
    light.position[0]=lightPos.x; light.position[1]=lightPos.y; light.position[2]=lightPos.z;
    light.power[0]=light.power[1]=light.power[2]=10.0;
    PerspectiveCamera cam = makeCamera({0,0,3.5}, {0,0,-1}, {0,1,0});
    MediumModel medium{sigmaT, sigmaS};
    SurfaceModel surface{{reflectance, reflectance, reflectance}};
    RenderParams params{seed, maxEvents, radius};
    return Renderer(device, scene, box, light, cam, medium, surface, w, h, params);
  }

  bool onBoxBoundary(D3 p, const MediumBox& box, double tol = 1e-6)
  {
    for (int axis = 0; axis < 3; ++axis)
    {
      if (std::fabs((&p.x)[axis] - (&box.lower[0])[axis]) < tol) return true;
      if (std::fabs((&p.x)[axis] - (&box.upper[0])[axis]) < tol) return true;
    }
    return false;
  }

  int transportVacuum()
  {
    RTCDevice device = rtcNewDevice("threads=1");
    MediumBox box{{-1,-1,-1},{1,1,1}};
    RTCScene scene = emptyScene(device);
    auto renderer = makeBasicRenderer(device, scene, box, {0,0,0},
                                      0.0, 0.0, 0.8, 0.05, 4, 42);
    TASK_CHECK(renderer.step(2000));
    const auto& diag = renderer.diagnostics();

    // No extinction: no volume collisions anywhere.
    for (const auto& ev : diag.events)
      TASK_CHECK(ev.kind != PHOTON_EVENT_VOLUME);

    size_t exits = 0;
    for (const auto& ev : diag.events)
      if (ev.kind == PHOTON_EVENT_BOX_EXIT) ++exits;
    TASK_CHECK(exits == diag.emissions);

    // Every exit event sits on the box boundary and flux is untouched.
    for (const auto& ev : diag.events)
    {
      if (ev.kind != PHOTON_EVENT_BOX_EXIT) continue;
      D3 p{ev.position[0], ev.position[1], ev.position[2]};
      TASK_CHECK(onBoxBoundary(p, box));
      TASK_CHECK(close(ev.flux[0], 10.0, 1e-12, 1e-10));
    }

    // Every photon index 0..N-1 appears in the event stream.
    std::vector<char> seen(diag.emissions, 0);
    for (const auto& ev : diag.events) seen[ev.photonIndex] = 1;
    for (char c : seen) TASK_CHECK(c == 1);

    // Vacuum segments never enter the pool, yet emissions count.
    TASK_CHECK(diag.beamCount == 0);
    TASK_CHECK(renderer.totalEmissions() == 2000);
    double rgb[3] = {9,9,9};
    renderer.pixel(2, 2, rgb);
    TASK_CHECK(rgb[0] == 0.0 && rgb[1] == 0.0 && rgb[2] == 0.0);

    rtcReleaseScene(scene);
    rtcReleaseDevice(device);
    return 0;
  }

  int transportMirror()
  {
    RTCDevice device = rtcNewDevice("threads=1");
    MediumBox box{{-1,-1,-1},{1,1,1}};
    RTCScene scene = closedMirrorBox(device, box);
    // Vacuum inside the box: the first event of every photon is a wall
    // reflection, so surface accounting is exact rather than stochastic.
    auto renderer = makeBasicRenderer(device, scene, box, {0,0,0},
                                      0.0, 0.0, 0.7, 0.05, 1, 77);
    TASK_CHECK(renderer.step(3000));
    const auto& diag = renderer.diagnostics();

    size_t surfaces = 0;
    for (const auto& ev : diag.events)
      if (ev.kind == PHOTON_EVENT_SURFACE) ++surfaces;
    TASK_CHECK(surfaces == diag.emissions);

    for (const auto& ev : diag.events)
    {
      if (ev.kind != PHOTON_EVENT_SURFACE) continue;
      // Hit point lies on one of the mirror walls.
      D3 p{ev.position[0], ev.position[1], ev.position[2]};
      const bool onWall =
        std::fabs(std::fabs(p.x) - 0.98) < 1e-6 ||
        std::fabs(std::fabs(p.y) - 0.98) < 1e-6 ||
        std::fabs(std::fabs(p.z) - 0.98) < 1e-6;
      TASK_CHECK(onWall);
      // Reflectance applied once, per channel.
      TASK_CHECK(close(ev.flux[0], 10.0 * 0.7, 1e-12, 1e-10));
      TASK_CHECK(close(ev.flux[1], 10.0 * 0.7, 1e-12, 1e-10));
      TASK_CHECK(close(ev.flux[2], 10.0 * 0.7, 1e-12, 1e-10));
      // Specular law: reflected direction is unit length and points
      // back into the box.
      D3 d{ev.direction[0], ev.direction[1], ev.direction[2]};
      TASK_CHECK(close(norm(d), 1.0, 1e-12, 1e-10));
    }

    // maxEvents = 1: each path terminates after its surface event.
    size_t terms = 0;
    for (const auto& ev : diag.events)
      if (ev.kind == PHOTON_EVENT_TERMINATE) ++terms;
    TASK_CHECK(terms == diag.emissions);

    rtcReleaseScene(scene);
    rtcReleaseDevice(device);
    return 0;
  }

  int transportVolume()
  {
    RTCDevice device = rtcNewDevice("threads=1");
    MediumBox box{{-20,-20,-20},{20,20,20}};
    RTCScene scene = emptyScene(device);
    const double sigmaT = 1.0, sigmaS = 0.4;
    auto renderer = makeBasicRenderer(device, scene, box, {0,0,0},
                                      sigmaT, sigmaS, 0.8, 0.05, 8, 123);
    TASK_CHECK(renderer.step(20000));
    const auto& diag = renderer.diagnostics();

    // Index events and beams by (photon, event index).
    std::vector<std::vector<EventRecord>> byPhoton(diag.emissions);
    for (const auto& ev : diag.events) byPhoton[ev.photonIndex].push_back(ev);

    size_t volumes = 0;
    for (const auto& ev : diag.events)
    {
      if (ev.kind != PHOTON_EVENT_VOLUME) continue;
      ++volumes;
      D3 d{ev.direction[0], ev.direction[1], ev.direction[2]};
      TASK_CHECK(close(norm(d), 1.0, 1e-12, 1e-10));
      // First volume event per photon: flux = power * albedo exactly.
      if (ev.eventIndex == 1)
        TASK_CHECK(close(ev.flux[0], 10.0 * sigmaS / sigmaT, 1e-12, 1e-10));
    }
    TASK_CHECK(volumes > 0);

    // Independent free-path law check: interior first collisions are
    // Exp(sigmaT), mean 1/sigmaT (the huge box makes truncation rare).
    double meanFree = 0.0;
    size_t interior = 0;
    for (const auto& ev : diag.events)
    {
      if (ev.kind != PHOTON_EVENT_VOLUME || ev.eventIndex != 1) continue;
      D3 p{ev.position[0], ev.position[1], ev.position[2]};
      const double s = norm(p);
      if (s < 5.0) { meanFree += s; ++interior; }
    }
    meanFree /= double(interior);
    TASK_CHECK(interior > 1000);
    TASK_CHECK(std::fabs(meanFree - 1.0 / sigmaT) < 0.05);

    // Beam audit against the event stream of the producing photon.
    // Segment k starts after k volume events: its flux is the photon
    // flux after exactly k albedo multiplications, and its start point
    // is either the light (k=0) or a recorded event position.
    TASK_CHECK(diag.beamCount >= diag.emissions);
    const double albedo = sigmaS / sigmaT;
    for (const auto& bd : diag.beams)
    {
      const unsigned k = bd.eventIndex;
      const double expectedFlux = 10.0 * std::pow(albedo, double(k));
      TASK_CHECK(close(bd.beam.flux[0], expectedFlux, 1e-12, 1e-10));
      TASK_CHECK(close(bd.beam.flux[1], expectedFlux, 1e-12, 1e-10));

      const D3 start = beamFrom(bd.beam);
      const D3 end = beamTo(bd.beam);
      TASK_CHECK(norm(end - start) > 0.0);

      bool startOk = (k == 0) ? (norm(start - D3{0,0,0}) < 1e-9) : false;
      bool endOk = false;
      for (const auto& ev : byPhoton[bd.photonIndex])
      {
        D3 p{ev.position[0], ev.position[1], ev.position[2]};
        if (k != 0 && ev.eventIndex == k &&
            (ev.kind == PHOTON_EVENT_VOLUME || ev.kind == PHOTON_EVENT_SURFACE))
          startOk = startOk || norm(start - p) < 1e-9;
        if (ev.kind == PHOTON_EVENT_VOLUME ||
            ev.kind == PHOTON_EVENT_BOX_EXIT ||
            ev.kind == PHOTON_EVENT_SURFACE)
          endOk = endOk || norm(end - p) < 1e-9;
      }
      TASK_CHECK(startOk);
      TASK_CHECK(endOk);
    }
    rtcReleaseScene(scene);
    rtcReleaseDevice(device);
    return 0;
  }

  int transportDenominator()
  {
    RTCDevice device = rtcNewDevice("threads=1");
    MediumBox box{{-1,-1,-1},{1,1,1}};
    RTCScene scene = emptyScene(device);
    // Weak extinction in a small box: every in-box segment stores a
    // beam (including exit-truncated ones), and no photon can store
    // more than maxEvents of them, while every emission still counts.
    auto renderer = makeBasicRenderer(device, scene, box, {0,0,0},
                                      0.05, 0.05, 0.8, 0.02, 2, 999);
    TASK_CHECK(renderer.step(1234));
    TASK_CHECK(renderer.totalEmissions() == 1234);
    TASK_CHECK(renderer.diagnostics().emissions == 1234);
    const size_t produced = renderer.diagnostics().beamCount;
    TASK_CHECK(produced >= 1234); // each photon keeps at least one segment
    TASK_CHECK(produced <= 2 * 1234); // at most maxEvents beams per photon
    std::vector<unsigned> perPhoton(1234, 0);
    for (const auto& bd : renderer.diagnostics().beams)
    {
      TASK_CHECK(bd.photonIndex < 1234);
      ++perPhoton[bd.photonIndex];
    }
    for (unsigned count : perPhoton)
      TASK_CHECK(count >= 1 && count <= 2);
    rtcReleaseScene(scene);
    rtcReleaseDevice(device);
    return 0;
  }

  int transportSingleScattering()
  {
    RTCDevice device = rtcNewDevice("threads=1");
    const MediumBox box{{-1,-1,-1},{1,1,1}};
    RTCScene scene = emptyScene(device);
    const double sigmaT = 0.6, sigmaS = 0.6, power = 10.0;
    const uint64_t seeds[4] = { 11, 47, 1009, 8675309 };

    // Independent 1D reference: isotropic point light, isotropic phase,
    // single scattering along the center eye ray inside the box. The
    // eye is inside the box, so the renderer integrates from t=0 up to
    // the far wall at t=1.99 (eye x=-0.99 looking towards +x).
    //   L = Phi*sigma_s/(16*pi^2) * integral exp(-sigma_t*(t+d))/d^2 dt
    D3 eye{-0.99, 0.3, 0.0};
    const double tEnter = 0.0;
    const double tExit = 1.99;
    const double reference =
      power * sigmaS / (16.0 * PI * PI) *
      adaptiveSimpson([&](double t)
      {
        const double x = eye.x + t;
        const double d = std::sqrt(x*x + 0.3*0.3);
        return std::exp(-sigmaT * (t + d)) / (d*d);
      }, tEnter, tExit, 1e-11);

    const double perSeedTol = 0.08; // predeclared MC tolerances
    const double meanTol = 0.04;
    double estimateSum = 0.0;

    for (uint64_t seed : seeds)
    {
      PointLight light;
      light.position[0]=0; light.position[1]=0; light.position[2]=0;
      light.power[0]=light.power[1]=light.power[2]=power;
      PerspectiveCamera cam = makeCamera(eye, {1,0,0}, {0,0,1}, 0.4);
      MediumModel medium{sigmaT, sigmaS};
      SurfaceModel surface{{0.8,0.8,0.8}};
      RenderParams params{seed, 1, 0.05};
      Renderer renderer(device, scene, box, light, cam, medium, surface,
                        5, 5, params);
      TASK_CHECK(renderer.step(100000));
      double rgb[3];
      renderer.pixel(2, 2, rgb);
      TASK_CHECK(rgb[0] > 0.0 && rgb[1] > 0.0 && rgb[2] > 0.0);
      TASK_CHECK(close(rgb[0], rgb[1], 1e-12, 1e-12)); // white light, grey medium

      const double bias = std::fabs(rgb[0] - reference) / reference;
      std::printf("single_scatter seed=%llu samples=100000 estimate=%.6f "
                  "reference=%.6f rel_bias=%.4f tolerance=%.2f\n",
                  (unsigned long long)seed, rgb[0], reference, bias, perSeedTol);
      TASK_CHECK(bias <= perSeedTol);
      estimateSum += rgb[0];
    }

    const double meanEstimate = estimateSum / 4.0;
    const double meanBias = std::fabs(meanEstimate - reference) / reference;
    std::printf("single_scatter mean estimate=%.6f reference=%.6f "
                "rel_bias=%.4f tolerance=%.2f\n",
                meanEstimate, reference, meanBias, meanTol);
    TASK_CHECK(meanBias <= meanTol);

    rtcReleaseScene(scene);
    rtcReleaseDevice(device);
    return 0;
  }

  // ====================================================================
  // progressive group
  // ====================================================================

  std::unique_ptr<Renderer> makeProgRenderer(RTCDevice device, RTCScene scene,
                                             uint64_t seed, double radius)
  {
    MediumBox box{{-1,-1,-1},{1,1,1}};
    PointLight light;
    light.position[0]=light.position[1]=light.position[2]=0;
    light.power[0]=light.power[1]=light.power[2]=6.0;
    PerspectiveCamera cam = makeCamera({0,0,2.5}, {0,0,-1}, {0,1,0}, 0.8);
    MediumModel medium{0.7, 0.5};
    SurfaceModel surface{{0.8,0.8,0.8}};
    RenderParams params{seed, 4, radius};
    return std::unique_ptr<Renderer>(new Renderer(
      device, scene, box, light, cam, medium, surface, 7, 5, params));
  }

  int progressiveStepZero()
  {
    RTCDevice device = rtcNewDevice("threads=1");
    MediumBox box{{-1,-1,-1},{1,1,1}};
    RTCScene scene = emptyScene(device);
    auto renderer = makeProgRenderer(device, scene, 7, 0.05);
    TASK_CHECK(renderer->step(100));
    const auto filmBefore = renderer->film();
    const uint64_t emissionsBefore = renderer->totalEmissions();
    TASK_CHECK(!renderer->step(0));
    TASK_CHECK(renderer->rounds() == 1);
    TASK_CHECK(renderer->totalEmissions() == emissionsBefore);
    TASK_CHECK(renderer->film() == filmBefore);
    rtcReleaseScene(scene);
    rtcReleaseDevice(device);
    return 0;
  }

  int progressiveRoundAccumulation()
  {
    RTCDevice device = rtcNewDevice("threads=1");
    MediumBox box{{-1,-1,-1},{1,1,1}};
    RTCScene scene = emptyScene(device);
    auto renderer = makeProgRenderer(device, scene, 7, 0.05);

    std::vector<double> expectedTotal(renderer->film().size(), 0.0);
    for (size_t round = 0; round < 3; ++round)
    {
      TASK_CHECK(renderer->step(1000 + 250 * round));
      const auto& roundFilm = renderer->roundFilm();
      for (size_t i = 0; i < expectedTotal.size(); ++i)
        expectedTotal[i] += roundFilm[i];
      TASK_CHECK(renderer->film() == expectedTotal);

      // The round film is exactly an independent query against the
      // current round's map on the center ray.
      double center[3], independent[3] = {0,0,0};
      renderer->roundPixel(3, 2, center);
      // Camera at z=2.5 looking towards -z: the box spans t in [1.5,3.5].
      RTCBeamQuery q = makeQuery({0,0,2.5},{0,0,-1}, 1.5, 3.5, 0.7, 0.5);
      RTCBeamStats stats;
      TASK_CHECK(rtcIntegratePhotonBeams(renderer->beamMap(), &q, independent, &stats));
      TASK_CHECK(closeVec(center, independent, 1e-12, 1e-14));
      TASK_CHECK(stats.nodeVisits + stats.beamTests > 0);
    }
    TASK_CHECK(renderer->totalEmissions() == 1000 + 1250 + 1500);
    TASK_CHECK(renderer->rounds() == 3);
    rtcReleaseScene(scene);
    rtcReleaseDevice(device);
    return 0;
  }

  int progressiveRadiusShrink()
  {
    RTCDevice device = rtcNewDevice("threads=1");
    MediumBox box{{-1,-1,-1},{1,1,1}};
    RTCScene scene = emptyScene(device);
    const double r0 = 0.06;
    auto renderer = makeProgRenderer(device, scene, 7, r0);

    double previous = std::numeric_limits<double>::infinity();
    for (size_t round = 0; round < 3; ++round)
    {
      TASK_CHECK(renderer->step(500));
      const double expected = r0 * std::pow(double(round + 1), -1.0 / 6.0);
      TASK_CHECK(close(renderer->currentRadius(), expected, 1e-12, 1e-14));
      TASK_CHECK(renderer->diagnostics().beams.size() > 0);
      for (const auto& bd : renderer->diagnostics().beams)
        TASK_CHECK(close(bd.beam.radius, expected, 1e-12, 1e-14));
      TASK_CHECK(renderer->currentRadius() < previous);
      previous = renderer->currentRadius();
    }
    rtcReleaseScene(scene);
    rtcReleaseDevice(device);
    return 0;
  }

  int progressiveOldPoolReplaced()
  {
    RTCDevice device = rtcNewDevice("threads=1");
    MediumBox box{{-1,-1,-1},{1,1,1}};
    RTCScene scene = emptyScene(device);
    auto renderer = makeProgRenderer(device, scene, 7, 0.05);

    TASK_CHECK(renderer->step(800));
    const size_t firstRoundBeams = renderer->diagnostics().beamCount;
    double firstCenter[3];
    renderer->roundPixel(3, 2, firstCenter);

    TASK_CHECK(renderer->step(800));
    // Round two's result is produced from the fresh map alone: it must
    // not be the round-one result repeated, and the map only holds the
    // current round's beam count.
    double secondCenter[3];
    renderer->roundPixel(3, 2, secondCenter);

    // The live map only answers for round two's pool: a fresh query
    // must reproduce the round-two film alone, never the pooled sum.
    double liveQuery[3] = {0,0,0};
    RTCBeamQuery live = makeQuery({0,0,2.5},{0,0,-1}, 1.5, 3.5, 0.7, 0.5);
    TASK_CHECK(rtcIntegratePhotonBeams(renderer->beamMap(), &live, liveQuery, nullptr));
    TASK_CHECK(closeVec(liveQuery, secondCenter, 1e-12, 1e-14));
    TASK_CHECK(!closeVec(liveQuery, firstCenter, 1e-9, 1e-12) ||
               firstRoundBeams != renderer->diagnostics().beamCount);

    // pixel() divides by the cumulative emission count, and the film is
    // the sum of exactly the two round films.
    double normalized[3];
    renderer->pixel(3, 2, normalized);
    const double scale = 1.0 / double(renderer->totalEmissions());
    const double raw = firstCenter[0] + secondCenter[0];
    TASK_CHECK(close(normalized[0], raw * scale, 1e-12, 1e-14));
    rtcReleaseScene(scene);
    rtcReleaseDevice(device);
    return 0;
  }

  int progressiveInterleave()
  {
    RTCDevice device = rtcNewDevice("threads=1");
    MediumBox box{{-1,-1,-1},{1,1,1}};
    RTCScene sceneA = emptyScene(device);
    RTCScene sceneB = emptyScene(device);
    RTCScene sceneC = emptyScene(device);
    auto a = makeProgRenderer(device, sceneA, 555, 0.05);
    auto b = makeProgRenderer(device, sceneB, 555, 0.05);
    auto c = makeProgRenderer(device, sceneC, 555, 0.05);

    // Interleaved execution of a and b; c runs the same steps back to
    // back. Results depend only on (seed, round, photon index).
    const size_t schedule[4] = { 500, 700, 500, 900 };
    for (size_t round = 0; round < 4; ++round)
    {
      TASK_CHECK(a->step(schedule[round]));
      TASK_CHECK(c->step(schedule[round]));
      TASK_CHECK(b->step(schedule[round]));

      for (size_t y = 0; y < a->height(); ++y)
        for (size_t x = 0; x < a->width(); ++x)
        {
          double pa[3], pb[3], pc[3];
          a->pixel(x, y, pa);
          b->pixel(x, y, pb);
          c->pixel(x, y, pc);
          TASK_CHECK(closeVec(pa, pb, 1e-12, 1e-16));
          TASK_CHECK(closeVec(pa, pc, 1e-12, 1e-16));
        }
      TASK_CHECK(a->diagnostics().beamCount == b->diagnostics().beamCount);
      TASK_CHECK(a->diagnostics().beamCount == c->diagnostics().beamCount);
    }
    TASK_CHECK(a->totalEmissions() == b->totalEmissions());
    TASK_CHECK(a->totalEmissions() == c->totalEmissions());

    rtcReleaseScene(sceneA);
    rtcReleaseScene(sceneB);
    rtcReleaseScene(sceneC);
    rtcReleaseDevice(device);
    return 0;
  }
}

const task_case task_cases[] =
{
  { "integral", "basic_ray",               integralBasic },
  { "integral", "nonunit_and_caps",        integralNonUnitAndCaps },
  { "integral", "parallel_rays",           integralParallel },
  { "integral", "overlap_tangent_thin",    integralOverlapTangentThin },
  { "integral", "validation",              integralValidation },
  { "integral", "lifetime_concurrency",    integralLifetimeAndConcurrency },

  { "index", "exhaustive_brute_force",     indexExhaustive },
  { "index", "large_sample_counters",      indexLargeSampleCount },
  { "index", "identity_and_stats",         indexIdentityAndStats },

  { "transport", "vacuum",                 transportVacuum },
  { "transport", "mirror",                 transportMirror },
  { "transport", "volume_collision",       transportVolume },
  { "transport", "emission_denominator",   transportDenominator },
  { "transport", "single_scattering_1d",   transportSingleScattering },

  { "progressive", "step_zero",            progressiveStepZero },
  { "progressive", "round_accumulation",   progressiveRoundAccumulation },
  { "progressive", "radius_shrink",        progressiveRadiusShrink },
  { "progressive", "old_pool_replaced",    progressiveOldPoolReplaced },
  { "progressive", "interleaved_instances", progressiveInterleave },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
