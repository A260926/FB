#ifndef CGLTF_STRUCTURED_DRIVER_H
#define CGLTF_STRUCTURED_DRIVER_H

#ifdef __cplusplus
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

extern "C" {
#endif

/* Public entry point. argv must include argv[0]. */
int cgltf_defense_main(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
namespace cgltf_structured {

static const int FORMAT_VERSION = 1;
static const std::string KEEP_SHARED_STRIDED = "shared-strided";

struct Vertex {
    std::string id;
    float x, y, z;
    Vertex() : x(0.0f), y(0.0f), z(0.0f) {}
    Vertex(const std::string &id_, float x_, float y_, float z_)
        : id(id_), x(x_), y(y_), z(z_) {}
};

struct Triangle {
    std::string id;
    std::string v[3];
    Triangle() {}
    Triangle(const std::string &id_, const std::string &a,
             const std::string &b, const std::string &c)
        : id(id_) {
        v[0] = a; v[1] = b; v[2] = c;
    }
};

struct BufferModel {
    std::string id;
    std::vector<unsigned char> bytes;
};

struct ViewModel {
    std::string id;
    std::string buffer;
    std::size_t offset;
    std::size_t size;
    std::size_t stride; /* 0 means tightly packed / no byteStride */
    int target;
    ViewModel() : offset(0), size(0), stride(0), target(0) {}
};

struct AccessorModel {
    std::string id;
    std::string role; /* POSITION or INDEX */
    int component_type;
    std::string type; /* VEC3 or SCALAR */
    std::string view;
    std::size_t offset; /* accessor.byteOffset */
    std::size_t explicit_stride; /* desired cgltf accessor.stride; 0 = natural */
    std::vector<Vertex> vertices; /* POSITION, compact stable order */
    std::vector<std::uint16_t> index_values; /* synchronized INDEX values */
    AccessorModel()
        : component_type(5126), offset(0), explicit_stride(0) {}
    std::size_t count() const {
        return role == "POSITION" ? vertices.size() : index_values.size();
    }
    std::size_t element_components() const { return type == "VEC3" ? 3u : 1u; }
    std::size_t element_size() const {
        return element_components() * (component_type == 5126 ? 4u : 2u);
    }
};

struct PrimModel {
    std::string id;
    std::string mesh;
    std::string indices;
    std::string position;
    int mode;
    std::vector<Triangle> triangles;
    PrimModel() : mode(4) {}
};

struct MeshModel {
    std::string id;
};

struct NodeModel {
    std::string id;
    std::string mesh;
    std::vector<std::string> children;
};

struct SceneModel {
    std::string id;
    std::vector<std::string> roots;
};

struct Plan {
    std::uint64_t seed;
    std::vector<BufferModel> buffers;
    std::vector<ViewModel> views;
    std::vector<AccessorModel> accessors;
    std::vector<PrimModel> prims;
    std::vector<MeshModel> meshes;
    std::vector<NodeModel> nodes;
    SceneModel scene;
    Plan() : seed(0) {}
};

struct Metrics {
    std::size_t objects;
    std::size_t triangles;
    std::size_t vertices;
    std::size_t buffer_bytes;
    std::size_t text_bytes;
    Metrics()
        : objects(0), triangles(0), vertices(0), buffer_bytes(0),
          text_bytes(0) {}
    bool lex_less(const Metrics &other) const {
        if (objects != other.objects) return objects < other.objects;
        if (triangles != other.triangles) return triangles < other.triangles;
        if (vertices != other.vertices) return vertices < other.vertices;
        if (buffer_bytes != other.buffer_bytes)
            return buffer_bytes < other.buffer_bytes;
        return text_bytes < other.text_bytes;
    }
    bool operator==(const Metrics &o) const {
        return objects == o.objects && triangles == o.triangles &&
               vertices == o.vertices && buffer_bytes == o.buffer_bytes &&
               text_bytes == o.text_bytes;
    }
};

struct Witness {
    bool found;
    std::string accessor_id;
    std::string first_prim;
    std::string second_prim;
    std::size_t offset;
    std::size_t stride;
    Witness() : found(false), offset(0), stride(0) {}
};

struct VerifyResult {
    bool ok;
    std::string first_id;
    std::string element;
    std::string message;
    VerifyResult() : ok(false) {}
    static VerifyResult failure(const std::string &id,
                                const std::string &element,
                                const std::string &message) {
        VerifyResult r;
        r.ok = false;
        r.first_id = id;
        r.element = element;
        r.message = message;
        return r;
    }
    static VerifyResult success() {
        VerifyResult r;
        r.ok = true;
        r.message = "OK";
        return r;
    }
};

struct ProcResult {
    bool spawn_ok;
    int exit_code;
    bool signaled;
    int signal_number;
    std::string output;
    ProcResult()
        : spawn_ok(false), exit_code(-1), signaled(false), signal_number(0) {}
    bool clean_exit_zero() const {
        return spawn_ok && !signaled && exit_code == 0;
    }
};

struct ProdBuffer {
    std::size_t index;
    std::string name;
    std::size_t size;
};

struct ProdView {
    std::size_t index;
    std::string name;
    int buffer;
    std::size_t offset;
    std::size_t size;
    std::size_t stride;
};

struct ProdAccessor {
    std::size_t index;
    std::string name;
    int component_type;
    std::string type;
    std::size_t count;
    int view;
    std::size_t offset;
    std::size_t stride;
};

struct ProdPrim {
    std::size_t mesh_index;
    std::size_t prim_index;
    int mode;
    int indices;
    int position;
};

struct ProdNode {
    std::size_t index;
    std::string name;
    int mesh;
    std::vector<std::size_t> children;
};

struct ProdShare {
    std::string kind;
    int accessor;
    std::size_t use_count;
    std::vector<std::string> users;
};

struct ProdValues {
    int accessor;
    std::size_t count;
    std::size_t components;
    std::vector<double> values;
};

struct ProductionData {
    std::string gltf_version;
    std::size_t scene;
    std::vector<ProdBuffer> buffers;
    std::vector<ProdView> views;
    std::vector<ProdAccessor> accessors;
    std::vector<ProdPrim> prims;
    std::vector<ProdNode> nodes;
    std::vector<std::vector<std::size_t> > scenes;
    std::vector<ProdShare> shares;
    std::vector<ProdValues> values;
    bool complete;
    ProductionData() : scene(0), complete(false) {}
};

enum CandidatePhase {
    PHASE_OUTSIDE = 0,
    PHASE_NODE = 1,
    PHASE_PRIMITIVE = 2,
    PHASE_RESOURCE = 3,
    PHASE_TRIANGLE = 4
};

struct Candidate {
    std::size_t index;
    CandidatePhase phase;
    std::string phase_name;
    std::string id;
    std::string target_kind;
    std::string target_id;
    std::string prim_id;
    std::string triangle_id;
    Candidate() : index(0), phase(PHASE_OUTSIDE) {}
};

struct ReduceOptions {
    std::string in_dir;
    std::string out_dir;
    std::string keep;
    std::uint64_t budget;
    bool resume;
    typedef std::function<VerifyResult(const std::string &stage_dir)> Verifier;
    Verifier verifier;
    ReduceOptions() : budget(0), resume(false) {}
};

struct ReduceOutcome {
    int return_code;
    std::string message;
    std::uint64_t observed;
    std::uint64_t accepted;
    std::size_t next_index;
    std::size_t total_candidates;
    bool done;
    ReduceOutcome()
        : return_code(0), observed(0), accepted(0), next_index(0),
          total_candidates(0), done(false) {}
};

/* model.cpp */
std::string temp_directory(const std::string &prefix);
bool make_directories(const std::string &path, std::string *error);
bool read_file(const std::string &path, std::string *out, std::string *error);
bool write_file_atomic(const std::string &path, const std::string &data,
                       std::string *error);

std::string plan_to_string(const Plan &plan);
bool plan_from_string(const std::string &text, Plan *plan,
                      std::string *error);
bool save_plan(const Plan &plan, const std::string &path,
               std::string *error);
bool load_plan(const std::string &path, Plan *plan, std::string *error);

Plan generate_plan(std::uint64_t seed);
bool generate_to_dir(std::uint64_t seed, const std::string &dir,
                     std::string *error);

int find_buffer(const Plan &p, const std::string &id);
int find_view(const Plan &p, const std::string &id);
int find_accessor(const Plan &p, const std::string &id);
int find_prim(const Plan &p, const std::string &id);
int find_mesh(const Plan &p, const std::string &id);
int find_node(const Plan &p, const std::string &id);

Metrics compute_metrics(const Plan &p, std::size_t text_bytes);
Witness find_witness(const Plan &p);
std::vector<std::string> scene_closure(const Plan &p);
bool has_required_keep(const Plan &p, const std::string &keep,
                       std::string *reason);

/* layout.cpp */
std::string base64_encode(const std::vector<unsigned char> &data);
void sync_index_values(Plan *p);
void recompute_bounds(const AccessorModel &a, float *out_min, float *out_max,
                      bool *has_bounds);
void rebuild_layout(Plan *p);
std::string serialize_gltf(const Plan &p);
bool write_case_files(Plan *plan, const std::string &dir,
                      Metrics *metrics, std::string *error);
VerifyResult verify_directory_real(const std::string &dir);

/* shrink.cpp */
std::vector<Candidate> build_candidates(const Plan &source);
bool apply_candidate(Plan *plan, const Candidate &candidate,
                     std::string *reason);
void collect_used_vertices_and_compact(Plan *p);
bool delete_node_subtree(Plan *p, const std::string &id);
bool delete_prim(Plan *p, const std::string &id);
bool delete_resource_if_unreferenced(Plan *p, const std::string &id,
                                     std::string *reason);
ReduceOutcome reduce_run(const ReduceOptions &options);

/* driver.cpp process/production helpers */
std::vector<char *> exec_argv_storage(const std::vector<std::string> &argv,
                                      std::vector<char> *storage);
ProcResult run_process(const std::vector<std::string> &argv);
ProcResult run_worker_process(const std::string &self_exe,
                              const std::string &dir);
bool parse_worker_output(const std::string &text, ProductionData *out,
                         std::string *error);
VerifyResult compare_production(const Plan &expected,
                                const ProductionData &actual);
VerifyResult evaluate_worker_result(const Plan &expected,
                                    const ProcResult &result);
VerifyResult production_read_gltf(const std::string &gltf_path,
                                  ProductionData *data);
int worker_main(const std::string &dir);
int check_main(const std::string &dir, const std::string &self_exe);
int reduce_command(int argc, char **argv, const std::string &self_exe);

} // namespace cgltf_structured
#endif /* __cplusplus */

#endif
