// Task regression suite for progressive stochastic transparency.
//
// Three groups are registered through task_support/check.h:
//   identity     - stable surface ids, opacity/hash contracts, depth ties
//   sampling     - opacity endpoints, occlusion, source-over convergence and
//                  deterministic replay, scalar/SIMD and ordering invariance
//   accumulation - frame sequencing, online mean, reset, draw/clear and
//                  renderer isolation
//
// Every case exercises the production TraditionalRasterizer draw/advance API;
// the only independently reimplemented math is the source-over expectation and
// direct evaluation of the documented hash.
#include "task_support/check.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <render/Rasterizer.hpp>
#include <render/Stochastic.hpp>
#include <set>
#include <stdexcept>
#include <vector>

using namespace SoftRasterizer;

namespace {

constexpr std::size_t Width = 32;
constexpr std::size_t Height = 32;
constexpr std::size_t Center = (Height / 2) * Width + (Width / 2);

bool vecEqual(const glm::vec3 &a, const glm::vec3 &b) {
  return a.x == b.x && a.y == b.y && a.z == b.z;
}

// Inside lambdas TASK_CHECK's early return has the wrong type, so use a
// throwing precondition helper there.
void require(bool condition) {
  if (!condition) {
    throw std::runtime_error("scene setup precondition failed");
  }
}

template <class Fn> bool throwsInvalidArgument(Fn &&fn) {
  try {
    fn();
  } catch (const std::invalid_argument &) {
    return true;
  } catch (...) {
    return false;
  }
  return false;
}

std::shared_ptr<Scene> makeScene(glm::vec3 background = glm::vec3(0)) {
  auto scene = std::make_shared<Scene>(
      "task", glm::vec3(0), glm::vec3(0, 0, -1), glm::vec3(0, 1, 0),
      background);
  scene->setProjectionMatrix(90.f, 0.05f, 100.f);
  return scene;
}

std::shared_ptr<Triangle> makeTriangle(glm::vec3 kd, float opacity,
                                       std::array<Vertex, 3> vertices) {
  auto material = std::make_shared<Material>();
  material->Kd = kd;
  material->setOpacity(opacity);
  auto triangle = std::make_shared<Triangle>(vertices, material);
  auto shader = std::make_shared<Shader>();
  shader->type = SHADERS_TYPE::TEXTURE;
  triangle->bindShader2Mesh(shader);
  return triangle;
}

// Camera-facing quad at a fixed depth; two triangles, TEXTURE (unlit) shading.
std::shared_ptr<Mesh> makeQuad(float z, glm::vec3 kd, float opacity) {
  std::vector<Vertex> vertices{
      Vertex({-0.8f, -0.8f, z}, {}, {0, 0}),
      Vertex({0.8f, -0.8f, z}, {}, {1, 0}),
      Vertex({0.8f, 0.8f, z}, {}, {1, 1}),
      Vertex({-0.8f, 0.8f, z}, {}, {0, 1})};
  std::vector<glm::uvec3> faces{{0, 1, 2}, {0, 2, 3}};
  auto material = std::make_shared<Material>();
  material->Kd = kd;
  material->setOpacity(opacity);
  auto mesh = std::make_shared<Mesh>(
      vertices, faces,
      std::vector<std::shared_ptr<Material>>{material, material});
  auto shader = std::make_shared<Shader>();
  shader->type = SHADERS_TYPE::TEXTURE;
  mesh->bindShader2Mesh(shader);
  return mesh;
}

std::unique_ptr<TraditionalRasterizer>
makeRasterizer(bool simd, std::uint64_t seed, std::uint64_t generation = 0) {
  auto rasterizer = std::make_unique<TraditionalRasterizer>(Width, Height);
  rasterizer->setSIMD(simd);
  rasterizer->configureStochastic(StochasticSettings{seed, generation});
  return rasterizer;
}

// ---------------------------------------------------------------------------
// identity group
// ---------------------------------------------------------------------------

int surfaceIdentityRules() {
  auto scene = makeScene();
  auto nearTriangle = makeTriangle(
      glm::vec3(1, 0, 0), 1.f,
      {Vertex({-0.5f, -0.5f, -2}), Vertex({0.5f, -0.5f, -2}),
       Vertex({0.f, 0.5f, -2})});
  auto farTriangle = makeTriangle(
      glm::vec3(0, 0, 1), 1.f,
      {Vertex({-0.5f, -0.5f, -4}), Vertex({0.5f, -0.5f, -4}),
       Vertex({0.f, 0.5f, -4})});
  TASK_CHECK(scene->addGraphicObj(nearTriangle, "near"));
  TASK_CHECK(scene->addGraphicObj(farTriangle, "far"));
  TASK_CHECK(scene->setSurfaceId("near", 100));
  TASK_CHECK(scene->surfaceId("near") == 100);
  TASK_CHECK(scene->surfaceId("far") == 0); // Automatic until prepare().
  TASK_CHECK(!scene->setSurfaceId("missing", 7));
  TASK_CHECK(throwsInvalidArgument([&] { scene->setSurfaceId("near", 0); }));

  scene->prepare();
  std::set<std::uint64_t> firstIds;
  for (const auto &triangle : scene->triangles()) {
    TASK_CHECK(triangle.surfaceId != 0);
    TASK_CHECK(firstIds.insert(triangle.surfaceId).second);
  }
  TASK_CHECK(firstIds.count(100) == 1);
  const auto automaticFar = scene->surfaceId("far");
  TASK_CHECK(automaticFar != 0 && automaticFar != 100);

  // Cache rebuild and the name-sorted object traversal must preserve ids.
  scene->buildBVHAccel();
  std::set<std::uint64_t> rebuiltIds;
  for (const auto &triangle : scene->triangles()) {
    TASK_CHECK(rebuiltIds.insert(triangle.surfaceId).second);
  }
  TASK_CHECK(rebuiltIds == firstIds);
  TASK_CHECK(scene->surfaceId("near") == 100);
  TASK_CHECK(scene->surfaceId("far") == automaticFar);

  // Duplicate explicit ids are rejected before any frame can be rendered.
  auto duplicate = makeScene();
  duplicate->addGraphicObj(
      makeTriangle(glm::vec3(1), 1.f,
                   {Vertex({-0.5f, -0.5f, -2}), Vertex({0.5f, -0.5f, -2}),
                    Vertex({0.f, 0.5f, -2})}),
      "a");
  duplicate->addGraphicObj(
      makeTriangle(glm::vec3(1), 1.f,
                   {Vertex({-0.5f, -0.5f, -4}), Vertex({0.5f, -0.5f, -4}),
                    Vertex({0.f, 0.5f, -4})}),
      "b");
  TASK_CHECK(duplicate->setSurfaceId("a", 5));
  TASK_CHECK(duplicate->setSurfaceId("b", 5));
  TASK_CHECK(throwsInvalidArgument([&] { duplicate->prepare(); }));
  return 0;
}

int opacityAndHashContract() {
  Material material;
  TASK_CHECK(material.opacity == 1.f);
  material.setOpacity(0.f);
  TASK_CHECK(material.opacity == 0.f);
  material.setOpacity(0.375f);
  TASK_CHECK(std::abs(material.opacity - 0.375f) < 1e-7f);
  const float before = material.opacity;
  TASK_CHECK(throwsInvalidArgument([&] { material.setOpacity(NAN); }));
  TASK_CHECK(throwsInvalidArgument([&] { material.setOpacity(-0.01f); }));
  TASK_CHECK(throwsInvalidArgument([&] { material.setOpacity(1.01f); }));
  TASK_CHECK(material.opacity == before); // Rejected writes change nothing.

  // Deterministic, pure-function hash.
  const auto h1 = stochasticHash64(11, 2, 3, 4, 5, 6, 7);
  const auto h2 = stochasticHash64(11, 2, 3, 4, 5, 6, 7);
  TASK_CHECK(h1 == h2);
  TASK_CHECK(stochasticHash64(11, 2, 3, 4, 5, 6, 8) != h1);
  TASK_CHECK(stochasticHash64(12, 2, 3, 4, 5, 6, 7) != h1);
  TASK_CHECK(stochasticHash64(11, 3, 3, 4, 5, 6, 7) != h1);
  TASK_CHECK(stochasticHash64(11, 2, 9, 4, 5, 6, 7) != h1);
  TASK_CHECK(stochasticHash64(11, 2, 3, 14, 5, 6, 7) != h1);
  TASK_CHECK(stochasticHash64(11, 2, 3, 4, 15, 6, 7) != h1);
  TASK_CHECK(stochasticHash64(11, 2, 3, 4, 5, 16, 7) != h1);

  // Randoms stay on [0, 1); every surface gets an independent draw.
  std::set<std::uint64_t> surfaceDraws;
  for (std::uint64_t surface = 1; surface <= 64; ++surface) {
    const float random =
        stochasticHashRandom(99, 0, 0, 10, 20, 3, surface);
    TASK_CHECK(std::isfinite(random) && random >= 0.f && random < 1.f);
    TASK_CHECK(surfaceDraws
                   .insert(stochasticHash64(99, 0, 0, 10, 20, 3, surface))
                   .second);
  }
  for (std::uint64_t i = 0; i < 2000; ++i) {
    const float random =
        stochasticHashRandom(i, i * 3, i * 7, i * 11, i * 13, i % 8, i + 1);
    TASK_CHECK(random >= 0.f && random < 1.f);
  }
  // r < 1 always passes for an opaque surface; exact replay is bit identical.
  TASK_CHECK(stochasticHashRandom(1, 0, 0, 0, 0, 0, 1) ==
             stochasticHashRandom(1, 0, 0, 0, 0, 0, 1));
  return 0;
}

int equalDepthSurfaceArbitration() {
  auto scene = makeScene();
  auto vertices = std::array<Vertex, 3>{Vertex({-0.7f, -0.7f, -2}),
                                        Vertex({0.7f, -0.7f, -2}),
                                        Vertex({0.f, 0.7f, -2})};
  scene->addGraphicObj(makeTriangle(glm::vec3(1, 0, 0), 1.f, vertices), "big");
  scene->addGraphicObj(makeTriangle(glm::vec3(0, 1, 0), 1.f, vertices),
                       "small");
  TASK_CHECK(scene->setSurfaceId("big", 9));
  TASK_CHECK(scene->setSurfaceId("small", 5));
  scene->prepare();

  auto rasterizer = makeRasterizer(true, 42);
  rasterizer->addScene(scene);
  rasterizer->advance(0, 1);
  TASK_CHECK(rasterizer->accumulatedFrames() == 1);
  for (int sample = 0; sample < StochasticSampleCount; ++sample) {
    // Equal depth is resolved by the smaller surfaceId, not submission order.
    TASK_CHECK(rasterizer->stochasticSampleSurface(Center, sample) == 5);
    TASK_CHECK(std::isfinite(rasterizer->stochasticSampleDepth(Center, sample)));
  }
  // The winning material is the green (id 5) surface.
  const auto color = rasterizer->pixels()[Center];
  TASK_CHECK(std::abs(color.g - 1.f) < 1e-6f &&
             std::abs(color.r) < 1e-6f && std::abs(color.b) < 1e-6f);
  return 0;
}

// ---------------------------------------------------------------------------
// sampling group
// ---------------------------------------------------------------------------

int zeroAndOneOpacity() {
  auto scene = makeScene(glm::vec3(0.1f, 0.2f, 0.3f));
  scene->addGraphicObj(makeQuad(-2, glm::vec3(1, 1, 1), 0.f), "ghost");
  TASK_CHECK(scene->setSurfaceId("ghost", 20));
  scene->prepare();
  auto invisible = makeRasterizer(true, 7);
  invisible->addScene(scene);
  invisible->advance(0, 2);
  for (int sample = 0; sample < StochasticSampleCount; ++sample) {
    TASK_CHECK(!std::isfinite(
        invisible->stochasticSampleDepth(Center, sample))); // No depth write.
    TASK_CHECK(invisible->stochasticSampleSurface(Center, sample) == 0);
  }
  const auto ghosted = invisible->pixels()[Center];
  TASK_CHECK(std::abs(ghosted.r - 0.1f) < 1e-6f &&
             std::abs(ghosted.g - 0.2f) < 1e-6f &&
             std::abs(ghosted.b - 0.3f) < 1e-6f);

  auto opaqueScene = makeScene(glm::vec3(0.1f, 0.2f, 0.3f));
  opaqueScene->addGraphicObj(makeQuad(-2, glm::vec3(1, 1, 1), 1.f),
                             "solid");
  TASK_CHECK(opaqueScene->setSurfaceId("solid", 21));
  opaqueScene->prepare();
  auto solid = makeRasterizer(false, 7);
  solid->addScene(opaqueScene);
  solid->advance(0, 1);
  for (int sample = 0; sample < StochasticSampleCount; ++sample) {
    // The quad mesh owns the [21, 22] id range (one id per source triangle).
    const auto id = solid->stochasticSampleSurface(Center, sample);
    TASK_CHECK(id == 21 || id == 22);
    TASK_CHECK(std::isfinite(solid->stochasticSampleDepth(Center, sample)));
  }
  const auto covered = solid->pixels()[Center];
  TASK_CHECK(std::abs(covered.r - 1.f) < 1e-6f &&
             std::abs(covered.g - 1.f) < 1e-6f &&
             std::abs(covered.b - 1.f) < 1e-6f);

  // A rejected stochastic fragment leaves its sample at background/infinity.
  auto halfScene = makeScene(glm::vec3(0));
  halfScene->addGraphicObj(makeQuad(-2, glm::vec3(1, 1, 1), 0.5f), "half");
  TASK_CHECK(halfScene->setSurfaceId("half", 22));
  halfScene->prepare();
  auto half = makeRasterizer(true, 123);
  half->addScene(halfScene);
  half->advance(0, 1);
  int accepted = 0;
  for (int sample = 0; sample < StochasticSampleCount; ++sample) {
    if (half->stochasticSampleSurface(Center, sample) != 0) {
      ++accepted;
    } else {
      TASK_CHECK(!std::isfinite(half->stochasticSampleDepth(Center, sample)));
    }
  }
  TASK_CHECK(accepted >= 1 && accepted < StochasticSampleCount);
  return 0;
}

int foregroundOpaqueOcclusion() {
  auto scene = makeScene();
  // Opaque red in front, transparent blue behind, same covered region. Each
  // quad mesh owns a two-id range, so leave a gap between the base ids.
  scene->addGraphicObj(makeQuad(-2, glm::vec3(1, 0, 0), 1.f), "front");
  scene->addGraphicObj(makeQuad(-3, glm::vec3(0, 0, 1), 0.5f), "back");
  TASK_CHECK(scene->setSurfaceId("front", 30));
  TASK_CHECK(scene->setSurfaceId("back", 32));
  scene->prepare();

  auto rasterizer = makeRasterizer(true, 55);
  rasterizer->addScene(scene);
  rasterizer->advance(0, 16);
  for (int sample = 0; sample < StochasticSampleCount; ++sample) {
    const auto id = rasterizer->stochasticSampleSurface(Center, sample);
    TASK_CHECK(id == 30 || id == 31); // Only the front quad can win.
  }
  const auto color = rasterizer->pixels()[Center];
  TASK_CHECK(std::abs(color.r - 1.f) < 1e-6f);
  TASK_CHECK(std::abs(color.g) < 1e-6f && std::abs(color.b) < 1e-6f);
  return 0;
}

int sourceOverConvergenceAndReplay() {
  const glm::vec3 background(0.05f, 0.08f, 0.11f);
  const glm::vec3 nearColor(1, 0, 0), farColor(0, 0, 1);
  const float nearOpacity = 0.6f, farOpacity = 0.4f;
  auto buildScene = [&] {
    auto scene = makeScene(background);
    scene->addGraphicObj(makeQuad(-2, nearColor, nearOpacity), "near");
    scene->addGraphicObj(makeQuad(-3, farColor, farOpacity), "far");
    require(scene->setSurfaceId("near", 40));
    require(scene->setSurfaceId("far", 42));
    scene->prepare();
    return scene;
  };

  auto scene = buildScene();
  auto rasterizer = makeRasterizer(true, 2026);
  rasterizer->addScene(scene);
  constexpr std::uint64_t frames = 512;
  rasterizer->advance(0, frames);
  TASK_CHECK(rasterizer->accumulatedFrames() == frames);

  // Independent per-pixel source-over expectation, derived without calling
  // any production stochastic code:
  //   E = a*Ca + (1-a)*(b*Cb + (1-b)*background).
  const double a = nearOpacity, b = farOpacity;
  glm::dvec3 expected;
  for (int channel = 0; channel < 3; ++channel) {
    expected[channel] =
        a * nearColor[channel] +
        (1.0 - a) * (b * farColor[channel] + (1.0 - b) * background[channel]);
  }
  const auto actual = rasterizer->pixels()[Center];
  for (int channel = 0; channel < 3; ++channel) {
    TASK_CHECK(std::isfinite(actual[channel]));
    TASK_CHECK(std::abs(double(actual[channel]) - expected[channel]) <= 0.025);
  }

  // Single-frame exact replay on a fresh renderer: same seed/frame must
  // reproduce every sample decision and the frame mean bit for bit.
  auto replayScene = buildScene();
  auto first = makeRasterizer(false, 2026);
  first->addScene(scene);
  first->advance(0, 1);
  auto replayed = makeRasterizer(false, 2026);
  replayed->addScene(replayScene);
  replayed->advance(0, 1);
  for (std::size_t pixel = 0; pixel < Width * Height; ++pixel) {
    for (int sample = 0; sample < StochasticSampleCount; ++sample) {
      TASK_CHECK(first->stochasticSampleSurface(pixel, sample) ==
                 replayed->stochasticSampleSurface(pixel, sample));
      const float d1 = first->stochasticSampleDepth(pixel, sample),
                  d2 = replayed->stochasticSampleDepth(pixel, sample);
      TASK_CHECK(std::isnan(d1) == std::isnan(d2) &&
                 (std::isnan(d1) || d1 == d2));
    }
    for (int channel = 0; channel < 3; ++channel) {
      TASK_CHECK(first->pixels()[pixel][channel] ==
                 replayed->pixels()[pixel][channel]);
    }
  }

  // Different seeds must drive visibly different single-frame samples.
  auto otherSeed = makeRasterizer(true, 9090);
  otherSeed->addScene(buildScene());
  otherSeed->advance(0, 1);
  int differences = 0;
  for (int sample = 0; sample < StochasticSampleCount; ++sample) {
    if (first->stochasticSampleSurface(Center, sample) !=
        otherSeed->stochasticSampleSurface(Center, sample)) {
      ++differences;
    }
  }
  TASK_CHECK(differences > 0);
  return 0;
}

int scalarSimdAndOrderInvariance() {
  const auto buildScene = [](bool reversedNames) {
    auto scene = makeScene();
    auto red = makeQuad(-2, glm::vec3(1, 0, 0), 0.55f);
    auto blue = makeQuad(-3, glm::vec3(0, 0, 1), 0.35f);
    if (!reversedNames) {
      scene->addGraphicObj(red, "near");
      scene->addGraphicObj(blue, "far");
    } else {
      scene->addGraphicObj(blue, "far");
      scene->addGraphicObj(red, "near");
    }
    require(scene->setSurfaceId("near", 50));
    require(scene->setSurfaceId("far", 52));
    scene->prepare();
    return scene;
  };

  auto simdRenderer = makeRasterizer(true, 88);
  simdRenderer->addScene(buildScene(false));
  simdRenderer->advance(0, 24);
  auto scalarRenderer = makeRasterizer(false, 88);
  scalarRenderer->addScene(buildScene(false));
  scalarRenderer->advance(0, 24);

  // Submission/name ordering cannot change hash-driven results because ids,
  // not order, identify surfaces; scalar and SIMD settings match exactly.
  auto reorderedRenderer = makeRasterizer(true, 88);
  reorderedRenderer->addScene(buildScene(true));
  reorderedRenderer->advance(0, 24);

  for (std::size_t pixel = 0; pixel < Width * Height; ++pixel) {
    for (int channel = 0; channel < 3; ++channel) {
      const float reference = simdRenderer->pixels()[pixel][channel];
      TASK_CHECK(scalarRenderer->pixels()[pixel][channel] == reference);
      TASK_CHECK(reorderedRenderer->pixels()[pixel][channel] == reference);
    }
    for (int sample = 0; sample < StochasticSampleCount; ++sample) {
      TASK_CHECK(simdRenderer->stochasticSampleSurface(pixel, sample) ==
                 scalarRenderer->stochasticSampleSurface(pixel, sample));
      TASK_CHECK(simdRenderer->stochasticSampleSurface(pixel, sample) ==
                 reorderedRenderer->stochasticSampleSurface(pixel, sample));
    }
  }
  return 0;
}

int perspectiveTexturedShading() {
  // Three different clip-space w values force perspective-correct UVs.
  auto texture = std::make_shared<TextureLoader>(
      2, 2, std::vector<glm::vec3>{{0.9f, 0.1f, 0.1f}, {0.1f, 0.9f, 0.1f},
                                   {0.1f, 0.1f, 0.9f}, {0.9f, 0.9f, 0.1f}});
  auto material = std::make_shared<Material>();
  material->Kd = glm::vec3(1);
  material->texture = texture;
  material->setOpacity(1.f);
  auto triangle = std::make_shared<Triangle>(
      std::array<Vertex, 3>{Vertex({-1, -1, -1}, {}, {0, 0}),
                            Vertex({1, -1, -4}, {}, {1, 0}),
                            Vertex({0, 1, -2}, {}, {0.5f, 1})},
      material);
  auto shader = std::make_shared<Shader>();
  shader->type = SHADERS_TYPE::TEXTURE;
  triangle->bindShader2Mesh(shader);

  auto scene = makeScene();
  scene->addGraphicObj(triangle, "slanted");
  scene->setSurfaceId("slanted", 60);
  scene->prepare();

  TraditionalRasterizer opaque(Width, Height);
  opaque.addScene(scene);
  opaque.draw();

  auto stochastic = makeRasterizer(true, 5);
  stochastic->addScene(scene);
  stochastic->advance(0, 1);
  TASK_CHECK(stochastic->accumulatedFrames() == 1);

  int compared = 0;
  for (std::size_t pixel = 0; pixel < Width * Height; ++pixel) {
    if (!std::isfinite(opaque.depth()[pixel])) {
      continue;
    }
    ++compared;
    // Opaque stochastic samples reuse the exact production shading rules.
    for (int channel = 0; channel < 3; ++channel) {
      TASK_CHECK(std::abs(stochastic->pixels()[pixel][channel] -
                          opaque.pixels()[pixel][channel]) < 1e-6f);
    }
  }
  TASK_CHECK(compared > 20);
  return 0;
}

// ---------------------------------------------------------------------------
// accumulation group
// ---------------------------------------------------------------------------

int frameRangeValidation() {
  auto scene = makeScene();
  scene->addGraphicObj(makeQuad(-2, glm::vec3(1), 0.5f), "quad");
  scene->setSurfaceId("quad", 70);
  scene->prepare();

  auto rasterizer = std::make_unique<TraditionalRasterizer>(Width, Height);
  rasterizer->addScene(scene);
  TASK_CHECK(throwsInvalidArgument([&] { rasterizer->advance(0, 1); }));

  rasterizer->configureStochastic(StochasticSettings{1, 0});
  // Frames must begin at zero.
  TASK_CHECK(throwsInvalidArgument([&] { rasterizer->advance(1, 1); }));
  TASK_CHECK(rasterizer->accumulatedFrames() == 0);
  TASK_CHECK(throwsInvalidArgument([&] { rasterizer->advance(0, 0); }));
  TASK_CHECK(throwsInvalidArgument(
      [&] { rasterizer->advance(1, std::uint64_t(-1)); }));

  rasterizer->advance(0, 2);
  TASK_CHECK(rasterizer->accumulatedFrames() == 2);
  const auto meanAfterTwo = rasterizer->pixels()[Center];
  // Repeats and gaps are rejected and leave the history untouched.
  TASK_CHECK(throwsInvalidArgument([&] { rasterizer->advance(1, 1); }));
  TASK_CHECK(throwsInvalidArgument([&] { rasterizer->advance(3, 1); }));
  TASK_CHECK(rasterizer->accumulatedFrames() == 2);
  for (int channel = 0; channel < 3; ++channel) {
    TASK_CHECK(rasterizer->pixels()[Center][channel] ==
               meanAfterTwo[channel]);
  }
  rasterizer->advance(2, 1);
  TASK_CHECK(rasterizer->accumulatedFrames() == 3);

  // An invalid (NaN) opacity is rejected before any accumulator update.
  scene->triangles().front().material->opacity = NAN;
  TASK_CHECK(throwsInvalidArgument([&] { rasterizer->advance(3, 1); }));
  TASK_CHECK(rasterizer->accumulatedFrames() == 3);
  scene->triangles().front().material->setOpacity(0.5f);

  // Reset clears the history and allows a brand new run from frame zero.
  rasterizer->resetStochastic();
  TASK_CHECK(rasterizer->accumulatedFrames() == 0);
  TASK_CHECK(!rasterizer->stochasticActive());
  rasterizer->configureStochastic(StochasticSettings{1, 0});
  rasterizer->advance(0, 1);
  TASK_CHECK(rasterizer->accumulatedFrames() == 1);

  // A rasterizer with no scene cannot accumulate.
  auto empty = makeRasterizer(true, 1);
  TASK_CHECK(throwsInvalidArgument([&] { empty->advance(0, 1); }));
  TASK_CHECK(empty->accumulatedFrames() == 0);
  return 0;
}

int onlineMeanAndResolve() {
  auto scene = makeScene();
  scene->addGraphicObj(makeQuad(-2, glm::vec3(1, 0.4f, 0.2f), 0.6f), "q");
  scene->setSurfaceId("q", 80);
  scene->prepare();

  auto bulk = makeRasterizer(true, 314);
  bulk->addScene(scene);
  bulk->advance(0, 4);

  // The same frames accumulated one at a time must give the same online mean.
  auto stepwise = makeRasterizer(true, 314);
  stepwise->addScene(scene);
  for (std::uint64_t frame = 0; frame < 4; ++frame) {
    stepwise->advance(frame, 1);
    TASK_CHECK(stepwise->accumulatedFrames() == frame + 1);
  }
  TASK_CHECK(bulk->accumulatedFrames() == stepwise->accumulatedFrames());
  for (std::size_t pixel = 0; pixel < Width * Height; ++pixel) {
    for (int channel = 0; channel < 3; ++channel) {
      TASK_CHECK(std::abs(bulk->pixels()[pixel][channel] -
                          stepwise->pixels()[pixel][channel]) < 1e-6f);
    }
  }

  // resolve is a read-only republish of the current mean.
  const glm::vec3 before = bulk->pixels()[Center];
  bulk->resolveStochastic();
  TASK_CHECK(bulk->accumulatedFrames() == 4);
  for (int channel = 0; channel < 3; ++channel) {
    TASK_CHECK(bulk->pixels()[Center][channel] == before[channel]);
  }
  // Advancing more frames keeps converging from the existing mean.
  bulk->advance(4, 4);
  TASK_CHECK(bulk->accumulatedFrames() == 8);
  return 0;
}

int drawClearAndLinesIsolation() {
  auto scene = makeScene(glm::vec3(0.2f));
  scene->addGraphicObj(makeQuad(-2, glm::vec3(1, 0, 0), 0.5f), "q");
  scene->setSurfaceId("q", 90);
  scene->prepare();

  auto stochastic = makeRasterizer(true, 66);
  stochastic->addScene(scene);
  stochastic->advance(0, 8);
  const auto stochasticMean = stochastic->pixels()[Center];
  TASK_CHECK(stochastic->accumulatedFrames() == 8);

  // Ordinary draw uses the opaque path and never touches stochastic history.
  stochastic->draw();
  TASK_CHECK(stochastic->accumulatedFrames() == 8);
  TraditionalRasterizer opaqueReference(Width, Height);
  opaqueReference.addScene(scene);
  opaqueReference.draw();
  for (std::size_t pixel = 0; pixel < Width * Height; ++pixel) {
    for (int channel = 0; channel < 3; ++channel) {
      TASK_CHECK(std::abs(stochastic->pixels()[pixel][channel] -
                          opaqueReference.pixels()[pixel][channel]) < 1e-6f);
    }
  }

  stochastic->clear();
  TASK_CHECK(stochastic->accumulatedFrames() == 8);
  TASK_CHECK(vecEqual(stochastic->pixels()[Center], glm::vec3(0)));
  TASK_CHECK(!std::isfinite(stochastic->depth()[Center]));
  // History survived clear(); resolve republishes the accumulated mean.
  stochastic->resolveStochastic();
  for (int channel = 0; channel < 3; ++channel) {
    TASK_CHECK(stochastic->pixels()[Center][channel] ==
               stochasticMean[channel]);
  }

  // LINES are rejected while a run is active.
  TASK_CHECK(throwsInvalidArgument(
      [&] { stochastic->draw(Primitive::LINES); }));
  TASK_CHECK(stochastic->accumulatedFrames() == 8);

  // Finite-coordinate input contract is enforced by the geometry types.
  TASK_CHECK(throwsInvalidArgument([&] {
    makeTriangle(glm::vec3(1), 1.f,
                 {Vertex({NAN, 0, -2}), Vertex({1, 0, -2}),
                  Vertex({0, 1, -2})});
  }));
  return 0;
}

int rendererIsolationAndReset() {
  auto scene = makeScene();
  scene->addGraphicObj(makeQuad(-2, glm::vec3(1, 0.5f, 0.25f), 0.5f), "q");
  scene->setSurfaceId("q", 100);
  scene->prepare();

  auto first = makeRasterizer(true, 111);
  auto second = makeRasterizer(true, 222);
  first->addScene(scene);
  second->addScene(scene);

  first->advance(0, 12);
  TASK_CHECK(first->accumulatedFrames() == 12);
  TASK_CHECK(second->accumulatedFrames() == 0); // Independent history.

  const auto firstMean = first->pixels()[Center];
  second->advance(0, 12);
  TASK_CHECK(second->accumulatedFrames() == 12);
  TASK_CHECK(first->accumulatedFrames() == 12);
  TASK_CHECK(vecEqual(first->pixels()[Center], firstMean));
  bool seedsDiffer = false;
  for (std::size_t pixel = 0; pixel < Width * Height; ++pixel) {
    if (!vecEqual(first->pixels()[pixel], second->pixels()[pixel])) {
      seedsDiffer = true;
      break;
    }
  }
  TASK_CHECK(seedsDiffer);

  // reset followed by reconfigure reproduces a fresh renderer's run.
  first->resetStochastic();
  TASK_CHECK(first->accumulatedFrames() == 0);
  first->configureStochastic(StochasticSettings{222, 0});
  first->advance(0, 12);
  for (std::size_t pixel = 0; pixel < Width * Height; ++pixel) {
    for (int channel = 0; channel < 3; ++channel) {
      TASK_CHECK(std::abs(first->pixels()[pixel][channel] -
                          second->pixels()[pixel][channel]) < 1e-6f);
    }
  }

  // A new generation selects an independent stream from the same seed.
  auto generationA = makeRasterizer(true, 333, 0);
  auto generationB = makeRasterizer(true, 333, 1);
  generationA->addScene(scene);
  generationB->addScene(scene);
  generationA->advance(0, 1);
  generationB->advance(0, 1);
  TASK_CHECK(!vecEqual(generationA->pixels()[Center],
                       generationB->pixels()[Center]));
  return 0;
}

} // namespace

extern "C" const struct task_case task_cases[] = {
    {"identity", "surface_identity_rules", surfaceIdentityRules},
    {"identity", "opacity_and_hash_contract", opacityAndHashContract},
    {"identity", "equal_depth_arbitration", equalDepthSurfaceArbitration},
    {"sampling", "zero_and_one_opacity", zeroAndOneOpacity},
    {"sampling", "foreground_opaque_occlusion", foregroundOpaqueOcclusion},
    {"sampling", "source_over_convergence", sourceOverConvergenceAndReplay},
    {"sampling", "scalar_simd_order_invariance", scalarSimdAndOrderInvariance},
    {"sampling", "perspective_textured_shading", perspectiveTexturedShading},
    {"accumulation", "frame_range_validation", frameRangeValidation},
    {"accumulation", "online_mean_and_resolve", onlineMeanAndResolve},
    {"accumulation", "draw_clear_lines_isolation", drawClearAndLinesIsolation},
    {"accumulation", "renderer_isolation_reset", rendererIsolationAndReset},
};

extern "C" const std::size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
