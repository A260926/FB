// Copyright 2009-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

// Regression tests for RTCInstanceExclusion. No main(): task_support/driver.c
// drives the registered task_cases.

#include <embree4/rtcore.h>

#include "task_support/check.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

namespace {

const unsigned INV = RTC_INVALID_GEOMETRY_ID;

// Geometry IDs used by the diamond fixture.
const unsigned ROOT_TRI = 100;
const unsigned IA = 101;       // regular (static) instance in root
const unsigned IB = 102;       // regular motion-blur instance in root
const unsigned IC = 103;       // instance array (motion blur) in root
const unsigned MID_USER = 200; // user geometry inside a dedicated test scene
const unsigned JA = 201;       // regular instance inside every mid scene
const unsigned JC = 202;       // instance array inside every mid scene
const unsigned LEAF_TRI = 300; // triangle inside the shared leaf scene

std::atomic<int> g_userIntersectCalls;
std::atomic<int> g_userOccludedCalls;
std::atomic<int> g_filterCalls;

void boundsFunction(const RTCBoundsFunctionArguments* args)
{
  RTCBounds* b = args->bounds_o;
  const float z = *(const float*)args->geometryUserPtr;
  b->lower_x = -1.0f; b->lower_y = -1.0f; b->lower_z = z - 0.25f;
  b->upper_x =  1.0f; b->upper_y =  1.0f; b->upper_z = z + 0.25f;
}

void userIntersect(const RTCIntersectFunctionNArguments* args)
{
  ++g_userIntersectCalls;
  RTCRayHit* rh = (RTCRayHit*)args->rayhit;
  const float z = *(const float*)args->geometryUserPtr;
  const float t = (z - rh->ray.org_z) / rh->ray.dir_z;
  if (t >= rh->ray.tnear && t <= rh->ray.tfar) {
    rh->ray.tfar = t;
    rh->hit.Ng_x = 0; rh->hit.Ng_y = 0; rh->hit.Ng_z = 1;
    rh->hit.u = 0.0f; rh->hit.v = 0.0f;
    rh->hit.primID = 0;
    rh->hit.geomID = args->geomID;
  }
}

void userOccluded(const RTCOccludedFunctionNArguments* args)
{
  ++g_userOccludedCalls;
  RTCRay* ray = (RTCRay*)args->ray;
  const float z = *(const float*)args->geometryUserPtr;
  const float t = (z - ray->org_z) / ray->dir_z;
  if (t >= ray->tnear && t <= ray->tfar) ray->tfar = -1.0f;
}

void argumentFilter(const RTCFilterFunctionNArguments* args)
{
  ++g_filterCalls;
  (void)args;
}

RTCGeometry makeTriangle(RTCDevice device)
{
  RTCGeometry g = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_TRIANGLE);
  float* v = (float*)rtcSetNewGeometryBuffer(g, RTC_BUFFER_TYPE_VERTEX, 0,
                                             RTC_FORMAT_FLOAT3, 3 * sizeof(float), 3);
  const float verts[9] = {-1, -1, 0,  1, -1, 0,  0, 1, 0};
  std::memcpy(v, verts, sizeof(verts));
  unsigned* idx = (unsigned*)rtcSetNewGeometryBuffer(g, RTC_BUFFER_TYPE_INDEX, 0,
                                                     RTC_FORMAT_UINT3, 3 * sizeof(unsigned), 1);
  idx[0] = 0; idx[1] = 1; idx[2] = 2;
  rtcSetGeometryEnableFilterFunctionFromArguments(g, true);
  rtcCommitGeometry(g);
  return g;
}

RTCGeometry makeUserGeometry(RTCDevice device, unsigned geomID, float localZ)
{
  RTCGeometry g = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_USER);
  float* plane = new float(localZ); // freed with the device lifetime of the test process
  rtcSetGeometryUserData(g, plane);
  rtcSetGeometryUserPrimitiveCount(g, 1);
  rtcSetGeometryBoundsFunction(g, boundsFunction, nullptr);
  rtcSetGeometryIntersectFunction(g, userIntersect);
  rtcSetGeometryOccludedFunction(g, userOccluded);
  rtcCommitGeometry(g);
  (void)geomID;
  return g;
}

void rowMajorTranslation(float* m12, float tx, float ty, float tz)
{
  const float m[12] = {
    1,0,0,tx,
    0,1,0,ty,
    0,0,1,tz
  };
  std::memcpy(m12, m, sizeof(m));
}

RTCGeometry makeInstance(RTCDevice device, RTCScene child, float tz, bool motionBlur)
{
  RTCGeometry g = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_INSTANCE);
  rtcSetGeometryInstancedScene(g, child);
  if (motionBlur) rtcSetGeometryTimeStepCount(g, 2);
  const unsigned steps = motionBlur ? 2u : 1u;
  for (unsigned s = 0; s < steps; ++s) {
    float m[12]; rowMajorTranslation(m, 0, 0, tz);
    rtcSetGeometryTransform(g, s, RTC_FORMAT_FLOAT3X4_ROW_MAJOR, m);
  }
  rtcCommitGeometry(g);
  return g;
}

RTCGeometry makeInstanceArray(RTCDevice device, RTCScene child, const float* shifts,
                               unsigned count, bool motionBlur)
{
  RTCGeometry g = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_INSTANCE_ARRAY);
  rtcSetGeometryInstancedScene(g, child);
  if (motionBlur) rtcSetGeometryTimeStepCount(g, 2);
  const unsigned steps = motionBlur ? 2u : 1u;
  for (unsigned s = 0; s < steps; ++s) {
    float* buf = (float*)rtcSetNewGeometryBuffer(
      g, RTC_BUFFER_TYPE_TRANSFORM, s, RTC_FORMAT_FLOAT3X4_ROW_MAJOR,
      12 * sizeof(float), count);
    for (unsigned i = 0; i < count; ++i)
      rowMajorTranslation(buf + 12 * i, 0, 0, shifts[i]);
  }
  rtcCommitGeometry(g);
  return g;
}

// Diamond-shaped graph: one shared leaf scene is reached through regular
// instances and instance arrays from several root placements.
struct Fixture {
  RTCDevice device = nullptr;
  RTCScene leaf = nullptr;   // shared sub scene
  RTCScene midA = nullptr;
  RTCScene midB = nullptr;
  RTCScene midC = nullptr;
  RTCScene root = nullptr;

  int build()
  {
    device = rtcNewDevice("threads=1");
    TASK_CHECK(device != nullptr);

    // Shared leaf: one triangle at local z=1.
    leaf = rtcNewScene(device);
    rtcSetSceneFlags(leaf, RTC_SCENE_FLAG_FILTER_FUNCTION_IN_ARGUMENTS);
    RTCGeometry tri = makeTriangle(device);
    rtcAttachGeometryByID(leaf, tri, LEAF_TRI);
    rtcReleaseGeometry(tri);
    rtcCommitScene(leaf);

    midA = rtcNewScene(device);
    rtcSetSceneFlags(midA, RTC_SCENE_FLAG_FILTER_FUNCTION_IN_ARGUMENTS);
    RTCGeometry aja = makeInstance(device, leaf, 2.0f, false);
    rtcAttachGeometryByID(midA, aja, JA);
    rtcReleaseGeometry(aja);
    const float midArrayShifts[2] = {4.0f, 6.0f};
    RTCGeometry ajc = makeInstanceArray(device, leaf, midArrayShifts, 2, false);
    rtcAttachGeometryByID(midA, ajc, JC);
    rtcReleaseGeometry(ajc);
    rtcCommitScene(midA);

    // midB/midC reuse the same leaf scene through identical layouts. The
    // motion-blur instances carry identical transforms at both time steps.
    // They deliberately hold only instances: exclusion rules address
    // instance paths, so a non-instance primitive here could not be blocked
    // and would defeat per-route isolation.
    midB = rtcNewScene(device);
    rtcSetSceneFlags(midB, RTC_SCENE_FLAG_FILTER_FUNCTION_IN_ARGUMENTS);
    RTCGeometry bja = makeInstance(device, leaf, 2.0f, false);
    rtcAttachGeometryByID(midB, bja, JA);
    rtcReleaseGeometry(bja);
    RTCGeometry bjc = makeInstanceArray(device, leaf, midArrayShifts, 2, false);
    rtcAttachGeometryByID(midB, bjc, JC);
    rtcReleaseGeometry(bjc);
    rtcCommitScene(midB);

    midC = rtcNewScene(device);
    rtcSetSceneFlags(midC, RTC_SCENE_FLAG_FILTER_FUNCTION_IN_ARGUMENTS);
    RTCGeometry cja = makeInstance(device, leaf, 2.0f, false);
    rtcAttachGeometryByID(midC, cja, JA);
    rtcReleaseGeometry(cja);
    RTCGeometry cjc = makeInstanceArray(device, leaf, midArrayShifts, 2, false);
    rtcAttachGeometryByID(midC, cjc, JC);
    rtcReleaseGeometry(cjc);
    rtcCommitScene(midC);

    root = rtcNewScene(device);
    rtcSetSceneFlags(root, RTC_SCENE_FLAG_FILTER_FUNCTION_IN_ARGUMENTS);

    RTCGeometry rootTri = makeTriangle(device);
    rtcAttachGeometryByID(root, rootTri, ROOT_TRI);
    rtcReleaseGeometry(rootTri);

    RTCGeometry iA = makeInstance(device, midA, 4.0f, false);
    rtcAttachGeometryByID(root, iA, IA);
    rtcReleaseGeometry(iA);

    RTCGeometry iB = makeInstance(device, midB, 6.0f, true);
    rtcAttachGeometryByID(root, iB, IB);
    rtcReleaseGeometry(iB);

    const float rootArrayShifts[2] = {8.0f, 10.0f};
    RTCGeometry iC = makeInstanceArray(device, midC, rootArrayShifts, 2, true);
    rtcAttachGeometryByID(root, iC, IC);
    rtcReleaseGeometry(iC);

    rtcCommitScene(root);
    return 0;
  }

