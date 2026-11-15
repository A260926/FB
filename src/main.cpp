#include <chrono>
#include <iostream>
#include <object/Sphere.hpp>
#include <render/PathTracing.hpp>
#include <render/Rasterizer.hpp>
using namespace SoftRasterizer;

namespace {
std::uint64_t integer(const std::string &value) {
  if (value.empty() ||
      value.find_first_not_of("0123456789") != std::string::npos) {
    throw std::invalid_argument("expected an unsigned integer: " + value);
  }
  return std::stoull(value);
}

std::shared_ptr<Material> createDiffuseMaterial(glm::vec3 color) {
  auto material = std::make_shared<Material>();
  material->Kd = color;
  return material;
}

std::shared_ptr<Mesh> createQuadMesh(glm::vec3 vertexA, glm::vec3 vertexB,
                                     glm::vec3 vertexC, glm::vec3 vertexD,
                                     glm::vec3 normal,
                                     std::shared_ptr<Material> material) {
  std::vector<Vertex> vertices{{vertexA, normal, {0, 0}},
                               {vertexB, normal, {1, 0}},
                               {vertexC, normal, {1, 1}},
                               {vertexD, normal, {0, 1}}};
  std::vector<glm::uvec3> faces{{0, 1, 2}, {0, 2, 3}};
  if (glm::dot(glm::cross(vertexB - vertexA, vertexC - vertexA), normal) < 0) {
    for (auto &face : faces) {
      std::swap(face.y, face.z);
    }
  }
  auto mesh = std::make_shared<Mesh>(vertices, faces);
  mesh->setMaterial(material);
  return mesh;
}

/*Set up the room, area emitter, mirror and glass used by both pipelines.*/
std::shared_ptr<Scene> createDemoScene() {
  auto scene =
      std::make_shared<Scene>("Room", glm::vec3(0, 1, 6), glm::vec3(0, 1, -1));
  auto white = createDiffuseMaterial(glm::vec3(0.73f));
  scene->addGraphicObj(createQuadMesh({-2, -1, 1}, {2, -1, 1}, {2, -1, -3},
                                      {-2, -1, -3}, {0, 1, 0}, white),
                       "floor");
  scene->addGraphicObj(createQuadMesh({-2, 3, 1}, {2, 3, 1}, {2, 3, -3},
                                      {-2, 3, -3}, {0, -1, 0}, white),
                       "ceiling");
  scene->addGraphicObj(createQuadMesh({-2, -1, -3}, {2, -1, -3}, {2, 3, -3},
                                      {-2, 3, -3}, {0, 0, 1}, white),
                       "back");
  scene->addGraphicObj(
      createQuadMesh({-2, -1, 1}, {-2, -1, -3}, {-2, 3, -3}, {-2, 3, 1},
                     {1, 0, 0}, createDiffuseMaterial({0.65, 0.08, 0.06})),
      "left");
  scene->addGraphicObj(createQuadMesh({2, -1, 1}, {2, -1, -3}, {2, 3, -3},
                                      {2, 3, 1}, {-1, 0, 0},
                                      createDiffuseMaterial({0.07, 0.5, 0.12})),
                       "right");
  auto lightMaterial = createDiffuseMaterial(glm::vec3(0));
  lightMaterial->emission = glm::vec3(9);
  scene->addGraphicObj(createQuadMesh({-0.65, 2.98, -0.2}, {0.65, 2.98, -0.2},
                                      {0.65, 2.98, -1.5}, {-0.65, 2.98, -1.5},
                                      {0, -1, 0}, lightMaterial),
                       "light");
  auto mirrorSphere =
      std::make_shared<Sphere>(glm::vec3(-0.9, -0.2, -1.6), 0.8f);
  auto mirrorMaterial = std::make_shared<Material>(MaterialType::REFLECTION);
  mirrorMaterial->Kd = glm::vec3(0.95f);
  mirrorSphere->setMaterial(mirrorMaterial);
  scene->addGraphicObj(mirrorSphere, "mirror");
  auto glassSphere =
      std::make_shared<Sphere>(glm::vec3(0.9, -0.25, -0.8), 0.75f);
  auto glassMaterial =
      std::make_shared<Material>(MaterialType::REFLECTION_AND_REFRACTION);
  glassMaterial->Kd = glm::vec3(1);
  glassSphere->setMaterial(glassMaterial);
  scene->addGraphicObj(glassSphere, "glass");
  return scene;
}
} // namespace

