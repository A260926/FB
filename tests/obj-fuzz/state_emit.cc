#include "state_model.h"

#include <cmath>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <set>
#include <sstream>
#include <vector>

namespace objstate {

namespace {

bool ReadToken(std::istringstream *stream, std::string *token) {
  return static_cast<bool>(*stream >> *token);
}

bool ParseCornerToken(const std::string &token, Corner *corner) {
  // Grammar: V<id><+|-> [ '/' T<id><+|-> ] [ '/' N<id><+|-> ]
  //          or V<id><+|-> "//" N<id><+|-> with no texcoord.
  if (token.size() < 3 || token[0] != 'V') {
    return false;
  }
  size_t pos = 1;
  size_t digit_begin = pos;
  while (pos < token.size() && std::isdigit(static_cast<unsigned char>(token[pos])) != 0) {
    pos++;
  }
  if (pos == digit_begin) {
    return false;
  }
  int id = std::atoi(token.substr(digit_begin, pos - digit_begin).c_str());
  if (id <= 0) {
    return false;
  }
  if (pos >= token.size() || (token[pos] != '+' && token[pos] != '-')) {
    return false;
  }
  corner->vertex_id = id;
  corner->negative_vertex = token[pos] == '-';
  pos++;
  corner->has_texcoord = false;
  corner->has_normal = false;
  if (pos == token.size()) {
    return true;
  }
  if (token[pos] != '/') {
    return false;
  }
  pos++;
  if (pos < token.size() && token[pos] == '/') {
    pos++;
    // normal-only
    if (pos >= token.size() || token[pos] != 'N') {
      return false;
    }
    pos++;
    size_t begin = pos;
    while (pos < token.size() &&
           std::isdigit(static_cast<unsigned char>(token[pos])) != 0) {
      pos++;
    }
    if (pos == begin || pos >= token.size() ||
        (token[pos] != '+' && token[pos] != '-')) {
      return false;
    }
    corner->normal_id = std::atoi(token.substr(begin, pos - begin).c_str());
    corner->negative_normal = token[pos] == '-';
    corner->has_normal = true;
    pos++;
    return pos == token.size() && corner->normal_id > 0;
  }
  // texcoord part
  if (pos >= token.size() || token[pos] != 'T') {
    return false;
  }
  pos++;
  size_t begin = pos;
  while (pos < token.size() &&
         std::isdigit(static_cast<unsigned char>(token[pos])) != 0) {
    pos++;
  }
  if (pos == begin || pos >= token.size() ||
      (token[pos] != '+' && token[pos] != '-')) {
    return false;
  }
  corner->texcoord_id = std::atoi(token.substr(begin, pos - begin).c_str());
  corner->negative_texcoord = token[pos] == '-';
  corner->has_texcoord = true;
  pos++;
  if (pos == token.size()) {
    return corner->texcoord_id > 0;
  }
  if (token[pos] != '/') {
    return false;
  }
  pos++;
  if (pos >= token.size() || token[pos] != 'N') {
    return false;
  }
  pos++;
  begin = pos;
  while (pos < token.size() &&
         std::isdigit(static_cast<unsigned char>(token[pos])) != 0) {
    pos++;
  }
  if (pos == begin || pos >= token.size() ||
      (token[pos] != '+' && token[pos] != '-')) {
    return false;
  }
  corner->normal_id = std::atoi(token.substr(begin, pos - begin).c_str());
  corner->negative_normal = token[pos] == '-';
  corner->has_normal = true;
  pos++;
  return pos == token.size() && corner->texcoord_id > 0 &&
         corner->normal_id > 0;
}

std::string CornerToken(const Corner &corner) {
  std::ostringstream os;
  os << "V" << corner.vertex_id << (corner.negative_vertex ? '-' : '+');
  if (corner.has_texcoord) {
    os << "/T" << corner.texcoord_id << (corner.negative_texcoord ? '-' : '+');
  }
  if (corner.has_normal) {
    os << (corner.has_texcoord ? "/N" : "//N") << corner.normal_id
       << (corner.negative_normal ? '-' : '+');
  }
  return os.str();
}

std::string NameToken(const std::string &name) {
  return name.empty() ? std::string("-") : name;
}

bool ParseNameToken(const std::string &token, std::string *name) {
  if (token == "-") {
    name->clear();
    return true;
  }
  if (!name) {
    return false;
  }
  *name = token;
  return true;
}

const Decl *FindDecl(const std::vector<Decl> &pool, int id) {
  for (size_t i = 0; i < pool.size(); i++) {
    if (pool[i].id == id) {
      return &pool[i];
    }
  }
  return NULL;
}

double CoordAt(const Decl &decl, int index) {
  return static_cast<double>(decl.coordinates[index]);
}

}  // namespace

std::string material_text(const Asset &asset) {
  std::ostringstream os;
  for (size_t i = 0; i < asset.materials.size(); i++) {
    os << "newmtl " << asset.materials[i].name << "\n";
    os << "Kd " << asset.materials[i].r << " " << asset.materials[i].g << " "
       << asset.materials[i].b << "\n";
  }
  return os.str();
}

std::string serialize_asset(const Asset &asset) {
  std::ostringstream os;
  os << "OBJSTATE_TRACE 1\n";
  os << "SEED " << asset.seed << "\n";
  for (size_t i = 0; i < asset.materials.size(); i++) {
    os << "MAT " << asset.materials[i].name << " " << asset.materials[i].r
       << " " << asset.materials[i].g << " " << asset.materials[i].b << "\n";
  }
  for (size_t i = 0; i < asset.events.size(); i++) {
    const Event &event = asset.events[i];
    switch (event.kind) {
      case kEventDeclaration: {
        const Decl &decl = event.decl;
        os << "D ";
        switch (decl.kind) {
          case kDeclVertex:
            os << "V " << decl.id << " " << decl.coordinates[0] << " "
               << decl.coordinates[1] << " " << decl.coordinates[2] << "\n";
            break;
          case kDeclTexcoord:
            os << "T " << decl.id << " " << decl.coordinates[0] << " "
               << decl.coordinates[1] << "\n";
            break;
          default:
            os << "N " << decl.id << " " << decl.coordinates[0] << " "
               << decl.coordinates[1] << " " << decl.coordinates[2] << "\n";
            break;
        }
        break;
      }
      case kEventObject:
        os << "E O " << event.id << " " << NameToken(event.name) << "\n";
        break;
      case kEventGroup:
        os << "E G " << event.id << " " << NameToken(event.name) << "\n";
        break;
      case kEventMaterial:
        os << "E U " << event.id << " " << event.name << "\n";
        break;
      case kEventSmoothing:
        os << "E S " << event.id << " " << event.smoothing << "\n";
        break;
      case kEventFace:
        os << "F " << event.face.id << " " << NameToken(event.face.shape_name)
           << " " << event.face.material_name << " " << event.face.smoothing
           << " " << CornerToken(event.face.corners[0]) << " "
           << CornerToken(event.face.corners[1]) << " "
           << CornerToken(event.face.corners[2]) << "\n";
        break;
      default:
        break;
    }
  }
  return os.str();
}

bool parse_asset(const std::string &text, Asset *asset, std::string *err) {
  if (!asset) {
    if (err) *err = "null asset";
    return false;
  }
  asset->materials.clear();
  asset->events.clear();
  asset->seed = 0;

  std::istringstream lines(text);
  std::string line;
  bool saw_header = false;
  while (std::getline(lines, line)) {
    if (!line.empty() && line[line.size() - 1] == '\r') {
      line.erase(line.size() - 1);
    }
    std::istringstream tokens(line);
    std::string keyword;
    if (!ReadToken(&tokens, &keyword)) {
      continue;
    }
    if (keyword == "OBJSTATE_TRACE") {
      std::string version;
      if (!ReadToken(&tokens, &version) || version != "1") {
        if (err) *err = "unsupported trace version";
        return false;
      }
      saw_header = true;
      continue;
    }
    if (!saw_header) {
      if (err) *err = "missing trace header";
      return false;
    }
    if (keyword == "SEED") {
      std::string value;
      if (!ReadToken(&tokens, &value)) {
        if (err) *err = "bad seed line";
        return false;
      }
      asset->seed = std::strtoull(value.c_str(), NULL, 10);
      continue;
    }
    if (keyword == "MAT") {
      Material material;
      std::string r;
      std::string g;
      std::string b;
      if (!ReadToken(&tokens, &material.name) || !ReadToken(&tokens, &r) ||
          !ReadToken(&tokens, &g) || !ReadToken(&tokens, &b)) {
        if (err) *err = "bad material line";
        return false;
      }
      material.r = std::strtol(r.c_str(), NULL, 10);
      material.g = std::strtol(g.c_str(), NULL, 10);
      material.b = std::strtol(b.c_str(), NULL, 10);
      if (!ParseNameToken(material.name, &material.name)) {
        if (err) *err = "bad material name";
        return false;
      }
      asset->materials.push_back(material);
      continue;
    }
    if (keyword == "D") {
      std::string kind;
      std::string id_token;
      if (!ReadToken(&tokens, &kind) || !ReadToken(&tokens, &id_token)) {
        if (err) *err = "bad declaration line";
        return false;
      }
      Event event;
      event.kind = kEventDeclaration;
      event.decl.id = std::atoi(id_token.c_str());
      if (kind == "V" || kind == "N") {
        event.decl.kind = kind == "V" ? kDeclVertex : kDeclNormal;
        std::string a;
        std::string b;
        std::string c;
        if (!ReadToken(&tokens, &a) || !ReadToken(&tokens, &b) ||
            !ReadToken(&tokens, &c)) {
          if (err) *err = "bad declaration coordinates";
          return false;
        }
        event.decl.coordinates[0] = std::strtol(a.c_str(), NULL, 10);
        event.decl.coordinates[1] = std::strtol(b.c_str(), NULL, 10);
        event.decl.coordinates[2] = std::strtol(c.c_str(), NULL, 10);
      } else if (kind == "T") {
        event.decl.kind = kDeclTexcoord;
        std::string a;
        std::string b;
        if (!ReadToken(&tokens, &a) || !ReadToken(&tokens, &b)) {
          if (err) *err = "bad texcoord declaration";
          return false;
        }
        event.decl.coordinates[0] = std::strtol(a.c_str(), NULL, 10);
        event.decl.coordinates[1] = std::strtol(b.c_str(), NULL, 10);
        event.decl.coordinates[2] = 0;
      } else {
        if (err) *err = "unknown declaration kind";
        return false;
      }
      event.id =
          (event.decl.kind == kDeclVertex
               ? "V"
               : (event.decl.kind == kDeclTexcoord ? "T" : "N")) +
          std::to_string(event.decl.id);
      asset->events.push_back(event);
      continue;
    }
    if (keyword == "E") {
      std::string kind;
      std::string id;
      if (!ReadToken(&tokens, &kind) || !ReadToken(&tokens, &id)) {
        if (err) *err = "bad state line";
        return false;
      }
      Event event;
      event.id = id;
      if (kind == "O" || kind == "G") {
        std::string name;
        if (!ReadToken(&tokens, &name) || !ParseNameToken(name, &event.name)) {
          if (err) *err = "bad state name";
          return false;
        }
        event.kind = kind == "O" ? kEventObject : kEventGroup;
      } else if (kind == "U") {
        std::string name;
        if (!ReadToken(&tokens, &name) || !ParseNameToken(name, &event.name)) {
          if (err) *err = "bad material state name";
          return false;
        }
        if (event.name.empty()) {
          if (err) *err = "empty material state name";
          return false;
        }
        event.kind = kEventMaterial;
      } else if (kind == "S") {
        std::string value;
        if (!ReadToken(&tokens, &value)) {
          if (err) *err = "bad smoothing state";
          return false;
        }
        event.kind = kEventSmoothing;
        event.smoothing = std::strtol(value.c_str(), NULL, 10);
      } else {
        if (err) *err = "unknown state kind";
        return false;
      }
      asset->events.push_back(event);
      continue;
    }
    if (keyword == "F") {
      Event event;
      event.kind = kEventFace;
      std::string shape;
      std::string material;
      std::string smoothing;
      if (!ReadToken(&tokens, &event.face.id) || !ReadToken(&tokens, &shape) ||
          !ReadToken(&tokens, &material) || !ReadToken(&tokens, &smoothing)) {
        if (err) *err = "bad face line";
        return false;
      }
      if (!ParseNameToken(shape, &event.face.shape_name) ||
          !ParseNameToken(material, &event.face.material_name)) {
        if (err) *err = "bad face names";
        return false;
      }
      event.face.smoothing = std::strtol(smoothing.c_str(), NULL, 10);
      for (int k = 0; k < 3; k++) {
        std::string token;
        if (!ReadToken(&tokens, &token) ||
            !ParseCornerToken(token, &event.face.corners[k])) {
          if (err) *err = "bad face corner token";
          return false;
        }
      }
      std::string extra;
      if (ReadToken(&tokens, &extra)) {
        if (err) *err = "trailing tokens on face line";
        return false;
      }
      event.id = event.face.id;
      asset->events.push_back(event);
      continue;
    }
    if (err) *err = "unknown trace keyword " + keyword;
    return false;
  }
  if (!saw_header) {
    if (err) *err = "missing trace header";
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// OBJ serialization
// ---------------------------------------------------------------------------

namespace {

struct PoolPosition {
  std::vector<int> ids;
  int IndexOf(int id) const {
    for (size_t i = 0; i < ids.size(); i++) {
      if (ids[i] == id) {
        return static_cast<int>(i);
      }
    }
    return -1;
  }
  int Size() const { return static_cast<int>(ids.size()); }
};

bool EncodeComponent(int ordinal, int pool_size, bool negative_style,
                     std::string *out) {
  if (ordinal < 0 || ordinal >= pool_size) {
    return false;
  }
  if (negative_style) {
    *out = std::to_string(ordinal - pool_size);
  } else {
    *out = std::to_string(ordinal + 1);
  }
  return true;
}

}  // namespace

bool emit_obj_text(const Asset &asset, bool all_positive, std::string *out,
                   std::string *err) {
  if (!out) {
    if (err) *err = "null output";
    return false;
  }
  std::ostringstream os;
  os << "mtllib case.mtl\n";
  PoolPosition pools[3];
  std::string shape_name;
  std::string material_name;
  long smoothing = 0;

  for (size_t i = 0; i < asset.events.size(); i++) {
    const Event &event = asset.events[i];
    switch (event.kind) {
      case kEventDeclaration: {
        const Decl &decl = event.decl;
        const int ordinal = pools[decl.kind].Size();
        pools[decl.kind].ids.push_back(decl.id);
        if (decl.kind == kDeclVertex) {
          os << "v " << decl.coordinates[0] << " " << decl.coordinates[1]
             << " " << decl.coordinates[2] << "\n";
        } else if (decl.kind == kDeclTexcoord) {
          os << "vt " << decl.coordinates[0] << " " << decl.coordinates[1]
             << "\n";
        } else {
          os << "vn " << decl.coordinates[0] << " " << decl.coordinates[1]
             << " " << decl.coordinates[2] << "\n";
        }
        (void)ordinal;
        break;
      }
      case kEventObject:
        shape_name = event.name;
        os << "o " << event.name << "\n";
        break;
      case kEventGroup:
        shape_name = event.name;
        os << "g " << event.name << "\n";
        break;
      case kEventMaterial:
        material_name = event.name;
        os << "usemtl " << event.name << "\n";
        break;
      case kEventSmoothing:
        smoothing = event.smoothing;
        if (event.smoothing == 0) {
          os << "s off\n";
        } else {
          os << "s " << event.smoothing << "\n";
        }
        break;
      case kEventFace: {
        os << "f";
        for (int k = 0; k < 3; k++) {
          const Corner &corner = event.face.corners[k];
          const int v_ord = pools[kDeclVertex].IndexOf(corner.vertex_id);
          if (v_ord < 0) {
            if (err) *err = "face references a removed vertex declaration";
            return false;
          }
          std::string token;
          if (!EncodeComponent(v_ord, pools[kDeclVertex].Size(),
                               !all_positive && corner.negative_vertex, &token)) {
            if (err) *err = "cannot encode vertex index";
            return false;
          }
          os << " " << token;
          if (corner.has_texcoord) {
            const int t_ord = pools[kDeclTexcoord].IndexOf(corner.texcoord_id);
            if (t_ord < 0) {
              if (err) *err = "face references a removed texcoord declaration";
              return false;
            }
            if (!EncodeComponent(t_ord, pools[kDeclTexcoord].Size(),
                                 !all_positive && corner.negative_texcoord,
                                 &token)) {
              if (err) *err = "cannot encode texcoord index";
              return false;
            }
            os << "/" << token;
          } else if (corner.has_normal) {
            os << "/";
          }
          if (corner.has_normal) {
            const int n_ord = pools[kDeclNormal].IndexOf(corner.normal_id);
            if (n_ord < 0) {
              if (err) *err = "face references a removed normal declaration";
              return false;
            }
            if (!EncodeComponent(n_ord, pools[kDeclNormal].Size(),
                                 !all_positive && corner.negative_normal,
                                 &token)) {
              if (err) *err = "cannot encode normal index";
              return false;
            }
            os << "/" << token;
          }
        }
        os << "\n";
        (void)shape_name;
        (void)material_name;
        (void)smoothing;
        break;
      }
      default:
        if (err) *err = "unknown event";
        return false;
    }
  }
  *out = os.str();
  return true;
}

// ---------------------------------------------------------------------------
// Reference event model
// ---------------------------------------------------------------------------

bool build_expected_records(const Asset &asset, std::vector<FaceRecord> *out,
                            std::string *err) {
  if (!out) {
    if (err) *err = "null output";
    return false;
  }
  out->clear();
  std::vector<Decl> pools[3];
  for (size_t i = 0; i < asset.events.size(); i++) {
    const Event &event = asset.events[i];
    if (event.kind == kEventDeclaration) {
      pools[event.decl.kind].push_back(event.decl);
      continue;
    }
    if (event.kind != kEventFace) {
      continue;
    }
    const Face &face = event.face;
    FaceRecord record;
    record.face_id = face.id;
    record.shape_name = face.shape_name;
    record.material_name = face.material_name;
    record.smoothing = face.smoothing;
    for (int k = 0; k < 3; k++) {
      const Corner &corner = face.corners[k];
      const Decl *vdecl = FindDecl(pools[kDeclVertex], corner.vertex_id);
      if (!vdecl) {
        if (err) *err = "face " + face.id + " references a missing vertex";
        return false;
      }
      record.corners[k].has_texcoord = corner.has_texcoord;
      record.corners[k].has_normal = corner.has_normal;
      record.corners[k].vertex_index =
          static_cast<int>(vdecl - &pools[kDeclVertex][0]);
      record.corners[k].texcoord_index = -1;
      record.corners[k].normal_index = -1;
      record.corners[k].vertex[0] = CoordAt(*vdecl, 0);
      record.corners[k].vertex[1] = CoordAt(*vdecl, 1);
      record.corners[k].vertex[2] = CoordAt(*vdecl, 2);
      if (corner.has_texcoord) {
        const Decl *tdecl = FindDecl(pools[kDeclTexcoord], corner.texcoord_id);
        if (!tdecl) {
          if (err) *err = "face " + face.id + " references a missing texcoord";
          return false;
        }
        record.corners[k].texcoord_index =
            static_cast<int>(tdecl - &pools[kDeclTexcoord][0]);
        record.corners[k].texcoord[0] = CoordAt(*tdecl, 0);
        record.corners[k].texcoord[1] = CoordAt(*tdecl, 1);
      }
      if (corner.has_normal) {
        const Decl *ndecl = FindDecl(pools[kDeclNormal], corner.normal_id);
        if (!ndecl) {
          if (err) *err = "face " + face.id + " references a missing normal";
          return false;
        }
        record.corners[k].normal_index =
            static_cast<int>(ndecl - &pools[kDeclNormal][0]);
        record.corners[k].normal[0] = CoordAt(*ndecl, 0);
        record.corners[k].normal[1] = CoordAt(*ndecl, 1);
        record.corners[k].normal[2] = CoordAt(*ndecl, 2);
      }
    }
    out->push_back(record);
  }
  return true;
}

// ---------------------------------------------------------------------------
// Independent OBJ text interpreter (pool sizes at the face, not final sizes)
// ---------------------------------------------------------------------------

namespace {

struct TextPool {
  std::vector<std::vector<double> > values;  // 3 (v/vn) or 2 (vt) components
  int Size() const { return static_cast<int>(values.size()); }
  bool Resolve(int token, bool *present, int *ordinal) const {
    if (token > 0) {
      *ordinal = token - 1;
    } else if (token < 0) {
      *ordinal = Size() + token;
    } else {
      *ordinal = -1;
      *present = false;
      return true;
    }
    if (*ordinal < 0 || *ordinal >= Size()) {
      return false;
    }
    *present = true;
    return true;
  }
};

bool ParseRealToken(const std::string &token, double *value) {
  if (token.empty()) {
    return false;
  }
  char *end = NULL;
  double parsed = std::strtod(token.c_str(), &end);
  if (!end || *end != '\0') {
    return false;
  }
  *value = parsed;
  return true;
}

bool SplitFaceToken(const std::string &token, std::string *parts,
                    int *part_count) {
  *part_count = 0;
  size_t begin = 0;
  while (true) {
    size_t slash = token.find('/', begin);
    if (*part_count >= 3) {
      return false;
    }
    if (slash == std::string::npos) {
      parts[*part_count] = token.substr(begin);
      (*part_count)++;
      break;
    }
    parts[*part_count] = token.substr(begin, slash - begin);
    (*part_count)++;
    begin = slash + 1;
  }
  return true;
}

}  // namespace

bool interpret_obj_text(const std::string &obj_text,
                        const std::string &mtl_text,
                        std::vector<FaceRecord> *out, std::string *err) {
  if (!out) {
    if (err) *err = "null output";
    return false;
  }
  out->clear();

  std::set<std::string> material_names;
  {
    std::istringstream mtl_lines(mtl_text);
    std::string line;
    while (std::getline(mtl_lines, line)) {
      std::istringstream tokens(line);
      std::string keyword;
      if (tokens >> keyword && keyword == "newmtl") {
        std::string name;
        if (tokens >> name) {
          material_names.insert(name);
        }
      }
    }
  }

  TextPool pools[3];
  std::string shape_name;
  std::string material_name;
  long smoothing = 0;
  int face_ordinal = 0;

  std::istringstream lines(obj_text);
  std::string line;
  while (std::getline(lines, line)) {
    if (!line.empty() && line[line.size() - 1] == '\r') {
      line.erase(line.size() - 1);
    }
    std::istringstream tokens(line);
    std::string keyword;
    if (!(tokens >> keyword)) {
      continue;
    }
    if (keyword == "mtllib") {
      continue;
    }
    if (keyword == "o") {
      std::string rest;
      std::getline(tokens, rest);
      size_t begin = 0;
      while (begin < rest.size() &&
             std::isspace(static_cast<unsigned char>(rest[begin])) != 0) {
        begin++;
      }
      shape_name = rest.substr(begin);
      while (!shape_name.empty() &&
             std::isspace(
                 static_cast<unsigned char>(shape_name[shape_name.size() - 1])) !=
                 0) {
        shape_name.erase(shape_name.size() - 1);
      }
      continue;
    }
    if (keyword == "g") {
      std::vector<std::string> names;
      std::string token;
      while (tokens >> token) {
        names.push_back(token);
      }
      if (!names.empty()) {
        std::ostringstream joined;
        joined << names[0];
        for (size_t i = 1; i < names.size(); i++) {
          joined << " " << names[i];
        }
        shape_name = joined.str();
      } else {
        shape_name.clear();
      }
      continue;
    }
    if (keyword == "usemtl") {
      std::string name;
      if (!(tokens >> name)) {
        name.clear();
      }
      if (!name.empty() && material_names.find(name) == material_names.end()) {
        if (err) *err = "usemtl references an unknown material " + name;
        return false;
      }
      material_name = name;
      continue;
    }
    if (keyword == "s") {
      std::string value;
      if (!(tokens >> value)) {
        continue;
      }
      if (value == "off") {
        smoothing = 0;
      } else {
        smoothing = std::strtol(value.c_str(), NULL, 10);
      }
      continue;
    }
    if (keyword == "v" || keyword == "vn" || keyword == "vt") {
      const int kind = keyword == "v" ? kDeclVertex
                                      : (keyword == "vt" ? kDeclTexcoord
                                                         : kDeclNormal);
      const int count = kind == kDeclTexcoord ? 2 : 3;
      std::vector<double> values;
      for (int c = 0; c < count; c++) {
        std::string token;
        if (!(tokens >> token)) {
          if (err) *err = "bad attribute line";
          return false;
        }
        double value = 0.0;
        if (!ParseRealToken(token, &value)) {
          if (err) *err = "bad attribute value";
          return false;
        }
        values.push_back(value);
      }
      pools[kind].values.push_back(values);
      continue;
    }
    if (keyword == "f") {
      std::string token;
      int corner_count = 0;
      FaceRecord record;
      while (tokens >> token) {
        if (corner_count >= 3) {
          if (err) *err = "non-triangle face";
          return false;
        }
        std::string parts[3];
        int part_count = 0;
        if (!SplitFaceToken(token, parts, &part_count)) {
          if (err) *err = "bad face token " + token;
          return false;
        }
        CornerRecord corner;
        bool present = false;
        int ordinal = 0;
        if (!pools[kDeclVertex].Resolve(std::atoi(parts[0].c_str()), &present,
                                        &ordinal) ||
            !present) {
          if (err) *err = "bad vertex token " + token;
          return false;
        }
        corner.vertex[0] = pools[kDeclVertex].values[ordinal][0];
        corner.vertex[1] = pools[kDeclVertex].values[ordinal][1];
        corner.vertex[2] = pools[kDeclVertex].values[ordinal][2];
        corner.vertex_index = ordinal;
        corner.has_texcoord = false;
        corner.has_normal = false;
        if (part_count >= 2 && !parts[1].empty()) {
          if (!pools[kDeclTexcoord].Resolve(std::atoi(parts[1].c_str()), &present,
                                            &ordinal) ||
              !present) {
            if (err) *err = "bad texcoord token " + token;
            return false;
          }
          corner.texcoord[0] = pools[kDeclTexcoord].values[ordinal][0];
          corner.texcoord[1] = pools[kDeclTexcoord].values[ordinal][1];
          corner.texcoord_index = ordinal;
          corner.has_texcoord = true;
        }
        if (part_count >= 3 && !parts[2].empty()) {
          if (!pools[kDeclNormal].Resolve(std::atoi(parts[2].c_str()), &present,
                                          &ordinal) ||
              !present) {
            if (err) *err = "bad normal token " + token;
            return false;
          }
          corner.normal[0] = pools[kDeclNormal].values[ordinal][0];
          corner.normal[1] = pools[kDeclNormal].values[ordinal][1];
          corner.normal[2] = pools[kDeclNormal].values[ordinal][2];
          corner.normal_index = ordinal;
          corner.has_normal = true;
        }
        record.corners[corner_count] = corner;
        corner_count++;
      }
      if (corner_count != 3) {
        if (err) *err = "face is not a triangle";
        return false;
      }
      record.shape_name = shape_name;
      record.material_name = material_name;
      record.smoothing = smoothing;
      record.face_id = "F" + std::to_string(face_ordinal);
      face_ordinal++;
      out->push_back(record);
      continue;
    }
    // Ignore unknown commands (matches the library's forgiving behavior).
  }
  return true;
}

// ---------------------------------------------------------------------------
// Comparisons
// ---------------------------------------------------------------------------

namespace {

bool DoubleEqual(double a, double b) { return a == b; }

std::string DescribeCorner(const CornerRecord &corner) {
  std::ostringstream os;
  os << "v=(" << format_double(corner.vertex[0]) << ","
     << format_double(corner.vertex[1]) << ","
     << format_double(corner.vertex[2]) << ")";
  if (corner.has_texcoord) {
    os << " vt=(" << format_double(corner.texcoord[0]) << ","
       << format_double(corner.texcoord[1]) << ")";
  } else {
    os << " vt=absent";
  }
  if (corner.has_normal) {
    os << " vn=(" << format_double(corner.normal[0]) << ","
       << format_double(corner.normal[1]) << ","
       << format_double(corner.normal[2]) << ")";
  } else {
    os << " vn=absent";
  }
  return os.str();
}

bool FillCornerMismatch(const CornerRecord &expected, const CornerRecord &actual,
                        const std::string &entry, const std::string &face_id,
                        int corner, Mismatch *mismatch) {
  if (expected.has_texcoord != actual.has_texcoord ||
      expected.has_normal != actual.has_normal) {
    mismatch->face_id = face_id;
    mismatch->corner = corner;
    mismatch->component = "presence";
    mismatch->entry = entry;
    mismatch->expected = DescribeCorner(expected);
    mismatch->actual = DescribeCorner(actual);
    return true;
  }
  for (int c = 0; c < 3; c++) {
    if (!DoubleEqual(expected.vertex[c], actual.vertex[c])) {
      mismatch->face_id = face_id;
      mismatch->corner = corner;
      mismatch->component = "vertex";
      mismatch->entry = entry;
      mismatch->expected = format_double(expected.vertex[c]);
      mismatch->actual = format_double(actual.vertex[c]);
      return true;
    }
  }
  if (expected.has_texcoord) {
    for (int c = 0; c < 2; c++) {
      if (!DoubleEqual(expected.texcoord[c], actual.texcoord[c])) {
        mismatch->face_id = face_id;
        mismatch->corner = corner;
        mismatch->component = "texcoord";
        mismatch->entry = entry;
        mismatch->expected = format_double(expected.texcoord[c]);
        mismatch->actual = format_double(actual.texcoord[c]);
        return true;
      }
    }
  }
  if (expected.has_normal) {
    for (int c = 0; c < 3; c++) {
      if (!DoubleEqual(expected.normal[c], actual.normal[c])) {
        mismatch->face_id = face_id;
        mismatch->corner = corner;
        mismatch->component = "normal";
        mismatch->entry = entry;
        mismatch->expected = format_double(expected.normal[c]);
        mismatch->actual = format_double(actual.normal[c]);
        return true;
      }
    }
  }
  return false;
}

}  // namespace

bool compare_records(const std::vector<FaceRecord> &expected,
                     const std::vector<FaceRecord> &actual,
                     const std::string &entry, Mismatch *mismatch) {
  return compare_record_values(expected, actual, entry, mismatch);
}

bool compare_record_values(const std::vector<FaceRecord> &expected,
                           const std::vector<FaceRecord> &actual,
                           const std::string &entry, Mismatch *mismatch) {
  if (!mismatch) {
    return false;
  }
  if (expected.size() != actual.size()) {
    mismatch->face_id = expected.size() < actual.size()
                            ? (expected.empty() ? std::string("<none>")
                                                : expected.back().face_id)
                            : expected[actual.size()].face_id;
    mismatch->corner = -1;
    mismatch->component = "face-count";
    mismatch->entry = entry;
    mismatch->expected = std::to_string(expected.size());
    mismatch->actual = std::to_string(actual.size());
    return false;
  }
  for (size_t i = 0; i < expected.size(); i++) {
    const FaceRecord &e = expected[i];
    const FaceRecord &a = actual[i];
    if (e.shape_name != a.shape_name) {
      mismatch->face_id = e.face_id;
      mismatch->corner = -1;
      mismatch->component = "shape";
      mismatch->entry = entry;
      mismatch->expected = e.shape_name.empty() ? "<empty>" : e.shape_name;
      mismatch->actual = a.shape_name.empty() ? "<empty>" : a.shape_name;
      return false;
    }
    if (e.material_name != a.material_name) {
      mismatch->face_id = e.face_id;
      mismatch->corner = -1;
      mismatch->component = "material";
      mismatch->entry = entry;
      mismatch->expected = e.material_name.empty() ? "<empty>" : e.material_name;
      mismatch->actual = a.material_name.empty() ? "<empty>" : a.material_name;
      return false;
    }
    if (e.smoothing != a.smoothing) {
      mismatch->face_id = e.face_id;
      mismatch->corner = -1;
      mismatch->component = "smoothing";
      mismatch->entry = entry;
      mismatch->expected = std::to_string(e.smoothing);
      mismatch->actual = std::to_string(a.smoothing);
      return false;
    }
    for (int k = 0; k < 3; k++) {
      if (FillCornerMismatch(e.corners[k], a.corners[k], entry, e.face_id, k,
                             mismatch)) {
        return false;
      }
    }
  }
  return true;
}

bool compare_production_records(const std::vector<FaceRecord> &expected,
                                const std::vector<FaceRecord> &actual,
                                const std::string &entry,
                                Mismatch *mismatch) {
  if (!compare_record_values(expected, actual, entry, mismatch)) {
    return false;
  }
  // Production records additionally must expose -1 exactly for the absent
  // texcoord/normal components (never 0, never a stale positive index).
  for (size_t i = 0; i < actual.size(); i++) {
    for (int k = 0; k < 3; k++) {
      const CornerRecord &corner = actual[i].corners[k];
      if (corner.vertex_index < 0) {
        mismatch->face_id = actual[i].face_id;
        mismatch->corner = k;
        mismatch->component = "vertex-index";
        mismatch->entry = entry;
        mismatch->expected = ">=0";
        mismatch->actual = std::to_string(corner.vertex_index);
        return false;
      }
      if (!corner.has_texcoord && corner.texcoord_index != -1) {
        mismatch->face_id = actual[i].face_id;
        mismatch->corner = k;
        mismatch->component = "texcoord-index";
        mismatch->entry = entry;
        mismatch->expected = "-1";
        mismatch->actual = std::to_string(corner.texcoord_index);
        return false;
      }
      if (!corner.has_normal && corner.normal_index != -1) {
        mismatch->face_id = actual[i].face_id;
        mismatch->corner = k;
        mismatch->component = "normal-index";
        mismatch->entry = entry;
        mismatch->expected = "-1";
        mismatch->actual = std::to_string(corner.normal_index);
        return false;
      }
    }
  }
  return true;
}

bool compare_obj_text_to_model(const Asset &asset, const std::string &obj_text,
                               const std::string &mtl_text,
                               const std::string &entry,
                               Mismatch *mismatch) {
  std::vector<FaceRecord> expected;
  std::vector<FaceRecord> actual;
  std::string err;
  if (!build_expected_records(asset, &expected, &err)) {
    if (mismatch) {
      mismatch->face_id = "<model>";
      mismatch->corner = -1;
      mismatch->component = "model";
      mismatch->entry = entry;
      mismatch->expected = "valid model";
      mismatch->actual = err;
    }
    return false;
  }
  if (!interpret_obj_text(obj_text, mtl_text, &actual, &err)) {
    if (mismatch) {
      mismatch->face_id = "<text>";
      mismatch->corner = -1;
      mismatch->component = "text";
      mismatch->entry = entry;
      mismatch->expected = "parseable OBJ text";
      mismatch->actual = err;
    }
    return false;
  }
  return compare_records(expected, actual, entry, mismatch);
}

}  // namespace objstate