  void release()
  {
    if (root) rtcReleaseScene(root);
    if (midA) rtcReleaseScene(midA);
    if (midB) rtcReleaseScene(midB);
    if (midC) rtcReleaseScene(midC);
    if (leaf) rtcReleaseScene(leaf);
    if (device) rtcReleaseDevice(device);
  }
};

RTCRayHit makeRay(float ox, float oy, float oz,
                  float dx, float dy, float dz,
                  float tnear, float tfar, unsigned id,
                  unsigned mask = ~0u, float time = 0.0f)
{
  RTCRayHit rh{};
  rh.ray.org_x = ox; rh.ray.org_y = oy; rh.ray.org_z = oz; rh.ray.tnear = tnear;
  rh.ray.dir_x = dx; rh.ray.dir_y = dy; rh.ray.dir_z = dz; rh.ray.time = time;
  rh.ray.tfar = tfar;
  rh.ray.mask = mask;
  rh.ray.id = id;
  rh.ray.flags = 0;
  rh.hit.geomID = INV;
  for (unsigned l = 0; l < RTC_MAX_INSTANCE_LEVEL_COUNT; ++l) {
    rh.hit.instID[l] = INV;
    rh.hit.instPrimID[l] = INV;
  }
  return rh;
}

RTCInstanceExclusionPath edge(unsigned geomID, unsigned instPrimID)
{
  RTCInstanceExclusionPath p{geomID, instPrimID};
  return p;
}

RTCInstanceExclusion makeHandle(RTCDevice device,
                                const std::vector<std::vector<RTCInstanceExclusionPath>>& paths)
{
  std::vector<const RTCInstanceExclusionPath*> ptrs;
  std::vector<size_t> lengths;
  ptrs.reserve(paths.size());
  lengths.reserve(paths.size());
  for (const auto& p : paths) {
    ptrs.push_back(p.empty() ? nullptr : p.data());
    lengths.push_back(p.size());
  }
  return rtcNewInstanceExclusion(device, ptrs.data(), lengths.data(), paths.size());
}

// One fully expanded leaf-triangle route through the diamond.
struct Route {
  std::vector<RTCInstanceExclusionPath> path;
  float worldT;
  float tnear;
  unsigned instID[RTC_MAX_INSTANCE_LEVEL_COUNT];
  unsigned instPrim[RTC_MAX_INSTANCE_LEVEL_COUNT];
};

Route leafRoute(unsigned rootGeom, unsigned rootPrim, float rootShift,
                unsigned midGeom, unsigned midPrim, float worldT)
{
  Route r{};
  r.path = {edge(rootGeom, rootPrim), edge(midGeom, midPrim)};
  r.worldT = worldT;
  r.tnear = rootShift + 1.0f; // skip the mid-scene user slab at root+0.5
  for (unsigned l = 0; l < RTC_MAX_INSTANCE_LEVEL_COUNT; ++l) {
    r.instID[l] = INV;
    r.instPrim[l] = INV;
  }
  // Recorded hit identity from the instance stack: each level's prim id is
  // the array element number at that level (0 for a regular instance).
  r.instID[0] = rootGeom; r.instPrim[0] = (rootPrim == INV ? 0u : rootPrim);
  r.instID[1] = midGeom;  r.instPrim[1] = (midPrim == INV ? 0u : midPrim);
  return r;
}

int expectNoDeviceError(RTCDevice device)
{
  RTCError err = rtcGetDeviceError(device);
  TASK_CHECK(err == RTC_ERROR_NONE);
  return 0;
}

// Deterministic two-level chain: root instance array (geom ROOT_AR, two
// elements) instantiates mid, whose instance array (geom MID_AR, two
// elements) instantiates a leaf triangle. No sibling instance geometries
// exist, so per-query transition counts are exactly determined.
const unsigned CHAIN_ROOT_AR = 401;
const unsigned CHAIN_MID_AR = 402;
const unsigned CHAIN_LEAF_TRI = 403;

struct ChainScene {
  RTCDevice device = nullptr;
  RTCScene leaf = nullptr;
  RTCScene mid = nullptr;
  RTCScene root = nullptr;

  int build(unsigned rootMask = ~0u)
  {
    device = rtcNewDevice("threads=1");
    TASK_CHECK(device != nullptr);

    leaf = rtcNewScene(device);
    RTCGeometry tri = makeTriangle(device);
    rtcAttachGeometryByID(leaf, tri, CHAIN_LEAF_TRI);
    rtcReleaseGeometry(tri);
    rtcCommitScene(leaf);

    mid = rtcNewScene(device);
    const float midShifts[2] = {3.0f, 5.0f};
    RTCGeometry midAr = makeInstanceArray(device, leaf, midShifts, 2, false);
    rtcAttachGeometryByID(mid, midAr, CHAIN_MID_AR);
    rtcReleaseGeometry(midAr);
    rtcCommitScene(mid);

    root = rtcNewScene(device);
    const float rootShifts[2] = {10.0f, 20.0f};
    RTCGeometry rootAr = makeInstanceArray(device, mid, rootShifts, 2, false);
    rtcAttachGeometryByID(root, rootAr, CHAIN_ROOT_AR);
    rtcReleaseGeometry(rootAr);
    if (rootMask != ~0u) rtcSetGeometryMask(rtcGetGeometry(root, CHAIN_ROOT_AR), rootMask);
    rtcCommitScene(root);
    return 0;
  }

  void release()
  {
    if (root) rtcReleaseScene(root);
    if (mid) rtcReleaseScene(mid);
    if (leaf) rtcReleaseScene(leaf);
    if (device) rtcReleaseDevice(device);
  }
};

} // namespace

// ============================================================================
// routes group: functional exclusion behavior on the diamond graph
// ============================================================================

