#ifndef MINIPBRT_SCENE_H
#define MINIPBRT_SCENE_H

/*
MIT License

Copyright (c) 2019 Vilya Harvey

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include "minipbrt.h"

#include <string>
#include <vector>


/// Scene queries for nested, animated instancing.
/// ==============================================
///
/// These helpers walk the object/instance hierarchy produced by minipbrt's
/// loader (or by a caller-constructed `Scene`) at a particular shutter time.
///
/// * `sample_scene` returns one `SceneDraw` per reachable triangle mesh. The
///   world matrix is evaluated by interpolating each transform in the chain
///   between its start and end matrices *before* multiplying the chain
///   together. `objectToInstance` is deliberately not applied separately: when
///   a shape belongs to an object its saved `shapeToWorld` already includes
///   the object's `objectToInstance` transform.
///
/// * `bound_scene_motion` returns one axis-aligned world-space box per
///   reachable triangle mesh which contains every mesh vertex for every time
///   in the closed interval `[begin, end]`. Matrices may become singular
///   during the interval; that does not affect the bound.
///
/// Both functions validate their inputs (including scenes built by hand rather
/// than by the loader). On failure they return `false`, store a human readable
/// message in `error`, and leave `out` untouched. On success they replace
/// `out` and clear `error`.
///
/// Only `TriangleMesh` shapes are supported. A reachable shape of any other
/// type causes the query to fail.

namespace minipbrt {

  /// A single triangle mesh draw produced by `sample_scene`.
  struct SceneDraw {
    uint32_t shape;                 //!< Index into `Scene::shapes`.
    std::vector<uint32_t> path;     //!< Instance indexes from the world root down to the shape's owner object. Empty for world-direct shapes.
    double world[4][4];             //!< Row-major shape-to-world matrix at the requested time.
    double normal[3][3];            //!< Row-major inverse-transpose of `world`'s linear 3x3 part.
    bool flipOrientation;           //!< Shape/instance reverseOrientation flags XOR (det(world) < 0).
  };


  /// Evaluate the scene at `time` and append one `SceneDraw` per reachable
  /// triangle mesh to `out`.
  bool sample_scene(const Scene& scene, double time,
                    std::vector<SceneDraw>& out, std::string& error);


  /// One motion bound produced by `bound_scene_motion`.
  struct SceneMotionBounds {
    uint32_t shape;                 //!< Index into `Scene::shapes`.
    std::vector<uint32_t> path;     //!< Instance indexes from the world root down to the shape's owner object.
    double min[3];                  //!< Lower bound of the world-space axis-aligned box.
    double max[3];                  //!< Upper bound of the world-space axis-aligned box.
  };


  /// Bound the world-space trajectory of every mesh vertex over the closed
  /// time interval `[begin, end]` (clamped to the scene's time range).
  ///
  /// `epsilon` must be finite and positive. Each returned bound is guaranteed
  /// to enclose the true trajectory and to be no further than
  /// `epsilon + 1e-8 * S` from the true extremum on each side, where `S` is
  /// the larger of 1 and the maximum absolute coordinate of that trajectory.
  bool bound_scene_motion(const Scene& scene, double begin, double end,
                          double epsilon,
                          std::vector<SceneMotionBounds>& out,
                          std::string& error);

} // namespace minipbrt

#endif // MINIPBRT_SCENE_H
