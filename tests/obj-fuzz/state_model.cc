#include "state_model.h"

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>

namespace objstate {

// ---------------------------------------------------------------------------
// Trivial constructors
// ---------------------------------------------------------------------------

Decl::Decl() : kind(kDeclVertex), id(0) {
  coordinates[0] = coordinates[1] = coordinates[2] = 0;
}

Corner::Corner()
    : vertex_id(0),
      texcoord_id(0),
      normal_id(0),
      has_texcoord(false),
      has_normal(false),
      negative_vertex(false),
      negative_texcoord(false),
      negative_normal(false) {}

Material::Material() : r(0), g(0), b(0) {}

Face::Face() : smoothing(0) {}

Event::Event() : kind(kEventDeclaration), smoothing(0) {}

Asset::Asset() : seed(0) {}

CornerSpec::CornerSpec()
    : vertex_id(0),
      texcoord_id(0),
      normal_id(0),
      has_texcoord(false),
      has_normal(false),
      negative_vertex(false),
      negative_texcoord(false),
      negative_normal(false) {}

FaceRecord::FaceRecord() : smoothing(0) {}

CornerRecord::CornerRecord()
    : has_texcoord(false),
      has_normal(false),
      vertex_index(-2),
      texcoord_index(-2),
      normal_index(-2) {
  vertex[0] = vertex[1] = vertex[2] = 0.0;
  texcoord[0] = texcoord[1] = 0.0;
  normal[0] = normal[1] = normal[2] = 0.0;
}

Mismatch::Mismatch() : corner(-1) {}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::string format_double(double value) {
  char buffer[64];
  if (value == 0.0) {
    return "0";
  }
  if (std::floor(value) == value && std::fabs(value) < 1e15) {
    std::snprintf(buffer, sizeof(buffer), "%.0f", value);
    return std::string(buffer);
  }
  std::snprintf(buffer, sizeof(buffer), "%.17g", value);
  return std::string(buffer);
}

namespace {

std::string DeclEventId(DeclKind kind, int id) {
  std::ostringstream os;
  switch (kind) {
    case kDeclVertex:
      os << "V" << id;
      break;
    case kDeclTexcoord:
      os << "T" << id;
      break;
    default:
      os << "N" << id;
      break;
  }
  return os.str();
}

bool NameIsLegal(const std::string &name, bool allow_empty) {
  if (name.empty()) {
    return allow_empty;
  }
  for (size_t i = 0; i < name.size(); i++) {
    const unsigned char c = static_cast<unsigned char>(name[i]);
    if (c < 0x20 || c > 0x7e || std::isspace(c)) {
      return false;
    }
  }
  return true;
}

struct DerivedState {
  std::string shape_name;
  std::string material_name;
  long smoothing;