static int routes_scalar_diamond()
{
  Fixture f;
  TASK_CHECK(f.build() == 0);

  // All twelve independently expanded leaf-triangle routes: three per root
  // placement (one regular mid instance, two mid array elements) times four
  // root placements. The leaf triangle lies in the local z=0 plane, so
  // world t is the sum of the instance translations along the route.
  Route routes[12] = {
    leafRoute(IA, INV, 4.0f,  JA, INV,  6.0f),
    leafRoute(IA, INV, 4.0f,  JC, 0,    8.0f),
    leafRoute(IA, INV, 4.0f,  JC, 1,   10.0f),
    leafRoute(IB, INV, 6.0f,  JA, INV,  8.0f),
    leafRoute(IB, INV, 6.0f,  JC, 0,   10.0f),
    leafRoute(IB, INV, 6.0f,  JC, 1,   12.0f),
    leafRoute(IC, 0,   8.0f, JA, INV, 10.0f),
    leafRoute(IC, 0,   8.0f,  JC, 0,   12.0f),
    leafRoute(IC, 0,   8.0f,  JC, 1,   14.0f),
    leafRoute(IC, 1,  10.0f, JA, INV, 12.0f),
    leafRoute(IC, 1,  10.0f,  JC, 0,   14.0f),
    leafRoute(IC, 1,  10.0f,  JC, 1,   16.0f),
  };

  for (unsigned target = 0; target < 12; ++target)
  {
    // Block every full leaf path except the target route. Same-source
    // placements are therefore selected by their exact instance path.
    std::vector<std::vector<RTCInstanceExclusionPath>> rules;
    for (unsigned other = 0; other < 12; ++other)
      if (other != target) rules.push_back(routes[other].path);
    RTCInstanceExclusion exc = makeHandle(f.device, rules);
    TASK_CHECK(exc != nullptr);

    RTCIntersectArguments ia; rtcInitIntersectArguments(&ia);
    RTCInstanceExclusion table[1] = {exc};
    ia.exclusions = table;
    ia.exclusionCount = 1;

    const Route& r = routes[target];
    RTCRayHit rh = makeRay(0, 0, 0, 0, 0, 1, r.tnear, 19.0f, 0);
    rtcIntersect1(f.root, &rh, &ia);
    TASK_CHECK(rh.hit.geomID == LEAF_TRI);
    TASK_CHECK(std::fabs(rh.ray.tfar - r.worldT) < 1e-5f);
    TASK_CHECK(std::fabs(rh.hit.u - 0.25f) < 1e-5f);
    TASK_CHECK(std::fabs(rh.hit.v - 0.5f) < 1e-5f);
    for (unsigned l = 0; l < RTC_MAX_INSTANCE_LEVEL_COUNT; ++l) {
      TASK_CHECK(rh.hit.instID[l] == r.instID[l]);
      TASK_CHECK(rh.hit.instPrimID[l] == r.instPrim[l]);
    }
    TASK_CHECK(rh.ray.id == 0);
    TASK_CHECK(rh.ray.mask == ~0u);

    // rtcTraversableIntersect1 must behave identically.
    RTCRayHit rh2 = makeRay(0, 0, 0, 0, 0, 1, r.tnear, 19.0f, 0);
    rtcTraversableIntersect1(rtcGetSceneTraversable(f.root), &rh2, &ia);
    TASK_CHECK(rh2.hit.geomID == LEAF_TRI);
    TASK_CHECK(std::fabs(rh2.ray.tfar - r.worldT) < 1e-5f);
    TASK_CHECK(std::fabs(rh2.hit.u - 0.25f) < 1e-5f);
    for (unsigned l = 0; l < RTC_MAX_INSTANCE_LEVEL_COUNT; ++l) {
      TASK_CHECK(rh2.hit.instID[l] == r.instID[l]);
      TASK_CHECK(rh2.hit.instPrimID[l] == r.instPrim[l]);
    }

    rtcReleaseInstanceExclusion(exc);
  }

  // Without any rules the nearest instance content is the IA/JA leaf
  // triangle at t=6.
  RTCIntersectArguments plain; rtcInitIntersectArguments(&plain);
  RTCRayHit nearest = makeRay(0, 0, 0, 0, 0, 1, 0, 19.0f, 0);
  rtcIntersect1(f.root, &nearest, &plain);
  TASK_CHECK(nearest.hit.geomID == LEAF_TRI);
  TASK_CHECK(nearest.hit.instID[0] == IA);
  TASK_CHECK(nearest.hit.instID[1] == JA);
  TASK_CHECK(std::fabs(nearest.ray.tfar - 6.0f) < 1e-5f);

  TASK_CHECK(expectNoDeviceError(f.device) == 0);
  f.release();
  return 0;
}

static int routes_prefix_subtree_and_near_face()
{
  Fixture f;
  TASK_CHECK(f.build() == 0);

  // Excluding IA removes its whole subtree (both JA and JC branches). The
  // nearest remaining route is the IB/JA leaf triangle at t=8.
  RTCInstanceExclusion exc = makeHandle(f.device, {{edge(IA, INV)}});
  TASK_CHECK(exc != nullptr);

  RTCIntersectArguments ia; rtcInitIntersectArguments(&ia);
  RTCInstanceExclusion table[1] = {exc};
  ia.exclusions = table; ia.exclusionCount = 1;

  RTCRayHit rh = makeRay(0, 0, 0, 0, 0, 1, 0, 19.0f, 0);
  rtcIntersect1(f.root, &rh, &ia);
  TASK_CHECK(rh.hit.geomID == LEAF_TRI);
  TASK_CHECK(std::fabs(rh.ray.tfar - 8.0f) < 1e-5f);
  TASK_CHECK(rh.hit.instID[0] == IB);

  // Same exclusion, non-unit ray direction: exclusion is path based and
  // independent of ray scaling. The shallow angle still crosses the
  // finite-sized triangle.
  RTCRayHit rhDir = makeRay(0, 0, 0, 0.05f, -0.03f, 1.0f, 0, 19.0f, 0);
  rtcIntersect1(f.root, &rhDir, &ia);
  TASK_CHECK(rhDir.hit.geomID == LEAF_TRI);
  TASK_CHECK(rhDir.hit.instID[0] == IB);

  // Excluding only one array element leaves the sibling element reachable:
  // block IC element 0 and start past every nearer route; IC element 1's
  // own JC element-0 leaf remains the unique nearest triangle.
  RTCInstanceExclusion excIC0 = makeHandle(f.device, {{edge(IC, 0)}});
  RTCInstanceExclusion table2[1] = {excIC0};
  ia.exclusions = table2;
  RTCRayHit rh3 = makeRay(0, 0, 0, 0, 0, 1, 13.25f, 19.0f, 0);
  rtcIntersect1(f.root, &rh3, &ia);
  TASK_CHECK(rh3.hit.geomID == LEAF_TRI);
  TASK_CHECK(rh3.hit.instID[0] == IC);
  TASK_CHECK(rh3.hit.instPrimID[0] == 1);
  TASK_CHECK(std::fabs(rh3.ray.tfar - 14.0f) < 1e-5f);
  rtcReleaseInstanceExclusion(excIC0);

  // Root-level non-instance primitives are never affected. The root
  // triangle lies at z=0, so start behind it and shoot backwards.
  RTCInstanceExclusion excAll = makeHandle(f.device, {
    {edge(IA, INV)}, {edge(IB, INV)}, {edge(IC, 0)}, {edge(IC, 1)}});
  RTCInstanceExclusion table3[1] = {excAll};
  ia.exclusions = table3;
  RTCRayHit rh4 = makeRay(0, 0, 2.0f, 0, 0, -1.0f, 0.0f, 10.0f, 0);
  rtcIntersect1(f.root, &rh4, &ia);
  TASK_CHECK(rh4.hit.geomID == ROOT_TRI);
  TASK_CHECK(std::fabs(rh4.ray.tfar - 2.0f) < 1e-5f);
  rtcReleaseInstanceExclusion(excAll);

  rtcReleaseInstanceExclusion(exc);
  TASK_CHECK(expectNoDeviceError(f.device) == 0);
  f.release();
  return 0;
}

static int routes_occluded_shadow_rays()
{
  Fixture f;
  TASK_CHECK(f.build() == 0);

  RTCInstanceExclusion exc = makeHandle(f.device, {{edge(IA, INV)}});
  TASK_CHECK(exc != nullptr);
  RTCOccludedArguments oa; rtcInitOccludedArguments(&oa);
  RTCInstanceExclusion table[1] = {exc};
  oa.exclusions = table; oa.exclusionCount = 1;

  // Short shadow ray ending inside the excluded IA subtree stays unblocked.
  RTCRayHit shortBox = makeRay(0, 0, 0, 0, 0, 1, 0, 5.5f, 0);
  rtcOccluded1(f.root, &shortBox.ray, &oa);
  TASK_CHECK(shortBox.ray.tfar >= 0.0f);
  TASK_CHECK(std::fabs(shortBox.ray.tfar - 5.5f) < 1e-6f);

  // A ray reaching the retained IB/JA leaf (t=8) reports occlusion, and the
  // result must not leak back into the short ray's stored distance.
  RTCRayHit longBox = makeRay(0, 0, 0, 0, 0, 1, 0, 8.5f, 0);
  rtcTraversableOccluded1(rtcGetSceneTraversable(f.root), &longBox.ray, &oa);
  TASK_CHECK(longBox.ray.tfar < 0.0f);
  TASK_CHECK(shortBox.ray.tfar >= 0.0f);

  // No exclusion: a ray window reaching the nearest IA/JA leaf at t=6
  // becomes occluded.
  RTCOccludedArguments plain; rtcInitOccludedArguments(&plain);
  RTCRayHit plainBox = makeRay(0, 0, 0, 0, 0, 1, 0, 6.5f, 0);
  rtcOccluded1(f.root, &plainBox.ray, &plain);
  TASK_CHECK(plainBox.ray.tfar < 0.0f);

  rtcReleaseInstanceExclusion(exc);
  TASK_CHECK(expectNoDeviceError(f.device) == 0);
  f.release();
  return 0;
}

