#include "task_support/check.h"

#include "tests/obj-fuzz/state_model.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

namespace {

using objstate::Asset;
using objstate::AssetBuilder;
using objstate::CornerSpec;
using objstate::Decl;
using objstate::FaceRecord;
using objstate::Mismatch;
using objstate::WorkerEntry;

// ---------------------------------------------------------------------------
// Small utilities
// ---------------------------------------------------------------------------

std::string TempDirBase() {
  char tmpl[512];
  const char *base = std::getenv("TMPDIR");
  if (!base || base[0] == '\0') {
    base = ".";
  }
  std::snprintf(tmpl, sizeof(tmpl), "%s/obj_state_XXXXXX", base);
  char *made = ::mkdtemp(tmpl);
  if (!made) {
    return std::string();
  }
  return std::string(made);
}

std::string Path(const std::string &dir, const std::string &name) {
  return dir + "/" + name;
}

std::string ReadAll(const std::string &path) {
  std::string text;
  if (!objstate::read_text_file(path, &text)) {
    return std::string();
  }
  return text;
}

bool WriteAll(const std::string &path, const std::string &text) {
  return objstate::write_text_file(path, text);
}

std::string WorkerProgram() {
  const char *override = std::getenv("OBJ_STATE_WORKER");
  if (override && override[0] != '\0') {
    return std::string(override);
  }
  return ".task-build/obj-state";
}

bool WriteScript(const std::string &path, const std::string &body) {
  if (!WriteAll(path, body)) {
    return false;
  }
  return ::chmod(path.c_str(), 0755) == 0;
}

// ---------------------------------------------------------------------------
// Sample assets used by several cases
// ---------------------------------------------------------------------------

struct SampleCorner {
  int v = 0;
  int vt = 0;
  int vn = 0;
  bool has_vt = false;
  bool has_vn = false;
  bool neg_v = false;
  bool neg_vt = false;
  bool neg_vn = false;
};

bool AddSampleFace(AssetBuilder *builder, const SampleCorner corners[3]) {
  CornerSpec spec[3];
  for (int k = 0; k < 3; k++) {
    spec[k].vertex_id = corners[k].v;
    spec[k].texcoord_id = corners[k].vt;
    spec[k].normal_id = corners[k].vn;
    spec[k].has_texcoord = corners[k].has_vt;
    spec[k].has_normal = corners[k].has_vn;
    spec[k].negative_vertex = corners[k].neg_v;
    spec[k].negative_texcoord = corners[k].neg_vt;
    spec[k].negative_normal = corners[k].neg_vn;
  }
  std::string err;
  return builder->add_face(spec, &err);
}

// A hand-built asset that exercises all four token forms, an anchor face with
// all-negative references, an unreferenced early declaration, and declarations
// after the anchor.
bool BuildMixedAsset(Asset *out) {
  AssetBuilder builder(2024);
  builder.add_material("mat_red", 1, 0, 0);
  builder.add_material("mat_blue", 0, 0, 1);
  builder.add_object("obj_main");
  builder.add_material_state("mat_red");
  builder.add_smoothing(1);

  Decl v0 = builder.add_vertex(0, 0, 0);     // unused by F0
  Decl v1 = builder.add_vertex(10, 0, 0);
  Decl v2 = builder.add_vertex(0, 10, 0);
  Decl v3 = builder.add_vertex(0, 0, 10);
  builder.add_texcoord(0, 0);
  builder.add_texcoord(5, 5);
  builder.add_normal(0, 0, 1);
  builder.add_normal(0, 1, 0);

  SampleCorner anchor[3];
  for (int k = 0; k < 3; k++) {
    anchor[k].v = (k == 0 ? v1 : (k == 1 ? v2 : v3)).id;
    anchor[k].vt = k % 2 == 0 ? 1 : 2;
    anchor[k].vn = k % 2 == 0 ? 1 : 2;
    anchor[k].has_vt = true;
    anchor[k].has_vn = true;
    anchor[k].neg_v = true;
    anchor[k].neg_vt = true;
    anchor[k].neg_vn = true;
  }
  if (!AddSampleFace(&builder, anchor)) {
    return false;
  }
  builder.add_group("grp_second");

  Decl v4 = builder.add_vertex(3, 0, 0);
  SampleCorner only_vertex[3];
  only_vertex[0].v = v1.id;
  only_vertex[0].vt = 0;
  only_vertex[0].vn = 0;
  only_vertex[0].has_vt = false;
  only_vertex[0].has_vn = false;
  only_vertex[0].neg_v = false;
  only_vertex[1].v = v2.id;
  only_vertex[1].vt = 0;
  only_vertex[1].vn = 0;
  only_vertex[1].has_vt = false;
  only_vertex[1].has_vn = false;
  only_vertex[1].neg_v = true;
  only_vertex[2].v = v4.id;
  only_vertex[2].vt = 0;
  only_vertex[2].vn = 0;
  only_vertex[2].has_vt = false;
  only_vertex[2].has_vn = false;
  only_vertex[2].neg_v = false;
  if (!AddSampleFace(&builder, only_vertex)) {
    return false;
  }
  SampleCorner only_normal[3];
  for (int k = 0; k < 3; k++) {
    only_normal[k].v = (k == 0 ? v1 : (k == 1 ? v3 : v4)).id;
    only_normal[k].vt = 0;
    only_normal[k].vn = (k % 2 == 0 ? 1 : 2);
    only_normal[k].has_vt = false;
    only_normal[k].has_vn = true;
    only_normal[k].neg_v = true;
    only_normal[k].neg_vt = false;
    only_normal[k].neg_vn = true;
  }
  if (!AddSampleFace(&builder, only_normal)) {
    return false;
  }
  builder.add_material_state("mat_blue");
  builder.add_smoothing(0);
  SampleCorner full[3];
  for (int k = 0; k < 3; k++) {
    full[k].v = (k == 0 ? v1 : (k == 1 ? v2 : v3)).id;
    full[k].vt = k % 2 == 0 ? 2 : 1;
    full[k].vn = k % 2 == 0 ? 2 : 1;
    full[k].has_vt = true;
    full[k].has_vn = true;
    full[k].neg_v = false;
    full[k].neg_vt = false;
    full[k].neg_vn = true;
  }
  if (!AddSampleFace(&builder, full)) {
    return false;
  }
  builder.add_material_state("mat_red");
  builder.add_smoothing(2);
  SampleCorner textured[3];
  for (int k = 0; k < 3; k++) {
    textured[k].v = (k == 0 ? v1 : (k == 1 ? v2 : v3)).id;
    textured[k].vt = (k % 2 == 0) ? 1 : 2;
    textured[k].vn = 0;
    textured[k].has_vt = true;
    textured[k].has_vn = false;
    textured[k].neg_v = false;
    textured[k].neg_vt = false;
    textured[k].neg_vn = false;
  }
  if (!AddSampleFace(&builder, textured)) {
    return false;
  }
  // Declarations after the last face.
  builder.add_vertex(7, 7, 7);
  builder.add_texcoord(9, 9);
  builder.add_normal(0, 0, 2);
  (void)v0;

  *out = builder.release();
  std::string err;
  return objstate::validate_asset(*out, false, &err);
}

// Emits OBJ text for a copy of `asset` whose first face's corner 0 references
// a different (still available) vertex declaration.  The text stays legal OBJ,
// so both production entries parse it identically; only the trace model can
// notice that the identity changed.
bool EmitWithShiftedCorner(const Asset &asset, std::string *obj,
                           std::string *face_id, std::string *err) {
  Asset mutated = asset;
  bool changed = false;
  for (size_t i = 0; i < mutated.events.size() && !changed; i++) {
    if (mutated.events[i].kind != objstate::kEventFace) {
      continue;
    }
    objstate::Face &face = mutated.events[i].face;
    // Collect vertex declarations available before this face.
    std::vector<int> available;
    for (size_t j = 0; j < i; j++) {
      if (mutated.events[j].kind == objstate::kEventDeclaration &&
          mutated.events[j].decl.kind == objstate::kDeclVertex) {
        available.push_back(mutated.events[j].decl.id);
      }
    }
    for (size_t k = 0; k < available.size(); k++) {
      if (available[k] != face.corners[0].vertex_id) {
        face.corners[0].vertex_id = available[k];
        changed = true;
        break;
      }
    }
    if (changed) {
      *face_id = face.id;
    }
  }
  if (!changed) {
    if (err) *err = "no alternate vertex declaration";
    return false;
  }
  return objstate::emit_obj_text(mutated, false, obj, err);
}

bool PersistAsset(const Asset &asset, const std::string &dir, std::string *obj,
                  std::string *mtl) {
  std::string err;
  if (!objstate::write_case_files(asset, dir, &err)) {
    return false;
  }
  *obj = ReadAll(Path(dir, "case.obj"));
  *mtl = ReadAll(Path(dir, "case.mtl"));
  return !obj->empty() && !mtl->empty();
}

// ---------------------------------------------------------------------------
// declaration group
// ---------------------------------------------------------------------------

int CaseGenerateProfile() {
  for (uint64_t seed = 1; seed <= 8; seed++) {
    Asset first;
    Asset second;
    std::string err;
    TASK_CHECK(objstate::generate_asset(seed, &first, &err));
    TASK_CHECK(objstate::generate_asset(seed, &second, &err));
    TASK_CHECK(objstate::serialize_asset(first) == objstate::serialize_asset(second));
    TASK_CHECK(objstate::verify_generated_asset(first, &err));

    // F0 exists with all three negative components and an early declaration it
    // does not reference.
    bool found_anchor = false;
    bool early_unreferenced = false;
    bool decl_after_anchor = false;
    int anchor_pos = -1;
    for (size_t i = 0; i < first.events.size(); i++) {
      if (first.events[i].kind == objstate::kEventFace &&
          first.events[i].face.id == "F0") {
        found_anchor = true;
        anchor_pos = static_cast<int>(i);
        for (int k = 0; k < 3; k++) {
          TASK_CHECK(first.events[i].face.corners[k].negative_vertex);
          TASK_CHECK(first.events[i].face.corners[k].negative_texcoord);
          TASK_CHECK(first.events[i].face.corners[k].negative_normal);
          TASK_CHECK(first.events[i].face.corners[k].has_texcoord);
          TASK_CHECK(first.events[i].face.corners[k].has_normal);
        }
      }
    }
    TASK_CHECK(found_anchor);
    TASK_CHECK(anchor_pos >= 0);
    for (size_t i = 0; i < static_cast<size_t>(anchor_pos); i++) {
      if (first.events[i].kind != objstate::kEventDeclaration) {
        continue;
      }
      bool referenced = false;
      for (size_t f = 0; f < first.events.size(); f++) {
        if (first.events[f].kind != objstate::kEventFace ||
            first.events[f].face.id != "F0") {
          continue;
        }
        for (int k = 0; k < 3; k++) {
          const objstate::Corner &corner = first.events[f].face.corners[k];
          if (first.events[i].decl.kind == objstate::kDeclVertex &&
              corner.vertex_id == first.events[i].decl.id) {
            referenced = true;
          }
          if (first.events[i].decl.kind == objstate::kDeclTexcoord &&
              corner.has_texcoord &&
              corner.texcoord_id == first.events[i].decl.id) {
            referenced = true;
          }
          if (first.events[i].decl.kind == objstate::kDeclNormal &&
              corner.has_normal &&
              corner.normal_id == first.events[i].decl.id) {
            referenced = true;
          }
        }
      }
      if (!referenced) {
        early_unreferenced = true;
      }
    }
    TASK_CHECK(early_unreferenced);
    for (size_t i = static_cast<size_t>(anchor_pos) + 1; i < first.events.size();
         i++) {
      if (first.events[i].kind == objstate::kEventDeclaration) {
        decl_after_anchor = true;
      }
    }
    TASK_CHECK(decl_after_anchor);

    // Independent pool sizes.
    int sizes[3] = {0, 0, 0};
    for (size_t i = 0; i < first.events.size(); i++) {
      if (first.events[i].kind == objstate::kEventDeclaration) {
        sizes[first.events[i].decl.kind]++;
      }
    }
    TASK_CHECK(sizes[0] != sizes[1] && sizes[0] != sizes[2] &&
               sizes[1] != sizes[2]);

    // Interleaving: at least one declaration occurs after the second face.
    int faces_seen = 0;
    bool interleaved = false;
    for (size_t i = 0; i < first.events.size(); i++) {
      if (first.events[i].kind == objstate::kEventFace) {
        faces_seen++;
      } else if (first.events[i].kind == objstate::kEventDeclaration &&
                 faces_seen >= 2) {
        interleaved = true;
      }
    }
    TASK_CHECK(interleaved);
    TASK_CHECK(sizes[0] <= 64 && sizes[1] <= 64 && sizes[2] <= 64);
    int face_total = 0;
    for (size_t i = 0; i < first.events.size(); i++) {
      if (first.events[i].kind == objstate::kEventFace) {
        face_total++;
      }
    }
    TASK_CHECK(face_total <= 64);

    // Every o/g switch either opens the initial segment or starts a segment
    // that contains at least one face.  A switch that is immediately followed
    // by another switch (or ends the file) is a dead segment.
    bool seen_switch = false;
    int faces_since_switch = 0;
    bool every_switch_usable = true;
    for (size_t i = 0; i < first.events.size(); i++) {
      const objstate::Event &event = first.events[i];
      if (event.kind == objstate::kEventFace) {
        faces_since_switch++;
        continue;
      }
      if (event.kind == objstate::kEventObject ||
          event.kind == objstate::kEventGroup) {
        if (seen_switch && faces_since_switch <= 0) {
          every_switch_usable = false;
        }
        seen_switch = true;
        faces_since_switch = 0;
      }
    }
    TASK_CHECK(every_switch_usable);
    TASK_CHECK(faces_since_switch > 0);

    // Coordinates stay in a finite integer magnitude range.
    for (size_t i = 0; i < first.events.size(); i++) {
      if (first.events[i].kind != objstate::kEventDeclaration) {
        continue;
      }
      for (int c = 0; c < 3; c++) {
        TASK_CHECK(first.events[i].decl.coordinates[c] >= -10 &&
                   first.events[i].decl.coordinates[c] <= 10);
      }
    }
  }
  return 0;
}

int CaseTraceRoundTrip() {
  Asset asset;
  TASK_CHECK(BuildMixedAsset(&asset));
  const std::string text = objstate::serialize_asset(asset);
  Asset parsed;
  std::string err;
  TASK_CHECK(objstate::parse_asset(text, &parsed, &err));
  TASK_CHECK(objstate::serialize_asset(parsed) == text);
  TASK_CHECK(objstate::validate_asset(parsed, false, &err));

  // The trace records declaration events (not the final pool), with stable
  // declaration ids and per-corner identity.
  TASK_CHECK(text.find("D V 1 ") != std::string::npos);
  TASK_CHECK(text.find("F F0 ") != std::string::npos);
  TASK_CHECK(text.find("V2-/T1-/N1-") != std::string::npos);
  TASK_CHECK(text.find("V2-//N1-") != std::string::npos);

  // Names are unique and reachable from faces.
  std::vector<FaceRecord> records;
  TASK_CHECK(objstate::build_expected_records(parsed, &records, &err));
  TASK_CHECK(records.size() == 5);
  TASK_CHECK(records[0].shape_name == "obj_main");
  TASK_CHECK(records[1].shape_name == "grp_second");
  TASK_CHECK(records[4].material_name == "mat_red");
  TASK_CHECK(records[4].smoothing == 2);
  return 0;
}

int CaseStateSnapshot() {
  Asset asset;
  TASK_CHECK(BuildMixedAsset(&asset));
  // Every face's recorded state must equal the state derived from its prefix,
  // and no face may silently inherit a switch that came later.
  std::string err;
  TASK_CHECK(objstate::validate_asset(asset, false, &err));
  Asset mutated = asset;
  for (size_t i = 0; i < mutated.events.size(); i++) {
    if (mutated.events[i].kind == objstate::kEventFace) {
      mutated.events[i].face.material_name = "mat_blue";
      break;
    }
  }
  TASK_CHECK(!objstate::validate_asset(mutated, false, &err));
  return 0;
}

// ---------------------------------------------------------------------------
// oracle group
// ---------------------------------------------------------------------------

int CaseOracleMatchesCaseObj() {
  std::string dir = TempDirBase();
  TASK_CHECK(!dir.empty());
  Asset asset;
  TASK_CHECK(BuildMixedAsset(&asset));
  std::string obj;
  std::string mtl;
  TASK_CHECK(PersistAsset(asset, dir, &obj, &mtl));
  Mismatch mismatch;
  TASK_CHECK(objstate::compare_obj_text_to_model(asset, obj, mtl,
                                                 "case-obj", &mismatch));
  // Every corner's parsed values equal the declaration values.
  std::vector<FaceRecord> expected;
  std::vector<FaceRecord> actual;
  std::string err;
  TASK_CHECK(objstate::build_expected_records(asset, &expected, &err));
  TASK_CHECK(objstate::interpret_obj_text(obj, mtl, &actual, &err));
  TASK_CHECK(expected.size() == actual.size());
  for (size_t i = 0; i < expected.size(); i++) {
    for (int k = 0; k < 3; k++) {
      TASK_CHECK(expected[i].corners[k].has_texcoord ==
                 actual[i].corners[k].has_texcoord);
      TASK_CHECK(expected[i].corners[k].has_normal ==
                 actual[i].corners[k].has_normal);
      for (int c = 0; c < 3; c++) {
        TASK_CHECK(expected[i].corners[k].vertex[c] ==
                   actual[i].corners[k].vertex[c]);
      }
    }
  }
  // A wrong corner component is diagnosed with stable face id, corner,
  // component and entry.
  std::string tampered;
  std::string face_id;
  std::string shift_err;
  TASK_CHECK(EmitWithShiftedCorner(asset, &tampered, &face_id, &shift_err));
  TASK_CHECK(!objstate::compare_obj_text_to_model(asset, tampered, mtl,
                                                  "ObjReader/preserve",
                                                  &mismatch));
  TASK_CHECK(mismatch.face_id == face_id);
  TASK_CHECK(mismatch.corner == 0);
  TASK_CHECK(mismatch.component == "vertex");
  TASK_CHECK(mismatch.entry == "ObjReader/preserve");
  return 0;
}

int CaseEarlyVersusFinalPool() {
  // The face's negative token must be resolved against the pool that existed
  // when the face was declared, not the final pool.
  const std::string obj =
      "mtllib case.mtl\n"
      "o obj\n"
      "usemtl m\n"
      "s 1\n"
      "v 1 0 0\n"
      "v 2 0 0\n"
      "v 3 0 0\n"
      "vt 0 0\n"
      "vn 0 0 1\n"
      "f -3/-1/-1 -2/-1/-1 -1/-1/-1\n"
      "v 4 0 0\n"
      "v 5 0 0\n";
  std::vector<FaceRecord> records;
  std::string err;
  TASK_CHECK(objstate::interpret_obj_text(obj, "newmtl m\nKd 1 0 0\n", &records,
                                          &err));
  TASK_CHECK(records.size() == 1);
  TASK_CHECK(records[0].corners[0].vertex[0] == 1);
  // A final-pool interpretation would wrongly resolve -3 to the third of five
  // vertices (x == 3).  Recomputing negatives from the final totals is thus
  // observably different.
  TASK_CHECK(records[0].corners[0].vertex[0] != 3);
  TASK_CHECK(records[0].corners[2].vertex[0] == 3);

  // A model whose face expects the final-pool identity must disagree with the
  // correctly-read early identity.
  AssetBuilder builder(77);
  builder.add_material("m", 1, 0, 0);
  builder.add_object("obj");
  builder.add_material_state("m");
  builder.add_smoothing(1);
  Decl v1 = builder.add_vertex(1, 0, 0);
  Decl v2 = builder.add_vertex(2, 0, 0);
  Decl v3 = builder.add_vertex(3, 0, 0);
  builder.add_vertex(4, 0, 0);
  builder.add_vertex(5, 0, 0);
  builder.add_texcoord(0, 0);
  builder.add_normal(0, 0, 1);
  CornerSpec corners[3];
  corners[0].vertex_id = v1.id;
  corners[1].vertex_id = v2.id;
  corners[2].vertex_id = v3.id;
  corners[0].negative_vertex = true;
  corners[1].negative_vertex = true;
  corners[2].negative_vertex = true;
  corners[0].texcoord_id = 1;
  corners[0].has_texcoord = true;
  corners[0].normal_id = 1;
  corners[0].has_normal = true;
  for (int k = 1; k < 3; k++) {
    corners[k].texcoord_id = 1;
    corners[k].has_texcoord = true;
    corners[k].normal_id = 1;
    corners[k].has_normal = true;
    corners[k].negative_texcoord = true;
    corners[k].negative_normal = true;
  }
  // The text order is x=1,2,3 for the first face; make the model's corner 0
  // the final-pool identity instead and verify the disagreement is caught.
  corners[0].vertex_id = v3.id;
  std::string face_err;
  TASK_CHECK(builder.add_face(corners, &face_err));
  Asset model = builder.release();
  // Corner 0 references the third declaration while the text's -3 points at
  // the first; the model must disagree for a different reason.
  Mismatch identity;
  TASK_CHECK(!objstate::compare_obj_text_to_model(
      model, obj, objstate::material_text(model), "identity", &identity));

  // Now build the matching model and verify the same text is accepted.
  AssetBuilder matching(77);
  matching.add_material("m", 1, 0, 0);
  matching.add_object("obj");
  matching.add_material_state("m");
  matching.add_smoothing(1);
  matching.add_vertex(1, 0, 0);
  matching.add_vertex(2, 0, 0);
  matching.add_vertex(3, 0, 0);
  matching.add_vertex(4, 0, 0);
  matching.add_vertex(5, 0, 0);
  matching.add_texcoord(0, 0);
  matching.add_normal(0, 0, 1);
  CornerSpec good[3];
  for (int k = 0; k < 3; k++) {
    good[k].vertex_id = k + 1;
    good[k].texcoord_id = 1;
    good[k].has_texcoord = true;
    good[k].normal_id = 1;
    good[k].has_normal = true;
    good[k].negative_vertex = true;
    good[k].negative_texcoord = true;
    good[k].negative_normal = true;
  }
  TASK_CHECK(matching.add_face(good, &face_err));
  Asset matching_model = matching.release();
  Mismatch accepted;
  TASK_CHECK(objstate::compare_obj_text_to_model(
      matching_model, obj, objstate::material_text(matching_model), "matching",
      &accepted));

  // If check used final-pool arithmetic, the third corner would resolve to the
  // fifth declaration instead of the third.
  std::string wrong_pool = obj;
  wrong_pool.replace(wrong_pool.find("f -3/-1/-1"), std::strlen("f -3/-1/-1"),
                     "f -5/-1/-1");
  Mismatch bad;
  TASK_CHECK(!objstate::compare_obj_text_to_model(matching_model, wrong_pool,
                                                  objstate::material_text(
                                                      matching_model),
                                                  "final-pool", &bad));
  return 0;
}

int CaseMissingComponentsAreMinusOne() {
  // Build a worker-like record with 0 for a missing texcoord; must be rejected.
  FaceRecord expected;
  expected.face_id = "F0";
  expected.shape_name = "obj";
  expected.material_name = "m";
  expected.smoothing = 1;
  for (int k = 0; k < 3; k++) {
    expected.corners[k].has_texcoord = false;
    expected.corners[k].has_normal = false;
    expected.corners[k].vertex_index = k;
    expected.corners[k].texcoord_index = -1;
    expected.corners[k].normal_index = -1;
  }
  FaceRecord actual = expected;
  actual.corners[1].texcoord_index = 0;
  Mismatch mismatch;
  TASK_CHECK(!objstate::compare_production_records(
      std::vector<FaceRecord>(1, expected), std::vector<FaceRecord>(1, actual),
      "LoadObj/preserve", &mismatch));
  TASK_CHECK(mismatch.component == "texcoord-index");
  TASK_CHECK(mismatch.corner == 1);

  actual = expected;
  actual.corners[2].normal_index = 2;
  TASK_CHECK(!objstate::compare_production_records(
      std::vector<FaceRecord>(1, expected), std::vector<FaceRecord>(1, actual),
      "ObjReader/preserve", &mismatch));
  TASK_CHECK(mismatch.component == "normal-index");
  TASK_CHECK(mismatch.corner == 2);

  actual = expected;
  actual.corners[0].texcoord_index = -1;
  actual.corners[0].normal_index = -1;
  actual.corners[1].texcoord_index = -1;
  actual.corners[1].normal_index = -1;
  actual.corners[2].texcoord_index = -1;
  actual.corners[2].normal_index = -1;
  TASK_CHECK(objstate::compare_production_records(
      std::vector<FaceRecord>(1, expected), std::vector<FaceRecord>(1, actual),
      "ObjReader/preserve", &mismatch));
  return 0;
}

int CaseTwoEntryPointsAreNotEnough() {
  // Both production entries parse this text identically, yet the trace says
  // something else.  Comparing only the two entries would pass; the model
  // reference must reject it.
  std::string dir = TempDirBase();
  TASK_CHECK(!dir.empty());
  Asset asset;
  TASK_CHECK(BuildMixedAsset(&asset));
  std::string obj;
  std::string mtl;
  TASK_CHECK(PersistAsset(asset, dir, &obj, &mtl));
  std::string tampered;
  std::string face_id;
  std::string shift_err;
  TASK_CHECK(EmitWithShiftedCorner(asset, &tampered, &face_id, &shift_err));

  std::vector<FaceRecord> reader_faces;
  std::vector<FaceRecord> load_faces;
  std::string err;
  TASK_CHECK(objstate::parse_production_text(tampered, mtl, true, &reader_faces,
                                             &err));
  TASK_CHECK(objstate::parse_production_text(tampered, mtl, false, &load_faces,
                                             &err));
  Mismatch between_entries;
  TASK_CHECK(objstate::compare_record_values(reader_faces, load_faces,
                                             "entries-only", &between_entries));
  std::vector<FaceRecord> expected;
  TASK_CHECK(objstate::build_expected_records(asset, &expected, &err));
  Mismatch versus_model;
  TASK_CHECK(!objstate::compare_record_values(expected, reader_faces,
                                              "model", &versus_model));
  TASK_CHECK(versus_model.face_id == face_id);
  TASK_CHECK(versus_model.corner == 0);
  return 0;
}

// ---------------------------------------------------------------------------
// reduce group
// ---------------------------------------------------------------------------

int CaseReduceBasic() {
  std::string dir = TempDirBase();
  TASK_CHECK(!dir.empty());
  Asset input;
  std::string err;
  TASK_CHECK(objstate::generate_asset(424242, &input, &err));
  TASK_CHECK(objstate::verify_generated_asset(input, &err));
  Asset reduced;
  std::string log;
  TASK_CHECK(objstate::reduce_asset(input, "F0", 200, &reduced, &log, &err));
  TASK_CHECK(objstate::validate_asset(reduced, false, &err));
  TASK_CHECK(reduced.events.size() < input.events.size());
  TASK_CHECK(log.find("OBJSTATE_REDUCTION 1") != std::string::npos);
  TASK_CHECK(log.find("ATTEMPT 1 ") != std::string::npos);

  // Anchor identity and negative style are preserved.
  std::vector<FaceRecord> input_records;
  std::vector<FaceRecord> reduced_records;
  TASK_CHECK(objstate::build_expected_records(input, &input_records, &err));
  TASK_CHECK(objstate::build_expected_records(reduced, &reduced_records, &err));
  bool has_anchor = false;
  for (size_t i = 0; i < reduced_records.size(); i++) {
    if (reduced_records[i].face_id == "F0") {
      has_anchor = true;
      // Same values and presence as the input anchor.
      const FaceRecord *anchor = NULL;
      for (size_t j = 0; j < input_records.size(); j++) {
        if (input_records[j].face_id == "F0") {
          anchor = &input_records[j];
        }
      }
      TASK_CHECK(anchor != NULL);
      for (int k = 0; k < 3; k++) {
        TASK_CHECK(anchor->corners[k].has_texcoord);
        TASK_CHECK(anchor->corners[k].has_normal);
        for (int c = 0; c < 3; c++) {
          TASK_CHECK(anchor->corners[k].vertex[c] ==
                     reduced_records[i].corners[k].vertex[c]);
        }
      }
      bool all_negative = true;
      for (size_t e = 0; e < reduced.events.size(); e++) {
        if (reduced.events[e].kind == objstate::kEventFace &&
            reduced.events[e].face.id == "F0") {
          for (int k = 0; k < 3; k++) {
            const objstate::Corner &corner = reduced.events[e].face.corners[k];
            if (!corner.negative_vertex || !corner.negative_texcoord ||
                !corner.negative_normal) {
              all_negative = false;
            }
          }
        }
      }
      TASK_CHECK(all_negative);
    }
  }
  TASK_CHECK(has_anchor);

  // Every surviving face keeps its values and state.
  for (size_t i = 0; i < reduced_records.size(); i++) {
    const FaceRecord &r = reduced_records[i];
    bool found = false;
    for (size_t j = 0; j < input_records.size(); j++) {
      const FaceRecord &e = input_records[j];
      if (e.face_id != r.face_id) {
        continue;
      }
      found = true;
      TASK_CHECK(e.shape_name == r.shape_name);
      TASK_CHECK(e.material_name == r.material_name);
      TASK_CHECK(e.smoothing == r.smoothing);
      for (int k = 0; k < 3; k++) {
        TASK_CHECK(e.corners[k].has_texcoord == r.corners[k].has_texcoord);
        TASK_CHECK(e.corners[k].has_normal == r.corners[k].has_normal);
        for (int c = 0; c < 3; c++) {
          TASK_CHECK(e.corners[k].vertex[c] == r.corners[k].vertex[c]);
        }
      }
    }
    TASK_CHECK(found);
  }

  // Both serializations of the reduced asset match the model.
  std::string preserve;
  std::string positive;
  TASK_CHECK(objstate::emit_obj_text(reduced, false, &preserve, &err));
  TASK_CHECK(objstate::emit_obj_text(reduced, true, &positive, &err));
  Mismatch reduced_mismatch;
  TASK_CHECK(objstate::compare_obj_text_to_model(
      reduced, preserve, objstate::material_text(reduced), "reduced/preserve",
      &reduced_mismatch));
  TASK_CHECK(objstate::compare_obj_text_to_model(
      reduced, positive, objstate::material_text(reduced), "reduced/positive",
      &reduced_mismatch));

  // Reduction output written to disk is readable and valid.
  std::string out_dir = Path(dir, "reduced");
  std::string message;
  TASK_CHECK(objstate::write_case_files(reduced, out_dir, &err));
  TASK_CHECK(objstate::obj_state_check_dir(out_dir, "", false, &message));
  TASK_CHECK(message.find("OBJ_STATE_VALID") != std::string::npos);
  return 0;
}

int CaseReduceMiddleDeclaration() {
  // Deleting an unreferenced middle declaration shifts negative tokens but
  // must not change identity.
  AssetBuilder builder(9090);
  builder.add_material("m", 1, 0, 0);
  builder.add_object("obj");
  builder.add_material_state("m");
  Decl v1 = builder.add_vertex(0, 0, 0);
  Decl v2 = builder.add_vertex(10, 0, 0);
  Decl unused = builder.add_vertex(5, 5, 5);
  Decl v3 = builder.add_vertex(0, 10, 0);
  CornerSpec corners[3];
  corners[0].vertex_id = v1.id;
  corners[1].vertex_id = v2.id;
  corners[2].vertex_id = v3.id;
  for (int k = 0; k < 3; k++) {
    corners[k].negative_vertex = true;
  }
  std::string err;
  TASK_CHECK(builder.add_face(corners, &err));
  Asset input = builder.release();
  TASK_CHECK(objstate::validate_asset(input, false, &err));

  Asset reduced;
  std::string log;
  TASK_CHECK(objstate::reduce_asset(input, "F0", 100, &reduced, &log, &err));
  TASK_CHECK(reduced.events.size() < input.events.size());
  // The unused declaration is gone, the referenced ones stay.
  bool unused_gone = true;
  bool v1_kept = false;
  for (size_t i = 0; i < reduced.events.size(); i++) {
    if (reduced.events[i].kind != objstate::kEventDeclaration) {
      continue;
    }
    if (reduced.events[i].decl.kind != objstate::kDeclVertex) {
      continue;
    }
    if (reduced.events[i].decl.id == unused.id) {
      unused_gone = false;
    }
    if (reduced.events[i].decl.id == v1.id) {
      v1_kept = true;
    }
  }
  TASK_CHECK(unused_gone);
  TASK_CHECK(v1_kept);

  // Negative tokens were re-encoded: the old numbers would point elsewhere now.
  std::string before;
  std::string after;
  TASK_CHECK(objstate::emit_obj_text(input, false, &before, &err));
  TASK_CHECK(objstate::emit_obj_text(reduced, false, &after, &err));
  std::vector<FaceRecord> before_records;
  std::vector<FaceRecord> after_records;
  TASK_CHECK(objstate::interpret_obj_text(before, objstate::material_text(input),
                                          &before_records, &err));
  TASK_CHECK(objstate::interpret_obj_text(after, objstate::material_text(reduced),
                                          &after_records, &err));
  TASK_CHECK(before_records[0].corners[0].vertex[0] ==
             after_records[0].corners[0].vertex[0]);
  TASK_CHECK(before_records[0].corners[2].vertex[1] ==
             after_records[0].corners[2].vertex[1]);
  TASK_CHECK(before != after);  // token numbers did change

  // The "delete the line but keep the old numbers" error.  Between the face
  // and its referenced vertices sits an unused declaration:
  //   at the face the pool is [R1, U, R2, R3], so R1 is -4, R2 -2, R3 -1.
  // Deleting U keeps R1..R3's identities but re-encodes R1 as -3.  Reusing the
  // old "-4" no longer resolves to R1 (it falls off the front of the pool), so
  // such text is not a legal result even though the identities were meant to
  // stay the same.
  AssetBuilder stale_builder(4243);
  stale_builder.add_material("m", 1, 0, 0);
  stale_builder.add_object("obj");
  stale_builder.add_material_state("m");
  Decl r1 = stale_builder.add_vertex(10, 0, 0);
  Decl unused_middle = stale_builder.add_vertex(6, 6, 6);
  Decl r2 = stale_builder.add_vertex(0, 10, 0);
  Decl r3 = stale_builder.add_vertex(0, 0, 10);
  CornerSpec stale_corners[3];
  stale_corners[0].vertex_id = r1.id;
  stale_corners[1].vertex_id = r2.id;
  stale_corners[2].vertex_id = r3.id;
  for (int k = 0; k < 3; k++) {
    stale_corners[k].negative_vertex = true;
  }
  std::string stale_face_err;
  TASK_CHECK(stale_builder.add_face(stale_corners, &stale_face_err));
  Asset stale_input = stale_builder.release();
  TASK_CHECK(objstate::validate_asset(stale_input, false, &err));

  std::string stale_original;
  TASK_CHECK(objstate::emit_obj_text(stale_input, false, &stale_original, &err));
  TASK_CHECK(stale_original.find("f -4 -2 -1") != std::string::npos);

  Asset stale_reduced;
  std::string stale_log;
  TASK_CHECK(objstate::reduce_asset(stale_input, "", 100, &stale_reduced,
                                    &stale_log, &err));
  TASK_CHECK(stale_reduced.events.size() < stale_input.events.size());
  // The unused middle declaration is gone.
  bool middle_gone = true;
  for (size_t i = 0; i < stale_reduced.events.size(); i++) {
    if (stale_reduced.events[i].kind == objstate::kEventDeclaration &&
        stale_reduced.events[i].decl.kind == objstate::kDeclVertex &&
        stale_reduced.events[i].decl.id == unused_middle.id) {
      middle_gone = false;
    }
  }
  TASK_CHECK(middle_gone);
  std::string stale_correct;
  TASK_CHECK(objstate::emit_obj_text(stale_reduced, false, &stale_correct, &err));
  // The negative number changed (-4 -> -3) even though the identity did not.
  TASK_CHECK(stale_correct.find("f -3 -2 -1") != std::string::npos);

  // Identities are preserved by the re-encoding.
  std::vector<FaceRecord> input_records;
  std::vector<FaceRecord> reduced_records;
  TASK_CHECK(objstate::build_expected_records(stale_input, &input_records, &err));
  TASK_CHECK(objstate::build_expected_records(stale_reduced, &reduced_records,
                                              &err));
  TASK_CHECK(input_records.size() == reduced_records.size());
  for (size_t k = 0; k < 3; k++) {
    for (int c = 0; c < 3; c++) {
      TASK_CHECK(input_records[0].corners[k].vertex[c] ==
                 reduced_records[0].corners[k].vertex[c]);
    }
  }
  Mismatch reduced_mismatch;
  TASK_CHECK(objstate::compare_obj_text_to_model(
      stale_reduced, stale_correct, objstate::material_text(stale_reduced),
      "reencoded", &reduced_mismatch));

  // Keeping the old "-4" while deleting the line yields text that cannot be
  // accepted as the reduced asset.
  std::string stale_text;
  {
    std::istringstream lines(stale_original);
    std::string line;
    while (std::getline(lines, line)) {
      if (line.size() >= 2 && line[0] == 'v' && line[1] == ' ') {
        if (line == "v 6 6 6") {
          continue;  // the deleted middle declaration
        }
        stale_text += line + "\n";
      } else if (line.size() >= 2 && line[0] == 'f' && line[1] == ' ') {
        stale_text += line + "\n";  // stale numbers, unchanged
      } else {
        stale_text += line + "\n";
      }
    }
  }
  TASK_CHECK(stale_text.find("f -4 -2 -1") != std::string::npos);
  Mismatch stale_mismatch;
  TASK_CHECK(!objstate::compare_obj_text_to_model(
      stale_reduced, stale_text, objstate::material_text(stale_reduced),
      "stale-numbers", &stale_mismatch));

  (void)r1;
  (void)r2;
  (void)r3;
  (void)unused_middle;
  return 0;
}

int CaseReduceProtectsState() {
  // Deleting usemtl/s/o events that a surviving face depends on must be
  // rejected, because it would silently change that face's state.
  AssetBuilder builder(31337);
  builder.add_material("red", 1, 0, 0);
  builder.add_material("blue", 0, 0, 1);
  builder.add_object("obj_a");
  builder.add_material_state("red");
  builder.add_smoothing(1);
  Decl v1 = builder.add_vertex(0, 0, 0);
  Decl v2 = builder.add_vertex(1, 0, 0);
  Decl v3 = builder.add_vertex(0, 1, 0);
  CornerSpec corners[3];
  corners[0].vertex_id = v1.id;
  corners[1].vertex_id = v2.id;
  corners[2].vertex_id = v3.id;
  std::string err;
  TASK_CHECK(builder.add_face(corners, &err));
  builder.add_material_state("blue");
  builder.add_smoothing(0);
  builder.add_group("obj_b");
  TASK_CHECK(builder.add_face(corners, &err));
  builder.add_material_state("red");
  builder.add_smoothing(3);
  TASK_CHECK(builder.add_face(corners, &err));
  Asset input = builder.release();

  Asset reduced;
  std::string log;
  TASK_CHECK(objstate::reduce_asset(input, "F0", 100, &reduced, &log, &err));
  // Every surviving face keeps its exact material/smoothing/name state.
  std::vector<FaceRecord> input_records;
  std::vector<FaceRecord> reduced_records;
  TASK_CHECK(objstate::build_expected_records(input, &input_records, &err));
  TASK_CHECK(objstate::build_expected_records(reduced, &reduced_records, &err));
  for (size_t i = 0; i < reduced_records.size(); i++) {
    for (size_t j = 0; j < input_records.size(); j++) {
      if (input_records[j].face_id != reduced_records[i].face_id) {
        continue;
      }
      TASK_CHECK(input_records[j].material_name ==
                 reduced_records[i].material_name);
      TASK_CHECK(input_records[j].smoothing == reduced_records[i].smoothing);
      TASK_CHECK(input_records[j].shape_name == reduced_records[i].shape_name);
    }
  }
  // And no state event could be removed without breaking a face, so rejection
  // reasons must mention the mismatch.
  TASK_CHECK(log.find("state does not match its event prefix") !=
             std::string::npos);
  return 0;
}

int CaseReduceBudgetAndDeterminism() {
  Asset input;
  std::string err;
  TASK_CHECK(objstate::generate_asset(5150, &input, &err));
  Asset a;
  Asset b;
  std::string log_a;
  std::string log_b;
  TASK_CHECK(objstate::reduce_asset(input, "F0", 16, &a, &log_a, &err));
  TASK_CHECK(objstate::reduce_asset(input, "F0", 16, &b, &log_b, &err));
  TASK_CHECK(log_a == log_b);
  TASK_CHECK(objstate::serialize_asset(a) == objstate::serialize_asset(b));
  TASK_CHECK(a.events.size() <= input.events.size());

  // Budget counts attempts that were rejected as well as accepted.
  Asset zero;
  std::string log_zero;
  TASK_CHECK(objstate::reduce_asset(input, "F0", 0, &zero, &log_zero, &err));
  TASK_CHECK(zero.events.size() == input.events.size());
  TASK_CHECK(log_zero.find("ATTEMPTS 0") != std::string::npos);

  // A budget of one allows exactly one attempt in the log.
  Asset one;
  std::string log_one;
  TASK_CHECK(objstate::reduce_asset(input, "F0", 1, &one, &log_one, &err));
  TASK_CHECK(log_one.find("ATTEMPTS 1") != std::string::npos);
  TASK_CHECK(log_one.find("ATTEMPT 2 ") == std::string::npos);

  // Anchor deletion is never attempted.
  TASK_CHECK(log_a.find("ATTEMPT 1 F0") == std::string::npos);
  for (size_t i = 0; i < a.events.size(); i++) {
    if (a.events[i].kind == objstate::kEventFace &&
        a.events[i].face.id == "F0") {
      break;
    }
  }
  // No-candidate case returns the original legal asset unchanged.
  Asset original;
  TASK_CHECK(objstate::generate_asset(5150, &original, &err));
  std::string dir_in = TempDirBase();
  std::string dir_out = TempDirBase();
  TASK_CHECK(!dir_in.empty() && !dir_out.empty());
  TASK_CHECK(objstate::write_case_files(original, dir_in, &err));
  std::string message;
  TASK_CHECK(objstate::obj_state_reduce_dir(dir_in, dir_out, "F0", 0, &message));
  TASK_CHECK(ReadAll(Path(dir_in, "case.obj")) ==
             ReadAll(Path(dir_out, "case.obj")));
  TASK_CHECK(ReadAll(Path(dir_in, "case.trace")) ==
             ReadAll(Path(dir_out, "case.trace")));
  return 0;
}

int CaseReduceRefusesReferencedDeclaration() {
  AssetBuilder builder(1212);
  builder.add_material("m", 1, 0, 0);
  builder.add_object("obj");
  builder.add_material_state("m");
  Decl v1 = builder.add_vertex(0, 0, 0);
  Decl v2 = builder.add_vertex(1, 0, 0);
  Decl v3 = builder.add_vertex(0, 1, 0);
  CornerSpec corners[3];
  corners[0].vertex_id = v1.id;
  corners[1].vertex_id = v2.id;
  corners[2].vertex_id = v3.id;
  std::string err;
  TASK_CHECK(builder.add_face(corners, &err));
  Asset input = builder.release();
  Asset reduced;
  std::string log;
  TASK_CHECK(objstate::reduce_asset(input, "F0", 100, &reduced, &log, &err));
  // The only declaration set is referenced by F0, so none can go; every
  // deletion attempt must be rejected with a reference reason.
  TASK_CHECK(log.find("references an unavailable declaration") !=
             std::string::npos);
  // The surviving trace still carries every declaration.
  int decls = 0;
  for (size_t i = 0; i < reduced.events.size(); i++) {
    if (reduced.events[i].kind == objstate::kEventDeclaration) {
      decls++;
    }
  }
  TASK_CHECK(decls == 3);
  return 0;
}

// ---------------------------------------------------------------------------
// process group
// ---------------------------------------------------------------------------

int CaseCheckWithWorkerProcess() {
  std::string dir = TempDirBase();
  TASK_CHECK(!dir.empty());
  Asset asset;
  std::string err;
  TASK_CHECK(objstate::generate_asset(8080, &asset, &err));
  TASK_CHECK(objstate::write_case_files(asset, dir, &err));
  std::string message;
  TASK_CHECK(objstate::obj_state_check_dir(dir, WorkerProgram(), true, &message));
  TASK_CHECK(message.find("OBJ_STATE_VALID") != std::string::npos);

  // The in-process path agrees.
  std::string inproc_message;
  TASK_CHECK(objstate::obj_state_check_dir(dir, "", false, &inproc_message));
  TASK_CHECK(inproc_message.find("OBJ_STATE_VALID") != std::string::npos);

  // A worker payload directory is also directly consumable by the worker.
  std::string payload = dir;
  std::vector<FaceRecord> expected;
  std::string payload_err;
  TASK_CHECK(objstate::prepare_worker_payload(asset, payload, &expected,
                                              &payload_err));
  std::string worker_out;
  std::string worker_err;
  TASK_CHECK(objstate::produce_worker_output(payload, &worker_out, &worker_err));
  std::string reply_message;
  TASK_CHECK(objstate::validate_worker_reply(expected, worker_out, 0,
                                             &reply_message));
  return 0;
}

int CaseWorkerReplyValidation() {
  std::string dir = TempDirBase();
  TASK_CHECK(!dir.empty());
  Asset asset;
  std::string err;
  TASK_CHECK(objstate::generate_asset(21, &asset, &err));
  TASK_CHECK(objstate::write_case_files(asset, dir, &err));
  std::vector<FaceRecord> expected;
  std::string payload = dir;
  TASK_CHECK(objstate::prepare_worker_payload(asset, payload, &expected, &err));
  std::string good;
  TASK_CHECK(objstate::produce_worker_output(payload, &good, &err));

  std::string message;
  TASK_CHECK(!objstate::validate_worker_reply(expected, "", 0, &message));
  TASK_CHECK(message.find("worker-output=<empty>") != std::string::npos);
  message.clear();
  TASK_CHECK(!objstate::validate_worker_reply(expected, good, 3, &message));
  TASK_CHECK(message.find("worker-exit=3") != std::string::npos);
  message.clear();
  // A reply that omits one of the four required entry/variant blocks is
  // rejected even though its remaining blocks match.
  {
    std::string missing_entry = good;
    const size_t marker = missing_entry.find("FACE LoadObj positive ");
    if (marker != std::string::npos) {
      const size_t line_end = missing_entry.find('\n', marker);
      const size_t block_start = missing_entry.find("FACE LoadObj positive ",
                                                    line_end + 1);
      const size_t erase_end = block_start == std::string::npos
                                   ? missing_entry.size()
                                   : block_start;
      (void)block_start;
      missing_entry.erase(marker, erase_end - marker);
    }
    TASK_CHECK(!objstate::validate_worker_reply(expected, missing_entry, 0,
                                                &message));
    TASK_CHECK(message.find("OBJ_STATE_INVALID") != std::string::npos);
  }
  message.clear();
  // Truncated output (drop the last record line) must not be treated as a
  // legal empty result.
  std::string truncated = good;
  const size_t last_newline = truncated.find_last_of('\n');
  if (last_newline != std::string::npos) {
    const size_t prev_newline =
        last_newline == 0 ? std::string::npos
                          : truncated.find_last_of('\n', last_newline - 1);
    truncated.erase(prev_newline == std::string::npos ? 0 : prev_newline + 1);
  }
  TASK_CHECK(!objstate::validate_worker_reply(expected, truncated, 0, &message));
  TASK_CHECK(message.find("OBJ_STATE_INVALID") != std::string::npos);
  message.clear();
  // A missing magic header is equally fatal.
  std::string no_magic = good;
  const size_t magic_end = no_magic.find('\n');
  if (magic_end != std::string::npos) {
    no_magic.erase(0, magic_end + 1);
  }
  TASK_CHECK(!objstate::validate_worker_reply(expected, no_magic, 0, &message));
  return 0;
}

int CaseIndependentProcessReplay() {
  std::string dir = TempDirBase();
  TASK_CHECK(!dir.empty());
  Asset asset;
  std::string err;
  TASK_CHECK(objstate::generate_asset(606, &asset, &err));
  TASK_CHECK(objstate::write_case_files(asset, dir, &err));
  std::string payload = dir;
  std::vector<FaceRecord> expected;
  TASK_CHECK(objstate::prepare_worker_payload(asset, payload, &expected, &err));

  const std::string program = WorkerProgram();
  std::vector<std::string> args;
  args.push_back("worker");
  args.push_back(payload);
  std::string out;
  int status = 0;
  std::string spawn_err;
  TASK_CHECK(objstate::spawn_capture(program, args, &out, &status, &spawn_err));
  std::string message;
  TASK_CHECK(objstate::validate_worker_reply(expected, out, status, &message));

  // Determinism across processes: the same payload yields byte-identical
  // replies.
  std::string out2;
  int status2 = 0;
  TASK_CHECK(objstate::spawn_capture(program, args, &out2, &status2, &spawn_err));
  TASK_CHECK(out == out2);

  // A process that prints nothing is not a valid worker.
  std::vector<std::string> silent_args;
  std::string silent_out;
  int silent_status = 0;
  TASK_CHECK(objstate::spawn_capture("/usr/bin/true", silent_args, &silent_out,
                                     &silent_status, &spawn_err));
  message.clear();
  TASK_CHECK(!objstate::validate_worker_reply(expected, silent_out,
                                             silent_status, &message));

  // A non-zero exit is a failure even if partial output is present.
  std::vector<std::string> fail_args;
  std::string fail_out;
  int fail_status = 0;
  TASK_CHECK(objstate::spawn_capture("/usr/bin/false", fail_args, &fail_out,
                                     &fail_status, &spawn_err));
  message.clear();
  TASK_CHECK(!objstate::validate_worker_reply(expected, fail_out, fail_status,
                                              &message));
  return 0;
}

int CaseCheckRejectsBadWorker() {
  std::string dir = TempDirBase();
  TASK_CHECK(!dir.empty());
  Asset asset;
  std::string err;
  TASK_CHECK(objstate::generate_asset(313, &asset, &err));
  TASK_CHECK(objstate::write_case_files(asset, dir, &err));
  const std::string silent = Path(dir, "silent_worker.sh");
  TASK_CHECK(WriteScript(silent, "#!/bin/sh\nexit 0\n"));
  std::string message;
  TASK_CHECK(!objstate::obj_state_check_dir(dir, silent, true, &message));
  TASK_CHECK(message.find("OBJ_STATE_INVALID") != std::string::npos);

  const std::string failing = Path(dir, "failing_worker.sh");
  TASK_CHECK(WriteScript(failing, "#!/bin/sh\nexit 7\n"));
  message.clear();
  TASK_CHECK(!objstate::obj_state_check_dir(dir, failing, true, &message));
  TASK_CHECK(message.find("worker-exit=7") != std::string::npos);

  // A worker that emits garbage must not be accepted.
  const std::string garbage = Path(dir, "garbage_worker.sh");
  TASK_CHECK(WriteScript(garbage, "#!/bin/sh\nprintf 'OBJSTATE_WORKER 1\\n'\n"));
  message.clear();
  TASK_CHECK(!objstate::obj_state_check_dir(dir, garbage, true, &message));
  TASK_CHECK(message.find("OBJ_STATE_INVALID") != std::string::npos);
  return 0;
}

int CaseCheckRejectsTruncatedCase() {
  std::string dir = TempDirBase();
  TASK_CHECK(!dir.empty());
  Asset asset;
  std::string err;
  TASK_CHECK(objstate::generate_asset(77, &asset, &err));
  TASK_CHECK(objstate::write_case_files(asset, dir, &err));
  // Empty case.obj must fail rather than succeed as an empty result.
  TASK_CHECK(WriteAll(Path(dir, "case.obj"), ""));
  std::string message;
  TASK_CHECK(!objstate::obj_state_check_dir(dir, "", false, &message));
  TASK_CHECK(message.find("empty") != std::string::npos || !message.empty());

  // A missing trace fails.
  TASK_CHECK(::unlink(Path(dir, "case.trace").c_str()) == 0);
  message.clear();
  TASK_CHECK(!objstate::obj_state_check_dir(dir, "", false, &message));
  return 0;
}

int CaseGenerateToDirectory() {
  std::string dir = TempDirBase();
  TASK_CHECK(!dir.empty());
  std::string message;
  TASK_CHECK(objstate::obj_state_generate_dir(9, dir, &message));
  TASK_CHECK(message.find("OBJ_STATE_GENERATED") != std::string::npos);
  TASK_CHECK(objstate::file_exists(Path(dir, "case.trace")));
  TASK_CHECK(objstate::file_exists(Path(dir, "case.obj")));
  TASK_CHECK(objstate::file_exists(Path(dir, "case.mtl")));
  Asset a;
  std::string err;
  TASK_CHECK(objstate::generate_asset(9, &a, &err));
  TASK_CHECK(ReadAll(Path(dir, "case.trace")) == objstate::serialize_asset(a));
  std::string again = Path(dir, "second");
  TASK_CHECK(objstate::obj_state_generate_dir(9, again, &message));
  TASK_CHECK(ReadAll(Path(dir, "case.trace")) ==
             ReadAll(Path(again, "case.trace")));
  TASK_CHECK(ReadAll(Path(dir, "case.obj")) == ReadAll(Path(again, "case.obj")));
  return 0;
}

int CaseReduceDirectoryRoundTrip() {
  std::string dir = TempDirBase();
  TASK_CHECK(!dir.empty());
  std::string message;
  TASK_CHECK(objstate::obj_state_generate_dir(1234, dir, &message));
  std::string out_dir = Path(dir, "reduced");
  TASK_CHECK(objstate::obj_state_reduce_dir(dir, out_dir, "F0", 32, &message));
  TASK_CHECK(objstate::file_exists(Path(out_dir, "case.trace")));
  TASK_CHECK(objstate::file_exists(Path(out_dir, "case.obj")));
  TASK_CHECK(objstate::file_exists(Path(out_dir, "case.mtl")));
  TASK_CHECK(objstate::file_exists(Path(out_dir, "reduction.log")));
  // Determinism across processes: re-running writes identical bytes.
  std::string out2 = Path(dir, "reduced2");
  std::string message2;
  TASK_CHECK(objstate::obj_state_reduce_dir(dir, out2, "F0", 32, &message2));
  TASK_CHECK(ReadAll(Path(out_dir, "reduction.log")) ==
             ReadAll(Path(out2, "reduction.log")));
  TASK_CHECK(ReadAll(Path(out_dir, "case.obj")) ==
             ReadAll(Path(out2, "case.obj")));
  TASK_CHECK(ReadAll(Path(out_dir, "case.trace")) ==
             ReadAll(Path(out2, "case.trace")));
  // The reduced case is still valid under check.
  std::string check_message;
  TASK_CHECK(objstate::obj_state_check_dir(out_dir, "", false, &check_message));
  TASK_CHECK(check_message.find("OBJ_STATE_VALID") != std::string::npos);

  // Cross-process determinism: two independently spawned reducers must emit
  // byte-identical logs and cases.
  const std::string program = WorkerProgram();
  const std::string cross_a = Path(dir, "cross_a");
  const std::string cross_b = Path(dir, "cross_b");
  std::vector<std::string> args_a;
  args_a.push_back("reduce");
  args_a.push_back(dir);
  args_a.push_back(cross_a);
  args_a.push_back("--anchor");
  args_a.push_back("F0");
  args_a.push_back("--budget");
  args_a.push_back("32");
  std::vector<std::string> args_b;
  args_b.push_back("reduce");
  args_b.push_back(dir);
  args_b.push_back(cross_b);
  args_b.push_back("--anchor");
  args_b.push_back("F0");
  args_b.push_back("--budget");
  args_b.push_back("32");
  std::string spawn_out;
  int spawn_status = 0;
  std::string spawn_err;
  TASK_CHECK(objstate::spawn_capture(program, args_a, &spawn_out, &spawn_status,
                                     &spawn_err));
  TASK_CHECK(spawn_status == 0);
  TASK_CHECK(objstate::spawn_capture(program, args_b, &spawn_out, &spawn_status,
                                     &spawn_err));
  TASK_CHECK(spawn_status == 0);
  TASK_CHECK(ReadAll(Path(cross_a, "reduction.log")) ==
             ReadAll(Path(cross_b, "reduction.log")));
  TASK_CHECK(ReadAll(Path(cross_a, "case.obj")) ==
             ReadAll(Path(cross_b, "case.obj")));
  TASK_CHECK(ReadAll(Path(cross_a, "case.trace")) ==
             ReadAll(Path(cross_b, "case.trace")));
  return 0;
}


}  // namespace

