#include "draco/attributes/attribute_quantization_transform.h"
#include "draco/compression/decode.h"
#include "draco/compression/encode.h"
#include "draco/mesh/triangle_soup_mesh_builder.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using V3 = std::array<float, 3>;
using V2 = std::array<float, 2>;
static void require(bool ok, const char *what) {
  if (!ok) throw std::runtime_error(what);
}
static std::unique_ptr<draco::Mesh> surface(bool closed) {
  const int n = closed ? 24 : 17, m = closed ? 16 : 11;
  const int parts = closed ? 1 : 2;
  const int width = closed ? n : n + 1, height = closed ? m : m + 1;
  std::vector<V3> xyz(parts * width * height);
  auto index = [=](int part, int i, int j) {
    return part * width * height + (i % width) * height + j % height;
  };
  for (int part = 0; part < parts; ++part)
    for (int i = 0; i < width; ++i)
      for (int j = 0; j < height; ++j) {
        const double u = 6.283185307179586 * i / n;
        const double v = 6.283185307179586 * j / m;
        V3 p;
        if (closed) p = {float((3 + std::cos(v)) * std::cos(u)),
                         float((3 + std::cos(v)) * std::sin(u)), float(std::sin(v))};
        else p = {float(i + 30 * part), float(j), float(.2 * std::sin(u) * std::cos(v))};
        xyz[index(part, i, j)] = p;
      }
  draco::TriangleSoupMeshBuilder b;
  b.Start(parts * n * m * 2);
  const int pa = b.AddAttribute(draco::GeometryAttribute::POSITION, 3, draco::DT_FLOAT32);
  const int uv = b.AddAttribute(draco::GeometryAttribute::TEX_COORD, 2, draco::DT_FLOAT32);
  int face = 0;
  for (int part = 0; part < parts; ++part)
    for (int i = 0; i < n; ++i)
      for (int j = 0; j < m; ++j) {
        const int ii[4] = {i, i + 1, i + 1, i};
        const int jj[4] = {j, j, j + 1, j + 1};
        const int tri[2][3] = {{0, 1, 2}, {0, 2, 3}};
        for (auto &t : tri) {
          V2 tc[3]; const float *p[3];
          for (int k = 0; k < 3; ++k) {
            p[k] = xyz[index(part, ii[t[k]], jj[t[k]])].data();
            // Wrap position identity at the torus seams, retain distinct UVs.
            tc[k] = {float(ii[t[k]]) / n + part + (!closed && i >= n / 2 ? .125f : 0.f), float(jj[t[k]]) / m};
          }
          b.SetAttributeValuesForFace(pa, draco::FaceIndex(face), p[0], p[1], p[2]);
          b.SetAttributeValuesForFace(uv, draco::FaceIndex(face), tc[0].data(), tc[1].data(), tc[2].data());
          ++face;
        }
      }
  auto mesh = b.Finalize();
  require(mesh && mesh->num_faces() == unsigned(parts * n * m * 2), "surface");
  return mesh;
}
static uint64_t fingerprint(const draco::Mesh &m) {
  uint64_t h = 1469598103934665603ull;
  auto bytes = [&](const void *p, size_t n) {
    const auto *b = static_cast<const unsigned char *>(p);
    while (n--) { h ^= *b++; h *= 1099511628211ull; }
  };
  auto integer = [&](uint64_t x) { for (int i = 0; i < 8; ++i) {
    unsigned char b = static_cast<unsigned char>(x >> (8 * i)); bytes(&b, 1);
  }};
  integer(m.num_faces()); integer(m.num_points()); integer(m.num_attributes());
  for (draco::FaceIndex f(0); f < m.num_faces(); ++f)
    for (auto p : m.face(f)) integer(p.value());
  for (int ai = 0; ai < m.num_attributes(); ++ai) {
    const auto *a = m.attribute(ai);
    integer(a->unique_id()); integer(a->attribute_type()); integer(a->data_type());
    integer(a->num_components()); integer(a->normalized()); integer(a->size());
    for (draco::AttributeValueIndex v(0); v < a->size(); ++v)
      bytes(a->GetAddress(v), draco::DataTypeLength(a->data_type()) * a->num_components());
    for (draco::PointIndex p(0); p < m.num_points(); ++p) integer(a->mapped_index(p).value());
  }
  return h;
}
static std::unique_ptr<draco::Mesh> decode(const draco::EncoderBuffer &b, bool skip) {
  draco::Decoder d;
  if (skip) { d.SetSkipAttributeTransform(draco::GeometryAttribute::POSITION);
              d.SetSkipAttributeTransform(draco::GeometryAttribute::TEX_COORD); }
  draco::DecoderBuffer in; in.Init(b.data(), b.size());
  auto result = d.DecodeMeshFromBuffer(&in); require(result.ok(), "decode");
  return std::move(result).value();
}
int main() try {
  for (bool closed : {false, true}) {
    auto source = surface(closed);
    for (int profile = 0; profile < 3; ++profile) {
      draco::Encoder e; e.SetEncodingMethod(draco::MESH_EDGEBREAKER_ENCODING);
      e.SetSpeedOptions(5, 5); // Select STANDARD traversal, including larger mesh.
      const int bits = profile ? 16 : 0;
      e.SetAttributeQuantization(draco::GeometryAttribute::POSITION, bits);
      e.SetAttributeQuantization(draco::GeometryAttribute::TEX_COORD, bits ? 14 : 0);
      if (profile) {
        require(e.SetAttributePredictionScheme(draco::GeometryAttribute::POSITION,
          profile == 1 ? draco::PREDICTION_DIFFERENCE : draco::MESH_PREDICTION_PARALLELOGRAM).ok(), "position predictor");
        require(e.SetAttributePredictionScheme(draco::GeometryAttribute::TEX_COORD,
          profile == 1 ? draco::PREDICTION_DIFFERENCE : draco::MESH_PREDICTION_TEX_COORDS_PORTABLE).ok(), "UV predictor");
      }
      draco::EncoderBuffer out;
      require(e.EncodeMeshToBuffer(*source, &out).ok() && out.size() > 12, "encode");
      require(static_cast<unsigned char>(out.data()[5]) == 2 &&
              static_cast<unsigned char>(out.data()[6]) == 2 &&
              static_cast<unsigned char>(out.data()[11]) == draco::MESH_EDGEBREAKER_STANDARD_ENCODING, "2.2 standard");
      auto normal = decode(out, false); auto again = decode(out, false);
      require(normal->num_faces() == source->num_faces() && normal->num_attributes() == 2, "geometry");
      const auto *pos = normal->GetNamedAttribute(draco::GeometryAttribute::POSITION);
      const auto *uv = normal->GetNamedAttribute(draco::GeometryAttribute::TEX_COORD);
      require(pos && uv && pos->size() < normal->num_points(), "seam mapping");
      const auto hash = fingerprint(*normal);
      require(hash == fingerprint(*again), "determinism");
      auto skipped = decode(out, true); const auto portable_hash = fingerprint(*skipped);
      if (bits) {
        for (int ai = 0; ai < skipped->num_attributes(); ++ai) {
          auto *a = skipped->attribute(ai); draco::AttributeQuantizationTransform transform;
          require(transform.InitFromAttribute(*a), "transform metadata");
          const auto *wanted = normal->attribute(ai);
          draco::PointAttribute restored;
          restored.Init(wanted->attribute_type(), wanted->num_components(), wanted->data_type(), wanted->normalized(), a->size());
          require(transform.InverseTransformAttribute(*a, &restored), "inverse transform");
          for (draco::AttributeValueIndex v(0); v < a->size(); ++v)
            require(std::memcmp(restored.GetAddress(v), wanted->GetAddress(v),
              wanted->num_components() * sizeof(float)) == 0, "skip inverse bits");
        }
      } else require(portable_hash == hash, "unquantized skip");
      std::cout << "SURFACE closed=" << closed << " profile=" << profile
                << " faces=" << normal->num_faces() << " points=" << normal->num_points()
                << " position_values=" << pos->size() << " uv_values=" << uv->size()
                << " bytes=" << out.size() << " mesh_hash=" << std::hex << hash
                << " portable_hash=" << portable_hash << std::dec << "\n";
    }
  }
  std::cout << "DRACO_SURFACE_BASELINE_COMPLETE\n";
} catch (const std::exception &e) { std::cerr << e.what() << "\n"; return 1; }
