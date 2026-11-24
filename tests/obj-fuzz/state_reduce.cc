#include "state_model.h"

#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

namespace objstate {

namespace {

std::string DirPath(const std::string &dir, const std::string &name) {
  if (dir.empty()) {
    return name;
  }
  return dir + "/" + name;
}

class GenerationRng {
 public:
  explicit GenerationRng(uint64_t seed) : state_(seed) {
    if (state_ == 0) {
      state_ = 0x9e3779b97f4a7c15ULL;
    }
  }
  uint64_t Next() {
    uint64_t x = state_;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    state_ = x;
    return x * 0x2545F4914F6CDD1DULL;
  }
  uint32_t NextU32() { return static_cast<uint32_t>(Next() >> 32); }
  size_t Uniform(size_t bound) {
    if (bound == 0) {
      return 0;
    }
    return static_cast<size_t>(Next() % bound);
  }
  long Range(long min_value, long max_value) {
    return min_value + static_cast<long>(
                           Uniform(static_cast<size_t>(max_value - min_value + 1)));
  }
  bool OneIn(size_t n) { return Uniform(n) == 0; }
  bool Bool() { return (NextU32() & 1u) != 0u; }

 private:
  uint64_t state_;
};

bool IsCollinear(const Decl &a, const Decl &b, const Decl &c) {
  const long e0x = b.coordinates[0] - a.coordinates[0];
  const long e0y = b.coordinates[1] - a.coordinates[1];
  const long e0z = b.coordinates[2] - a.coordinates[2];
  const long e1x = c.coordinates[0] - b.coordinates[0];
  const long e1y = c.coordinates[1] - b.coordinates[1];
  const long e1z = c.coordinates[2] - b.coordinates[2];
  const long cx = e0y * e1z - e0z * e1y;
  const long cy = e0z * e1x - e0x * e1z;
  const long cz = e0x * e1y - e0y * e1x;
  return cx == 0 && cy == 0 && cz == 0;
}

// Picks three distinct, non-collinear vertices from `verts`.
bool PickVertices(const std::vector<Decl> &verts, GenerationRng *rng, int *ids) {
  const size_t n = verts.size();
  if (n < 3) {
    return false;
  }
  for (size_t trial = 0; trial < 128; trial++) {
    const size_t a = rng->Uniform(n);
    const size_t b = rng->Uniform(n);
    const size_t c = rng->Uniform(n);
    if (a == b || b == c || a == c) {
      continue;
    }
    if (!IsCollinear(verts[a], verts[b], verts[c])) {
      ids[0] = verts[a].id;
      ids[1] = verts[b].id;
      ids[2] = verts[c].id;
      return true;
    }
  }
  for (size_t ia = 0; ia < n; ia++) {
    for (size_t ib = ia + 1; ib < n; ib++) {
      for (size_t ic = ib + 1; ic < n; ic++) {
        if (!IsCollinear(verts[ia], verts[ib], verts[ic])) {
          ids[0] = verts[ia].id;
          ids[1] = verts[ib].id;
          ids[2] = verts[ic].id;
          return true;
        }
      }
    }
  }
  return false;
}

enum TokenForm {
  kFormVertexOnly = 0,
  kFormVertexNormal = 1,
  kFormVertexTexcoord = 2,
  kFormFull = 3
};

bool AddGeneratedFace(AssetBuilder *builder, const std::vector<Decl> &verts,
                      const std::vector<Decl> &texcoords,
                      const std::vector<Decl> &normals, GenerationRng *rng,
                      TokenForm form, bool positive_vertex,
                      bool positive_texcoord, bool positive_normal,
                      std::string *err) {
  CornerSpec corners[3];
  int vids[3] = {0, 0, 0};
  if (!PickVertices(verts, rng, vids)) {
    if (err) *err = "cannot pick a non-degenerate triangle";
    return false;
  }
  const bool need_texcoord = form == kFormVertexTexcoord || form == kFormFull;
  const bool need_normal = form == kFormVertexNormal || form == kFormFull;
  if (need_texcoord && texcoords.empty()) {
    if (err) *err = "no texcoord declaration available";
    return false;
  }
  if (need_normal && normals.empty()) {
    if (err) *err = "no normal declaration available";
    return false;
  }
  for (int k = 0; k < 3; k++) {
    corners[k].vertex_id = vids[k];
    corners[k].negative_vertex = !positive_vertex;
    if (need_texcoord) {
      const Decl &decl = texcoords[rng->Uniform(texcoords.size())];
      corners[k].texcoord_id = decl.id;
      corners[k].has_texcoord = true;
      corners[k].negative_texcoord = !positive_texcoord;
    }
    if (need_normal) {
      const Decl &decl = normals[rng->Uniform(normals.size())];
      corners[k].normal_id = decl.id;
      corners[k].has_normal = true;
      corners[k].negative_normal = !positive_normal;
    }
  }
  return builder->add_face(corners, err);
}

Decl AddVertexDecl(AssetBuilder *builder, GenerationRng *rng) {
  return builder->add_vertex(rng->Range(-10, 10), rng->Range(-10, 10),
                             rng->Range(-10, 10));
}

Decl AddTexcoordDecl(AssetBuilder *builder, GenerationRng *rng) {
  return builder->add_texcoord(rng->Range(-6, 6), rng->Range(-6, 6));
}

Decl AddNormalDecl(AssetBuilder *builder, GenerationRng *rng) {
  long x = rng->Range(-4, 4);
  long y = rng->Range(-4, 4);
  long z = rng->Range(-4, 4);
  if (x == 0 && y == 0 && z == 0) {
    x = 1;
  }
  return builder->add_normal(x, y, z);
}

std::string UniqueName(const char *prefix, int ordinal) {
  std::ostringstream os;
  os << prefix << "_" << ordinal;
  return os.str();
}

}  // namespace

bool generate_asset(uint64_t seed, Asset *out, std::string *err) {
  if (!out) {
    if (err) *err = "null output";
    return false;
  }
  GenerationRng rng(seed);
  const int v_count = 8 + static_cast<int>(rng.Uniform(5));   // 8..12
  const int vt_count = 4 + static_cast<int>(rng.Uniform(2));  // 4..5
  const int vn_count = 6 + static_cast<int>(rng.Uniform(2));  // 6..7
  const int face_count = 6 + static_cast<int>(rng.Uniform(9));  // 6..14

  AssetBuilder builder(seed);
  builder.add_material("mat_red", 1, 0, 0);
  builder.add_material("mat_green", 0, 1, 0);
  builder.add_material("mat_blue", 0, 0, 1);

  builder.add_object("obj_main");
  builder.add_material_state("mat_red");
  builder.add_smoothing(1);

  std::vector<Decl> verts;
  std::vector<Decl> texcoords;
  std::vector<Decl> normals;

  // Early vertex that the anchor face F0 must not reference.
  verts.push_back(AddVertexDecl(&builder, &rng));
  // Anchor vertices, generated until the triple is non-degenerate.  The
  // build-up length always matches the number of declarations actually made.
  std::vector<Decl> anchor_candidates = verts;
  while (true) {
    anchor_candidates.push_back(AddVertexDecl(&builder, &rng));
    const size_t n = anchor_candidates.size();
    if (n >= 4 &&
        !IsCollinear(anchor_candidates[n - 3], anchor_candidates[n - 2],
                     anchor_candidates[n - 1])) {
      break;
    }
  }
  verts = anchor_candidates;
  const size_t anchor_base = verts.size() - 3;
  const Decl *anchor_vertices[3] = {&verts[anchor_base],
                                    &verts[anchor_base + 1],
                                    &verts[anchor_base + 2]};
  while (IsCollinear(*anchor_vertices[0], *anchor_vertices[1],
                     *anchor_vertices[2])) {
    verts.push_back(AddVertexDecl(&builder, &rng));
    anchor_vertices[0] = &verts[verts.size() - 3];
    anchor_vertices[1] = &verts[verts.size() - 2];
    anchor_vertices[2] = &verts[verts.size() - 1];
  }
  texcoords.push_back(AddTexcoordDecl(&builder, &rng));
  texcoords.push_back(AddTexcoordDecl(&builder, &rng));
  normals.push_back(AddNormalDecl(&builder, &rng));
  normals.push_back(AddNormalDecl(&builder, &rng));

  // Anchor face F0: all-negative v/vt/vn references, and the very first vertex
  // declaration stays unreferenced by it.
  {
    CornerSpec corners[3];
    for (int k = 0; k < 3; k++) {
      corners[k].vertex_id = anchor_vertices[k]->id;
      corners[k].negative_vertex = true;
      corners[k].texcoord_id = texcoords[static_cast<size_t>(k % 2)].id;
      corners[k].has_texcoord = true;
      corners[k].negative_texcoord = true;
      corners[k].normal_id = normals[static_cast<size_t>(k % 2)].id;
      corners[k].has_normal = true;
      corners[k].negative_normal = true;
    }
    if (!builder.add_face(corners, err)) {
      return false;
    }
  }

  // Group switch after F0 (legal: a face already exists in the previous
  // segment).
  builder.add_group("grp_main");

  const int initial_v = static_cast<int>(verts.size());
  const int initial_vt = static_cast<int>(texcoords.size());
  const int initial_vn = static_cast<int>(normals.size());
  int reserve_after_last_v = static_cast<int>(rng.Uniform(3));  // 0..2
  int reserve_after_last_t = static_cast<int>(rng.Uniform(2));  // 0..1
  int reserve_after_last_n = 1 + static_cast<int>(rng.Uniform(2));  // 1..2

  const char *materials[3] = {"mat_red", "mat_green", "mat_blue"};
  const long smoothing_values[3] = {0, 1, 2};

  int object_switch_ordinal = 0;
  for (int face_index = 1; face_index < face_count; face_index++) {
    // Emit zero or more declarations before this face (interleaving).
    int wanted = 0;
    if (rng.OneIn(2)) {
      wanted = 1;
    } else if (rng.OneIn(4)) {
      wanted = 2;
    }
    for (int i = 0; i < wanted; i++) {
      const size_t choice = rng.Uniform(3);
      if (choice == 0 && static_cast<int>(verts.size()) <
                             v_count - reserve_after_last_v) {
        verts.push_back(AddVertexDecl(&builder, &rng));
      } else if (choice == 1 && static_cast<int>(texcoords.size()) <
                                    vt_count - reserve_after_last_t) {
        texcoords.push_back(AddTexcoordDecl(&builder, &rng));
      } else if (static_cast<int>(normals.size()) <
                 vn_count - reserve_after_last_n) {
        normals.push_back(AddNormalDecl(&builder, &rng));
      }
    }

    if (face_index != 1 && rng.OneIn(3)) {
      object_switch_ordinal++;
      if (rng.Bool()) {
        builder.add_group(UniqueName("grp", object_switch_ordinal));
      } else {
        builder.add_object(UniqueName("obj", object_switch_ordinal));
      }
    }
    if (rng.OneIn(4)) {
      builder.add_material_state(materials[rng.Uniform(3)]);
    }
    if (rng.OneIn(3)) {
      builder.add_smoothing(smoothing_values[rng.Uniform(3)]);
    }

    TokenForm form;
    bool pv = rng.Bool();
    bool pt = rng.Bool();
    bool pn = rng.Bool();
    if (face_index == 1) {
      form = kFormVertexOnly;
      pv = false;
    } else if (face_index == 2) {
      form = kFormVertexNormal;
      pv = true;
      pn = false;
    } else if (face_index == 3) {
      form = kFormVertexTexcoord;
      pv = false;
      pt = true;
    } else if (face_index == 4) {
      form = kFormFull;
      pv = true;
      pt = true;
      pn = true;
    } else {
      form = static_cast<TokenForm>(rng.Uniform(4));
      pv = rng.Bool();
      pt = rng.Bool();
      pn = rng.Bool();
    }
    if (!AddGeneratedFace(&builder, verts, texcoords, normals, &rng, form, pv,
                          pt, pn, err)) {
      return false;
    }
  }

  // Fill up to the exact pool sizes, then append the reserved tail so every
  // pool has declarations after the last face.
  while (static_cast<int>(verts.size()) < v_count - reserve_after_last_v) {
    verts.push_back(AddVertexDecl(&builder, &rng));
  }
  while (static_cast<int>(texcoords.size()) < vt_count - reserve_after_last_t) {
    texcoords.push_back(AddTexcoordDecl(&builder, &rng));
  }
  while (static_cast<int>(normals.size()) < vn_count - reserve_after_last_n) {
    normals.push_back(AddNormalDecl(&builder, &rng));
  }
  for (int i = 0; i < reserve_after_last_v; i++) {
    verts.push_back(AddVertexDecl(&builder, &rng));
  }
  for (int i = 0; i < reserve_after_last_t; i++) {
    texcoords.push_back(AddTexcoordDecl(&builder, &rng));
  }
  for (int i = 0; i < reserve_after_last_n; i++) {
    normals.push_back(AddNormalDecl(&builder, &rng));
  }
  (void)initial_v;
  (void)initial_vt;
  (void)initial_vn;

  *out = builder.release();
  std::string validate_err;
  if (!validate_generation_profile(*out, &validate_err)) {
    if (err) *err = "generated asset violates the profile: " + validate_err;
    return false;
  }
  return true;
}

bool verify_generated_asset(const Asset &asset, std::string *err) {
  return validate_generation_profile(asset, err);
}

bool write_case_files(const Asset &asset, const std::string &dir,
                      std::string *err) {
  if (!ensure_directory(dir)) {
    if (err) *err = "cannot create directory " + dir;
    return false;
  }
  std::string obj_text;
  std::string emit_err;
  if (!emit_obj_text(asset, /*all_positive=*/false, &obj_text, &emit_err)) {
    if (err) *err = emit_err;
    return false;
  }
  if (!write_text_file(DirPath(dir, "case.trace"), serialize_asset(asset)) ||
      !write_text_file(DirPath(dir, "case.obj"), obj_text) ||
      !write_text_file(DirPath(dir, "case.mtl"), material_text(asset))) {
    if (err) *err = "cannot write case files in " + dir;
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Reduction
// ---------------------------------------------------------------------------

namespace {

enum AttemptOutcome {
  kAccepted = 0,
  kRejectedModel = 1,
  kRejectedInvalid = 2
};

struct AttemptResult {
  bool accepted;
  std::string reason;
  std::vector<FaceRecord> candidate_records;

  AttemptResult() : accepted(false) {}
};

bool KeepsAnchor(const Asset &asset, const std::string &anchor) {
  for (size_t i = 0; i < asset.events.size(); i++) {
    if (asset.events[i].kind == kEventFace &&
        asset.events[i].face.id == anchor) {
      return true;
    }
  }
  return false;
}

bool EvaluateCandidate(const Asset &current, size_t event_index,
                       const std::string &anchor, Asset *candidate,
                       AttemptResult *result) {
  *candidate = current;
  candidate->events.clear();
  candidate->events.reserve(current.events.size());
  for (size_t i = 0; i < current.events.size(); i++) {
    if (i == event_index) {
      continue;
    }
    candidate->events.push_back(current.events[i]);
  }
  result->accepted = false;
  if (!anchor.empty() && !KeepsAnchor(*candidate, anchor)) {
    result->reason = "would remove anchor face " + anchor;
    return false;
  }
  if (candidate->events.size() >= current.events.size()) {
    result->reason = "event count does not strictly decrease";
    return false;
  }
  std::string validate_err;
  if (!validate_asset(*candidate, false, &validate_err)) {
    result->reason = validate_err;
    return false;
  }

  // Both serializations must independently reinterpret to the model's values
  // using pool sizes at each face (not the final pool size).
  const std::string mtl = material_text(*candidate);
  const char *entries[2] = {"reduced-candidate/preserve",
                            "reduced-candidate/positive"};
  for (int variant = 0; variant < 2; variant++) {
    std::string obj_text;
    std::string emit_err;
    if (!emit_obj_text(*candidate, variant == 1, &obj_text, &emit_err)) {
      result->reason = "cannot serialize candidate: " + emit_err;
      return false;
    }
    Mismatch mismatch;
    if (!compare_obj_text_to_model(*candidate, obj_text, mtl, entries[variant],
                                   &mismatch)) {
      result->reason = std::string("serialization disagrees with model: ") +
                       describe_mismatch(mismatch);
      return false;
    }
  }

  if (!build_expected_records(*candidate, &result->candidate_records,
                              &validate_err)) {
    result->reason = validate_err;
    return false;
  }
  result->accepted = true;
  return true;
}

bool AttemptIsCandidate(const Event &event, int phase,
                        const std::string &anchor) {
  if (phase == 0) {
    if (event.kind != kEventFace) {
      return false;
    }
    if (!anchor.empty() && event.face.id == anchor) {
      return false;
    }
    return true;
  }
  if (phase == 1) {
    return event.kind == kEventDeclaration;
  }
  return is_state_event_kind(event.kind);
}

}  // namespace

bool reduce_asset(const Asset &input, const std::string &anchor,
                  long long budget, Asset *out, std::string *log_text,
                  std::string *err) {
  if (!out || !log_text) {
    if (err) *err = "null output";
    return false;
  }
  std::string validate_err;
  if (!validate_asset(input, false, &validate_err)) {
    if (err) *err = "input asset is invalid: " + validate_err;
    return false;
  }
  if (!anchor.empty() && !KeepsAnchor(input, anchor)) {
    if (err) *err = "input asset has no anchor face " + anchor;
    return false;
  }
  Asset current = input;
  std::ostringstream log;
  log << "OBJSTATE_REDUCTION 1\n";
  log << "INPUT_EVENTS " << input.events.size() << "\n";
  log << "ANCHOR " << anchor << "\n";
  log << "BUDGET " << budget << "\n";
  long long attempts = 0;
  bool changed_any = false;

  while (true) {
    if (attempts >= budget) {
      break;
    }
    bool accepted_this_round = false;
    for (int phase = 0; phase < 3 && !accepted_this_round; phase++) {
      for (size_t i = 0; i < current.events.size(); i++) {
        if (attempts >= budget) {
          break;
        }
        const Event &event = current.events[i];
        if (!AttemptIsCandidate(event, phase, anchor)) {
          continue;
        }
        attempts++;
        Asset candidate;
        AttemptResult result;
        EvaluateCandidate(current, i, anchor, &candidate, &result);
        log << "ATTEMPT " << attempts << " " << event.id << " "
            << (result.accepted ? "ACCEPT committed" : "REJECT " + result.reason)
            << "\n";
        if (result.accepted) {
          current = candidate;
          changed_any = true;
          accepted_this_round = true;
          break;  // restart from the first category (faces)
        }
      }
    }
    if (!accepted_this_round) {
      break;
    }
    if (attempts >= budget) {
      break;
    }
  }

  log << "ATTEMPTS " << attempts << "\n";
  log << "FINAL_EVENTS " << current.events.size() << "\n";
  log << "CHANGED " << (changed_any ? 1 : 0) << "\n";
  *out = current;
  *log_text = log.str();
  return true;
}

bool obj_state_reduce_dir(const std::string &in_dir, const std::string &out_dir,
                          const std::string &anchor, long long budget,
                          std::string *message) {
  Asset input;
  std::string err;
  std::string trace_text;
  if (!read_text_file(DirPath(in_dir, "case.trace"), &trace_text)) {
    if (message) *message = "cannot read " + DirPath(in_dir, "case.trace");
    return false;
  }
  if (!parse_asset(trace_text, &input, &err)) {
    if (message) *message = "cannot parse trace: " + err;
    return false;
  }
  Asset reduced;
  std::string reduction_log;
  if (!reduce_asset(input, anchor, budget, &reduced, &reduction_log, &err)) {
    if (message) *message = err;
    return false;
  }
  if (!ensure_directory(out_dir)) {
    if (message) *message = "cannot create " + out_dir;
    return false;
  }
  bool changed = false;
  for (size_t i = 0; i < reduced.events.size(); i++) {
    if (i >= input.events.size() ||
        reduced.events[i].id != input.events[i].id ||
        reduced.events[i].kind != input.events[i].kind) {
      changed = true;
      break;
    }
  }
  if (reduced.events.size() != input.events.size()) {
    changed = true;
  }

  if (!changed) {
    // No candidate was committed: return the legal input asset as-is.
    bool copied = true;
    copied = copied &&
             copy_file_bytes(DirPath(in_dir, "case.obj"),
                             DirPath(out_dir, "case.obj"));
    copied = copied &&
             copy_file_bytes(DirPath(in_dir, "case.mtl"),
                             DirPath(out_dir, "case.mtl"));
    copied = copied &&
             copy_file_bytes(DirPath(in_dir, "case.trace"),
                             DirPath(out_dir, "case.trace"));
    if (!copied) {
      if (!write_case_files(input, out_dir, &err)) {
        if (message) *message = "cannot write unchanged output: " + err;
        return false;
      }
    }
  } else if (!write_case_files(reduced, out_dir, &err)) {
    if (message) *message = "cannot write reduced output: " + err;
    return false;
  }

  if (!write_text_file(DirPath(out_dir, "reduction.log"), reduction_log)) {
    if (message) *message = "cannot write reduction.log";
    return false;
  }
  std::ostringstream summary;
  summary << "OBJ_STATE_REDUCED events=" << reduced.events.size()
          << " input_events=" << input.events.size()
          << " changed=" << (changed ? 1 : 0);
  if (message) *message = summary.str();
  return true;
}

}  // namespace objstate
