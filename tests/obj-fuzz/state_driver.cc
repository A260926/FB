#include "state_model.h"

#include "tiny_obj_loader.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace objstate {

namespace {

const char kWorkerMagic[] = "OBJSTATE_WORKER 1";

std::string DirPath(const std::string &dir, const std::string &name) {
  if (dir.empty()) {
    return name;
  }
  return dir + "/" + name;
}

// ---------------------------------------------------------------------------
// Flattening library output into canonical records
// ---------------------------------------------------------------------------

bool FlattenShape(const tinyobj::attrib_t &attrib,
                  const tinyobj::shape_t &shape,
                  const std::vector<tinyobj::material_t> &materials,
                  int *face_ordinal, std::vector<FaceRecord> *out,
                  std::string *err) {
  const tinyobj::mesh_t &mesh = shape.mesh;
  size_t index_cursor = 0;
  for (size_t f = 0; f < mesh.num_face_vertices.size(); f++) {
    const unsigned int count = mesh.num_face_vertices[f];
    if (count != 3) {
      if (err) *err = "production parser produced a non-triangle face";
      return false;
    }
    if (index_cursor + 3 > mesh.indices.size()) {
      if (err) *err = "production index stream truncated";
      return false;
    }
    FaceRecord record;
    record.face_id = "F" + std::to_string(*face_ordinal);
    (*face_ordinal)++;
    record.shape_name = shape.name;
    record.smoothing =
        static_cast<long>(mesh.smoothing_group_ids[f]);
    const int material_id = mesh.material_ids[f];
    if (material_id >= 0 &&
        static_cast<size_t>(material_id) < materials.size()) {
      record.material_name = materials[static_cast<size_t>(material_id)].name;
    } else {
      record.material_name.clear();
    }
    for (int k = 0; k < 3; k++) {
      const tinyobj::index_t &index = mesh.indices[index_cursor++];
      CornerRecord &corner = record.corners[k];
      corner.has_texcoord = index.texcoord_index >= 0;
      corner.has_normal = index.normal_index >= 0;
      corner.vertex_index = index.vertex_index;
      corner.texcoord_index = index.texcoord_index;
      corner.normal_index = index.normal_index;
      if (index.vertex_index < 0 ||
          static_cast<size_t>(index.vertex_index) * 3 + 2 >=
              attrib.vertices.size()) {
        if (err) *err = "production vertex index out of range";
        return false;
      }
      corner.vertex[0] = attrib.vertices[index.vertex_index * 3 + 0];
      corner.vertex[1] = attrib.vertices[index.vertex_index * 3 + 1];
      corner.vertex[2] = attrib.vertices[index.vertex_index * 3 + 2];
      if (corner.has_texcoord) {
        if (static_cast<size_t>(index.texcoord_index) * 2 + 1 >=
            attrib.texcoords.size()) {
          if (err) *err = "production texcoord index out of range";
          return false;
        }
        corner.texcoord[0] = attrib.texcoords[index.texcoord_index * 2 + 0];
        corner.texcoord[1] = attrib.texcoords[index.texcoord_index * 2 + 1];
      } else if (index.texcoord_index != -1) {
        if (err) *err = "production texcoord index is neither valid nor -1";
        return false;
      }
      if (corner.has_normal) {
        if (static_cast<size_t>(index.normal_index) * 3 + 2 >=
            attrib.normals.size()) {
          if (err) *err = "production normal index out of range";
          return false;
        }
        corner.normal[0] = attrib.normals[index.normal_index * 3 + 0];
        corner.normal[1] = attrib.normals[index.normal_index * 3 + 1];
        corner.normal[2] = attrib.normals[index.normal_index * 3 + 2];
      } else if (index.normal_index != -1) {
        if (err) *err = "production normal index is neither valid nor -1";
        return false;
      }
    }
    out->push_back(record);
  }
  if (index_cursor != mesh.indices.size()) {
    if (err) *err = "production index stream has trailing entries";
    return false;
  }
  return true;
}

bool FlattenLegacy(const tinyobj::attrib_t &attrib,
                   const std::vector<tinyobj::shape_t> &shapes,
                   const std::vector<tinyobj::material_t> &materials,
                   std::vector<FaceRecord> *out, std::string *err) {
  out->clear();
  int ordinal = 0;
  for (size_t i = 0; i < shapes.size(); i++) {
    if (!FlattenShape(attrib, shapes[i], materials, &ordinal, out, err)) {
      return false;
    }
  }
  if (out->size() == 0) {
    if (err) *err = "production parser produced no faces";
    return false;
  }
  return true;
}

}  // namespace

