#ifndef INCREMENTAL_RAY_CACHE_HPP_
#define INCREMENTAL_RAY_CACHE_HPP_

// Incremental preview for the ray tracer.
//
// When enabled, the first Trace() records the full ray tree: every
// recursive node stores its incident ray, remaining depth, current IOR,
// real hit, local (direct) lighting and its reflection/refraction
// children, plus the queries the node consumed:
//   - the nearest-hit query (closed segment origin->old hit, or the whole
//     forward ray truncated to the fixed edit domain on a miss),
//   - one shadow query per light that reached Lights::ShadowFactor
//     (full finite point-light segment or directional ray truncated to
//     the edit domain - never just the old occluder),
//   - one point probe per SurroundingIOR call.
//
// Finite segments/points live in a reverse spatial index (QueryIndex);
// object hits and material/IOR reads additionally live in an
// object -> consumers reverse map. SphereEdit batches are validated and
// applied atomically; the next Trace() then invalidates only the
// consumers found via the pre/post-edit bounds and replays the earliest
// invalidated queries, recombining colors up the parent chain. The result
// is byte-identical to a full Trace() of the same scene with the
// incremental path disabled. See README.md ("Incremental preview").

#include "vec.hpp"
#include "ray.hpp"
#include "objects.hpp"
#include "query_index.hpp"
#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

class RayTracer;
class Lights;
class Camera;

namespace incremental {

// One atomic sphere replacement: new center, radius and material.
// `sphere` must be owned by (registered with) the RayTracer.
struct SphereEdit {
  Sphere* sphere = nullptr;
  Vec3f center{0.0f, 0.0f, 0.0f};
  float radius = 0.0f;
  Material material{};
};

// Per-frame work counters of the incremental engine (see README).
struct FrameStats {
  uint64_t index_nodes_visited = 0;   // grid nodes touched in the index
  uint64_t object_tests = 0;          // Object::Intersects calls
  uint64_t ray_nodes_replayed = 0;    // ray nodes re-traced this frame
  uint64_t shadow_queries_replayed = 0;   // shadow queries re-evaluated
  uint64_t probe_queries_replayed = 0;    // medium probes re-evaluated
  uint64_t pixel_reductions = 0;      // pixels not recomputed this frame
  uint64_t created_nodes = 0;         // ray nodes created this frame
  uint64_t retired_nodes = 0;         // ray nodes retired this frame
};

enum class NodeType : uint8_t { Root = 0, Reflection = 1, Refraction = 2 };

// Public read-only view of one recorded ray node.
struct NodeInfo {
  uint64_t id = 0;               // monotonic, never reused
  uint64_t parent_id = 0;        // consumer parent (0 for pixel roots)
  NodeType type = NodeType::Root;
  int object_slot = -1;          // hit object's slot, -1 on miss
  uint64_t reflection_child = 0;
  uint64_t refraction_child = 0;
  bool alive = false;
};

class Engine {
public:
  Engine() = default;

  // Validate the edit domain against the scene and enable the incremental
  // path. Returns false (and changes nothing) when the domain is not
  // finite, when some object's finite bounds are not enclosed by it, or
  // when the scene uses emissive glow (emit_strength > 0).
  bool Enable(RayTracer& tracer, const AABB& domain);
  void Disable();
  bool enabled() const { return enabled_; }

  // Validate the whole batch, then atomically commit it (syncing each
  // Sphere's bounds). On rejection geometry, image and cache are
  // untouched and false is returned.
  bool ApplyEdits(RayTracer& tracer, const std::vector<SphereEdit>& edits);

  // Drives one frame: cold record, replay, or no-op.
  void Trace(RayTracer& tracer, int max_reflections);

  const FrameStats& stats() const { return stats_; }
  std::vector<NodeInfo> LiveNodes() const;
  bool NodeInfoOf(uint64_t id, NodeInfo& out) const;
  uint64_t RootId(unsigned row, unsigned col) const;

  // ---- recorder hooks (driven by RayTracer/Lights, not public API) ----
  void Begin(const Ray& ray, int depth, float ior_current);
  void OnHitQuery(bool hit, float t, const Vec3f& hit_point,
                  const Vec3f& normal, int object_slot, const Ray& ray);
  void OnOrientation(float n1, float n2);
  void ExpectChild(NodeType type);
  void OnRefraction(const Vec3f& dir, bool tir, bool attempted);
  void SetDirect(const Vec3u8& direct);
  void SetWeights(float w_direct, float refl_weight, float trans_weight,
                  float tint_w);
  void End(const Vec3u8& color);
  void OnProbeContainer(int slot);
  void OnProbePoint(const Vec3f& point);
  void RecordShadowQuery(const Vec3f& origin, const Vec3f& dir,
                         float t_max);
  void CountObjectTest() { ++stats_.object_tests; }

private:
  // object -> consumer dependency kinds (stored as low bit of the key)
  static constexpr uint8_t kDepHit = 0;    // node hit this object
  static constexpr uint8_t kDepProbe = 1;  // node read this object's IOR

  // invalidation flags
  static constexpr uint32_t kRegeom = 1u;   // re-trace the node
  static constexpr uint32_t kRelight = 2u;  // re-run direct lighting
  static constexpr uint32_t kReior = 4u;    // re-run orientation/weights
  static constexpr uint32_t kMat = 8u;      // hit material changed
  static constexpr uint32_t kIorUpd = 16u;  // parent's medium IOR changed

