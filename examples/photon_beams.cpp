// Copyright 2009-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

// Windowless photon beam demonstration: builds a mirror box filled with
// a homogeneous participating medium, runs progressive photon-beam
// rounds through photon_beams::Renderer and writes a binary PPM image
// named beams.ppm into the directory given on the command line.

#include <embree4/rtcore.h>
#include "tutorials/photon_beams/renderer.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace photon_beams;

namespace
{
  struct Vertex { float x, y, z; };

  RTCScene buildMirrorBox(RTCDevice device, const MediumBox& box)
  {
    RTCScene scene = rtcNewScene(device);
    RTCGeometry geom = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_TRIANGLE);

    auto* vertices = (Vertex*) rtcSetNewGeometryBuffer(
      geom, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3,
      sizeof(Vertex), 8);
    auto* indices = (unsigned*) rtcSetNewGeometryBuffer(
      geom, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT3,
      3 * sizeof(unsigned), 12);

    const double l[3] = { box.lower[0], box.lower[1], box.lower[2] };
    const double u[3] = { box.upper[0], box.upper[1], box.upper[2] };
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
    const Vertex corners[8] =
    {
      {float(l[0]),float(l[1]),float(l[2])}, {float(l[0]),float(l[1]),float(u[2])},
      {float(l[0]),float(u[1]),float(l[2])}, {float(l[0]),float(u[1]),float(u[2])},
      {float(u[0]),float(l[1]),float(l[2])}, {float(u[0]),float(l[1]),float(u[2])},
      {float(u[0]),float(u[1]),float(l[2])}, {float(u[0]),float(u[1]),float(u[2])}
    };
    for (int i = 0; i < 8; ++i) vertices[i] = corners[i];
    for (int i = 0; i < 12; ++i)
      for (int j = 0; j < 3; ++j)
        indices[3*i+j] = faces[i][j];

    rtcCommitGeometry(geom);
    rtcAttachGeometry(scene, geom);
    rtcReleaseGeometry(geom);
    rtcCommitScene(scene);
    return scene;
  }

  void writePPM(const std::string& path,
                const Renderer& renderer, size_t width, size_t height)
  {
    FILE* file = std::fopen(path.c_str(), "wb");
    if (!file)
    {
      std::fprintf(stderr, "cannot open %s\n", path.c_str());
      std::exit(1);
    }
    std::fprintf(file, "P6\n%zu %zu\n255\n", width, height);
    for (size_t y = 0; y < height; ++y)
    {
      for (size_t x = 0; x < width; ++x)
      {
        double rgb[3];
        renderer.pixel(x, y, rgb);
        unsigned char pixel[3];
        for (int c = 0; c < 3; ++c)
        {
          // Linear result -> simple clamp and gamma 2.2 for display.
          double v = rgb[c];
          if (!(v >= 0.0)) v = 0.0;
          v = std::pow(std::min(v, 1.0), 1.0 / 2.2);
          pixel[c] = (unsigned char) std::lround(255.0 * v);
        }
        std::fwrite(pixel, 1, 3, file);
      }
    }
    std::fclose(file);
  }
}

int main(int argc, char** argv)
{
  if (argc != 2)
  {
    std::fprintf(stderr, "usage: %s <output-directory>\n", argv[0]);
    return 1;
  }
  const std::string outDir = argv[1];

  RTCDevice device = rtcNewDevice("threads=1");
  if (!device) return 2;

  const MediumBox box = { { -1.0, -1.0, -1.0 }, { 1.0, 1.0, 1.0 } };
  RTCScene scene = buildMirrorBox(device, box);

  PointLight light;
  light.position[0] = 0.0; light.position[1] = 0.0; light.position[2] = 0.0;
  light.power[0] = 18.0; light.power[1] = 18.0; light.power[2] = 18.0;

  PerspectiveCamera camera;
  camera.origin[0] = 0.0; camera.origin[1] = 0.0; camera.origin[2] = 1.4;
  camera.direction[0] = 0.0; camera.direction[1] = 0.0; camera.direction[2] = -1.0;
  camera.up[0] = 0.0; camera.up[1] = 1.0; camera.up[2] = 0.0;
  camera.fovY = 1.1;

  const MediumModel medium = { 0.55, 0.45 };       // sigma_t, sigma_s
  const SurfaceModel surface = { { 0.85, 0.85, 0.85 } };
  const RenderParams params = { /*seed*/ 1234567, /*maxEvents*/ 8, /*radius*/ 0.04 };

  const size_t width = 64, height = 48;
  Renderer renderer(device, scene, box, light, camera, medium, surface,
                    width, height, params);

  for (size_t round = 0; round < 3; ++round)
    renderer.step(20000);

  writePPM(outDir + "/beams.ppm", renderer, width, height);

  const auto& diag = renderer.diagnostics();
  std::printf("rounds=%zu emissions=%llu last_round_beams=%zu "
              "beamTests=%llu supportIntervals=%llu\n",
              renderer.rounds(),
              (unsigned long long) renderer.totalEmissions(),
              diag.beamCount,
              (unsigned long long) diag.beamTests,
              (unsigned long long) diag.supportIntervals);

  rtcReleaseScene(scene);
  rtcReleaseDevice(device);
  return 0;
}