bool parse_production_text(const std::string &obj_text,
                           const std::string &mtl_text, bool use_reader,
                           std::vector<FaceRecord> *out, std::string *err) {
  if (!out) {
    if (err) *err = "null output";
    return false;
  }
  if (use_reader) {
    tinyobj::ObjReaderConfig config;
    config.triangulate = false;
    config.vertex_color = false;
    tinyobj::ObjReader reader;
    if (!reader.ParseFromString(obj_text, mtl_text, config)) {
      if (err) *err = "ObjReader failed: " + reader.Error();
      return false;
    }
    return FlattenLegacy(reader.GetAttrib(), reader.GetShapes(),
                         reader.GetMaterials(), out, err);
  }
  tinyobj::attrib_t attrib;
  std::vector<tinyobj::shape_t> shapes;
  std::vector<tinyobj::material_t> materials;
  std::string warn;
  std::string load_err;
  std::istringstream obj_stream(obj_text);
  std::istringstream mtl_stream(mtl_text);
  tinyobj::MaterialStreamReader material_reader(mtl_stream);
  const bool ok = tinyobj::LoadObj(&attrib, &shapes, &materials, &warn,
                                   &load_err, &obj_stream, &material_reader,
                                   /*triangulate=*/false,
                                   /*default_vcols_fallback=*/false);
  if (!ok || !load_err.empty()) {
    if (err) *err = "LoadObj failed: " + load_err;
    return false;
  }
  return FlattenLegacy(attrib, shapes, materials, out, err);
}

// ---------------------------------------------------------------------------
// Worker protocol
// ---------------------------------------------------------------------------

std::string worker_magic() { return kWorkerMagic; }

std::string worker_record_line(const std::string &entry,
                               const std::string &variant,
                               const FaceRecord &record) {
  std::ostringstream os;
  os << "FACE " << entry << " " << variant << " " << record.face_id << " "
     << (record.shape_name.empty() ? "-" : record.shape_name) << " "
     << record.material_name << " " << record.smoothing;
  for (int k = 0; k < 3; k++) {
    const CornerRecord &corner = record.corners[k];
    os << " " << format_double(corner.vertex[0]) << " "
       << format_double(corner.vertex[1]) << " "
       << format_double(corner.vertex[2]);
    if (corner.has_texcoord) {
      os << " " << format_double(corner.texcoord[0]) << " "
         << format_double(corner.texcoord[1]);
    } else {
      os << " - -";
    }
    if (corner.has_normal) {
      os << " " << format_double(corner.normal[0]) << " "
         << format_double(corner.normal[1]) << " "
         << format_double(corner.normal[2]);
    } else {
      os << " - - -";
    }
    // Raw indices as reported by the production parser.  Missing components
    // must be exactly -1; this is checked against the model.
    os << " i" << corner.vertex_index << ":" << corner.texcoord_index << ":"
       << corner.normal_index;
  }
  os << "\n";
  return os.str();
}

namespace {

bool EmitOne(const std::string &dir, const char *file, const char *entry,
             const char *variant, bool use_reader, std::string *out,
             std::string *err) {
  std::string obj_text;
  std::string mtl_text;
  if (!read_text_file(DirPath(dir, file), &obj_text)) {
    if (err) *err = std::string("cannot read ") + file;
    return false;
  }
  if (!read_text_file(DirPath(dir, "state.mtl"), &mtl_text)) {
    if (err) *err = "cannot read state.mtl";
    return false;
  }
  std::vector<FaceRecord> faces;
  if (!parse_production_text(obj_text, mtl_text, use_reader, &faces, err)) {
    if (err) *err = std::string("while reading ") + file + ": " + *err;
    return false;
  }
  for (size_t i = 0; i < faces.size(); i++) {
    *out += worker_record_line(entry, variant, faces[i]);
  }
  return true;
}

}  // namespace