  struct Node {
    uint64_t id = 0;
    uint64_t parent = 0;
    NodeType type = NodeType::Root;
    bool alive = true;
    // inputs
    Ray ray{Vec3f{0, 0, 0}, Vec3f{0, 0, 1}};
    int depth = 0;
    float ior_current = 1.0f;
    std::size_t pixel_index = 0;  // roots only
    // real hit snapshot
    bool hit = false;
    float t = 0.0f;
    Vec3f hit_point{};
    Vec3f normal{};
    int object_slot = -1;
    // local lighting (after the (1 - transparency) scaling); for miss
    // nodes this holds the background color
    Vec3u8 direct{0, 0, 0};
    Vec3u8 color{0, 0, 0};
    // combine terms (nodes with children)
    float w_direct = 1.0f;
    float refl_weight = 0.0f;
    float trans_weight = 0.0f;
    float tint_w = 0.0f;
    float out_n1 = 1.0f;  // IOR handed to the reflection child
    float out_n2 = 1.0f;  // IOR handed to the refraction child
    bool refr_attempted = false;
    bool tir = false;
    Vec3f refr_dir{};
    // children
    bool has_refl_child = false;
    bool has_refr_child = false;
    uint64_t refl_child = 0;
    uint64_t refr_child = 0;
    bool is_base() const { return !has_refl_child && !has_refr_child; }
    // owned dependency records
    uint64_t hit_handle = 0;
    std::vector<uint64_t> shadow_handles;
    std::vector<uint64_t> probe_handles;
    std::vector<std::pair<int, uint8_t>> obj_deps;  // (slot, dep kind)
  };

  struct Snapshot {
    Vec3f center{};
    float radius = 0.0f;
    Material material{};
    AABB bounds{};
  };

  struct LightPrint {
    int type = 0;
    float intensity = 0.0f;
    bool has_data = false;
    Vec3f data{};
  };

  Node* Find(uint64_t id);
  const Node* Find(uint64_t id) const;
  uint64_t CurrentNodeId() const;
  void AddObjDep(Node& n, int slot, uint8_t kind);

  void ClearCache();
  void RecordAll(int max_reflections);
  void Invalidate();
  void Replay();

  uint64_t RecordSubtree(uint64_t parent, NodeType type, const Ray& ray,
                         int depth, float ior);
  void RegeomNode(uint64_t id);
  void RetireSubtree(uint64_t id);
  void RelightNode(Node& n);
  void ReiorNode(Node& n);
  void RecombineNode(uint64_t id);
  bool BaseStatusFlipped(const Node& n) const;

  void QueryBox(const AABB& box);
  void Flag(uint64_t id, uint32_t mask);
  void RemoveShadowRecords(Node& n);
  void RemoveProbeRecords(Node& n);
  void RemoveAllRecords(Node& n);

  bool ClipRayToDomain(const Vec3f& o, const Vec3f& d, float& t0,
                       float& t1) const;

  bool CameraChanged() const;
  bool LightsChanged() const;
  void StoreFingerprints(int max_reflections);

  static NodeInfo MakeInfo(const Node& n);

  RayTracer* tracer_ = nullptr;
  const std::vector<std::unique_ptr<Object>>* objects_ = nullptr;
  Lights* lights_ = nullptr;
  const Camera* camera_ = nullptr;
  Image* image_ = nullptr;

  bool enabled_ = false;
  bool ever_recorded_ = false;
  bool edits_pending_ = false;
  bool replaying_ = false;
  AABB domain_{};

  QueryIndex index_;
  std::deque<Node> arena_;  // stable storage; nodes are never erased
  std::unordered_map<uint64_t, uint32_t> where_;  // id -> arena index
  uint64_t next_id_ = 1;    // monotonic; retired ids are never reused
  std::vector<uint64_t> root_ids_;  // one root per pixel
  std::unordered_map<uint64_t, std::unordered_set<uint64_t>> obj_consumers_;
  uint64_t live_count_ = 0;

  // recorder state
  std::vector<uint64_t> stack_;
  uint64_t current_override_ = 0;
  bool has_explicit_ = false;
  uint64_t explicit_parent_ = 0;
  NodeType explicit_type_ = NodeType::Root;
  NodeType pending_child_type_ = NodeType::Root;
  uint64_t root_completed_id_ = 0;
  std::size_t current_pixel_ = 0;
  std::vector<int> pending_containers_;

  // pending (merged) edits since the last completed image
  std::unordered_map<int, Snapshot> pending_edits_;

  // replay state
  std::unordered_map<uint64_t, uint32_t> flags_;
  std::set<uint64_t> queue_;
  std::set<uint64_t, std::greater<uint64_t>> recombine_set_;
  std::unordered_set<uint64_t> dirty_pixels_;

  // full-rebuild fingerprints
  int depth_ = -1;
  Vec3f cam_center_{};
  float cam_depth_ = 0.0f;
  std::size_t cam_w_ = 0;
  std::size_t cam_h_ = 0;
  std::array<Vec3f, 4> cam_corners_{};
  std::vector<LightPrint> lights_print_;
  uint64_t background_version_ = 0;
  uint64_t objects_version_ = 0;

  FrameStats stats_;
};

// While an engine is tracing/recording this points to it, so that the
// static lighting code (Lights::ShadowFactor) can count its intersection
// tests and record its shadow queries. Single-threaded use only.
inline Engine* g_active_engine = nullptr;

}  // namespace incremental

#endif  // INCREMENTAL_RAY_CACHE_HPP_
