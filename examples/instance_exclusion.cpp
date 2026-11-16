// Copyright 2009-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

// Window-free instance exclusion example. Renders one nested-instance scene
// twice with two different RTCInstanceExclusion configurations and writes:
//   <output_dir>/exclude_a.ppm  (the near instance A is excluded)
//   <output_dir>/exclude_b.ppm  (the far instance B is excluded)
//
// Build and run through the task harness:
//   bash task_support/run.sh target
// or directly after building:
//   .task-build/task-example <output_dir>

#include <embree4/rtcore.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

const unsigned GEOM_INST_A = 10;
const unsigned GEOM_INST_B = 11;
const unsigned LEAF_GEOM   = 20;
const unsigned INV = RTC_INVALID_GEOMETRY_ID;

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
  rtcCommitGeometry(g);
  return g;
}

RTCGeometry makeInstance(RTCDevice device, RTCScene child,
                         float tx, float tz, float scale)
{
  RTCGeometry g = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_INSTANCE);
  rtcSetGeometryInstancedScene(g, child);
  // Row-major 3x4: uniform scale plus translation.
  const float m[12] = {
    scale, 0, 0, tx,
    0, scale, 0, 0,
    0, 0, scale, tz
  };
  rtcSetGeometryTransform(g, 0, RTC_FORMAT_FLOAT3X4_ROW_MAJOR, m);
  rtcCommitGeometry(g);
  return g;
}

RTCInstanceExclusion makeSinglePath(RTCDevice device, unsigned geomID)
{
  RTCInstanceExclusionPath path[1] = {{geomID, INV}};
  const RTCInstanceExclusionPath* ptrs[1] = {path};
  size_t lengths[1] = {1};
  return rtcNewInstanceExclusion(device, ptrs, lengths, 1);
}

struct RGB { unsigned char r, g, b; };

RGB shade(const RTCRayHit& rh)
{
  if (rh.hit.geomID == INV) return {0, 0, 0}; // background
  if (rh.hit.instID[0] == GEOM_INST_A) return {220, 40, 40};  // red placement
  if (rh.hit.instID[0] == GEOM_INST_B) return {40, 90, 230};  // blue placement
  return {230, 230, 230};
}

void render(RTCScene root, RTCInstanceExclusion exclusion,
            int width, int height, std::vector<unsigned char>& pixels)
{
  pixels.resize((size_t)width * height * 3);

  RTCIntersectArguments args;
  rtcInitIntersectArguments(&args);
  RTCInstanceExclusion table[1] = {exclusion};
  args.exclusions = table;
  args.exclusionCount = exclusion ? 1u : 0u;

  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      RTCRayHit rh{};
      rh.ray.org_x = 0; rh.ray.org_y = 0; rh.ray.org_z = 0; rh.ray.tnear = 0;
      // Map pixel centers to [-2, 2] in x/y; rays travel along +z.
      rh.ray.dir_x = (2.0f * (x + 0.5f) / width - 1.0f) * 2.0f;
      rh.ray.dir_y = (2.0f * (y + 0.5f) / height - 1.0f) * 2.0f;
      rh.ray.dir_z = 1.0f;
      rh.ray.time = 0.0f;
      rh.ray.tfar = 1.0e30f;
      rh.ray.mask = ~0u;
      rh.ray.id = 0;
      rh.ray.flags = 0;
      rh.hit.geomID = INV;
      for (unsigned l = 0; l < RTC_MAX_INSTANCE_LEVEL_COUNT; ++l) {
        rh.hit.instID[l] = INV;
        rh.hit.instPrimID[l] = INV;
      }
      rtcIntersect1(root, &rh, &args);
      const RGB c = shade(rh);
      const size_t o = ((size_t)y * width + x) * 3;
      pixels[o] = c.r; pixels[o + 1] = c.g; pixels[o + 2] = c.b;
    }
  }
}

bool writePPM(const char* path, int width, int height,
              const std::vector<unsigned char>& pixels)
{
  FILE* f = std::fopen(path, "wb");
  if (!f) return false;
  std::fprintf(f, "P6\n%d %d\n255\n", width, height);
  const bool ok =
    std::fwrite(pixels.data(), 1, pixels.size(), f) == pixels.size();
  std::fclose(f);
  return ok;
}

} // namespace

int main(int argc, char** argv)
{
  if (argc != 2) {
    std::fprintf(stderr,
      "usage: %s <output_dir>\n"
      "  writes exclude_a.ppm and exclude_b.ppm into <output_dir>\n",
      argv[0]);
    return 1;
  }

  RTCDevice device = rtcNewDevice("threads=1");
  if (!device) return 2;

  RTCScene leaf = rtcNewScene(device);
  RTCGeometry tri = makeTriangle(device);
  rtcAttachGeometryByID(leaf, tri, LEAF_GEOM);
  rtcReleaseGeometry(tri);
  rtcCommitScene(leaf);

  RTCScene root = rtcNewScene(device);
  // Two placements of the same leaf scene: near red A on the left, far
  // blue B on the right.
  RTCGeometry instA = makeInstance(device, leaf, -0.7f, 3.0f, 0.7f);
  rtcAttachGeometryByID(root, instA, GEOM_INST_A);
  rtcReleaseGeometry(instA);
  RTCGeometry instB = makeInstance(device, leaf, 0.7f, 6.0f, 0.7f);
  rtcAttachGeometryByID(root, instB, GEOM_INST_B);
  rtcReleaseGeometry(instB);
  rtcCommitScene(root);

  RTCInstanceExclusion excA = makeSinglePath(device, GEOM_INST_A);
  RTCInstanceExclusion excB = makeSinglePath(device, GEOM_INST_B);
  if (!excA || !excB || rtcGetDeviceError(device) != RTC_ERROR_NONE) return 3;

  const int width = 96, height = 96;
  std::vector<unsigned char> pixels;

  char pathA[1024], pathB[1024];
  std::snprintf(pathA, sizeof(pathA), "%s/exclude_a.ppm", argv[1]);
  std::snprintf(pathB, sizeof(pathB), "%s/exclude_b.ppm", argv[1]);

  render(root, excA, width, height, pixels); // A excluded: only B remains
  if (!writePPM(pathA, width, height, pixels)) {
    std::fprintf(stderr, "failed to write %s\n", pathA);
    return 4;
  }
  render(root, excB, width, height, pixels); // B excluded: only A remains
  if (!writePPM(pathB, width, height, pixels)) {
    std::fprintf(stderr, "failed to write %s\n", pathB);
    return 5;
  }

  std::printf("wrote %s (instance %u excluded, placement %u retained)\n",
              pathA, GEOM_INST_A, GEOM_INST_B);
  std::printf("wrote %s (instance %u excluded, placement %u retained)\n",
              pathB, GEOM_INST_B, GEOM_INST_A);
  std::printf("rerun: %s %s\n", argv[0], argv[1]);

  rtcReleaseInstanceExclusion(excA);
  rtcReleaseInstanceExclusion(excB);
  rtcReleaseScene(root);
  rtcReleaseScene(leaf);
  rtcReleaseDevice(device);
  return 0;
}
