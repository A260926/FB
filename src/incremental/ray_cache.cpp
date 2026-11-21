#include "ray_cache.hpp"

#include "ray_tracer.hpp"
#include "light.hpp"
#include "camera.hpp"
#include <algorithm>
#include <cmath>

namespace incremental {

namespace {

bool SameVecExact(const Vec3f& a, const Vec3f& b) {
  return a.x == b.x && a.y == b.y && a.z == b.z;
}

bool SameMaterial(const Material& a, const Material& b) {
  return a.color == b.color && a.specular == b.specular &&
         a.reflective == b.reflective && a.transparency == b.transparency &&
         a.refractive_index == b.refractive_index && a.tint == b.tint &&
         a.emission == b.emission && a.emit_strength == b.emit_strength;
}

bool ValidEditMaterial(const Material& m) {
  if (!std::isfinite(m.specular) || m.specular < 0.0f) return false;
  const auto in01 = [](float v) { return v >= 0.0f && v <= 1.0f; };
  if (!in01(m.reflective) || !in01(m.transparency) || !in01(m.tint))
    return false;
  if (!(m.refractive_index >= 1.0f && m.refractive_index <= 3.0f))
    return false;
  if (m.emit_strength != 0.0f) return false;
  return true;
}

bool FiniteBox(const AABB& b) {
  return std::isfinite(b.min.x) && std::isfinite(b.min.y) &&
         std::isfinite(b.min.z) && std::isfinite(b.max.x) &&
         std::isfinite(b.max.y) && std::isfinite(b.max.z) &&
         b.min.x <= b.max.x && b.min.y <= b.max.y && b.min.z <= b.max.z;
}

bool BoxContained(const AABB& box, const AABB& domain) {
  for (int i = 0; i < 3; ++i)
    if (box.min[i] < domain.min[i] || box.max[i] > domain.max[i])
      return false;
  return true;
}

// same bounds arithmetic as the Sphere constructor
AABB SphereBounds(const Vec3f& c, float r) {
  AABB b;
  b.min = Vec3f{c.x - r, c.y - r, c.z - r};
  b.max = Vec3f{c.x + r, c.y + r, c.z + r};
  b.Inflate(eps);
  return b;
}

}  // namespace

//---------------------------------------------------------------------
// enable / disable / edits
//---------------------------------------------------------------------

bool Engine::Enable(RayTracer& tracer, const AABB& domain) {
  if (!FiniteBox(domain)) return false;
  for (const auto& obj : tracer.objects_) {
    // the scene must not use emissive glow (halo); see README
    if (obj->material.emit_strength > eps) return false;
    // every finite object bound must be enclosed by the edit domain
    if (FiniteBox(obj->bounds) && !BoxContained(obj->bounds, domain))
      return false;
  }
  Disable();
  domain_ = domain;
  index_.Reset(domain_);
  enabled_ = true;
  return true;
}

void Engine::Disable() {
  ClearCache();
  enabled_ = false;
  edits_pending_ = false;
  pending_edits_.clear();
}

bool Engine::ApplyEdits(RayTracer& tracer,
                        const std::vector<SphereEdit>& edits) {
  if (!enabled_) return false;
  // ---- validate the whole batch first (atomic commit below) ----
  std::unordered_set<const Sphere*> seen;
  for (const SphereEdit& e : edits) {
    if (e.sphere == nullptr) return false;
    if (!seen.insert(e.sphere).second) return false;  // duplicate sphere
    // pointer ownership: the sphere must be registered with this tracer
    bool owned = false;
    for (const auto& obj : tracer.objects_)
      if (obj.get() == e.sphere) {
        owned = true;
        break;
      }
    if (!owned) return false;
    // finite values and a positive radius
    if (!std::isfinite(e.center.x) || !std::isfinite(e.center.y) ||
        !std::isfinite(e.center.z))
      return false;
    if (!std::isfinite(e.radius) || !(e.radius > 0.0f)) return false;
    // material ranges
    if (!ValidEditMaterial(e.material)) return false;
    // the new bounds must stay inside the edit domain
    if (!BoxContained(SphereBounds(e.center, e.radius), domain_))
      return false;
  }
  // ---- commit ----
  for (const SphereEdit& e : edits) {
    int slot = -1;
    for (std::size_t i = 0; i < tracer.objects_.size(); ++i)
      if (tracer.objects_[i].get() == e.sphere) {
        slot = static_cast<int>(i);
        break;
      }
    Sphere* s = e.sphere;
    // snapshot the state as of the last completed image on first touch,
    // so multiple batches merge against the final scene state
    if (!pending_edits_.count(slot)) {
      pending_edits_[slot] =
          Snapshot{s->center, s->radius, s->material, s->bounds};
    }
    s->center = e.center;
    s->radius = e.radius;
    s->material = e.material;
    s->bounds = SphereBounds(e.center, e.radius);  // sync actual bounds
  }
  if (!edits.empty()) edits_pending_ = true;
  return true;
}

//---------------------------------------------------------------------
// frame driver
//---------------------------------------------------------------------

void Engine::Trace(RayTracer& tracer, int max_reflections) {
  tracer_ = &tracer;
  objects_ = &tracer.objects_;
  lights_ = &tracer.lights_;
  camera_ = &tracer.camera_;
  image_ = &tracer.image_;
  stats_ = FrameStats{};
  index_.visit_count = 0;
  g_active_engine = this;
  tracer.rec_ = this;

  const bool rebuild = !ever_recorded_ || max_reflections != depth_ ||
                       CameraChanged() || LightsChanged() ||
                       background_version_ != tracer.background_version_ ||
                       objects_version_ != tracer.objects_version_;
  if (rebuild) {
    // an object-set change may have moved an object outside the domain;
    // fall back to the plain path in that case
    bool domain_ok = true;
    for (const auto& obj : tracer.objects_) {
      if (obj->material.emit_strength > eps ||
          (FiniteBox(obj->bounds) && !BoxContained(obj->bounds, domain_))) {
        domain_ok = false;
        break;
      }
    }
    if (!domain_ok) {
      Disable();
      tracer.rec_ = nullptr;
      g_active_engine = nullptr;
      tracer.TraceFull(max_reflections);
      return;
    }
    ClearCache();
    RecordAll(max_reflections);
    StoreFingerprints(max_reflections);
    ever_recorded_ = true;
    // the fresh record already reflects any pending edits
    edits_pending_ = false;
    pending_edits_.clear();
  } else if (edits_pending_) {
    replaying_ = true;
    Invalidate();
    Replay();
    replaying_ = false;
    edits_pending_ = false;
    pending_edits_.clear();
  } else {
    // no edits and no rebuild triggers: nothing to recompute
    stats_.pixel_reductions = root_ids_.size();
  }
  stats_.index_nodes_visited = index_.visit_count;
  tracer.rec_ = nullptr;
  g_active_engine = nullptr;
}

void Engine::ClearCache() {
  for (uint64_t rid : root_ids_) RetireSubtree(rid);
  root_ids_.clear();
  arena_.clear();
  where_.clear();
  obj_consumers_.clear();
  flags_.clear();
  queue_.clear();
  recombine_set_.clear();
  dirty_pixels_.clear();
  stack_.clear();
  current_override_ = 0;
  has_explicit_ = false;
  pending_containers_.clear();
  index_.Reset(domain_);
  live_count_ = 0;
  ever_recorded_ = false;
}

void Engine::RecordAll(int max_reflections) {
  const auto raster = tracer_->CurrentRaster();
  const std::size_t w = camera_->width();
  const std::size_t h = camera_->height();
  root_ids_.assign(w * h, 0);
  for (std::size_t row = 0; row < h; ++row) {
    for (std::size_t col = 0; col < w; ++col) {
      current_pixel_ = row * w + col;
      const Vec3u8 c = tracer_->TracePixel(raster, row, col, max_reflections);
      image_->at(row, col) = c;
      root_ids_[current_pixel_] = root_completed_id_;
      if (Node* n = Find(root_completed_id_)) n->pixel_index = current_pixel_;
    }
  }
}

//---------------------------------------------------------------------
// invalidation
//---------------------------------------------------------------------

void Engine::Invalidate() {
  for (const auto& kv : pending_edits_) {
    const int slot = kv.first;
    const Snapshot& snap = kv.second;
    Sphere* s = static_cast<Sphere*>((*objects_)[slot].get());
    const bool moved =
        !SameVecExact(s->center, snap.center) || s->radius != snap.radius;
    const bool mat_changed = !SameMaterial(s->material, snap.material);
    if (moved) {
      // look up candidates with the pre- and post-edit bounds
      QueryBox(snap.bounds);
      QueryBox(s->bounds);
    }
    if (mat_changed) {
      auto it = obj_consumers_.find(slot);
      if (it != obj_consumers_.end())
        for (uint64_t key : it->second)
          Flag(key >> 1, (key & 1) == kDepHit ? kMat : kReior);
    }
  }
}

void Engine::QueryBox(const AABB& box) {
  index_.Query(box, [this](uint64_t consumer, QueryIndex::Kind kind) {
    Flag(consumer, kind == QueryIndex::Kind::Hit
                       ? kRegeom
                       : kind == QueryIndex::Kind::Shadow ? kRelight
                                                          : kReior);
  });
}

void Engine::Flag(uint64_t id, uint32_t mask) {
  auto it = flags_.find(id);
  if (it == flags_.end())
    flags_[id] = mask;
  else
    it->second |= mask;
  queue_.insert(id);
}

//---------------------------------------------------------------------
// replay
//---------------------------------------------------------------------

void Engine::Replay() {
  // process the earliest invalidated queries first (parents before
  // children): a re-traced ancestor retires its whole subtree
  while (!queue_.empty()) {
    const uint64_t id = *queue_.begin();
    queue_.erase(queue_.begin());
    auto fit = flags_.find(id);
    if (fit == flags_.end()) continue;
    const uint32_t mask = fit->second;
    flags_.erase(fit);
    Node* np = Find(id);
    if (!np || !np->alive) continue;
    if (mask & kRegeom) {
      RegeomNode(id);
      continue;
    }
    if ((mask & kMat) && BaseStatusFlipped(*np)) {
      RegeomNode(id);
      continue;
    }
    if (mask & (kRelight | kMat)) RelightNode(*np);
    if (mask & (kReior | kMat | kIorUpd)) ReiorNode(*np);
    RecombineNode(id);
  }
  // propagate color changes up the parent chain (children first)
  while (!recombine_set_.empty()) {
    const uint64_t id = *recombine_set_.begin();
    recombine_set_.erase(recombine_set_.begin());
    Node* np = Find(id);
    if (!np || !np->alive) continue;
    RecombineNode(id);
  }
  const std::size_t w = camera_->width();
  for (uint64_t pix : dirty_pixels_) {
    Node* root = Find(root_ids_[pix]);
    if (root) image_->at(pix / w, pix % w) = root->color;
  }
  stats_.pixel_reductions = root_ids_.size() - dirty_pixels_.size();
  dirty_pixels_.clear();
}

uint64_t Engine::RecordSubtree(uint64_t parent, NodeType type,
                               const Ray& ray, int depth, float ior) {
  has_explicit_ = true;
  explicit_parent_ = parent;
  explicit_type_ = type;
  const uint64_t before = stats_.created_nodes;
  tracer_->TraceRay(ray, depth, ior);
  if (replaying_) stats_.ray_nodes_replayed += stats_.created_nodes - before;
  return root_completed_id_;
}

void Engine::RegeomNode(uint64_t id) {
  Node backup = *Find(id);  // copy: the arena grows while recording
  const uint64_t new_root = RecordSubtree(backup.parent, backup.type,
                                          backup.ray, backup.depth,
                                          backup.ior_current);
  if (backup.parent == 0) {
    root_ids_[backup.pixel_index] = new_root;
    if (Node* rn = Find(new_root)) rn->pixel_index = backup.pixel_index;
    dirty_pixels_.insert(backup.pixel_index);
  } else if (Node* p = Find(backup.parent); p && p->alive) {
    if (backup.type == NodeType::Reflection)
      p->refl_child = new_root;
    else
      p->refr_child = new_root;
    recombine_set_.insert(backup.parent);
  }
  RetireSubtree(id);
}

void Engine::RetireSubtree(uint64_t id) {
  Node* np = Find(id);
  if (!np || !np->alive) return;
  const bool hrc = np->has_refl_child;
  const bool hfc = np->has_refr_child;
  const uint64_t rc = np->refl_child;
  const uint64_t fc = np->refr_child;
  if (hrc && rc) RetireSubtree(rc);
  if (hfc && fc) RetireSubtree(fc);
  RemoveAllRecords(*np);
  np->alive = false;
  ++stats_.retired_nodes;
  --live_count_;
}

void Engine::RelightNode(Node& n) {
  if (!n.hit) return;
  RemoveShadowRecords(n);
  current_override_ = n.id;
  Object* obj = (*objects_)[n.object_slot].get();
  Vec3u8 direct =
      lights_->ColorAt(*objects_, *obj, n.hit_point, *camera_, n.normal);
  const float mat_trans =
      std::clamp(obj->material.transparency, 0.0f, 1.0f);
  direct *= (1.0f - mat_trans);
  current_override_ = 0;
  n.direct = direct;
}

void Engine::ReiorNode(Node& n) {
  if (!n.hit || n.is_base()) return;
  RemoveProbeRecords(n);
  Object* obj = (*objects_)[n.object_slot].get();
  current_override_ = n.id;
  TraceRecord rec;
  rec.hit = true;
  rec.t = n.t;
  rec.hit_point = n.hit_point;
  rec.normal = n.normal;
  rec.obj = obj;
  const auto ori = tracer_->RayOrientation(rec, n.ray.dir, n.ior_current);
  current_override_ = 0;

  const float mat_trans =
      std::clamp(obj->material.transparency, 0.0f, 1.0f);
  const float mat_refl =
      std::clamp(obj->material.reflective, 0.0f, 1.0f);

  // refraction direction under the new orientation
  const bool attempted = mat_trans > eps;
  bool tir = false;
  Vec3f refr_dir{};
  if (attempted) {
    const auto r = tracer_->Refract(n.ray.dir, ori.normal, ori.eta,
                                    ori.cos_i);
    tir = r.first;
    refr_dir = r.second;
  }
  const bool want_child = attempted && !tir;
  const bool stale = attempted != n.refr_attempted ||
                     (attempted && tir != n.tir) ||
                     (want_child && !SameVecExact(refr_dir, n.refr_dir));
  if (stale) {
    if (n.has_refr_child) {
      const uint64_t old = n.refr_child;
      n.refr_child = 0;
      n.has_refr_child = false;
      RetireSubtree(old);
    }
    if (want_child) {
      Ray child_ray{Vec3f{0, 0, 0}, Vec3f{0, 0, 1}};
      child_ray.origin =
          n.hit_point + refr_dir * eps * RayTracer::eps_factor;
      child_ray.dir = refr_dir;
      n.refr_child = RecordSubtree(n.id, NodeType::Refraction, child_ray,
                                   n.depth - 1, ori.n2);
      n.has_refr_child = true;
    }
  }
  n.refr_attempted = attempted;
  n.tir = tir;
  n.refr_dir = refr_dir;

  // Fresnel weights (same arithmetic as TraceRay)
  const float R = tracer_->Schlick(ori.n1, ori.n2, ori.cos_i);
  float trans_weight = mat_trans * (1.0f - R);
  float refl_weight = mat_refl + R * mat_trans;
  if (attempted && tir) {
    trans_weight = 0.0f;
    refl_weight = std::min(1.0f, refl_weight + mat_trans);
  }
  n.refl_weight = refl_weight;
  n.trans_weight = trans_weight;
  n.w_direct = 1.0f - std::min(refl_weight + trans_weight, 1.0f);
  n.tint_w = want_child ? obj->material.tint * mat_trans : 0.0f;
  n.out_n1 = ori.n1;
  n.out_n2 = ori.n2;

  // propagate the medium IOR to the children that keep their rays
  if (n.has_refl_child) {
    if (Node* c = Find(n.refl_child);
        c && c->alive && c->ior_current != ori.n1) {
      c->ior_current = ori.n1;
      Flag(c->id, kIorUpd);
    }
  }
  if (n.has_refr_child) {
    if (Node* c = Find(n.refr_child);
        c && c->alive && c->ior_current != ori.n2) {
      c->ior_current = ori.n2;
      Flag(c->id, kIorUpd);
    }
  }
}

void Engine::RecombineNode(uint64_t id) {
  Node* np = Find(id);
  if (!np || !np->alive) return;
  Node& n = *np;
  Vec3u8 c;
  if (!n.hit || n.is_base()) {
    c = n.direct;
  } else {
    const Vec3u8 refl_col =
        n.has_refl_child ? Find(n.refl_child)->color : Vec3u8{0, 0, 0};
    Vec3u8 refr_color{0, 0, 0};
    if (n.has_refr_child) {
      refr_color = Find(n.refr_child)->color;
      // tint heuristic (same arithmetic as TraceRay)
      const Vec3u8& cc = (*objects_)[n.object_slot]->material.color;
      const float tint_w = n.tint_w;
      auto ApplyTint = [&](uint8_t col_next, uint8_t color_curr) {
        const float curr_norm = static_cast<float>(color_curr) / 255.0f;
        const float w = (1.0f - tint_w) + tint_w * curr_norm;
        return static_cast<uint8_t>(std::min(255.0f, col_next * w));
      };
      refr_color = Vec3u8{ApplyTint(refr_color.x, cc.x),
                          ApplyTint(refr_color.y, cc.y),
                          ApplyTint(refr_color.z, cc.z)};
    }
    c = Vec3u8{
        static_cast<uint8_t>(n.direct.x * n.w_direct +
                             refl_col.x * n.refl_weight +
                             refr_color.x * n.trans_weight),
        static_cast<uint8_t>(n.direct.y * n.w_direct +
                             refl_col.y * n.refl_weight +
                             refr_color.y * n.trans_weight),
        static_cast<uint8_t>(n.direct.z * n.w_direct +
                             refl_col.z * n.refl_weight +
                             refr_color.z * n.trans_weight)};
  }
  if (c != n.color) {
    n.color = c;
    if (n.parent)
      recombine_set_.insert(n.parent);
    else
      dirty_pixels_.insert(n.pixel_index);
  }
}

bool Engine::BaseStatusFlipped(const Node& n) const {
  if (!n.hit) return false;
  const Object* obj = (*objects_)[n.object_slot].get();
  const bool now_base =
      (n.depth <= 0) ||
      (std::clamp(obj->material.reflective, 0.0f, 1.0f) < eps &&
       std::clamp(obj->material.transparency, 0.0f, 1.0f) < eps);
  return now_base != n.is_base();
}

//---------------------------------------------------------------------
// recorder hooks
//---------------------------------------------------------------------

void Engine::Begin(const Ray& ray, int depth, float ior_current) {
  Node n;
  n.id = next_id_++;
  n.ray = ray;
  n.depth = depth;
  n.ior_current = ior_current;
  if (stack_.empty()) {
    if (has_explicit_) {
      n.parent = explicit_parent_;
      n.type = explicit_type_;
      has_explicit_ = false;
    }
  } else {
    n.parent = stack_.back();
    n.type = pending_child_type_;
    Node& p = arena_[where_[n.parent]];
    if (n.type == NodeType::Reflection) {
      p.refl_child = n.id;
      p.has_refl_child = true;
    } else {
      p.refr_child = n.id;
      p.has_refr_child = true;
    }
  }
  where_[n.id] = static_cast<uint32_t>(arena_.size());
  const uint64_t id = n.id;
  arena_.push_back(std::move(n));
  stack_.push_back(id);
  ++stats_.created_nodes;
  ++live_count_;
}

void Engine::OnHitQuery(bool hit, float t, const Vec3f& hit_point,
                        const Vec3f& normal, int object_slot,
                        const Ray& ray) {
  Node& n = arena_[where_[CurrentNodeId()]];
  n.hit = hit;
  n.t = t;
  n.hit_point = hit_point;
  n.normal = normal;
  n.object_slot = object_slot;
  if (hit) {
    // the nearest-hit query depends on the closed segment to the old hit
    n.hit_handle = index_.Insert(n.id, QueryIndex::Kind::Hit, ray.origin,
                                 ray.origin + ray.dir * t);
    AddObjDep(n, object_slot, kDepHit);
  } else {
    // on a miss it depends on the whole forward ray, conservatively
    // truncated to the fixed edit domain
    float t0, t1;
    if (ClipRayToDomain(ray.origin, ray.dir, t0, t1))
      n.hit_handle =
          index_.Insert(n.id, QueryIndex::Kind::Hit,
                        ray.origin + ray.dir * t0,
                        ray.origin + ray.dir * t1);
  }
}

void Engine::OnOrientation(float n1, float n2) {
  Node& n = arena_[where_[CurrentNodeId()]];
  n.out_n1 = n1;
  n.out_n2 = n2;
}

void Engine::ExpectChild(NodeType type) { pending_child_type_ = type; }

void Engine::OnRefraction(const Vec3f& dir, bool tir, bool attempted) {
  Node& n = arena_[where_[CurrentNodeId()]];
  n.refr_dir = dir;
  n.tir = tir;
  n.refr_attempted = attempted;
}

void Engine::SetDirect(const Vec3u8& direct) {
  arena_[where_[CurrentNodeId()]].direct = direct;
}

void Engine::SetWeights(float w_direct, float refl_weight,
                        float trans_weight, float tint_w) {
  Node& n = arena_[where_[CurrentNodeId()]];
  n.w_direct = w_direct;
  n.refl_weight = refl_weight;
  n.trans_weight = trans_weight;
  n.tint_w = tint_w;
}

void Engine::End(const Vec3u8& color) {
  Node& n = arena_[where_[stack_.back()]];
  n.color = color;
  if (!n.hit) n.direct = color;  // miss node: background color
  stack_.pop_back();
  if (stack_.empty()) root_completed_id_ = n.id;
}

void Engine::OnProbeContainer(int slot) {
  pending_containers_.push_back(slot);
}

void Engine::OnProbePoint(const Vec3f& point) {
  Node& n = arena_[where_[CurrentNodeId()]];
  const uint64_t h =
      index_.Insert(n.id, QueryIndex::Kind::Probe, point, point);
  if (h) n.probe_handles.push_back(h);
  for (int slot : pending_containers_) AddObjDep(n, slot, kDepProbe);
  pending_containers_.clear();
  if (replaying_) ++stats_.probe_queries_replayed;
}

void Engine::RecordShadowQuery(const Vec3f& origin, const Vec3f& dir,
                               float t_max) {
  Node& n = arena_[where_[CurrentNodeId()]];
  float t1 = t_max;
  if (!std::isfinite(t1)) {
    // directional light: truncate the infinite ray to the edit domain
    float t0;
    if (!ClipRayToDomain(origin, dir, t0, t1)) {
      if (replaying_) ++stats_.shadow_queries_replayed;
      return;
    }
  }
  const uint64_t h = index_.Insert(n.id, QueryIndex::Kind::Shadow, origin,
                                   origin + dir * t1);
  if (h) n.shadow_handles.push_back(h);
  if (replaying_) ++stats_.shadow_queries_replayed;
}

//---------------------------------------------------------------------
// record removal helpers
//---------------------------------------------------------------------

void Engine::AddObjDep(Node& n, int slot, uint8_t kind) {
  n.obj_deps.emplace_back(slot, kind);
  obj_consumers_[slot].insert((n.id << 1) | kind);
}

void Engine::RemoveShadowRecords(Node& n) {
  for (uint64_t h : n.shadow_handles) index_.Remove(h);
  n.shadow_handles.clear();
}

void Engine::RemoveProbeRecords(Node& n) {
  for (uint64_t h : n.probe_handles) index_.Remove(h);
  n.probe_handles.clear();
  auto& deps = n.obj_deps;
  for (std::size_t i = 0; i < deps.size();) {
    if (deps[i].second == kDepProbe) {
      obj_consumers_[deps[i].first].erase((n.id << 1) | kDepProbe);
      deps[i] = deps.back();
      deps.pop_back();
    } else {
      ++i;
    }
  }
}

void Engine::RemoveAllRecords(Node& n) {
  if (n.hit_handle) index_.Remove(n.hit_handle);
  RemoveShadowRecords(n);
  RemoveProbeRecords(n);
  for (const auto& d : n.obj_deps)
    obj_consumers_[d.first].erase((n.id << 1) | d.second);
  n.obj_deps.clear();
}

//---------------------------------------------------------------------
// misc helpers
//---------------------------------------------------------------------

Engine::Node* Engine::Find(uint64_t id) {
  const auto it = where_.find(id);
  return it == where_.end() ? nullptr : &arena_[it->second];
}

const Engine::Node* Engine::Find(uint64_t id) const {
  const auto it = where_.find(id);
  return it == where_.end() ? nullptr : &arena_[it->second];
}

uint64_t Engine::CurrentNodeId() const {
  return current_override_ ? current_override_ : stack_.back();
}

bool Engine::ClipRayToDomain(const Vec3f& o, const Vec3f& d, float& t0,
                             float& t1) const {
  t0 = 0.0f;
  t1 = std::numeric_limits<float>::infinity();
  for (int i = 0; i < 3; ++i) {
    const float mn = domain_.min[i];
    const float mx = domain_.max[i];
    if (std::abs(d[i]) < 1e-12f) {
      if (o[i] < mn || o[i] > mx) return false;
      continue;
    }
    const float inv = 1.0f / d[i];
    float ta = (mn - o[i]) * inv;
    float tb = (mx - o[i]) * inv;
    if (ta > tb) std::swap(ta, tb);
    t0 = std::max(t0, ta);
    t1 = std::min(t1, tb);
    if (t0 > t1) return false;
  }
  return t1 > 0.0f;
}

bool Engine::CameraChanged() const {
  if (!SameVecExact(cam_center_, camera_->center()) ||
      cam_depth_ != camera_->depth() || cam_w_ != camera_->width() ||
      cam_h_ != camera_->height())
    return true;
  const auto corners = camera_->CornersWorld();
  for (int i = 0; i < 4; ++i)
    if (!SameVecExact(cam_corners_[i], corners[i])) return true;
  return false;
}

bool Engine::LightsChanged() const {
  const auto& items = lights_->items();
  if (items.size() != lights_print_.size()) return true;
  for (std::size_t i = 0; i < items.size(); ++i) {
    const Light& l = items[i];
    const LightPrint& p = lights_print_[i];
    if (static_cast<int>(l.type) != p.type || l.intensity != p.intensity)
      return true;
    const bool has = l.data.has_value();
    if (has != p.has_data) return true;
    if (has && !SameVecExact(*l.data, p.data)) return true;
  }
  return false;
}

void Engine::StoreFingerprints(int max_reflections) {
  depth_ = max_reflections;
  cam_center_ = camera_->center();
  cam_depth_ = camera_->depth();
  cam_w_ = camera_->width();
  cam_h_ = camera_->height();
  cam_corners_ = camera_->CornersWorld();
  lights_print_.clear();
  for (const Light& l : lights_->items())
    lights_print_.push_back(LightPrint{static_cast<int>(l.type),
                                       l.intensity, l.data.has_value(),
                                       l.data.value_or(Vec3f{})});
  background_version_ = tracer_->background_version_;
  objects_version_ = tracer_->objects_version_;
}

NodeInfo Engine::MakeInfo(const Node& n) {
  NodeInfo info;
  info.id = n.id;
  info.parent_id = n.parent;
  info.type = n.type;
  info.object_slot = n.object_slot;
  info.reflection_child = n.has_refl_child ? n.refl_child : 0;
  info.refraction_child = n.has_refr_child ? n.refr_child : 0;
  info.alive = n.alive;
  return info;
}

std::vector<NodeInfo> Engine::LiveNodes() const {
  std::vector<NodeInfo> out;
  out.reserve(live_count_);
  for (const Node& n : arena_)
    if (n.alive) out.push_back(MakeInfo(n));
  return out;
}

bool Engine::NodeInfoOf(uint64_t id, NodeInfo& out) const {
  const Node* n = Find(id);
  if (!n) return false;
  out = MakeInfo(*n);
  return true;
}

uint64_t Engine::RootId(unsigned row, unsigned col) const {
  if (!camera_) return 0;
  const std::size_t w = camera_->width();
  const std::size_t h = camera_->height();
  if (row >= h || col >= w) return 0;
  const std::size_t idx = static_cast<std::size_t>(row) * w + col;
  if (idx >= root_ids_.size()) return 0;
  return root_ids_[idx];
}

}  // namespace incremental