bool produce_worker_output(const std::string &dir, std::string *out,
                           std::string *err) {
  if (!out) {
    if (err) *err = "null output";
    return false;
  }
  *out = kWorkerMagic;
  *out += "\n";
  if (!EmitOne(dir, "state.orig.obj", "ObjReader", "preserve", true, out, err)) {
    return false;
  }
  if (!EmitOne(dir, "state.orig.obj", "LoadObj", "preserve", false, out, err)) {
    return false;
  }
  if (!EmitOne(dir, "state.pos.obj", "ObjReader", "positive", true, out, err)) {
    return false;
  }
  if (!EmitOne(dir, "state.pos.obj", "LoadObj", "positive", false, out, err)) {
    return false;
  }
  return true;
}

bool parse_worker_output(const std::string &text,
                         std::vector<WorkerEntry> *entries, std::string *err) {
  if (!entries) {
    if (err) *err = "null entries";
    return false;
  }
  entries->clear();
  std::istringstream lines(text);
  std::string line;
  bool saw_magic = false;
  std::string current_key;
  WorkerEntry *current = NULL;
  while (std::getline(lines, line)) {
    if (!line.empty() && line[line.size() - 1] == '\r') {
      line.erase(line.size() - 1);
    }
    if (line.empty()) {
      continue;
    }
    if (!saw_magic) {
      if (line != kWorkerMagic) {
        if (err) *err = "missing worker magic";
        return false;
      }
      saw_magic = true;
      continue;
    }
    std::istringstream tokens(line);
    std::string keyword;
    if (!(tokens >> keyword) || keyword != "FACE") {
      if (err) *err = "unexpected worker line: " + line;
      return false;
    }
    std::string entry_name;
    std::string variant;
    FaceRecord record;
    if (!(tokens >> entry_name >> variant >> record.face_id)) {
      if (err) *err = "truncated worker record";
      return false;
    }
    std::string shape_token;
    if (!(tokens >> shape_token >> record.material_name >> record.smoothing)) {
      if (err) *err = "truncated worker record";
      return false;
    }
    record.shape_name = shape_token == "-" ? std::string() : shape_token;
    for (int k = 0; k < 3; k++) {
      CornerRecord &corner = record.corners[k];
      std::string vx;
      std::string vy;
      std::string vz;
      if (!(tokens >> vx >> vy >> vz)) {
        if (err) *err = "truncated worker vertex record";
        return false;
      }
      corner.vertex[0] = std::strtod(vx.c_str(), NULL);
      corner.vertex[1] = std::strtod(vy.c_str(), NULL);
      corner.vertex[2] = std::strtod(vz.c_str(), NULL);
      std::string tx;
      std::string ty;
      if (!(tokens >> tx >> ty)) {
        if (err) *err = "truncated worker texcoord record";
        return false;
      }
      if (tx == "-" && ty == "-") {
        corner.has_texcoord = false;
      } else {
        corner.has_texcoord = true;
        corner.texcoord[0] = std::strtod(tx.c_str(), NULL);
        corner.texcoord[1] = std::strtod(ty.c_str(), NULL);
      }
      std::string nx;
      std::string ny;
      std::string nz;
      if (!(tokens >> nx >> ny >> nz)) {
        if (err) *err = "truncated worker normal record";
        return false;
      }
      if (nx == "-" && ny == "-" && nz == "-") {
        corner.has_normal = false;
      } else {
        corner.has_normal = true;
        corner.normal[0] = std::strtod(nx.c_str(), NULL);
        corner.normal[1] = std::strtod(ny.c_str(), NULL);
        corner.normal[2] = std::strtod(nz.c_str(), NULL);
      }
      std::string index_token;
      if (!(tokens >> index_token) || index_token.size() < 2 ||
          index_token[0] != 'i') {
        if (err) *err = "missing worker index triple";
        return false;
      }
      {
        const size_t first = index_token.find(':');
        const size_t second = first == std::string::npos
                                  ? std::string::npos
                                  : index_token.find(':', first + 1);
        if (first == std::string::npos || second == std::string::npos) {
          if (err) *err = "bad worker index triple";
          return false;
        }
        corner.vertex_index =
            std::atoi(index_token.substr(1, first - 1).c_str());
        corner.texcoord_index =
            std::atoi(index_token.substr(first + 1, second - first - 1).c_str());
        corner.normal_index =
            std::atoi(index_token.substr(second + 1).c_str());
      }
    }
    const std::string key = entry_name + "/" + variant;
    if (current_key != key) {
      WorkerEntry fresh;
      fresh.parser = entry_name;
      fresh.variant = variant;
      entries->push_back(fresh);
      current = &entries->back();
      current_key = key;
    }
    current->faces.push_back(record);
  }
  if (!saw_magic) {
    if (err) *err = "empty worker output";
    return false;
  }
  if (entries->empty()) {
    if (err) *err = "worker produced no records";
    return false;
  }
  return true;
}