static int routes_user_geometry_not_entered()
{
  // Dedicated scene: the excluded subtree itself contains a user geometry,
  // so a zero callback count proves the subtree was never traversed rather
  // than merely losing a leaf-primitive contest.
  RTCDevice device = rtcNewDevice("threads=1");
  TASK_CHECK(device != nullptr);

  RTCScene inner = rtcNewScene(device);
  rtcSetSceneFlags(inner, RTC_SCENE_FLAG_FILTER_FUNCTION_IN_ARGUMENTS);
  RTCGeometry user = makeUserGeometry(device, MID_USER, 1.0f);
  rtcAttachGeometryByID(inner, user, MID_USER);
  rtcReleaseGeometry(user);
  RTCGeometry innerTri = makeTriangle(device);
  rtcAttachGeometryByID(inner, innerTri, LEAF_TRI);
  rtcReleaseGeometry(innerTri);
  rtcCommitScene(inner);

  RTCScene plainLeaf = rtcNewScene(device);
  rtcSetSceneFlags(plainLeaf, RTC_SCENE_FLAG_FILTER_FUNCTION_IN_ARGUMENTS);
  RTCGeometry farTri = makeTriangle(device);
  rtcAttachGeometryByID(plainLeaf, farTri, LEAF_TRI);
  rtcReleaseGeometry(farTri);
  rtcCommitScene(plainLeaf);

  RTCScene root = rtcNewScene(device);
  rtcSetSceneFlags(root, RTC_SCENE_FLAG_FILTER_FUNCTION_IN_ARGUMENTS);
  const unsigned CB_IA = 501, CB_IB = 502;
  RTCGeometry iA = makeInstance(device, inner, 4.0f, false);
  rtcAttachGeometryByID(root, iA, CB_IA);
  rtcReleaseGeometry(iA);
  RTCGeometry iB = makeInstance(device, plainLeaf, 9.0f, false);
  rtcAttachGeometryByID(root, iB, CB_IB);
  rtcReleaseGeometry(iB);
  rtcCommitScene(root);

  RTCInstanceExclusion exc = makeHandle(device, {{edge(CB_IA, INV)}});
  TASK_CHECK(exc != nullptr);

  RTCIntersectArguments ia; rtcInitIntersectArguments(&ia);
  RTCInstanceExclusion table[1] = {exc};
  ia.exclusions = table; ia.exclusionCount = 1;
  RTCOccludedArguments oa; rtcInitOccludedArguments(&oa);
  oa.exclusions = table; oa.exclusionCount = 1;

  g_userIntersectCalls = 0;
  g_userOccludedCalls = 0;
  RTCRayHit rh = makeRay(0, 0, 0, 0, 0, 1, 0, 6.0f, 0);
  rtcIntersect1(root, &rh, &ia);
  TASK_CHECK(g_userIntersectCalls.load() == 0); // IA subtree never entered
  TASK_CHECK(rh.hit.geomID == INV);            // window ends before IB leaf

  RTCRayHit sh = makeRay(0, 0, 0, 0, 0, 1, 0, 6.0f, 0);
  rtcOccluded1(root, &sh.ray, &oa);
  TASK_CHECK(g_userOccludedCalls.load() == 0);
  TASK_CHECK(sh.ray.tfar >= 0.0f);

  // Sanity: without exclusion the same traversal does enter the callback.
  // Start past the inner leaf triangle (world t=4) so the user slab at
  // world t=5 becomes the recorded nearest hit.
  g_userIntersectCalls = 0;
  RTCIntersectArguments plain; rtcInitIntersectArguments(&plain);
  RTCRayHit rh2 = makeRay(0, 0, 0, 0, 0, 1, 4.5f, 6.0f, 0);
  rtcIntersect1(root, &rh2, &plain);
  TASK_CHECK(g_userIntersectCalls.load() >= 1);
  TASK_CHECK(rh2.hit.geomID == MID_USER);
  TASK_CHECK(std::fabs(rh2.ray.tfar - 5.0f) < 1e-5f);

  // Leaf-level argument filters still run on a retained hit.
  g_filterCalls = 0;
  ia.flags = (RTCRayQueryFlags)(RTC_RAY_QUERY_FLAG_INCOHERENT |
                                RTC_RAY_QUERY_FLAG_INVOKE_ARGUMENT_FILTER);
  ia.filter = argumentFilter;
  RTCRayHit rh3 = makeRay(0, 0, 0, 0, 0, 1, 0, 12.0f, 0);
  rtcIntersect1(root, &rh3, &ia);
  TASK_CHECK(rh3.hit.geomID == LEAF_TRI);
  TASK_CHECK(rh3.hit.instID[0] == CB_IB);
  TASK_CHECK(g_filterCalls.load() >= 1);

  rtcReleaseInstanceExclusion(exc);
  TASK_CHECK(rtcGetDeviceError(device) == RTC_ERROR_NONE);
  rtcReleaseScene(root);
  rtcReleaseScene(inner);
  rtcReleaseScene(plainLeaf);
  rtcReleaseDevice(device);
  return 0;
}

static int routes_packet4_mixed_lanes()
{
  Fixture f;
  TASK_CHECK(f.build() == 0);

  // Lane-specific rules through different ray.id values. The lanes cover
  // all four combinations: lane 0 hits the motion-blur regular instance,
  // lane 1 the static regular instance, lane 3 the static instance array,
  // while lane 2 is an inactive sentinel.
  RTCInstanceExclusion excIA = makeHandle(f.device, {{edge(IA, INV)}});
  RTCInstanceExclusion excIB = makeHandle(f.device, {{edge(IB, INV)}});
  RTCInstanceExclusion excIC = makeHandle(f.device, {{edge(IC, 0)}, {edge(IC, 1)}});
  RTCInstanceExclusion excNone = makeHandle(f.device, {});
  TASK_CHECK(excIA && excIB && excIC && excNone);

  RTCIntersectArguments ia; rtcInitIntersectArguments(&ia);
  RTCInstanceExclusion table[4] = {excIA, excIB, excIC, excNone};
  ia.exclusions = table; ia.exclusionCount = 4;

  RTCRayHit4 rh{};
  const float ox[4] = {0,0,0,0}, oy[4] = {0,0,0,0}, oz[4] = {0,0,0,0};
  const float dx[4] = {0,0,0,0}, dy[4] = {0,0,0,0}, dz[4] = {1,1,1,1};
  const float times[4] = {0.0f, 1.0f, 0.5f, 0.25f};
  const float tnear[4] = {0,0,0,0};
  const float tfar[4] = {19.0f, 19.0f, 19.0f, 19.0f};
  const unsigned masks[4] = {~0u, ~0u, ~0u, ~0u};
  const unsigned ids[4] = {0, 1, 2, 3};
  const unsigned flags[4] = {0,0,0,0};
  std::memcpy(rh.ray.org_x, ox, 4*sizeof(float));
  std::memcpy(rh.ray.org_y, oy, 4*sizeof(float));
  std::memcpy(rh.ray.org_z, oz, 4*sizeof(float));
  std::memcpy(rh.ray.tnear, tnear, 4*sizeof(float));
  std::memcpy(rh.ray.dir_x, dx, 4*sizeof(float));
  std::memcpy(rh.ray.dir_y, dy, 4*sizeof(float));
  std::memcpy(rh.ray.dir_z, dz, 4*sizeof(float));
  std::memcpy(rh.ray.time, times, 4*sizeof(float));
  std::memcpy(rh.ray.tfar, tfar, 4*sizeof(float));
  std::memcpy(rh.ray.mask, masks, 4*sizeof(unsigned));
  std::memcpy(rh.ray.id, ids, 4*sizeof(unsigned));
  std::memcpy(rh.ray.flags, flags, 4*sizeof(unsigned));
  for (unsigned l = 0; l < RTC_MAX_INSTANCE_LEVEL_COUNT; ++l) {
    for (unsigned k = 0; k < 4; ++k) {
      rh.hit.instID[l][k] = INV;
      rh.hit.instPrimID[l][k] = INV;
    }
  }
  for (unsigned k = 0; k < 4; ++k) rh.hit.geomID[k] = INV;

  // Lane 2 is an inactive sentinel carrying an untouched tfar marker.
  const int valid[4] = {-1, -1, 0, -1};
  rh.ray.tfar[2] = -777.0f;
  rtcIntersect4(valid, f.root, &rh, &ia);

  TASK_CHECK(rh.hit.instID[0][0] == IB);            // IA excluded
  TASK_CHECK(rh.hit.instID[0][1] == IA);            // IB excluded
  TASK_CHECK(std::fabs(rh.ray.tfar[2] + 777.0f) < 1e-6f); // inactive untouched
  TASK_CHECK(rh.hit.geomID[2] == INV);
  TASK_CHECK(rh.hit.instID[0][3] == IA);            // empty rule: nearest path
  TASK_CHECK(std::fabs(rh.ray.tfar[3] - 6.0f) < 1e-5f);
  TASK_CHECK(rh.ray.id[0] == 0 && rh.ray.id[1] == 1 &&
             rh.ray.id[2] == 2 && rh.ray.id[3] == 3);

  // Occluded packet: one lane terminates early without polluting others.
  RTCOccludedArguments oa; rtcInitOccludedArguments(&oa);
  oa.exclusions = table; oa.exclusionCount = 4;
  RTCRay4 r4{};
  // Lane 0's window ends before the retained IB route; lanes 1 and 3 reach
  // the IA/JA leaf at t=6.
  const float shadowFar[4] = {5.0f, 7.0f, 5.0f, 7.0f};
  std::memcpy(r4.org_x, ox, 4*sizeof(float));
  std::memcpy(r4.org_y, oy, 4*sizeof(float));
  std::memcpy(r4.org_z, oz, 4*sizeof(float));
  std::memcpy(r4.dir_x, dx, 4*sizeof(float));
  std::memcpy(r4.dir_y, dy, 4*sizeof(float));
  std::memcpy(r4.dir_z, dz, 4*sizeof(float));
  std::memcpy(r4.time, times, 4*sizeof(float));
  std::memcpy(r4.tnear, tnear, 4*sizeof(float));
  std::memcpy(r4.tfar, shadowFar, 4*sizeof(float));
  std::memcpy(r4.mask, masks, 4*sizeof(unsigned));
  std::memcpy(r4.id, ids, 4*sizeof(unsigned));
  std::memcpy(r4.flags, flags, 4*sizeof(unsigned));
  r4.tfar[2] = -777.0f;
  rtcOccluded4(valid, f.root, &r4, &oa);
  TASK_CHECK(r4.tfar[0] >= 0.0f);  // IA excluded: t=5 window sees nothing
  TASK_CHECK(r4.tfar[1] < 0.0f);   // IB excluded but the IA/JA leaf blocks
  TASK_CHECK(std::fabs(r4.tfar[2] + 777.0f) < 1e-6f);
  TASK_CHECK(r4.tfar[3] < 0.0f);   // no rule: IA/JA leaf blocks
  TASK_CHECK(r4.tfar[0] >= 0.0f);  // re-check lane 0 after sibling termination

  rtcReleaseInstanceExclusion(excIA);
  rtcReleaseInstanceExclusion(excIB);
  rtcReleaseInstanceExclusion(excIC);
  rtcReleaseInstanceExclusion(excNone);
  TASK_CHECK(expectNoDeviceError(f.device) == 0);
  f.release();
  return 0;
}