  DerivedState() : smoothing(0) {}
};

double CrossNonZero(const Decl &a, const Decl &b, const Decl &c) {
  const double e0x = static_cast<double>(b.coordinates[0] - a.coordinates[0]);
  const double e0y = static_cast<double>(b.coordinates[1] - a.coordinates[1]);
  const double e0z = static_cast<double>(b.coordinates[2] - a.coordinates[2]);
  const double e1x = static_cast<double>(c.coordinates[0] - b.coordinates[0]);
  const double e1y = static_cast<double>(c.coordinates[1] - b.coordinates[1]);
  const double e1z = static_cast<double>(c.coordinates[2] - b.coordinates[2]);
  const double cx = e0y * e1z - e0z * e1y;
  const double cy = e0z * e1x - e0x * e1z;
  const double cz = e0x * e1y - e0y * e1x;
  return cx * cx + cy * cy + cz * cz;
}

}  // namespace

// ---------------------------------------------------------------------------
// AssetBuilder
// ---------------------------------------------------------------------------

AssetBuilder::AssetBuilder(uint64_t seed) : next_face_id_(0), next_event_ordinal_(0),
                                           current_smoothing_(0) {
  asset_.seed = seed;
  next_decl_id_[kDeclVertex] = 1;
  next_decl_id_[kDeclTexcoord] = 1;
  next_decl_id_[kDeclNormal] = 1;
}

Event &AssetBuilder::append_event(EventKind kind, const std::string &id) {
  Event event;
  event.kind = kind;
  event.id = id;
  asset_.events.push_back(event);
  return asset_.events.back();
}

void AssetBuilder::add_material(const std::string &name, long r, long g, long b) {
  Material material;
  material.name = name;
  material.r = r;
  material.g = g;
  material.b = b;
  asset_.materials.push_back(material);
}

Decl AssetBuilder::add_vertex(long x, long y, long z) {
  Decl decl;
  decl.kind = kDeclVertex;
  decl.id = next_decl_id_[kDeclVertex]++;
  decl.coordinates[0] = x;
  decl.coordinates[1] = y;
  decl.coordinates[2] = z;
  vertices_.push_back(decl);
  Event event;
  event.kind = kEventDeclaration;
  event.id = DeclEventId(decl.kind, decl.id);
  event.decl = decl;
  asset_.events.push_back(event);
  return decl;
}

Decl AssetBuilder::add_texcoord(long u, long v) {
  Decl decl;
  decl.kind = kDeclTexcoord;
  decl.id = next_decl_id_[kDeclTexcoord]++;
  decl.coordinates[0] = u;
  decl.coordinates[1] = v;
  decl.coordinates[2] = 0;
  texcoords_.push_back(decl);
  Event event;
  event.kind = kEventDeclaration;
  event.id = DeclEventId(decl.kind, decl.id);
  event.decl = decl;
  asset_.events.push_back(event);
  return decl;
}

Decl AssetBuilder::add_normal(long x, long y, long z) {
  Decl decl;
  decl.kind = kDeclNormal;
  decl.id = next_decl_id_[kDeclNormal]++;
  decl.coordinates[0] = x;
  decl.coordinates[1] = y;
  decl.coordinates[2] = z;
  normals_.push_back(decl);
  Event event;
  event.kind = kEventDeclaration;
  event.id = DeclEventId(decl.kind, decl.id);
  event.decl = decl;
  asset_.events.push_back(event);
  return decl;
}

void AssetBuilder::add_object(const std::string &name) {
  Event event;
  event.kind = kEventObject;
  event.name = name;
  event.id = "S" + std::to_string(next_event_ordinal_++);
  asset_.events.push_back(event);
  current_shape_ = name;
}

void AssetBuilder::add_group(const std::string &name) {
  Event event;
  event.kind = kEventGroup;
  event.name = name;
  event.id = "S" + std::to_string(next_event_ordinal_++);
  asset_.events.push_back(event);
  current_shape_ = name;
}

void AssetBuilder::add_material_state(const std::string &name) {
  Event event;
  event.kind = kEventMaterial;
  event.name = name;
  event.id = "S" + std::to_string(next_event_ordinal_++);
  asset_.events.push_back(event);
  current_material_ = name;
}

void AssetBuilder::add_smoothing(long value) {
  Event event;
  event.kind = kEventSmoothing;
  event.smoothing = value;
  event.id = "S" + std::to_string(next_event_ordinal_++);
  asset_.events.push_back(event);
  current_smoothing_ = value;
}

const Decl *AssetBuilder::find_decl(DeclKind kind, int id) const {
  const std::vector<Decl> *list = NULL;
  switch (kind) {
    case kDeclVertex:
      list = &vertices_;
      break;
    case kDeclTexcoord:
      list = &texcoords_;
      break;
    default:
      list = &normals_;
      break;
  }
  for (size_t i = 0; i < list->size(); i++) {
    if ((*list)[i].id == id) {
      return &(*list)[i];
    }
  }
  return NULL;
}

bool AssetBuilder::add_face(const CornerSpec corners[3], std::string *err) {
  if (!corners) {
    if (err) *err = "missing corners";
    return false;
  }
  Face face;
  face.id = "F" + std::to_string(next_face_id_);
  face.shape_name = current_shape_;
  face.material_name = current_material_;
  face.smoothing = current_smoothing_;
  for (int k = 0; k < 3; k++) {
    if (corners[k].vertex_id <= 0) {
      if (err) *err = "corner without a vertex declaration";
      return false;
    }
    if (!find_decl(kDeclVertex, corners[k].vertex_id)) {
      if (err) *err = "unknown vertex declaration " + std::to_string(corners[k].vertex_id);
      return false;
    }
    if (corners[k].has_texcoord) {
      if (corners[k].texcoord_id <= 0 ||
          !find_decl(kDeclTexcoord, corners[k].texcoord_id)) {
        if (err) *err = "unknown texcoord declaration";
        return false;
      }
    } else if (corners[k].texcoord_id != 0 || corners[k].negative_texcoord) {
      if (err) *err = "inconsistent absent texcoord";
      return false;
    }
    if (corners[k].has_normal) {
      if (corners[k].normal_id <= 0 ||
          !find_decl(kDeclNormal, corners[k].normal_id)) {
        if (err) *err = "unknown normal declaration";
        return false;
      }
    } else if (corners[k].normal_id != 0 || corners[k].negative_normal) {
      if (err) *err = "inconsistent absent normal";
      return false;
    }
    face.corners[k].vertex_id = corners[k].vertex_id;
    face.corners[k].has_texcoord = corners[k].has_texcoord;
    face.corners[k].texcoord_id = corners[k].texcoord_id;
    face.corners[k].has_normal = corners[k].has_normal;
    face.corners[k].normal_id = corners[k].normal_id;
    face.corners[k].negative_vertex = corners[k].negative_vertex;
    face.corners[k].negative_texcoord = corners[k].negative_texcoord;
    face.corners[k].negative_normal = corners[k].negative_normal;
  }
  Event event;
  event.kind = kEventFace;
  event.id = face.id;
  event.face = face;
  asset_.events.push_back(event);
  next_face_id_++;
  return true;
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

namespace {

struct ValidationContext {
  std::vector<Decl> pool[3];
  std::set<std::string> material_names;
  DerivedState state;
  int faces_seen;
  bool saw_switch;
  int faces_since_switch;
  std::string previous_shape;
  std::set<std::string> shape_names;
  int max_decl_coverage_forms;  // bitmask of token forms seen
  bool found_early_unreferenced_by_anchor;
  bool found_decl_after_anchor;
  bool found_decl_between_faces;
  int anchor_index;
};

bool MaterialExists(const Asset &asset, const std::string &name) {
  for (size_t i = 0; i < asset.materials.size(); i++) {
    if (asset.materials[i].name == name) {
      return true;
    }
  }
  return false;
}

const Decl *FindPoolDecl(const std::vector<Decl> &pool, int id) {
  for (size_t i = 0; i < pool.size(); i++) {
    if (pool[i].id == id) {
      return &pool[i];
    }
  }
  return NULL;
}

bool CornerReferencesAvailable(const Corner &corner,
                               const std::vector<Decl> &vertex_pool,
                               const std::vector<Decl> &texcoord_pool,
                               const std::vector<Decl> &normal_pool) {
  if (corner.vertex_id <= 0 || !FindPoolDecl(vertex_pool, corner.vertex_id)) {
    return false;
  }
  if (corner.has_texcoord) {
    if (corner.texcoord_id <= 0 ||
        !FindPoolDecl(texcoord_pool, corner.texcoord_id)) {
      return false;
    }
  } else if (corner.texcoord_id != 0 || corner.negative_texcoord) {
    return false;
  }
  if (corner.has_normal) {
    if (corner.normal_id <= 0 || !FindPoolDecl(normal_pool, corner.normal_id)) {
      return false;
    }
  } else if (corner.normal_id != 0 || corner.negative_normal) {
    return false;
  }
  return true;
}

int TokenFormBit(const Corner &corner) {
  int bit = 0;
  if (corner.has_texcoord) bit |= 1;
  if (corner.has_normal) bit |= 2;
  return bit;
}

}  // namespace

bool validate_generation_profile(const Asset &asset, std::string *err) {
  std::string local;
  if (!validate_asset(asset, true, &local)) {
    if (err) *err = local;
    return false;
  }

  // Anchor face F0 must exist and its three corners must carry all-negative
  // vertex/texcoord/normal references.
  const Face *anchor = NULL;
  int anchor_event_index = -1;
  for (size_t i = 0; i < asset.events.size(); i++) {
    if (asset.events[i].kind == kEventFace && asset.events[i].face.id == "F0") {
      anchor = &asset.events[i].face;
      anchor_event_index = static_cast<int>(i);
      break;
    }
  }
  if (!anchor) {
    if (err) *err = "missing anchor face F0";
    return false;
  }
  for (int k = 0; k < 3; k++) {
    const Corner &corner = anchor->corners[k];
    if (!corner.has_texcoord || !corner.has_normal ||
        !corner.negative_vertex || !corner.negative_texcoord ||
        !corner.negative_normal) {
      if (err) *err = "anchor face F0 lacks all-negative v/vt/vn references";
      return false;
    }
  }

  // Early declaration (before F0) that F0 does not reference.
  bool early_unreferenced = false;
  for (int i = 0; i < anchor_event_index && !early_unreferenced; i++) {
    const Event &event = asset.events[static_cast<size_t>(i)];
    if (event.kind != kEventDeclaration) {
      continue;
    }
    bool referenced = false;
    for (int k = 0; k < 3 && !referenced; k++) {
      const Corner &corner = anchor->corners[k];
      if (event.decl.kind == kDeclVertex && corner.vertex_id == event.decl.id) {
        referenced = true;
      }
      if (event.decl.kind == kDeclTexcoord && corner.has_texcoord &&
          corner.texcoord_id == event.decl.id) {
        referenced = true;
      }
      if (event.decl.kind == kDeclNormal && corner.has_normal &&
          corner.normal_id == event.decl.id) {
        referenced = true;
      }
    }
    if (!referenced) {
      early_unreferenced = true;
    }
  }
  if (!early_unreferenced) {
    if (err) *err = "no early declaration unreferenced by F0";
    return false;
  }

  // Declaration after F0.
  bool decl_after = false;
  bool decl_between_faces = false;
  int faces_before = 0;
  for (size_t i = 0; i < asset.events.size(); i++) {
    const Event &event = asset.events[i];
    if (event.kind == kEventFace) {
      faces_before++;
      continue;
    }
    if (event.kind == kEventDeclaration) {
      if (static_cast<int>(i) > anchor_event_index) {
        decl_after = true;
      }
      if (faces_before >= 2) {
        decl_between_faces = true;
      }
    }
  }
  if (!decl_after) {
    if (err) *err = "no declaration after F0";
    return false;
  }
  if (!decl_between_faces) {
    if (err) *err = "declarations are not interleaved with faces";
    return false;
  }

  // Both positive and negative styles for every component kind.
  bool style[3][2] = {{false, false}, {false, false}, {false, false}};
  int forms = 0;
  for (size_t i = 0; i < asset.events.size(); i++) {
    if (asset.events[i].kind != kEventFace) {
      continue;
    }
    const Face &face = asset.events[i].face;
    for (int k = 0; k < 3; k++) {
      const Corner &corner = face.corners[k];
      style[kDeclVertex][corner.negative_vertex ? 1 : 0] = true;
      if (corner.has_texcoord) {
        style[kDeclTexcoord][corner.negative_texcoord ? 1 : 0] = true;
      }
      if (corner.has_normal) {
        style[kDeclNormal][corner.negative_normal ? 1 : 0] = true;
      }
      forms |= 1 << TokenFormBit(corner);
    }
  }
  for (int kind = 0; kind < 3; kind++) {
    if (!style[kind][0] || !style[kind][1]) {
      if (err) *err = "missing positive or negative index style";
      return false;
    }
  }
  // All four token forms: v, v/vt, v//vn, v/vt/vn.
  if (forms != 0x0f) {
    if (err) *err = "not all four face token forms are covered";
    return false;
  }

  // The three declaration pools must vary independently (distinct sizes).
  int sizes[3];
  sizes[kDeclVertex] = 0;
  sizes[kDeclTexcoord] = 0;
  sizes[kDeclNormal] = 0;
  for (size_t i = 0; i < asset.events.size(); i++) {
    if (asset.events[i].kind == kEventDeclaration) {
      sizes[asset.events[i].decl.kind]++;
    }
  }
  if (sizes[0] == sizes[1] || sizes[0] == sizes[2] || sizes[1] == sizes[2]) {
    if (err) *err = "declaration pool sizes are not independent";
    return false;
  }

  // Each o/g switch introduces a segment that contains faces before the next
  // switch; validate_asset already enforces non-empty segments.
  return true;
}

bool validate_asset(const Asset &asset, bool generation_profile,
                    std::string *err) {
  std::set<std::string> seen_decl_ids[3];
  std::set<std::string> seen_face_ids;
  std::set<std::string> seen_event_ids;
  std::set<std::string> shape_names;
  std::set<std::string> material_names;
  for (size_t i = 0; i < asset.materials.size(); i++) {
    if (!NameIsLegal(asset.materials[i].name, false)) {
      if (err) *err = "illegal material name";
      return false;
    }
    if (material_names.count(asset.materials[i].name)) {
      if (err) *err = "duplicate material name";
      return false;
    }
    material_names.insert(asset.materials[i].name);
  }

  std::vector<Decl> pool[3];
  DerivedState state;
  bool saw_switch = false;
  int faces_since_switch = 0;
  int face_count = 0;
  int decl_counts[3] = {0, 0, 0};
  std::string previous_decl_id[3];

  for (size_t i = 0; i < asset.events.size(); i++) {
    const Event &event = asset.events[i];
    if (event.id.empty()) {
      if (err) *err = "event without a stable id";
      return false;
    }
    if (seen_event_ids.count(event.id)) {
      if (err) *err = "duplicate event id " + event.id;
      return false;
    }
    seen_event_ids.insert(event.id);

    switch (event.kind) {
      case kEventDeclaration: {
        const Decl &decl = event.decl;
        if (decl.kind < kDeclVertex || decl.kind > kDeclNormal) {
          if (err) *err = "bad declaration kind";
          return false;
        }
        if (decl.id <= 0) {
          if (err) *err = "bad declaration id";
          return false;
        }
        if (event.id != DeclEventId(decl.kind, decl.id)) {
          if (err) *err = "declaration id does not match event id";
          return false;
        }
        if (seen_decl_ids[decl.kind].count(event.id)) {
          if (err) *err = "duplicate declaration id " + event.id;
          return false;
        }
        seen_decl_ids[decl.kind].insert(event.id);
        if (generation_profile) {
          if (decl.id != decl_counts[decl.kind] + 1) {
            if (err) *err = "declaration ids are not sequential";
            return false;
          }
        }
        for (int c = 0; c < 3; c++) {
          if (decl.coordinates[c] < -1000000L || decl.coordinates[c] > 1000000L) {
            if (err) *err = "declaration coordinate out of range";
            return false;
          }
        }
        pool[decl.kind].push_back(decl);
        decl_counts[decl.kind]++;
        if (generation_profile &&
            decl_counts[decl.kind] > kMaxGenerationDeclarationsPerKind) {
          if (err) *err = "too many declarations of one kind";
          return false;
        }
        break;
      }
      case kEventObject:
      case kEventGroup: {
        if (!NameIsLegal(event.name, false)) {
          if (err) *err = "illegal object/group name";
          return false;
        }
        if (generation_profile && saw_switch && faces_since_switch <= 0) {
          if (err) *err = "object/group switch introduces an empty segment";
          return false;
        }
        if (shape_names.count(event.name)) {
          if (err) *err = "duplicate object/group name";
          return false;
        }
        shape_names.insert(event.name);
        saw_switch = true;
        faces_since_switch = 0;
        state.shape_name = event.name;
        break;
      }
      case kEventMaterial: {
        if (!NameIsLegal(event.name, false)) {
          if (err) *err = "illegal material state name";
          return false;
        }
        if (!MaterialExists(asset, event.name)) {
          if (err) *err = "material state references unknown material";
          return false;
        }
        state.material_name = event.name;
        break;
      }
      case kEventSmoothing: {
        if (event.smoothing < 0) {
          if (err) *err = "negative smoothing id";
          return false;
        }
        state.smoothing = event.smoothing;
        break;
      }
      case kEventFace: {
        const Face &face = event.face;
        if (face.id != event.id) {
          if (err) *err = "face id does not match event id";
          return false;
        }
        if (seen_face_ids.count(face.id)) {
          if (err) *err = "duplicate face id " + face.id;
          return false;
        }
        seen_face_ids.insert(face.id);
        if (!NameIsLegal(face.shape_name, !generation_profile)) {
          if (err) *err = "illegal face shape name";
          return false;
        }
        if (!NameIsLegal(face.material_name, !generation_profile)) {
          if (err) *err = "illegal face material name";
          return false;
        }
        if (face.material_name.empty()) {
          if (err) *err = "face without a material";
          return false;
        }
        if (!MaterialExists(asset, face.material_name)) {
          if (err) *err = "face references unknown material";
          return false;
        }
        if (face.smoothing < 0) {
          if (err) *err = "negative face smoothing id";
          return false;
        }
        // Faces must agree with the state derived from the event stream; a
        // reduced asset therefore cannot silently change a live face's
        // object/group/material/smoothing.
        if (face.shape_name != state.shape_name ||
            face.material_name != state.material_name ||
            face.smoothing != state.smoothing) {
          if (err) {
            *err = "face " + face.id + " state does not match its event prefix";
          }
          return false;
        }
        for (int k = 0; k < 3; k++) {
          if (!CornerReferencesAvailable(face.corners[k], pool[kDeclVertex],
                                         pool[kDeclTexcoord], pool[kDeclNormal])) {
            if (err) *err = "face " + face.id + " references an unavailable declaration";
            return false;
          }
        }
        const Decl *a = FindPoolDecl(pool[kDeclVertex], face.corners[0].vertex_id);
        const Decl *b = FindPoolDecl(pool[kDeclVertex], face.corners[1].vertex_id);
        const Decl *c = FindPoolDecl(pool[kDeclVertex], face.corners[2].vertex_id);
        if (!a || !b || !c) {
          if (err) *err = "face " + face.id + " has an unknown vertex";
          return false;
        }
        if (CrossNonZero(*a, *b, *c) <= 0.0) {
          if (err) *err = "face " + face.id + " is degenerate";
          return false;
        }
        face_count++;
        faces_since_switch++;
        if (generation_profile && face_count > kMaxGenerationFaces) {
          if (err) *err = "too many faces";
          return false;
        }
        break;
      }
      default:
        if (err) *err = "unknown event kind";
        return false;
    }
  }

  if (asset.events.empty()) {
    if (err) *err = "asset has no events";
    return false;
  }
  if (face_count <= 0) {
    if (err) *err = "asset has no faces";
    return false;
  }
  if (generation_profile && saw_switch && faces_since_switch <= 0) {
    if (err) *err = "trailing object/group segment has no faces";
    return false;
  }
  if (generation_profile) {
    // Generation always uses increasing face ordinals F0, F1, ...
    int ordinal = 0;
    for (size_t i = 0; i < asset.events.size(); i++) {
      if (asset.events[i].kind == kEventFace) {
        if (asset.events[i].face.id != "F" + std::to_string(ordinal)) {
          if (err) *err = "face ids are not sequential";
          return false;
        }
        ordinal++;
      }
    }
  }
  if (err) err->clear();
  return true;
}

bool is_state_event_kind(EventKind kind) {
  return kind == kEventObject || kind == kEventGroup ||
         kind == kEventMaterial || kind == kEventSmoothing;
}

// ---------------------------------------------------------------------------
// Filesystem helpers
// ---------------------------------------------------------------------------
bool file_exists(const std::string &path) {
  struct stat st;
  return ::stat(path.c_str(), &st) == 0;
}

bool ensure_directory(const std::string &dir) {
  if (dir.empty()) {
    return false;
  }
  std::string current;
  size_t i = 0;
  while (i < dir.size()) {
    size_t next = dir.find('/', i);
    if (next == std::string::npos) {
      next = dir.size();
    }
    current = dir.substr(0, next);
    if (!current.empty()) {
      struct stat st;
      if (::stat(current.c_str(), &st) == 0) {
        if (!S_ISDIR(st.st_mode)) {
          return false;
        }
      } else {
        if (::mkdir(current.c_str(), 0755) != 0 && errno != EEXIST) {
          return false;
        }
      }
    }
    i = next + 1;
  }
  return true;
}

bool read_text_file(const std::string &path, std::string *out) {
  std::ifstream input(path.c_str(), std::ios::binary);
  if (!input) {
    return false;
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  if (out) {
    *out = buffer.str();
  }
  return true;
}

bool write_text_file(const std::string &path, const std::string &text) {
  std::ofstream output(path.c_str(), std::ios::binary | std::ios::trunc);
  if (!output) {
    return false;
  }
  output.write(text.data(), static_cast<std::streamsize>(text.size()));
  output.close();
  return static_cast<bool>(output) || output.eof();
}

bool copy_file_bytes(const std::string &from, const std::string &to) {
  std::string text;
  if (!read_text_file(from, &text)) {
    return false;
  }
  return write_text_file(to, text);
}

}  // namespace objstate