bool compare_worker_output(const std::vector<FaceRecord> &expected,
                           const std::vector<WorkerEntry> &entries,
                           std::string *diagnostic) {
  // Both production entries must appear, for both serialization styles.  A
  // worker that silently omits one of them is not a valid reply.
  const char *required[4][2] = {{"ObjReader", "preserve"},
                                {"LoadObj", "preserve"},
                                {"ObjReader", "positive"},
                                {"LoadObj", "positive"}};
  for (size_t i = 0; i < 4; i++) {
    bool found = false;
    for (size_t j = 0; j < entries.size(); j++) {
      if (entries[j].parser == required[i][0] &&
          entries[j].variant == required[i][1]) {
        found = true;
      }
    }
    if (!found) {
      if (diagnostic) {
        *diagnostic = std::string("OBJ_STATE_INVALID missing-entry=") +
                      required[i][0] + "/" + required[i][1];
      }
      return false;
    }
  }
  if (entries.size() != 4) {
    if (diagnostic) {
      *diagnostic = "OBJ_STATE_INVALID unexpected-entry-count=" +
                    std::to_string(entries.size());
    }
    return false;
  }
  for (size_t i = 0; i < entries.size(); i++) {
    const WorkerEntry &entry = entries[i];
    Mismatch mismatch;
    const std::string label = entry.parser + "/" + entry.variant;
    if (!compare_production_records(expected, entry.faces, label, &mismatch)) {
      if (diagnostic) {
        *diagnostic = "OBJ_STATE_INVALID " + describe_mismatch(mismatch);
      }
      return false;
    }
  }
  return true;
}

bool validate_worker_reply(const std::vector<FaceRecord> &expected,
                           const std::string &worker_stdout, int exit_status,
                           std::string *message) {
  if (exit_status != 0) {
    std::ostringstream os;
    os << "OBJ_STATE_INVALID worker-exit=" << exit_status;
    if (worker_stdout.empty()) {
      os << " worker-output=<empty>";
    } else {
      std::string first_line = worker_stdout;
      const size_t newline = first_line.find('\n');
      if (newline != std::string::npos) {
        first_line.erase(newline);
      }
      os << " worker-output=" << first_line;
    }
    if (message) *message = os.str();
    return false;
  }
  if (worker_stdout.empty()) {
    if (message) *message = "OBJ_STATE_INVALID worker-output=<empty>";
    return false;
  }
  std::vector<WorkerEntry> entries;
  std::string parse_err;
  if (!parse_worker_output(worker_stdout, &entries, &parse_err)) {
    if (message) *message = "OBJ_STATE_INVALID worker-output=" + parse_err;
    return false;
  }
  std::string diagnostic;
  if (!compare_worker_output(expected, entries, &diagnostic)) {
    if (message) *message = diagnostic;
    return false;
  }
  return true;
}

