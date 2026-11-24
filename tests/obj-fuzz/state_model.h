#ifndef TINYOBJ_TESTS_OBJ_STATE_MODEL_H_
#define TINYOBJ_TESTS_OBJ_STATE_MODEL_H_

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

// Shared declaration/event model for the state-aware OBJ fuzz target.
//
// The trace is the authoritative record: it stores stable declaration ids,
// stable face ids, per-corner reference identity (declaration ids, not final
// pool positions), and the object/group/material/smoothing state that was in
// effect when the face was declared.  The parsing library is never used to
// produce this record.
//
// Everything below lives in namespace objstate.  The only global entry point
// is obj_state_main(), which takes argv[0] so that check can re-execute itself
// as the worker process.

namespace objstate {

const int kMaxGenerationDeclarationsPerKind = 64;
const int kMaxGenerationFaces = 64;

enum DeclKind {
  kDeclVertex = 0,
  kDeclTexcoord = 1,
  kDeclNormal = 2
};

struct Decl {
  DeclKind kind;
  int id;              // stable per-kind id, never renumbered
  long coordinates[3]; // v/vn: xyz, vt: uv in [0], [1]

  Decl();
};

struct Corner {
  int vertex_id;
  int texcoord_id;   // 0 when has_texcoord is false
  int normal_id;     // 0 when has_normal is false
  bool has_texcoord;
  bool has_normal;
  bool negative_vertex;
  bool negative_texcoord;
  bool negative_normal;

  Corner();
};

struct Material {
  std::string name;
  long r;
  long g;
  long b;

  Material();
};

struct Face {
  std::string id;            // stable face id: F0, F1, ...
  std::string shape_name;    // current o/g name (tinyobj shape.name semantics)
  std::string material_name;
  long smoothing;
  Corner corners[3];

  Face();
};

enum EventKind {
  kEventDeclaration = 0,
  kEventObject = 1,
  kEventGroup = 2,
  kEventMaterial = 3,
  kEventSmoothing = 4,
  kEventFace = 5
};

struct Event {
  EventKind kind;
  std::string id;  // declaration: V<id>/T<id>/N<id>, state: E<ordinal>,
                   // face: face.id
  Decl decl;
  Face face;
  std::string name;  // o/g/usemtl name
  long smoothing;    // s value

  Event();
};

struct Asset {
  uint64_t seed;
  std::vector<Material> materials;
  std::vector<Event> events;  // interleaved declaration/state/face order

  Asset();
};

struct CornerSpec {
  int vertex_id;
  int texcoord_id;
  int normal_id;
  bool has_texcoord;
  bool has_normal;
  bool negative_vertex;
  bool negative_texcoord;
  bool negative_normal;

  CornerSpec();
};

// Convenience builder used by the generator and by hand-written scenarios in
// the tests.  Declaration ids and event ids are assigned stably here; nothing
// ever renumbers an existing declaration.
class AssetBuilder {
 public:
  explicit AssetBuilder(uint64_t seed = 0);

  void add_material(const std::string &name, long r, long g, long b);

  Decl add_vertex(long x, long y, long z);
  Decl add_texcoord(long u, long v);
  Decl add_normal(long x, long y, long z);

  void add_object(const std::string &name);
  void add_group(const std::string &name);
  void add_material_state(const std::string &name);
  void add_smoothing(long value);

  // Adds a triangle face using the current object/group/material/smoothing
  // state.  Returns false (and fills err) only for structurally impossible
  // requests such as unknown declaration ids.
  bool add_face(const CornerSpec corners[3], std::string *err);

  const Asset &asset() const { return asset_; }
  Asset release() { return asset_; }

  const std::vector<Decl> &vertices() const { return vertices_; }
  const std::vector<Decl> &texcoords() const { return texcoords_; }
  const std::vector<Decl> &normals() const { return normals_; }

  const Decl *find_decl(DeclKind kind, int id) const;

 private:
  Event &append_event(EventKind kind, const std::string &id);

