// Copyright 2025 Photon beam task.
// SPDX-License-Identifier: Apache-2.0

#include "tutorials/photon_beams/renderer.h"

#include <embree4/rtcore.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace
{
  struct Vertex { float x, y, z, w; };

  RTCScene createMirrorScene(RTCDevice device)
  {
    RTCScene scene = rtcNewScene(device);
    RTCGeometry geometry = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_TRIANGLE);

    Vertex* vertices = static_cast<Vertex*>(rtcSetNewGeometryBuffer(
      geometry, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3,
      sizeof(Vertex), 4));
    unsigned* indices = static_cast<unsigned*>(rtcSetNewGeometryBuffer(
      geometry, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT3,
      3 * sizeof(unsigned), 2));

    /* A large ideal-mirror floor inside/below the fog box. */
    vertices[0] = {-5.0f, -1.35f, -5.0f, 0.0f};
    vertices[1] = { 5.0f, -1.35f, -5.0f, 0.0f};
    vertices[2] = { 5.0f, -1.35f,  5.0f, 0.0f};
    vertices[3] = {-5.0f, -1.35f,  5.0f, 0.0f};
    const unsigned triangles[6] = {0, 1, 2, 0, 2, 3};
    for (int i = 0; i < 6; ++i) indices[i] = triangles[i];

    rtcCommitGeometry(geometry);
    rtcAttachGeometry(scene, geometry);
    rtcReleaseGeometry(geometry);
    rtcCommitScene(scene);
    return scene;
  }

  std::uint8_t encodeChannel(double linear, double exposure)
  {
    double mapped = 1.0 - std::exp(-exposure * std::max(0.0, linear));
    mapped = std::pow(std::clamp(mapped, 0.0, 1.0), 1.0 / 2.2);
    return static_cast<std::uint8_t>(std::lround(255.0 * mapped));
  }
}

int main(int argc, char** argv)
{
  const std::string outputDirectory = argc > 1 ? argv[1] : ".";
  const std::string outputPath = outputDirectory + "/beams.ppm";

  RTCDevice device = rtcNewDevice("threads=1");
  if (!device)
  {
    std::cerr << "failed to create Embree device\n";
    return 2;
  }

  RTCScene scene = createMirrorScene(device);

  constexpr int width = 96;
  constexpr int height = 72;
  photon_beams::Renderer renderer(
    device,
    scene,
    photon_beams::Box{{-2.5, -1.25, -2.5}, {2.5, 2.5, 2.5}},
    0.34,
    0.27,
    photon_beams::Vec3{0.15, 0.75, 0.65},
    photon_beams::Vec3{5.0, 4.2, 3.4},
    photon_beams::PerspectiveCamera{
      {0.0, 0.55, 5.2}, {0.0, 0.25, 0.0}, {0.0, 1.0, 0.0}, 42.0},
    width,
    height,
    photon_beams::Settings{0x5151bea7ULL, 6, 0.075});
  renderer.mirrorReflectance = {0.92, 0.95, 1.0};

  constexpr std::size_t photonsPerRound = 20000;
  constexpr int rounds = 6;
  for (int round = 0; round < rounds; ++round)
  {
    if (!renderer.step(photonsPerRound))
    {
      std::cerr << "renderer step failed\n";
      return 3;
    }
    std::cout << "round " << (round + 1) << '/' << rounds
              << " beams=" << renderer.currentBeams.size()
              << " radius=" << renderer.currentRadius
              << " emissions=" << renderer.emittedPhotons << '\n';
  }

  std::ofstream output(outputPath, std::ios::binary);
  if (!output)
  {
    std::cerr << "cannot open " << outputPath << '\n';
    return 4;
  }

  output << "P6\n" << width << ' ' << height << "\n255\n";
  for (int y = 0; y < height; ++y)
  {
    for (int x = 0; x < width; ++x)
    {
      photon_beams::Vec3 rgb = renderer.pixel(x, y);
      const double exposure = 5.0;
      const unsigned char bytes[3] = {
        encodeChannel(rgb.x, exposure),
        encodeChannel(rgb.y, exposure),
        encodeChannel(rgb.z, exposure)
      };
      output.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
    }
  }

  std::cout << "wrote " << outputPath
            << " total_emissions=" << renderer.emittedPhotons
            << " query_node_visits=" << renderer.lastQueryStats.nodeVisits
            << " query_beam_tests=" << renderer.lastQueryStats.beamTests << '\n';

  rtcReleaseScene(scene);
  rtcReleaseDevice(device);
  return output ? 0 : 5;
}