bool prepare_worker_payload(const Asset &asset, const std::string &work_dir,
                            std::vector<FaceRecord> *expected,
                            std::string *err) {
  std::string obj_preserve;
  std::string obj_positive;
  if (!emit_obj_text(asset, false, &obj_preserve, err) ||
      !emit_obj_text(asset, true, &obj_positive, err)) {
    return false;
  }
  if (!ensure_directory(work_dir)) {
    if (err) *err = "cannot create worker directory";
    return false;
  }
  if (!write_text_file(DirPath(work_dir, "state.orig.obj"), obj_preserve) ||
      !write_text_file(DirPath(work_dir, "state.pos.obj"), obj_positive) ||
      !write_text_file(DirPath(work_dir, "state.mtl"), material_text(asset))) {
    if (err) *err = "cannot write worker payload";
    return false;
  }
  if (expected && !build_expected_records(asset, expected, err)) {
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Process spawning
// ---------------------------------------------------------------------------

bool spawn_capture(const std::string &program,
                   const std::vector<std::string> &args, std::string *out,
                   int *exit_status, std::string *err) {
  int pipe_fds[2];
  if (::pipe(pipe_fds) != 0) {
    if (err) *err = "cannot create pipe";
    return false;
  }
  std::vector<char *> argv;
  argv.push_back(const_cast<char *>(program.c_str()));
  for (size_t i = 0; i < args.size(); i++) {
    argv.push_back(const_cast<char *>(args[i].c_str()));
  }
  argv.push_back(NULL);

  const pid_t pid = ::fork();
  if (pid < 0) {
    ::close(pipe_fds[0]);
    ::close(pipe_fds[1]);
    if (err) *err = "cannot fork";
    return false;
  }
  if (pid == 0) {
    ::close(pipe_fds[0]);
    int stdout_fd = ::dup2(pipe_fds[1], 1);
    if (stdout_fd < 0) {
      _exit(127);
    }
    ::close(pipe_fds[1]);
    ::execvp(program.c_str(), &argv[0]);
    _exit(127);
  }
  ::close(pipe_fds[1]);
  std::string captured;
  char buffer[4096];
  while (true) {
    const ssize_t n = ::read(pipe_fds[0], buffer, sizeof(buffer));
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    if (n == 0) {
      break;
    }
    captured.append(buffer, static_cast<size_t>(n));
  }
  ::close(pipe_fds[0]);
  int status = 0;
  if (::waitpid(pid, &status, 0) != pid) {
    if (err) *err = "waitpid failed";
    return false;
  }
  if (exit_status) {
    *exit_status = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
  }
  if (out) {
    *out = captured;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Check
// ---------------------------------------------------------------------------

bool ReadCaseOrTrace(const std::string &dir, const std::string &trace_name,
                     const std::string &obj_name, const std::string &mtl_name,
                     Asset *asset, std::string *obj_text, std::string *mtl_text,
                     std::string *err) {
  std::string trace;
  if (!read_text_file(DirPath(dir, trace_name), &trace)) {
    if (err) *err = "cannot read " + trace_name;
    return false;
  }
  if (!parse_asset(trace, asset, err)) {
    if (err) *err = "cannot parse " + trace_name + ": " + *err;
    return false;
  }
  std::string local;
  if (!validate_asset(*asset, false, &local)) {
    if (err) *err = "trace invalid: " + local;
    return false;
  }
  if (!read_text_file(DirPath(dir, obj_name), obj_text)) {
    if (err) *err = "cannot read " + obj_name;
    return false;
  }
  if (!read_text_file(DirPath(dir, mtl_name), mtl_text)) {
    if (err) *err = "cannot read " + mtl_name;
    return false;
  }
  if (obj_text->empty() || mtl_text->empty()) {
    if (err) *err = "case OBJ/MTL is empty (truncated input)";
    return false;
  }
  return true;
}

bool CheckDirectory(const std::string &dir, const std::string &worker_program,
                    bool use_worker, std::string *message) {
  Asset trace_asset;
  std::string trace_obj;
  std::string trace_mtl;
  if (!ReadCaseOrTrace(dir, "case.trace", "case.obj", "case.mtl", &trace_asset,
                       &trace_obj, &trace_mtl, message)) {
    return false;
  }

  // Independently re-interpret the case OBJ text with pool sizes at each face.
  Mismatch mismatch;
  if (!compare_obj_text_to_model(trace_asset, trace_obj, trace_mtl,
                                 "text-oracle/preserve", &mismatch)) {
    if (message) *message = describe_mismatch(mismatch);
    return false;
  }

  // Build the two normal OBJ serializations and write the worker payload.
  std::string err;
  std::vector<FaceRecord> expected;
  // The worker reads DIR directly: the two normal OBJ serializations and the
  // MTL text live next to the trace as state.orig.obj / state.pos.obj /
  // state.mtl.
  const std::string work_dir = dir;
  if (!prepare_worker_payload(trace_asset, work_dir, &expected, &err)) {
    if (message) *message = err;
    return false;
  }

  bool worker_ok = true;
  if (use_worker) {
    const std::string program = worker_program.empty() ? "obj-state" : worker_program;
    std::vector<std::string> args;
    args.push_back("worker");
    args.push_back(work_dir);
    std::string out;
    int status = 0;
    if (!spawn_capture(program, args, &out, &status, &err)) {
      if (message) *message = "worker spawn failed: " + err;
      return false;
    }
    std::string worker_message;
    worker_ok = validate_worker_reply(expected, out, status, &worker_message);
    if (!worker_ok) {
      if (message) *message = worker_message;
      return false;
    }
  } else {
    // In-process production comparison for tests: both entry points, both
    // serializations.
    std::string obj_preserve;
    std::string obj_positive;
    if (!emit_obj_text(trace_asset, false, &obj_preserve, &err) ||
        !emit_obj_text(trace_asset, true, &obj_positive, &err)) {
      if (message) *message = err;
      return false;
    }
    std::vector<FaceRecord> reader_preserve;
    std::vector<FaceRecord> load_preserve;
    std::vector<FaceRecord> reader_positive;
    std::vector<FaceRecord> load_positive;
    if (!parse_production_text(obj_preserve, trace_mtl, true, &reader_preserve,
                               &err) ||
        !parse_production_text(obj_preserve, trace_mtl, false, &load_preserve,
                               &err) ||
        !parse_production_text(obj_positive, trace_mtl, true, &reader_positive,
                               &err) ||
        !parse_production_text(obj_positive, trace_mtl, false, &load_positive,
                               &err)) {
      if (message) *message = err;
      return false;
    }
    std::vector<WorkerEntry> entries;
    WorkerEntry a;
    a.parser = "ObjReader";
    a.variant = "preserve";
    a.faces = reader_preserve;
    entries.push_back(a);
    WorkerEntry b;
    b.parser = "LoadObj";
    b.variant = "preserve";
    b.faces = load_preserve;
    entries.push_back(b);
    WorkerEntry c;
    c.parser = "ObjReader";
    c.variant = "positive";
    c.faces = reader_positive;
    entries.push_back(c);
    WorkerEntry d;
    d.parser = "LoadObj";
    d.variant = "positive";
    d.faces = load_positive;
    entries.push_back(d);
    std::string diagnostic;
    if (!compare_worker_output(expected, entries, &diagnostic)) {
      if (message) *message = diagnostic;
      return false;
    }
  }
  (void)worker_ok;

  // Also verify each serialization against the text oracle.
  std::string obj_preserve;
  std::string obj_positive;
  if (!emit_obj_text(trace_asset, false, &obj_preserve, &err) ||
      !emit_obj_text(trace_asset, true, &obj_positive, &err)) {
    if (message) *message = err;
    return false;
  }
  if (!compare_obj_text_to_model(trace_asset, obj_preserve, trace_mtl,
                                 "check/preserve", &mismatch)) {
    if (message) *message = describe_mismatch(mismatch);
    return false;
  }
  if (!compare_obj_text_to_model(trace_asset, obj_positive, trace_mtl,
                                "check/positive", &mismatch)) {
    if (message) *message = describe_mismatch(mismatch);
    return false;
  }

  // Case OBJ text itself must agree with the model.
  Mismatch case_mismatch;
  Asset case_asset;
  std::string case_obj;
  std::string case_mtl;
  if (!ReadCaseOrTrace(dir, "case.trace", "case.obj", "case.mtl", &case_asset,
                       &case_obj, &case_mtl, &err)) {
    if (message) *message = err;
    return false;
  }
  if (!compare_obj_text_to_model(case_asset, case_obj, case_mtl, "case-obj",
                                 &case_mismatch)) {
    if (message) *message = describe_mismatch(case_mismatch);
    return false;
  }

  std::ostringstream summary;
  summary << "OBJ_STATE_VALID faces=" << expected.size();
  if (message) *message = summary.str();
  return true;
}

bool obj_state_generate_dir(uint64_t seed, const std::string &dir,
                            std::string *message) {
  Asset asset;
  std::string err;
  if (!generate_asset(seed, &asset, &err)) {
    if (message) *message = err;
    return false;
  }
  std::string verify;
  if (!verify_generated_asset(asset, &verify)) {
    if (message) *message = "generated asset failed profile: " + verify;
    return false;
  }
  if (!write_case_files(asset, dir, &err)) {
    if (message) *message = err;
    return false;
  }
  std::ostringstream summary;
  summary << "OBJ_STATE_GENERATED events=" << asset.events.size()
          << " faces=" << (asset.events.size());
  if (message) *message = summary.str();
  return true;
}

bool obj_state_check_dir(const std::string &dir, const std::string &worker_program,
                         bool use_worker, std::string *message) {
  return CheckDirectory(dir, worker_program, use_worker, message);
}

namespace {

int obj_state_inner_main(int argc, char **argv) {
  std::vector<std::string> args;
  for (int i = 0; i < argc; i++) {
    args.push_back(argv[i]);
  }
  if (args.size() < 2) {
    std::cerr << "usage: obj_state_main <generate|check|reduce|worker> ...\n";
    return 2;
  }
  const std::string command = args[1];
  std::string message;
  if (command == "generate") {
    if (args.size() != 4) {
      std::cerr << "usage: obj_state_main generate SEED DIR\n";
      return 2;
    }
    char *end = NULL;
    const uint64_t seed =
        static_cast<uint64_t>(std::strtoull(args[2].c_str(), &end, 10));
    if (!end || *end != '\0') {
      std::cerr << "bad seed\n";
      return 2;
    }
    if (!obj_state_generate_dir(seed, args[3], &message)) {
      std::cerr << "generate failed: " << message << "\n";
      return 1;
    }
    std::cout << message << "\n";
    return 0;
  }
  if (command == "check") {
    if (args.size() != 3) {
      std::cerr << "usage: obj_state_main check DIR\n";
      return 2;
    }
    if (!obj_state_check_dir(args[2], args[0], true, &message)) {
      std::cerr << (message.empty() ? "check failed" : message) << "\n";
      return 1;
    }
    std::cout << message << "\n";
    return 0;
  }
  if (command == "worker") {
    if (args.size() != 3) {
      std::cerr << "usage: obj_state_main worker DIR\n";
      return 2;
    }
    std::string out;
    std::string err;
    if (!produce_worker_output(args[2], &out, &err)) {
      std::cerr << "worker failed: " << err << "\n";
      return 1;
    }
    std::cout << out;
    return 0;
  }
  if (command == "reduce") {
    std::string in_dir;
    std::string out_dir;
    std::string anchor;
    long long budget = -1;
    int positional = 0;
    for (size_t i = 2; i < args.size(); i++) {
      const std::string &arg = args[i];
      if (arg == "--anchor") {
        if (i + 1 >= args.size()) {
          std::cerr << "missing --anchor value\n";
          return 2;
        }
        anchor = args[++i];
      } else if (arg == "--budget") {
        if (i + 1 >= args.size()) {
          std::cerr << "missing --budget value\n";
          return 2;
        }
        budget = std::strtoll(args[++i].c_str(), NULL, 10);
      } else {
        if (positional == 0) {
          in_dir = arg;
        } else if (positional == 1) {
          out_dir = arg;
        } else {
          std::cerr << "unexpected argument " << arg << "\n";
          return 2;
        }
        positional++;
      }
    }
    if (positional != 2 || budget < 0) {
      std::cerr << "usage: obj_state_main reduce IN OUT --anchor F0 --budget N\n";
      return 2;
    }
    if (!obj_state_reduce_dir(in_dir, out_dir, anchor, budget, &message)) {
      std::cerr << "reduce failed: " << message << "\n";
      return 1;
    }
    std::cout << message << "\n";
    return 0;
  }
  std::cerr << "unknown command " << command << "\n";
  return 2;
}

}  // namespace

std::string describe_mismatch(const Mismatch &mismatch) {
  std::ostringstream os;
  os << "OBJ_STATE_MISMATCH face=" << mismatch.face_id;
  if (mismatch.corner >= 0) {
    os << " corner=" << mismatch.corner;
  }
  os << " component=" << mismatch.component << " entry=" << mismatch.entry
     << " expected=" << mismatch.expected << " actual=" << mismatch.actual;
  return os.str();
}

}  // namespace objstate

int obj_state_main(int argc, char **argv) {
  return objstate::obj_state_inner_main(argc, argv);
}