static int routes_wide_packet_scalar_fallback()
{
  // On builds without native 8/16-wide SIMD, rtcIntersect8/16 iterate through
  // the scalar intersector per lane. The exclusion state must still follow
  // each lane's ray.id through that fallback, including inactive sentinels.
  Fixture f;
  TASK_CHECK(f.build() == 0);

  RTCInstanceExclusion excIA = makeHandle(f.device, {{edge(IA, INV)}});
  RTCInstanceExclusion excNone = makeHandle(f.device, {});
  TASK_CHECK(excIA && excNone);

  RTCIntersectArguments ia; rtcInitIntersectArguments(&ia);
  RTCInstanceExclusion table[8] = {
    excIA, excNone, excIA, excNone, excIA, excNone, excIA, excNone};
  ia.exclusions = table; ia.exclusionCount = 8;

  RTCRayHit8 rh{};
  for (unsigned k = 0; k < 8; ++k) {
    rh.ray.org_x[k] = 0; rh.ray.org_y[k] = 0; rh.ray.org_z[k] = 0;
    rh.ray.tnear[k] = 0;
    rh.ray.dir_x[k] = 0; rh.ray.dir_y[k] = 0; rh.ray.dir_z[k] = 1;
    rh.ray.time[k] = 0.5f * k;
    rh.ray.tfar[k] = 19.0f;
    rh.ray.mask[k] = ~0u;
    rh.ray.id[k] = k;
    rh.ray.flags[k] = 0;
    rh.hit.geomID[k] = INV;
    for (unsigned l = 0; l < RTC_MAX_INSTANCE_LEVEL_COUNT; ++l) {
      rh.hit.instID[l][k] = INV;
      rh.hit.instPrimID[l][k] = INV;
    }
  }
  // Lanes 2 and 5 are inactive sentinels with a marker tfar.
  const int valid[8] = {-1, -1, 0, -1, -1, 0, -1, -1};
  rh.ray.tfar[2] = -777.0f;
  rh.ray.tfar[5] = -888.0f;
  rtcIntersect8(valid, f.root, &rh, &ia);

  for (unsigned k = 0; k < 8; ++k) {
    if (k == 2 || k == 5) {
      TASK_CHECK(rh.hit.geomID[k] == INV);
      continue;
    }
    TASK_CHECK(rh.hit.geomID[k] == LEAF_TRI);
    if (k % 2 == 0) TASK_CHECK(rh.hit.instID[0][k] == IB); // IA excluded
    else            TASK_CHECK(rh.hit.instID[0][k] == IA); // no rule
  }
  TASK_CHECK(std::fabs(rh.ray.tfar[2] + 777.0f) < 1e-6f);
  TASK_CHECK(std::fabs(rh.ray.tfar[5] + 888.0f) < 1e-6f);

  // Same coverage for rtcOccluded8: inactive lanes stay untouched.
  RTCOccludedArguments oa; rtcInitOccludedArguments(&oa);
  oa.exclusions = table; oa.exclusionCount = 8;
  RTCRay8 r8{};
  for (unsigned k = 0; k < 8; ++k) {
    r8.org_x[k] = 0; r8.org_y[k] = 0; r8.org_z[k] = 0; r8.tnear[k] = 0;
    r8.dir_x[k] = 0; r8.dir_y[k] = 0; r8.dir_z[k] = 1;
    r8.time[k] = 0.25f; r8.tfar[k] = 7.0f; r8.mask[k] = ~0u;
    r8.id[k] = k; r8.flags[k] = 0;
  }
  r8.tfar[2] = -777.0f; r8.tfar[5] = -888.0f;
  rtcOccluded8(valid, f.root, &r8, &oa);
  for (unsigned k = 0; k < 8; ++k) {
    if (k == 2 || k == 5) continue;
    if (k % 2 == 0) TASK_CHECK(r8.tfar[k] >= 0.0f); // IA excluded: window clear
    else            TASK_CHECK(r8.tfar[k] < 0.0f);  // IA/JA leaf blocks
  }
  TASK_CHECK(std::fabs(r8.tfar[2] + 777.0f) < 1e-6f);
  TASK_CHECK(std::fabs(r8.tfar[5] + 888.0f) < 1e-6f);

  rtcReleaseInstanceExclusion(excIA);
  rtcReleaseInstanceExclusion(excNone);
  TASK_CHECK(expectNoDeviceError(f.device) == 0);
  f.release();
  return 0;
}

static int routes_motion_blur_exclusion()
{
  // A moving instance array: element 0 translates from z=4 (time 0) to
  // z=8 (time 1). Excluding element 0 must work through the motion-blur
  // intersectors at every sampled time.
  RTCDevice device = rtcNewDevice("threads=1");
  TASK_CHECK(device != nullptr);

  RTCScene leaf = rtcNewScene(device);
  RTCGeometry tri = makeTriangle(device);
  rtcAttachGeometryByID(leaf, tri, LEAF_TRI);
  rtcReleaseGeometry(tri);
  rtcCommitScene(leaf);

  RTCGeometry g = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_INSTANCE_ARRAY);
  rtcSetGeometryInstancedScene(g, leaf);
  rtcSetGeometryTimeStepCount(g, 2);
  float* t0 = (float*)rtcSetNewGeometryBuffer(
    g, RTC_BUFFER_TYPE_TRANSFORM, 0, RTC_FORMAT_FLOAT3X4_ROW_MAJOR, 12 * sizeof(float), 1);
  float* t1 = (float*)rtcSetNewGeometryBuffer(
    g, RTC_BUFFER_TYPE_TRANSFORM, 1, RTC_FORMAT_FLOAT3X4_ROW_MAJOR, 12 * sizeof(float), 1);
  rowMajorTranslation(t0, 0, 0, 4.0f);
  rowMajorTranslation(t1, 0, 0, 8.0f);
  rtcCommitGeometry(g);

  RTCScene root = rtcNewScene(device);
  const unsigned MB_AR = 601;
  rtcAttachGeometryByID(root, g, MB_AR);
  rtcReleaseGeometry(g);
  rtcCommitScene(root);

  RTCInstanceExclusion exc = makeHandle(device, {{edge(MB_AR, 0)}});
  TASK_CHECK(exc != nullptr);
  RTCIntersectArguments ia; rtcInitIntersectArguments(&ia);
  RTCInstanceExclusion table[1] = {exc};
  ia.exclusions = table; ia.exclusionCount = 1;

  const float sampleTimes[4] = {0.0f, 0.25f, 0.75f, 1.0f};
  for (float time : sampleTimes) {
    RTCRayHit rh = makeRay(0, 0, 0, 0, 0, 1, 0, 30.0f, 0, ~0u, time);
    rtcIntersect1(root, &rh, &ia);
    TASK_CHECK(rh.hit.geomID == INV); // moving element 0 always excluded
  }

  // Without the rule the moving element is hit at the interpolated plane.
  RTCIntersectArguments plain; rtcInitIntersectArguments(&plain);
  RTCRayHit ref = makeRay(0, 0, 0, 0, 0, 1, 0, 30.0f, 0, ~0u, 0.5f);
  rtcIntersect1(root, &ref, &plain);
  TASK_CHECK(ref.hit.geomID == LEAF_TRI);
  TASK_CHECK(std::fabs(ref.ray.tfar - 6.0f) < 1e-5f); // lerp(4,8,0.5)

  // Packet motion-blur path with mixed times.
  RTCInstanceExclusion excNone = rtcNewInstanceExclusion(device, nullptr, nullptr, 0);
  TASK_CHECK(excNone != nullptr);
  RTCInstanceExclusion ptable[4] = {exc, exc, exc, excNone};
  RTCIntersectArguments pia; rtcInitIntersectArguments(&pia);
  pia.exclusions = ptable; pia.exclusionCount = 4;
  RTCRayHit4 rh4{};
  for (unsigned k = 0; k < 4; ++k) {
    rh4.ray.org_x[k] = 0; rh4.ray.org_y[k] = 0; rh4.ray.org_z[k] = 0;
    rh4.ray.tnear[k] = 0;
    rh4.ray.dir_x[k] = 0; rh4.ray.dir_y[k] = 0; rh4.ray.dir_z[k] = 1;
    rh4.ray.time[k] = sampleTimes[k];
    rh4.ray.tfar[k] = 30.0f; rh4.ray.mask[k] = ~0u;
    rh4.ray.id[k] = k; rh4.ray.flags[k] = 0;
    rh4.hit.geomID[k] = INV;
    for (unsigned l = 0; l < RTC_MAX_INSTANCE_LEVEL_COUNT; ++l) {
      rh4.hit.instID[l][k] = INV; rh4.hit.instPrimID[l][k] = INV;
    }
  }
  const int valid4[4] = {-1, -1, -1, -1};
  rtcIntersect4(valid4, root, &rh4, &pia);
  for (unsigned k = 0; k < 3; ++k) TASK_CHECK(rh4.hit.geomID[k] == INV);
  TASK_CHECK(rh4.hit.geomID[3] == LEAF_TRI);

  rtcReleaseInstanceExclusion(excNone);
  rtcReleaseInstanceExclusion(exc);
  TASK_CHECK(rtcGetDeviceError(device) == RTC_ERROR_NONE);
  rtcReleaseScene(root);
  rtcReleaseScene(leaf);
  rtcReleaseDevice(device);
  return 0;
}