  Asset asset_;
  std::vector<Decl> vertices_;
  std::vector<Decl> texcoords_;
  std::vector<Decl> normals_;
  int next_decl_id_[3];
  int next_face_id_;
  int next_event_ordinal_;
  std::string current_shape_;
  std::string current_material_;
  long current_smoothing_;
};

// Structural validation shared by the checker and the reducer: event order,
// declaration ids, state snapshots, legal materials/smoothing, non-degenerate
// triangles.  When generation_profile is true it additionally enforces the
// generator's contract (F0 all-negative with v/vt/vn, early un-referenced
// declaration, declaration after F0, interleaving, coverage of all four token
// forms, segment-per-switch, size caps).
bool validate_asset(const Asset &asset, bool generation_profile,
                    std::string *err);
bool validate_generation_profile(const Asset &asset, std::string *err);
bool is_state_event_kind(EventKind kind);

std::string serialize_asset(const Asset &asset);
bool parse_asset(const std::string &text, Asset *asset, std::string *err);

std::string material_text(const Asset &asset);

// Serializes the event order back to OBJ text.  all_positive=false preserves
// each corner component's recorded +/- style, recomputing the numeric token
// from the identity and the pool that exists at that point in the (possibly
// reduced) event order.  all_positive=true emits ordinal+1 tokens for every
// component.  Faces with only a vertex token omit the texcoord/normal parts.
bool emit_obj_text(const Asset &asset, bool all_positive, std::string *out,
                   std::string *err);

// Deterministic generation for generate SEED DIR.  The result always satisfies
// the generation profile validated by validate_generation_profile().
bool generate_asset(uint64_t seed, Asset *out, std::string *err);

// Checks that the generated asset really satisfies the generation profile.
bool verify_generated_asset(const Asset &asset, std::string *err);

// Writes DIR/case.trace, DIR/case.obj (style-preserving) and DIR/case.mtl.
bool write_case_files(const Asset &asset, const std::string &dir,
                      std::string *err);

// ---------------------------------------------------------------------------
// Independent declaration-event oracle (the reference model).
// ---------------------------------------------------------------------------

struct CornerRecord {
  bool has_texcoord;
  bool has_normal;
  double vertex[3];
  double texcoord[2];
  double normal[3];
  // Raw index values as reported by a production parser (worker records only;
  // model/oracle records leave them at -2).  Missing components must be -1.
  int vertex_index;
  int texcoord_index;
  int normal_index;

  CornerRecord();
};

struct FaceRecord {
  std::string face_id;  // stable id when produced by the model, F<ordinal>
                        // when produced by the production worker
  std::string shape_name;
  std::string material_name;
  long smoothing;
  CornerRecord corners[3];

  FaceRecord();
};

// Reference records derived only from the trace's declaration/state events.
bool build_expected_records(const Asset &asset, std::vector<FaceRecord> *out,
                            std::string *err);

// Interprets OBJ/MTL text with its own pool bookkeeping: negative tokens are
// resolved against the pool size at the face that uses them, never against the
// final pool.  This is a second, independent implementation used to verify
// that a serialization really encodes the model's identities.
bool interpret_obj_text(const std::string &obj_text,
                        const std::string &mtl_text,
                        std::vector<FaceRecord> *out, std::string *err);

struct Mismatch {
  std::string face_id;
  int corner;              // -1 for face-level fields
  std::string component;   // vertex/texcoord/normal/shape/material/smoothing/...
  std::string entry;       // ObjReader/preserve, text-oracle/preserve, ...
  std::string expected;
  std::string actual;