const struct task_case task_cases[] = {
    {"declaration", "generate-profile", &CaseGenerateProfile},
    {"declaration", "trace-round-trip", &CaseTraceRoundTrip},
    {"declaration", "state-snapshot", &CaseStateSnapshot},
    {"oracle", "oracle-matches-case-obj", &CaseOracleMatchesCaseObj},
    {"oracle", "early-versus-final-pool", &CaseEarlyVersusFinalPool},
    {"oracle", "missing-components-are-minus-one",
     &CaseMissingComponentsAreMinusOne},
    {"oracle", "two-entry-points-are-not-enough",
     &CaseTwoEntryPointsAreNotEnough},
    {"reduce", "reduce-basic", &CaseReduceBasic},
    {"reduce", "reduce-middle-declaration", &CaseReduceMiddleDeclaration},
    {"reduce", "reduce-protects-state", &CaseReduceProtectsState},
    {"reduce", "reduce-budget-and-determinism", &CaseReduceBudgetAndDeterminism},
    {"reduce", "reduce-refuses-referenced-declaration",
     &CaseReduceRefusesReferencedDeclaration},
    {"process", "check-with-worker-process", &CaseCheckWithWorkerProcess},
    {"process", "worker-reply-validation", &CaseWorkerReplyValidation},
    {"process", "independent-process-replay", &CaseIndependentProcessReplay},
    {"process", "check-rejects-bad-worker", &CaseCheckRejectsBadWorker},
    {"process", "check-rejects-truncated-case", &CaseCheckRejectsTruncatedCase},
    {"process", "generate-to-directory", &CaseGenerateToDirectory},
    {"process", "reduce-directory-round-trip", &CaseReduceDirectoryRoundTrip},
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