static int routes_mask_ordering()
{
  // The root instance array gets a mask the ray mask fails. The ray also
  // carries a matching exclusion rule: the mask test runs first, so the
  // trie must never be consulted.
  ChainScene c;
  TASK_CHECK(c.build(0xF0u) == 0);

  RTCInstanceExclusionPath rule[1] = {edge(CHAIN_ROOT_AR, 0)};
  const RTCInstanceExclusionPath* rulePtr = rule;
  size_t ruleLen = 1;
  RTCInstanceExclusion exc = rtcNewInstanceExclusion(c.device, &rulePtr, &ruleLen, 1);
  TASK_CHECK(exc != nullptr);
  RTCIntersectArguments ia; rtcInitIntersectArguments(&ia);
  RTCInstanceExclusion table[1] = {exc};
  ia.exclusions = table; ia.exclusionCount = 1;
  RTCInstanceExclusionStats stats{};
  ia.exclusionStats = &stats;

  RTCRayHit rh = makeRay(0, 0, 0, 0, 0, 1, 0, 30.0f, 0, 0x0Fu);
  rtcIntersect1(c.root, &rh, &ia);
  TASK_CHECK(stats.transitions == 0); // mask test rejected the array first
  TASK_CHECK(stats.comparisons == 0);
  TASK_CHECK(rh.hit.geomID == INV);

  // Same scene with a compatible mask now enters the trie: element 0 is
  // excluded while element 1 still reaches its nearest mid element.
  ChainScene c2;
  TASK_CHECK(c2.build(~0u) == 0);
  stats.transitions = 0; stats.comparisons = 0;
  RTCRayHit rh2 = makeRay(0, 0, 0, 0, 0, 1, 0, 30.0f, 0, 0x0Fu);
  rtcIntersect1(c2.root, &rh2, &ia);
  TASK_CHECK(stats.transitions >= 1);
  TASK_CHECK(rh2.hit.geomID == CHAIN_LEAF_TRI);
  TASK_CHECK(rh2.hit.instPrimID[0] == 1);
  TASK_CHECK(std::fabs(rh2.ray.tfar - 23.0f) < 1e-5f); // root 20 + mid 3

  rtcReleaseInstanceExclusion(exc);
  c2.release();
  c.release();
  return 0;
}

// ============================================================================
// trie group: sparse trie compilation and statistics semantics
// ============================================================================

static int trie_construction_errors()
{
  RTCDevice device = rtcNewDevice("threads=1");
  TASK_CHECK(device != nullptr);

  RTCInstanceExclusionPath good[2] = {edge(1, INV), edge(2, 0)};
  const RTCInstanceExclusionPath* goodPtr = good;
  size_t goodLen = 2;

  // Empty rule table is legal.
  RTCInstanceExclusion empty = rtcNewInstanceExclusion(device, nullptr, nullptr, 0);
  TASK_CHECK(empty != nullptr);
  rtcReleaseInstanceExclusion(empty);

  // Null paths / lengths with non-zero count.
  TASK_CHECK(rtcNewInstanceExclusion(device, nullptr, &goodLen, 1) == nullptr);
  TASK_CHECK(rtcGetDeviceError(device) == RTC_ERROR_INVALID_ARGUMENT);
  TASK_CHECK(rtcNewInstanceExclusion(device, &goodPtr, nullptr, 1) == nullptr);
  TASK_CHECK(rtcGetDeviceError(device) == RTC_ERROR_INVALID_ARGUMENT);

  // Null individual path.
  const RTCInstanceExclusionPath* nullPtr = nullptr;
  TASK_CHECK(rtcNewInstanceExclusion(device, &nullPtr, &goodLen, 1) == nullptr);
  TASK_CHECK(rtcGetDeviceError(device) == RTC_ERROR_INVALID_ARGUMENT);

  // Empty path and path longer than RTC_MAX_INSTANCE_LEVEL_COUNT.
  size_t zeroLen = 0;
  TASK_CHECK(rtcNewInstanceExclusion(device, &goodPtr, &zeroLen, 1) == nullptr);
  TASK_CHECK(rtcGetDeviceError(device) == RTC_ERROR_INVALID_ARGUMENT);
  size_t tooLong = RTC_MAX_INSTANCE_LEVEL_COUNT + 1;
  TASK_CHECK(rtcNewInstanceExclusion(device, &goodPtr, &tooLong, 1) == nullptr);
  TASK_CHECK(rtcGetDeviceError(device) == RTC_ERROR_INVALID_ARGUMENT);

  // Invalid geomID.
  RTCInstanceExclusionPath badGeom[1] = {edge(INV, INV)};
  const RTCInstanceExclusionPath* badPtr = badGeom;
  size_t one = 1;
  TASK_CHECK(rtcNewInstanceExclusion(device, &badPtr, &one, 1) == nullptr);
  TASK_CHECK(rtcGetDeviceError(device) == RTC_ERROR_INVALID_ARGUMENT);

  // Null device reports through the thread-local error and returns null.
  TASK_CHECK(rtcNewInstanceExclusion(nullptr, &goodPtr, &goodLen, 1) == nullptr);

  // Valid construction retains/releases cleanly.
  RTCInstanceExclusion ok = rtcNewInstanceExclusion(device, &goodPtr, &goodLen, 1);
  TASK_CHECK(ok != nullptr);
  rtcRetainInstanceExclusion(ok);
  rtcReleaseInstanceExclusion(ok);
  rtcReleaseInstanceExclusion(ok);
  TASK_CHECK(rtcGetDeviceError(device) == RTC_ERROR_NONE);

  rtcReleaseDevice(device);
  return 0;
}

static int trie_deep_copy_and_merge()
{
  RTCDevice device = rtcNewDevice("threads=1");
  TASK_CHECK(device != nullptr);

  // Duplicate paths and descendants covered by an ancestor merge silently.
  RTCInstanceExclusionPath p1[2] = {edge(7, INV), edge(8, 3)};
  RTCInstanceExclusionPath p2[2] = {edge(7, INV), edge(8, 3)};   // duplicate
  RTCInstanceExclusionPath p3[3] = {edge(7, INV), edge(8, 3), edge(9, INV)}; // descendant
  const RTCInstanceExclusionPath* ptrs[3] = {p1, p2, p3};
  size_t lens[3] = {2, 2, 3};
  RTCInstanceExclusion exc = rtcNewInstanceExclusion(device, ptrs, lens, 3);
  TASK_CHECK(exc != nullptr);

  // Destroy/mutate every input array after construction.
  RTCInstanceExclusionPath garbage[3] = {edge(INV, INV), edge(INV, INV), edge(INV, INV)};
  std::memcpy(p1, garbage, sizeof(p1));
  std::memcpy(p2, garbage, sizeof(p2));
  std::memcpy(p3, garbage, sizeof(p3));
  TASK_CHECK(rtcGetDeviceError(device) == RTC_ERROR_NONE);

  // The handle still interprets the original IDs on a matching scene.
  Fixture f;
  TASK_CHECK(f.build() == 0);
  RTCIntersectArguments ia; rtcInitIntersectArguments(&ia);
  RTCInstanceExclusion table[1] = {exc};
  ia.exclusions = table; ia.exclusionCount = 1;
  RTCRayHit rh = makeRay(0, 0, 0, 0, 0, 1, 0, 19.0f, 0);
  rtcIntersect1(f.root, &rh, &ia);
  TASK_CHECK(rh.hit.geomID == LEAF_TRI); // no path matched: normal nearest hit
  TASK_CHECK(rh.hit.instID[0] == IA);
  TASK_CHECK(rtcGetDeviceError(device) == RTC_ERROR_NONE);

  rtcReleaseInstanceExclusion(exc);
  f.release();
  rtcReleaseDevice(device);
  return 0;
}