  Mismatch();
};

// Compares expected (model) records to actual records in order.  Returns false
// and fills the first mismatch on disagreement.
bool compare_records(const std::vector<FaceRecord> &expected,
                     const std::vector<FaceRecord> &actual,
                     const std::string &entry, Mismatch *mismatch);

// Compares model records to production records: same values/presence, but the
// production side must additionally report raw indices, with -1 exactly for
// the missing texcoord/normal components.
bool compare_production_records(const std::vector<FaceRecord> &expected,
                                const std::vector<FaceRecord> &actual,
                                const std::string &entry,
                                Mismatch *mismatch);

bool compare_record_values(const std::vector<FaceRecord> &expected,
                           const std::vector<FaceRecord> &actual,
                           const std::string &entry, Mismatch *mismatch);

bool compare_obj_text_to_model(const Asset &asset, const std::string &obj_text,
                               const std::string &mtl_text,
                               const std::string &entry,
                               Mismatch *mismatch);

std::string describe_mismatch(const Mismatch &mismatch);

// ---------------------------------------------------------------------------
// Worker protocol.
// ---------------------------------------------------------------------------

struct WorkerEntry {
  std::string parser;   // ObjReader / LoadObj
  std::string variant;  // preserve / positive
  std::vector<FaceRecord> faces;
};

// Reads DIR/state.orig.obj, DIR/state.pos.obj and DIR/state.mtl through the two
// production entry points (ObjReader::ParseFromString and streaming LoadObj with
// an in-memory MaterialStreamReader, triangulation off) and formats the full
// canonical per-face record set on stdout.
bool produce_worker_output(const std::string &dir, std::string *out,
                           std::string *err);

// Same two production entry points, but called in-process.  Used by check to
// load state.orig.obj / state.pos.obj exactly the way the worker does.
bool parse_production_text(const std::string &obj_text,
                           const std::string &mtl_text, bool use_reader,
                           std::vector<FaceRecord> *out, std::string *err);

// Shared serialization of one worker record block (used for consistency checks
// and for tests that inject malformed worker output).
std::string worker_record_line(const std::string &entry,
                               const std::string &variant,
                               const FaceRecord &record);
std::string worker_magic();

bool parse_worker_output(const std::string &text,
                         std::vector<WorkerEntry> *entries, std::string *err);

// Compares the worker entries (in protocol order) against expected records.
bool compare_worker_output(const std::vector<FaceRecord> &expected,
                           const std::vector<WorkerEntry> &entries,
                           std::string *diagnostic);

// Verifies a complete worker reply (exit status + stdout) exactly as check
// does: nonzero status, empty output, truncation, bad magic and every
// per-face disagreement are failures; only a fully matching reply succeeds.
bool validate_worker_reply(const std::vector<FaceRecord> &expected,
                           const std::string &worker_stdout, int exit_status,
                           std::string *message);

// Writes DIR/state.orig.obj (original index style) and DIR/state.pos.obj
// (all-positive indices) plus DIR/state.mtl, returning the model records to
// compare the worker against.
bool prepare_worker_payload(const Asset &asset, const std::string &work_dir,
                            std::vector<FaceRecord> *expected,
                            std::string *err);

// Spawns program with args, capturing stdout and stderr into out.  Returns
// false if the process could not be started or waited on.
bool spawn_capture(const std::string &program,
                   const std::vector<std::string> &args, std::string *out,
                   int *exit_status, std::string *err);

// ---------------------------------------------------------------------------
// Top-level operations.
// ---------------------------------------------------------------------------

bool obj_state_generate_dir(uint64_t seed, const std::string &dir,
                            std::string *message);
bool obj_state_check_dir(const std::string &dir,
                         const std::string &worker_program,
                         bool use_worker, std::string *message);
bool obj_state_reduce_dir(const std::string &in_dir, const std::string &out_dir,
                          const std::string &anchor, long long budget,
                          std::string *message);

// In-memory reduction used by generate/check/demo and the tests.  `anchor` may
// be empty to mean "no anchor".  Emits the deterministic reduction.log text.
bool reduce_asset(const Asset &input, const std::string &anchor, long long budget,
                  Asset *out, std::string *log_text, std::string *err);

// Filesystem helpers (POSIX, no shell).
bool ensure_directory(const std::string &dir);
bool read_text_file(const std::string &path, std::string *out);
bool write_text_file(const std::string &path, const std::string &text);
bool copy_file_bytes(const std::string &from, const std::string &to);
bool file_exists(const std::string &path);

std::string format_double(double value);

}  // namespace objstate

// Public driver entry point.  argv[0] is used by `check` to re-execute the
// same binary as the `worker` sub-process.
int obj_state_main(int argc, char **argv);

#endif  // TINYOBJ_TESTS_OBJ_STATE_MODEL_H_