int main(int argc, char **argv) {
  try {
    std::string mode = "raster", output = "render.ppm", obj;
    std::size_t width = 512, height = 512, spp = 64;
    std::uint64_t seed = 1;
    bool scalar = false, wire = false, gui = false;
    for (int i = 1; i < argc; ++i) {
      std::string arg = argv[i];
      auto value = [&]() {
        if (i + 1 >= argc) {
          throw std::invalid_argument("missing value for " + arg);
        }
        return std::string(argv[++i]);
      };
      if (arg == "--mode") {
        mode = value();
      } else if (arg == "--output") {
        output = value();
      } else if (arg == "--obj") {
        obj = value();
      } else if (arg == "--width") {
        width = integer(value());
      } else if (arg == "--height") {
        height = integer(value());
      } else if (arg == "--spp") {
        spp = integer(value());
      } else if (arg == "--seed") {
        seed = integer(value());
      } else if (arg == "--scalar") {
        scalar = true;
      } else if (arg == "--wireframe") {
        wire = true;
      } else if (arg == "--gui") {
        gui = true;
      } else if (arg == "--help") {
        std::cout << "Software-Rasterizer [--mode raster|pt] [--width N "
                     "--height N] [--spp N --seed N]\n  [--obj file.obj] "
                     "[--output file.ppm] [--scalar] [--wireframe] [--gui]\n";
        return 0;
      } else {
        throw std::invalid_argument("unknown argument: " + arg);
      }
    }
    /*Load a model or use the built-in room.*/
    auto scene = obj.empty() ? createDemoScene() : std::make_shared<Scene>();
    if (!obj.empty()) {
      auto mesh = ObjLoader(obj).load();
      scene->addGraphicObj(mesh, "model");
      auto box = mesh->getBounds();
      if (box.empty()) {
        throw std::invalid_argument("OBJ has no geometry");
      }
      auto center = box.centroid();
      float size = std::max(0.01f, maxComponent(box.diagonal()));
      scene->setViewMatrix(center + glm::vec3(0, 0, 2 * size), center,
                           {0, 1, 0});
      scene->setProjectionMatrix(45, size * 0.001f, size * 20);
      scene->addLight("key", std::make_shared<light_struct>(
                                 center + glm::vec3(size, size, size),
                                 glm::vec3(8 * size * size)));
    } else if (mode == "raster") {
      scene->addLight("key", std::make_shared<light_struct>(
                                 glm::vec3(0, 2.5, -0.5), glm::vec3(30)));
    }
    /*Choose the rendering pipeline, then render and save one frame.*/
    std::unique_ptr<RenderingPipeline> renderer;
    if (mode == "raster") {
      auto renderPipeline =
          std::make_unique<TraditionalRasterizer>(width, height);
      renderPipeline->setSIMD(!scalar);
      renderer = std::move(renderPipeline);
    } else if (mode == "pt") {
      auto renderPipeline = std::make_unique<PathTracing>(width, height);
      PathTracingSettings settings;
      settings.samplesPerPixel = spp;
      settings.seed = seed;
      renderPipeline->configure(settings);
      renderer = std::move(renderPipeline);
    } else {
      throw std::invalid_argument("mode must be raster or pt");
    }
    renderer->addScene(scene);
    auto start = std::chrono::steady_clock::now();
    auto primitive = wire ? Primitive::LINES : Primitive::TRIANGLES;
    if (gui) {
      renderer->display(primitive);
    } else {
      renderer->draw(primitive);
    }
    renderer->save(output);
    auto seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
            .count();
    std::cout << mode << " " << width << "x" << height << " saved to " << output
              << " in " << seconds << " s\n";
  } catch (const std::exception &error) {
    std::cerr << "Error: " << error.what() << '\n';
    return 1;
  }
}