static int trie_many_rules_no_scan()
{
  RTCDevice device = rtcNewDevice("threads=1");
  TASK_CHECK(device != nullptr);

  // 4096 rules sharing one common prefix edge (root array element 0) and
  // fanning out into 4096 different second edges below it. The second-edge
  // IDs intentionally do not occur in the scene: the handle stores IDs only
  // and remains valid regardless of the committed scene.
  std::vector<std::vector<RTCInstanceExclusionPath>> paths;
  paths.reserve(4096);
  for (unsigned i = 0; i < 4096; ++i) {
    paths.push_back({edge(CHAIN_ROOT_AR, 0), edge(500u + i, INV)});
  }
  RTCInstanceExclusion exc = makeHandle(device, paths);
  TASK_CHECK(exc != nullptr);

  ChainScene c;
  TASK_CHECK(c.build() == 0);
  RTCIntersectArguments ia; rtcInitIntersectArguments(&ia);
  RTCInstanceExclusion table[1] = {exc};
  ia.exclusions = table; ia.exclusionCount = 1;
  RTCInstanceExclusionStats stats;
  stats.transitions = 0xdeadbeef; stats.comparisons = 0xdeadbeef; // must be reset
  ia.exclusionStats = &stats;

  // tfar stops after the nearest mid element but before the second one and
  // before root element 1, so exactly two instance edges are traversed:
  // the shared root edge (match) plus one mid edge (miss in the fan node).
  RTCRayHit rh = makeRay(0, 0, 0, 0, 0, 1, 0, 14.5f, 0);
  rtcIntersect1(c.root, &rh, &ia);
  TASK_CHECK(rh.hit.geomID == CHAIN_LEAF_TRI);
  TASK_CHECK(rh.hit.instPrimID[0] == 0);
  TASK_CHECK(stats.transitions == 2);
  // The fan node holds 4096 edges but lookup is a binary search: about
  // log2(4096)=12 key comparisons, never a scan of the 4096 rules.
  TASK_CHECK(stats.comparisons >= 11);
  TASK_CHECK(stats.comparisons <= 14);
  TASK_CHECK(stats.comparisons < 100);

  // A second query resets the counters instead of accumulating on top.
  rtcIntersect1(c.root, &rh, &ia);
  TASK_CHECK(stats.transitions == 2);

  // Empty/null cursor: rays without a rule never touch the trie.
  RTCIntersectArguments plain; rtcInitIntersectArguments(&plain);
  RTCInstanceExclusionStats plainStats;
  plainStats.transitions = 11; plainStats.comparisons = 22;
  plain.exclusionStats = &plainStats;
  RTCRayHit rh2 = makeRay(0, 0, 0, 0, 0, 1, 0, 30.0f, 0);
  rtcIntersect1(c.root, &rh2, &plain);
  TASK_CHECK(plainStats.transitions == 0);
  TASK_CHECK(plainStats.comparisons == 0);

  // A null handle inside a non-empty table means the ray has no rule.
  RTCInstanceExclusion withNull[1] = {nullptr};
  RTCIntersectArguments nullArgs; rtcInitIntersectArguments(&nullArgs);
  nullArgs.exclusions = withNull; nullArgs.exclusionCount = 1;
  RTCInstanceExclusionStats nullStats;
  nullStats.transitions = 5; nullStats.comparisons = 7;
  nullArgs.exclusionStats = &nullStats;
  RTCRayHit rh3 = makeRay(0, 0, 0, 0, 0, 1, 0, 30.0f, 0);
  rtcIntersect1(c.root, &rh3, &nullArgs);
  TASK_CHECK(nullStats.transitions == 0);
  TASK_CHECK(nullStats.comparisons == 0);
  TASK_CHECK(rh3.hit.geomID == CHAIN_LEAF_TRI);

  // An empty-rule handle likewise performs no work but stays valid.
  RTCInstanceExclusion empty = rtcNewInstanceExclusion(device, nullptr, nullptr, 0);
  TASK_CHECK(empty != nullptr);
  RTCInstanceExclusion emptyTable[1] = {empty};
  RTCIntersectArguments emptyArgs; rtcInitIntersectArguments(&emptyArgs);
  emptyArgs.exclusions = emptyTable; emptyArgs.exclusionCount = 1;
  RTCInstanceExclusionStats emptyStats;
  emptyStats.transitions = 9; emptyStats.comparisons = 9;
  emptyArgs.exclusionStats = &emptyStats;
  RTCRayHit rh4 = makeRay(0, 0, 0, 0, 0, 1, 0, 30.0f, 0);
  rtcIntersect1(c.root, &rh4, &emptyArgs);
  TASK_CHECK(emptyStats.transitions == 0);
  TASK_CHECK(emptyStats.comparisons == 0);
  TASK_CHECK(rh4.hit.geomID == CHAIN_LEAF_TRI);

  rtcReleaseInstanceExclusion(empty);
  rtcReleaseInstanceExclusion(exc);
  c.release();
  rtcReleaseDevice(device);
  return 0;
}

static int trie_prefix_terminal_skips_descendants()
{
  ChainScene c;
  TASK_CHECK(c.build() == 0);

  // Ancestor rule plus a descendant rule in the same subtree: once the
  // ancestor matches, the subtree is rejected wholesale and the descendant
  // rule adds no transition work.
  RTCInstanceExclusion both = makeHandle(c.device, {
    {edge(CHAIN_ROOT_AR, 0)},
    {edge(CHAIN_ROOT_AR, 0), edge(CHAIN_MID_AR, 1)}});
  TASK_CHECK(both != nullptr);

  RTCIntersectArguments ia; rtcInitIntersectArguments(&ia);
  RTCInstanceExclusion table[1] = {both};
  ia.exclusions = table; ia.exclusionCount = 1;
  RTCInstanceExclusionStats stats{};
  ia.exclusionStats = &stats;

  RTCRayHit rh = makeRay(0, 0, 0, 0, 0, 1, 0, 30.0f, 0);
  rtcIntersect1(c.root, &rh, &ia);
  // Root element 0 subtree never entered; element 1's leaf is the hit.
  TASK_CHECK(rh.hit.geomID == CHAIN_LEAF_TRI);
  TASK_CHECK(rh.hit.instPrimID[0] == 1);
  // Two root-level edge attempts (element 0 terminal, element 1 miss) and
  // no mid-level transition at all for the blocked element 0.
  TASK_CHECK(stats.transitions == 2);
  TASK_CHECK(stats.comparisons == 2);

  // Diverging prefix: a rule whose first edge does not match kills the
  // cursor, and later instance edges attempt no further transitions.
  RTCInstanceExclusion diverge = makeHandle(c.device, {{edge(999u, INV), edge(998u, INV)}});
  RTCInstanceExclusion table2[1] = {diverge};
  ia.exclusions = table2;
  stats.transitions = 0; stats.comparisons = 0;
  RTCRayHit rh2 = makeRay(0, 0, 0, 0, 0, 1, 0, 14.5f, 0);
  rtcIntersect1(c.root, &rh2, &ia);
  TASK_CHECK(rh2.hit.geomID == CHAIN_LEAF_TRI);
  TASK_CHECK(rh2.hit.instPrimID[0] == 0);
  // The first root edge mismatched once; the mid edge below it is skipped
  // because the cursor is already dead.
  TASK_CHECK(stats.transitions == 1);
  TASK_CHECK(stats.comparisons == 1);

  rtcReleaseInstanceExclusion(diverge);
  rtcReleaseInstanceExclusion(both);
  c.release();
  return 0;
}

// ============================================================================
// state group: cursor lifecycle, reuse across queries/threads, empty configs
// ============================================================================

