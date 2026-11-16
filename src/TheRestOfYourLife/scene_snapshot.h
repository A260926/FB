#ifndef SCENE_SNAPSHOT_H
#define SCENE_SNAPSHOT_H
//==============================================================================================
// Persistent, forkable scene snapshots for preview rendering.
//
// A scene_snapshot is an immutable version of the whole scene. It copies the
// *metadata* describing prototypes and instances, and it independently owns
// everything it needs to answer spatial and lighting queries, so it stays
// valid after the construction input, any parent snapshot, or any edit caller
// has been destroyed.
//
// Immutable resource contract
// ----------------------------
// The hittable object handed over in a scene_prototype is shared, never deep
// copied: geometry, materials and BLAS (e.g. a bvh_node inside a prototype) are
// built once and reused by every instance and every forked snapshot that
// references that prototype. In exchange, the caller MUST NOT mutate a
// prototype's hittable (or anything reachable through it, such as its materials
// or an inner BVH) after handing it to a snapshot. A snapshot only controls its
// own per-version descriptions and the state derived from them; it never
// mutates shared prototype resources. To change geometry or a material, publish
// a replacement prototype through updated().
//
// Instance placement is intentionally restricted to a rigid transform: rotate
// about Y first, then translate. There is no general affine transform and no
// per-instance motion.
//==============================================================================================

#include "rtweekend.h"
#include "hittable.h"

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>


// A shareable, immutable piece of scene geometry. Several instances may
// reference the same prototype, in which case they share one production
// hittable (and any BLAS it contains). `emissive` marks prototypes that act as
// area lights; emissive prototypes must support the hittable random/pdf_value
// sampling interface (sphere and quad do).
struct scene_prototype {
    shared_ptr<hittable> object;
    bool emissive = false;
};


// One placement of a prototype. `prototype` is the stable prototype slot. The
// transform is rotate-yaw-about-Y then translate. A disabled instance keeps its
// slot and its BVH leaf but takes part in neither hit testing nor light
// sampling.
struct scene_instance_desc {
    size_t prototype;
    vec3 translation;
    double yaw_degrees = 0.0;
    bool enabled = true;
};


class scene_snapshot {
  public:
    // Build a snapshot. Prototype and instance slots are the input indices and
    // remain permanently stable across forks. Throws std::invalid_argument for
    // a null prototype object, a non-finite transform, a non-finite yaw, or an
    // instance referencing a missing prototype.
    scene_snapshot(const std::vector<scene_prototype>& prototypes,
                   const std::vector<scene_instance_desc>& instances);

    scene_snapshot(const scene_snapshot&) = default;
    scene_snapshot(scene_snapshot&&) = default;
    scene_snapshot& operator=(const scene_snapshot&) = default;
    scene_snapshot& operator=(scene_snapshot&&) = default;
    ~scene_snapshot() = default;

    // The hittable world for this version. Empty scenes return a hittable that
    // never reports a hit.
    const hittable& world() const;

    // The light set for this version: exactly the enabled, emissive instances,
    // sampled uniformly per instance. With no lights this returns a hittable
    // whose pdf_value is 0 and which is never sampled via random() by the
    // camera's zero-light path.
    const hittable& lights() const;

    size_t light_count() const;
    size_t prototype_count() const;
    size_t instance_count() const;

    // Fork this snapshot. Both maps are whole-item replacements keyed by stable
    // slot: a prototype replacement automatically reaches every instance still
    // referencing that prototype; an instance replacement uses its final
    // description (including a possibly changed prototype). All validation
    // (out-of-range or duplicated slots, null object, non-finite transform, or
    // an invalid prototype reference) runs before anything is produced, so on a
    // thrown std::invalid_argument this snapshot, its identities, geometry and
    // light set are all unchanged. Empty update maps are valid and produce a new
    // snapshot sharing the current root.
    scene_snapshot updated(
        const std::vector<std::pair<size_t, scene_prototype>>& prototype_updates,
        const std::vector<std::pair<size_t, scene_instance_desc>>& instance_updates
    ) const;

  private:
    struct instance_state {
        size_t prototype;
        vec3 translation;
        double yaw_degrees;
        bool enabled;
        // Production leaf for this version: identity wrapper over the shared
        // prototype (rotated/translated, or null when disabled).
        shared_ptr<hittable> leaf;
    };

    std::vector<scene_prototype> prototypes;
    std::vector<instance_state> instances;
    shared_ptr<hittable> world_root;   // BVH over all instance leaves
    shared_ptr<hittable> world_empty;  // fallback for an empty scene
    shared_ptr<hittable> light_set;    // uniform mixture of enabled emissives
    size_t num_lights = 0;

    scene_snapshot() = default;

    void build_derived();
};


#endif
