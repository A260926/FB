#ifndef MINIPBRT_SCENE_H
#define MINIPBRT_SCENE_H

#include "minipbrt.h"

#include <cstdint>
#include <string>
#include <vector>

namespace minipbrt {

  /// One reachable shape instance at a particular shutter time.
  ///
  /// `path` contains Scene::instances indices from the world root to the leaf
  /// instance.  It is empty for shapes stored directly in the world.
  struct SceneDraw {
    uint32_t shape;
    std::vector<uint32_t> path;
    double world[4][4];   // Row-major; matrices act on column vectors.
    double normal[3][3];  // Inverse transpose of world's linear 3x3 part.
    bool flipOrientation;
  };

  /// Evaluate all reachable triangle shapes at `time`.
  ///
  /// On failure neither `scene` nor `out` is modified and `error` describes
  /// the problem.  On success `out` is replaced by the complete draw list and
  /// `error` is cleared.
  bool sample_scene(const Scene& scene, double time,
                    std::vector<SceneDraw>& out, std::string& error);

  /// A conservative motion bound for one reachable shape/path.
  struct SceneMotionBounds {
    uint32_t shape;
    std::vector<uint32_t> path;
    double min[3];
    double max[3];
  };

  /// Bound complete vertex trajectories over [begin, end].
  ///
  /// `epsilon` must be finite and positive.  Bounds are conservative (they
  /// never miss an extreme) and have controlled excess width.  On failure
  /// `out` is left unchanged.
  bool bound_scene_motion(const Scene& scene, double begin, double end,
                          double epsilon,
                          std::vector<SceneMotionBounds>& out,
                          std::string& error);

} // namespace minipbrt

#endif // MINIPBRT_SCENE_H
