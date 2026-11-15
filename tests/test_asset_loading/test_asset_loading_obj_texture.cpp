#include <TestHelpers.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>

using namespace SoftRasterizer;
using namespace SoftRasterizer::TestHelpers;

namespace {

TEST(AssetLoading, MultiShapeMaterialsNormalsAndPNG) {
  TextureLoader texture(2, 2, {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 1, 1}});
  expectNear(texture.getTextureColor({0, 1}), {1, 0, 0});
  expectNear(texture.getTextureColor({0, 0}), {0, 0, 1});
  expectNear(texture.getTextureColor({.5, .5}), glm::vec3(.5f));
  expectNear(srgbToLinear(.5f), .214041f, .00001f);
  expectNear(linearToSrgb(srgbToLinear(.5f)), .5f);
  auto tmp = std::filesystem::temp_directory_path() /
             ("rasterizer-regression-" +
              std::to_string(
                  std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directory(tmp);

  struct Cleanup {
    std::filesystem::path path;

    ~Cleanup() {
      std::error_code ec;
      std::filesystem::remove_all(path, ec);
    }
  } cleanup{tmp};

  {
    std::ofstream m(tmp / "two.mtl");
    m << "newmtl red\nKd 1 0 0\nnewmtl green\nKd 0 1 0\nKe 2 3 4\n";
  }
  {
    std::ofstream o(tmp / "two.obj");
    o << "mtllib two.mtl\nv -2 0 -2\nv -1 0 -2\nv -2 1 -2\nv 1 0 -2\nv 2 0 "
         "-2\nv 1 1 -2\nvn 0 0 1\nvt 0 .25\no first\nusemtl red\nf 1/1/1 2/1/1 "
         "3/1/1\no second\nusemtl green\nf 4 5 6\n";
  }
  auto mesh = ObjLoader((tmp / "two.obj").string()).load();
  ASSERT_TRUE(mesh->triangleCount() == 2) << "multi-shape face count wrong";
  auto a = mesh->getIntersect(Ray({-1.8f, .2f, 0}, {0, 0, -1}));
  auto b = mesh->getIntersect(Ray({1.2f, .2f, 0}, {0, 0, -1}));
  ASSERT_TRUE(a.intersected && b.intersected) << "multi-shape geometry lost";
  expectNear(a.material->Kd, {1, 0, 0});
  expectNear(b.material->emission, {2, 3, 4});
  expectNear(a.uv.y, .25f);
  expectNear(b.normal, {0, 0, 1});
  auto cornell = ObjLoader(std::string(TEST_ASSET_DIR) +
                           "/cornellbox/cornellbox_full_model/cornell_box.obj")
                     .load();
  ASSERT_TRUE(cornell->triangleCount() > 20) << "Cornell shapes missing";
  auto crate =
      ObjLoader(std::string(TEST_ASSET_DIR) + "/Crate/Crate1.obj").load();
  ASSERT_TRUE(crate->triangleCount() == 12) << "crate load failed";
  std::vector<RasterTriangle> triangles;
  crate->appendRasterTriangles(triangles);
  ASSERT_TRUE(triangles[0].material->texture != nullptr)
      << "MTL texture missing";
  auto cube = ObjLoader(std::string(TEST_ASSET_DIR) + "/cube/cube.obj").load();
  ASSERT_TRUE(cube->triangleCount() == 12) << "textured cube load failed";
  TextureLoader png(std::string(TEST_ASSET_DIR) + "/spot/spot_texture.png");
  ASSERT_TRUE(finite(png.getTextureColor({.3, .6}))) << "PNG decode failed";
  EXPECT_THROW(([&] { ObjLoader((tmp / "missing.obj").string()).load(); })(),
               std::exception);
}

} // namespace