static int state_context_restoration_and_identity()
{
  Fixture f;
  TASK_CHECK(f.build() == 0);

  RTCInstanceExclusion exc = makeHandle(f.device, {{edge(IA, INV)}});
  TASK_CHECK(exc != nullptr);
  RTCIntersectArguments ia; rtcInitIntersectArguments(&ia);
  RTCInstanceExclusion table[1] = {exc};
  ia.exclusions = table; ia.exclusionCount = 1;

  RTCRayQueryContext ctx; rtcInitRayQueryContext(&ctx);
  ia.context = &ctx;

  for (int q = 0; q < 5; ++q) {
    RTCRayHit rh = makeRay(0, 0, 0, 0, 0, 1, 0, 19.0f, 0);
    rtcIntersect1(f.root, &rh, &ia);
    TASK_CHECK(rh.hit.geomID == LEAF_TRI);
    TASK_CHECK(rh.hit.instID[0] == IB); // IA excluded, IB is now nearest
    TASK_CHECK(ctx.instStackSize == 0);
    TASK_CHECK(ctx.instID[0] == INV);
    TASK_CHECK(rh.ray.id == 0);
    TASK_CHECK(rh.ray.mask == ~0u);
  }

  // Occluded queries restore the context as well.
  RTCOccludedArguments oa; rtcInitOccludedArguments(&oa);
  oa.exclusions = table; oa.exclusionCount = 1;
  oa.context = &ctx;
  RTCRayHit sh = makeRay(0, 0, 0, 0, 0, 1, 0, 7.0f, 0);
  rtcOccluded1(f.root, &sh.ray, &oa);
  TASK_CHECK(ctx.instStackSize == 0);
  TASK_CHECK(ctx.instID[0] == INV);

  // The ray mask is unchanged by the query.
  TASK_CHECK(sh.ray.mask == ~0u);

  rtcReleaseInstanceExclusion(exc);
  TASK_CHECK(expectNoDeviceError(f.device) == 0);
  f.release();
  return 0;
}

static int state_empty_config_matches_baseline()
{
  Fixture f;
  TASK_CHECK(f.build() == 0);

  RTCRayHit baseline = makeRay(0, 0, 0, 0, 0, 1, 0, 19.0f, 0);
  rtcIntersect1(f.root, &baseline, nullptr);

  RTCIntersectArguments ia; rtcInitIntersectArguments(&ia);
  TASK_CHECK(ia.exclusions == nullptr);
  TASK_CHECK(ia.exclusionCount == 0);
  TASK_CHECK(ia.exclusionStats == nullptr);
  RTCRayHit configured = makeRay(0, 0, 0, 0, 0, 1, 0, 19.0f, 0);
  rtcIntersect1(f.root, &configured, &ia);
  TASK_CHECK(configured.hit.geomID == baseline.hit.geomID);
  TASK_CHECK(std::fabs(configured.ray.tfar - baseline.ray.tfar) < 1e-6f);

  RTCOccludedArguments oa; rtcInitOccludedArguments(&oa);
  TASK_CHECK(oa.exclusions == nullptr);
  TASK_CHECK(oa.exclusionCount == 0);
  TASK_CHECK(oa.exclusionStats == nullptr);
  RTCRayHit b1 = makeRay(0, 0, 0, 0, 0, 1, 0, 5.0f, 0);
  RTCRayHit b2 = makeRay(0, 0, 0, 0, 0, 1, 0, 5.0f, 0);
  rtcOccluded1(f.root, &b1.ray, nullptr);
  rtcOccluded1(f.root, &b2.ray, &oa);
  TASK_CHECK((b1.ray.tfar < 0.0f) == (b2.ray.tfar < 0.0f));

  // Existing filters keep working with an empty exclusion configuration.
  g_filterCalls = 0;
  ia.flags = (RTCRayQueryFlags)(RTC_RAY_QUERY_FLAG_INCOHERENT |
                                RTC_RAY_QUERY_FLAG_INVOKE_ARGUMENT_FILTER);
  ia.filter = argumentFilter;
  RTCRayHit filtered = makeRay(0, 0, 0, 0, 0, 1, 0, 19.0f, 0);
  rtcIntersect1(f.root, &filtered, &ia);
  TASK_CHECK(g_filterCalls.load() >= 1);

  TASK_CHECK(expectNoDeviceError(f.device) == 0);
  f.release();
  return 0;
}

static int state_interleaved_and_concurrent_reuse()
{
  Fixture f;
  TASK_CHECK(f.build() == 0);

  RTCInstanceExclusion excA = makeHandle(f.device, {{edge(IA, INV)}});
  RTCInstanceExclusion excB = makeHandle(f.device, {{edge(IB, INV)}});
  TASK_CHECK(excA && excB);

  const int iterations = 200;
  for (int i = 0; i < iterations; ++i) {
    RTCInstanceExclusion pick = (i & 1) ? excB : excA;
    RTCIntersectArguments ia; rtcInitIntersectArguments(&ia);
    RTCInstanceExclusion table[1] = {pick};
    ia.exclusions = table; ia.exclusionCount = 1;
    RTCRayHit rh = makeRay(0, 0, 0, 0, 0, 1, 0, 19.0f, 0);
    rtcIntersect1(f.root, &rh, &ia);
    const unsigned expected = (i & 1) ? IA : IB;
    TASK_CHECK(rh.hit.instID[0] == expected);
  }

  // Read-only concurrent reuse from several threads.
  std::vector<std::thread> threads;
  std::atomic<int> failures(0);
  for (unsigned t = 0; t < 4; ++t) {
    threads.emplace_back([&, t]() {
      for (int i = 0; i < 100; ++i) {
        RTCInstanceExclusion pick = ((i + t) & 1) ? excB : excA;
        RTCIntersectArguments ia; rtcInitIntersectArguments(&ia);
        RTCInstanceExclusion table[1] = {pick};
        ia.exclusions = table; ia.exclusionCount = 1;
        RTCRayHit rh = makeRay(0, 0, 0, 0, 0, 1, 0, 19.0f, 0);
        rtcIntersect1(f.root, &rh, &ia);
        const unsigned expected = ((i + t) & 1) ? IA : IB;
        if (rh.hit.instID[0] != expected) failures.fetch_add(1);
      }
    });
  }
  for (auto& th : threads) th.join();
  TASK_CHECK(failures.load() == 0);

  rtcReleaseInstanceExclusion(excA);
  rtcReleaseInstanceExclusion(excB);
  TASK_CHECK(expectNoDeviceError(f.device) == 0);
  f.release();
  return 0;
}

static int state_commit_reinterpret_and_rebuild()
{
  Fixture f;
  TASK_CHECK(f.build() == 0);

  // Rules store IDs only: update a transform, recommit, and the same handle
  // keeps interpreting the current instance ID path.
  RTCInstanceExclusion exc = makeHandle(f.device, {{edge(IA, INV)}});
  TASK_CHECK(exc != nullptr);

  RTCGeometry gIA = rtcGetGeometry(f.root, IA);
  float moved[12]; rowMajorTranslation(moved, 0, 0, 4.75f);
  rtcSetGeometryTransform(gIA, 0, RTC_FORMAT_FLOAT3X4_ROW_MAJOR, moved);
  rtcCommitGeometry(gIA);
  rtcCommitScene(f.root);

  RTCIntersectArguments ia; rtcInitIntersectArguments(&ia);
  RTCInstanceExclusion table[1] = {exc};
  ia.exclusions = table; ia.exclusionCount = 1;
  RTCRayHit rh = makeRay(0, 0, 0, 0, 0, 1, 0, 19.0f, 0);
  rtcIntersect1(f.root, &rh, &ia);
  TASK_CHECK(rh.hit.instID[0] != IA); // still excluded by ID path
  TASK_CHECK(rh.hit.instID[0] == IB);

  // A fresh scene rebuilt with the same IDs accepts the unchanged handle.
  f.release();
  Fixture g2;
  TASK_CHECK(g2.build() == 0);
  RTCRayHit rh2 = makeRay(0, 0, 0, 0, 0, 1, 0, 19.0f, 0);
  rtcIntersect1(g2.root, &rh2, &ia);
  TASK_CHECK(rh2.hit.instID[0] != IA);
  g2.release();

  rtcReleaseInstanceExclusion(exc);
  return 0;
}

extern const struct task_case task_cases[];
const struct task_case task_cases[] = {
  {"routes", "scalar_diamond",                routes_scalar_diamond},
  {"routes", "prefix_subtree_and_near_face",  routes_prefix_subtree_and_near_face},
  {"routes", "occluded_shadow_rays",          routes_occluded_shadow_rays},
  {"routes", "user_geometry_not_entered",     routes_user_geometry_not_entered},
  {"routes", "packet4_mixed_lanes",           routes_packet4_mixed_lanes},
  {"routes", "wide_packet_scalar_fallback",   routes_wide_packet_scalar_fallback},
  {"routes", "motion_blur_exclusion",         routes_motion_blur_exclusion},
  {"routes", "mask_ordering",                 routes_mask_ordering},
  {"trie",   "construction_errors",           trie_construction_errors},
  {"trie",   "deep_copy_and_merge",           trie_deep_copy_and_merge},
  {"trie",   "many_rules_no_scan",            trie_many_rules_no_scan},
  {"trie",   "prefix_terminal_skips_descendants", trie_prefix_terminal_skips_descendants},
  {"state",  "context_restoration_and_identity",  state_context_restoration_and_identity},
  {"state",  "empty_config_matches_baseline",     state_empty_config_matches_baseline},
  {"state",  "interleaved_and_concurrent_reuse",  state_interleaved_and_concurrent_reuse},
  {"state",  "commit_reinterpret_and_rebuild",    state_commit_reinterpret_and_rebuild},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
