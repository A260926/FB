// Copyright 2026 The Manifold Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Regression tests for deferred (lazy) Warp / WarpBatch execution.
//
// The geometry reference for every ordering check is an independent "eager"
// sequence: after each step its result is read with GetMeshGL64 and rebuilt
// into a fresh Manifold, so the deferred expression and the reference share
// no internal state. Comparisons cover vertex positions, source IDs, volume
// and vertex properties, not just the aggregate volume.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <thread>
#include <vector>

#include <manifold/manifold.h>

#include "task_support/check.h"

using namespace manifold;

namespace {

constexpr double kTol = 1e-9;

// Force an observation, then rebuild from the exported mesh. This is the
// eager reference sequence called out in the task: read at every step.
Manifold Snapshot(const Manifold& m) { return Manifold(m.GetMeshGL64()); }

void WarpFormula(vec3& v) { v.x += 0.5 * v.z * v.z; }

bool VecNear(const vec3& a, const vec3& b) {
  return std::abs(a.x - b.x) <= kTol && std::abs(a.y - b.y) <= kTol &&
         std::abs(a.z - b.z) <= kTol;
}

bool LessVec(const vec3& a, const vec3& b) {
  if (a.x != b.x) return a.x < b.x;
  if (a.y != b.y) return a.y < b.y;
  return a.z < b.z;
}

std::vector<vec3> SortedPositions(const MeshGL64& m) {
  std::vector<vec3> out;
  out.reserve(m.NumVert());
  for (uint64_t i = 0; i < m.NumVert(); ++i) out.push_back(m.GetVertPos(i));
  std::sort(out.begin(), out.end(), LessVec);
  return out;
}

std::vector<std::vector<double>> SortedPropRows(const MeshGL64& m) {
  const int numProp = static_cast<int>(m.numProp) - 3;
  std::vector<std::vector<double>> rows;
  if (numProp <= 0) return rows;
  rows.reserve(m.NumVert());
  for (uint64_t i = 0; i < m.NumVert(); ++i) {
    std::vector<double> row(numProp);
    for (int p = 0; p < numProp; ++p)
      row[p] = m.vertProperties[i * m.numProp + 3 + p];
    rows.push_back(std::move(row));
  }
  std::sort(rows.begin(), rows.end());
  return rows;
}

bool PropsNear(const std::vector<std::vector<double>>& a,
               const std::vector<std::vector<double>>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i].size() != b[i].size()) return false;
    for (size_t j = 0; j < a[i].size(); ++j)
      if (std::abs(a[i][j] - b[i][j]) > kTol) return false;
  }
  return true;
}

bool PositionsNear(const std::vector<vec3>& a, const std::vector<vec3>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (!VecNear(a[i], b[i])) return false;
  return true;
}

// Full geometry comparison: positions, provenance, volume, properties and
// tangent buffers. The eager reference round-trips through MeshGL64 at every
// step, which merges same-ID runs, so provenance is compared as the source-ID
// set rather than run granularity.
bool MeshesEquivalent(const Manifold& a, const Manifold& b) {
  if (a.Status() != Manifold::Error::NoError ||
      b.Status() != Manifold::Error::NoError)
    return false;
  if (a.NumVert() != b.NumVert() || a.NumTri() != b.NumTri() ||
      a.NumProp() != b.NumProp() || a.NumPropVert() != b.NumPropVert())
    return false;
  if (std::abs(a.Volume() - b.Volume()) > kTol) return false;
  const MeshGL64 ma = a.GetMeshGL64();
  const MeshGL64 mb = b.GetMeshGL64();
  if (!PositionsNear(SortedPositions(ma), SortedPositions(mb))) return false;
  auto idsA = ma.runOriginalID;
  auto idsB = mb.runOriginalID;
  std::sort(idsA.begin(), idsA.end());
  std::sort(idsB.begin(), idsB.end());
  idsA.erase(std::unique(idsA.begin(), idsA.end()), idsA.end());
  idsB.erase(std::unique(idsB.begin(), idsB.end()), idsB.end());
  if (idsA != idsB) return false;
  if (ma.triVerts.size() != mb.triVerts.size()) return false;
  if (!PropsNear(SortedPropRows(ma), SortedPropRows(mb))) return false;
  if (ma.halfedgeTangent.size() != mb.halfedgeTangent.size()) return false;
  for (size_t i = 0; i < ma.halfedgeTangent.size(); ++i)
    if (std::abs(ma.halfedgeTangent[i] - mb.halfedgeTangent[i]) > kTol)
      return false;
  return true;
}

Manifold MakeRoot() {
  // Boolean expression shared by the deferred and reference paths. The
  // MeshGL64 snapshot keeps identical source IDs on both sides.
  auto shell = Manifold::Cube({2, 3, 4}) -
               Manifold::Cube({1, 1, 6}).Translate({0.5, 1, -1});
  return Snapshot(shell);
}

// ---------------------------------------------------------------------------
// deferred group: callbacks run only on first observation, then are reused.
// ---------------------------------------------------------------------------

int ScalarDeferred() {
  auto root = MakeRoot();
  const size_t numVert = root.NumVert();
  int calls = 0;
  auto warped = root.Warp([&](vec3& v) {
    ++calls;
    WarpFormula(v);
  });
  // Building the expression and copying it must not touch the callback.
  Manifold copy = warped;
  TASK_CHECK(calls == 0);
  TASK_CHECK(warped.Status() == Manifold::Error::NoError);
  TASK_CHECK(calls == static_cast<int>(numVert));
  // Repeated queries reuse the single materialization.
  TASK_CHECK(warped.NumVert() == numVert);
  (void)warped.Volume();
  (void)warped.GetMeshGL64();
  TASK_CHECK(copy.Status() == Manifold::Error::NoError);
  TASK_CHECK(calls == static_cast<int>(numVert));
  return 0;
}

int BatchDeferred() {
  auto root = MakeRoot();
  const size_t numVert = root.NumVert();
  int batchCalls = 0;
  int seenVertices = 0;
  auto warped = root.WarpBatch([&](VecView<vec3> vv) {
    ++batchCalls;
    seenVertices += static_cast<int>(vv.size());
    for (vec3& v : vv) WarpFormula(v);
  });
  TASK_CHECK(batchCalls == 0);
  (void)warped.Status();
  // One stage means exactly one batch call over the whole vertex array.
  TASK_CHECK(batchCalls == 1);
  TASK_CHECK(seenVertices == static_cast<int>(numVert));
  (void)warped.Volume();
  (void)warped.GetMeshGL64();
  TASK_CHECK(batchCalls == 1);
  return 0;
}

int CopiesBeforeObservation() {
  auto root = MakeRoot();
  int calls = 0;
  Manifold deferred;
  {
    auto scoped = root.Warp([&](vec3& v) {
      ++calls;
      WarpFormula(v);
    });
    deferred = scoped;  // copy keeps the shared deferred node alive.
  }
  TASK_CHECK(calls == 0);
  TASK_CHECK(deferred.Status() == Manifold::Error::NoError);
  TASK_CHECK(calls == static_cast<int>(root.NumVert()));
  // A second copy after observation cannot re-run the stage.
  Manifold again = deferred;
  (void)again.Volume();
  TASK_CHECK(calls == static_cast<int>(root.NumVert()));
  return 0;
}

int ChainedAffinesDoNotRewarp() {
  auto root = MakeRoot();
  int calls = 0;
  auto warped = root.Warp([&](vec3& v) {
    ++calls;
    WarpFormula(v);
  });
  // Several affines stacked above the warp add no new warp stages.
  auto composed =
      warped.Translate({1, 2, 3}).Scale({2, 1, 1}).Rotate(0, 30, 0);
  TASK_CHECK(calls == 0);
  TASK_CHECK(composed.Status() == Manifold::Error::NoError);
  TASK_CHECK(calls == static_cast<int>(root.NumVert()));
  (void)composed.Volume();
  (void)composed.GetMeshGL64();
  TASK_CHECK(calls == static_cast<int>(root.NumVert()));

  // Geometry matches the eager warp-first-then-affine sequence.
  Manifold ref = Snapshot(root);
  ref = Snapshot(ref.Warp(WarpFormula));
  ref = Snapshot(ref.Translate({1, 2, 3}));
  ref = Snapshot(ref.Scale({2, 1, 1}));
  ref = Snapshot(ref.Rotate(0, 30, 0));
  TASK_CHECK(MeshesEquivalent(composed, ref));
  return 0;
}

int ErrorSourceSkipsCallbacks() {
  auto root = Manifold::Cube();
  // Only some vertices become non-finite: the bbox stays finite, so the
  // post-warp IsFinite() check classifies the result as NonFiniteVertex.
  // (Making every vertex non-finite is swallowed as an empty mesh by
  // CalculateBBox, which is the library's existing behavior.)
  auto bad = root.Warp([](vec3& v) {
    if (v.x > 0.9 && v.y > 0.9 && v.z > 0.9)
      v.x = std::numeric_limits<double>::quiet_NaN();
  });
  TASK_CHECK(bad.Status() == Manifold::Error::NonFiniteVertex);

  int scalarCalls = 0;
  int batchCalls = 0;
  auto chainedScalar = bad.Warp([&](vec3& v) {
    ++scalarCalls;
    v.x += 1.0;
  });
  auto chainedBatch = bad.WarpBatch([&](VecView<vec3> vv) {
    ++batchCalls;
    for (vec3& v : vv) v.y += 1.0;
  });
  TASK_CHECK(chainedScalar.Status() == Manifold::Error::NonFiniteVertex);
  TASK_CHECK(chainedBatch.Status() == Manifold::Error::NonFiniteVertex);
  // A warp that produced non-finite positions propagates the status and later
  // warp stages never invoke their callbacks.
  TASK_CHECK(scalarCalls == 0);
  TASK_CHECK(batchCalls == 0);

  // An already-errored source (observed before the warp) must skip the
  // callback as well.
  Manifold errored = bad;
  int sourceCalls = 0;
  auto fromError = errored.Warp([&](vec3& v) {
    ++sourceCalls;
    v.z += 2.0;
  });
  TASK_CHECK(fromError.Status() == Manifold::Error::NonFiniteVertex);
  TASK_CHECK(sourceCalls == 0);
  return 0;
}

int DeepChainDestroyedUnobserved() {
  constexpr int kDepth = 10000;
  int calls = 0;
  {
    auto m = Manifold::Cube();
    for (int i = 0; i < kDepth; ++i)
      m = m.Warp([&](vec3& v) {
        ++calls;
        v.x += 0.0001;
      });
    // Destroyed without any observation: the callbacks never run.
  }
  TASK_CHECK(calls == 0);
  return 0;
}

int DeepChainObserved() {
  constexpr int kDepth = 10000;
  const size_t numVert = Manifold::Cube().NumVert();
  int calls = 0;
  auto m = Manifold::Cube();
  for (int i = 0; i < kDepth; ++i)
    m = m.Warp([&](vec3& v) {
      ++calls;
      v.x += 0.0001;
    });
  TASK_CHECK(m.Status() == Manifold::Error::NoError);
  // Every stage visits every vertex exactly once.
  TASK_CHECK(calls == kDepth * static_cast<int>(numVert));
  // The materialized chain is reused by further queries.
  (void)m.Volume();
  TASK_CHECK(calls == kDepth * static_cast<int>(numVert));
  return 0;
}

// ---------------------------------------------------------------------------
// ordering group: stage boundaries and parity with the eager reference.
// ---------------------------------------------------------------------------

int WarpAfterBooleanMatchesEager() {
  auto root = MakeRoot();
  Manifold deferred = root.Warp(WarpFormula);

  Manifold eager = Snapshot(root);
  eager = Snapshot(eager.Warp(WarpFormula));

  TASK_CHECK(MeshesEquivalent(deferred, eager));
  // The task's triangle-mesh reference value for this expression.
  TASK_CHECK(std::abs(deferred.Volume() - 20.0) <= kTol);
  // Exact provenance: warping preserves the source run IDs of its own input
  // (the eager snapshot reference collapses multi-run meshes, which is why the
  // general equivalence check uses the ID set).
  TASK_CHECK(deferred.GetMeshGL64().runOriginalID ==
             root.GetMeshGL64().runOriginalID);
  return 0;
}

int BooleanStageBoundary() {
  // Warp applied after the Boolean sees the whole combined mesh; warping the
  // operands first is a different expression with a different volume (20 vs
  // 24 on the triangle path).
  auto cubeA = Manifold::Cube({2, 3, 4});
  auto cubeB = Manifold::Cube({1, 1, 6}).Translate({0.5, 1, -1});

  auto afterBoolean = (cubeA - cubeB).Warp(WarpFormula);

  auto warpedA = cubeA.Warp(WarpFormula);
  auto warpedB = cubeB.Warp(WarpFormula);
  auto beforeBoolean = warpedA - warpedB;

  TASK_CHECK(afterBoolean.Status() == Manifold::Error::NoError);
  TASK_CHECK(beforeBoolean.Status() == Manifold::Error::NoError);
  TASK_CHECK(std::abs(afterBoolean.Volume() - 20.0) <= kTol);
  TASK_CHECK(std::abs(beforeBoolean.Volume() - 24.0) <= kTol);

  // Eager references rebuilt with explicit reads at every step.
  Manifold refAfter = Snapshot(cubeA - cubeB);
  refAfter = Snapshot(refAfter.Warp(WarpFormula));
  Manifold refBefore = Snapshot(Snapshot(cubeA).Warp(WarpFormula)) -
                      Snapshot(Snapshot(cubeB).Warp(WarpFormula));
  TASK_CHECK(MeshesEquivalent(afterBoolean, refAfter));
  TASK_CHECK(MeshesEquivalent(beforeBoolean, Snapshot(refBefore)));
  return 0;
}

int WarpAffineNonCommuting() {
  auto root = MakeRoot();
  const vec3 shift{1.0, 2.0, 3.0};

  Manifold warpThenTranslate = root.Warp(WarpFormula).Translate(shift);
  Manifold translateThenWarp = root.Translate(shift).Warp(WarpFormula);

  Manifold refWtt = Snapshot(root);
  refWtt = Snapshot(refWtt.Warp(WarpFormula));
  refWtt = Snapshot(refWtt.Translate(shift));
  Manifold refTtw = Snapshot(root);
  refTtw = Snapshot(refTtw.Translate(shift));
  refTtw = Snapshot(refTtw.Warp(WarpFormula));

  TASK_CHECK(MeshesEquivalent(warpThenTranslate, refWtt));
  TASK_CHECK(MeshesEquivalent(translateThenWarp, refTtw));
  // The two orderings must not collapse into the same geometry.
  const auto p1 = SortedPositions(warpThenTranslate.GetMeshGL64());
  const auto p2 = SortedPositions(translateThenWarp.GetMeshGL64());
  TASK_CHECK(!PositionsNear(p1, p2));
  return 0;
}

int BatchMatchesScalar() {
  auto root = MakeRoot();
  int scalarCalls = 0;
  int batchCalls = 0;
  auto scalar = root.Warp([&](vec3& v) {
    ++scalarCalls;
    WarpFormula(v);
  });
  auto batch = root.WarpBatch([&](VecView<vec3> vv) {
    ++batchCalls;
    for (vec3& v : vv) WarpFormula(v);
  });
  TASK_CHECK(MeshesEquivalent(scalar, batch));
  TASK_CHECK(scalarCalls == static_cast<int>(root.NumVert()));
  TASK_CHECK(batchCalls == 1);
  return 0;
}

int MixedBooleanAffineWarp() {
  auto root = MakeRoot();
  auto other = Manifold::Cube({1, 2, 1}).Translate({3, 0, 1});

  // Union, difference, intersection and affines interleaved with warps, all
  // deferred until the final observation.
  Manifold deferred =
      (root + other).Warp(WarpFormula).Scale({1, 1, 2}).Translate({0, 1, 0});
  deferred = (deferred ^ root.Translate({0, 1, 0}).Warp(WarpFormula))
                 .Warp([](vec3& v) { v.y += 0.25 * v.x; });
  deferred = deferred - other.Translate({0, 1, 0});

  Manifold ref = Snapshot(root + other);
  ref = Snapshot(ref.Warp(WarpFormula));
  ref = Snapshot(ref.Scale({1, 1, 2}));
  ref = Snapshot(ref.Translate({0, 1, 0}));
  Manifold refOperand = Snapshot(root.Translate({0, 1, 0}));
  refOperand = Snapshot(refOperand.Warp(WarpFormula));
  ref = Snapshot(ref ^ refOperand);
  ref = Snapshot(ref.Warp([](vec3& v) { v.y += 0.25 * v.x; }));
  ref = Snapshot(ref - Snapshot(other.Translate({0, 1, 0})));

  TASK_CHECK(MeshesEquivalent(deferred, ref));
  return 0;
}

int PropertiesNormalsTangentsPreserved() {
  auto root = MakeRoot();
  auto withProps = root.SetProperties(
      3, [](double* p, vec3 v, const double*) {
        p[0] = v.x;
        p[1] = v.y;
        p[2] = v.z;
      });
  withProps = withProps.CalculateNormals(0).SmoothByNormals(0);

  int calls = 0;
  Manifold deferred = withProps.Warp([&](vec3& v) {
    ++calls;
    WarpFormula(v);
  });

  Manifold eager = Snapshot(withProps);
  eager = Snapshot(eager.Warp(WarpFormula));

  TASK_CHECK(calls == 0);
  TASK_CHECK(MeshesEquivalent(deferred, eager));
  TASK_CHECK(calls == static_cast<int>(root.NumVert()));
  return 0;
}

// ---------------------------------------------------------------------------
// sharing group: shared nodes materialize once and forks stay independent.
// ---------------------------------------------------------------------------

int SharedPredecessorForks() {
  auto root = MakeRoot();
  int calls = 0;
  auto warped = root.Warp([&](vec3& v) {
    ++calls;
    WarpFormula(v);
  });
  Manifold forkA = warped;
  Manifold forkB = warped;
  TASK_CHECK(calls == 0);
  (void)forkA.Volume();
  (void)forkB.GetMeshGL64();
  // Both forks hold the same warp node: the callback ran exactly one stage's
  // worth.
  TASK_CHECK(calls == static_cast<int>(root.NumVert()));
  TASK_CHECK(MeshesEquivalent(forkA, forkB));

  // A brand new Warp on the observed predecessor is a brand new stage.
  int moreCalls = 0;
  auto nextStage = warped.Warp([&](vec3& v) {
    ++moreCalls;
    v.z += 0.1;
  });
  TASK_CHECK(moreCalls == 0);
  (void)nextStage.Status();
  TASK_CHECK(moreCalls == static_cast<int>(root.NumVert()));
  TASK_CHECK(calls == static_cast<int>(root.NumVert()));
  return 0;
}

int ConcurrentCopiesEvaluateOnce() {
  auto root = MakeRoot();
  const size_t numVert = root.NumVert();
  std::atomic<int> calls{0};
  auto warped = root.Warp([&](vec3& v) {
    calls.fetch_add(1, std::memory_order_relaxed);
    WarpFormula(v);
  });
  Manifold a = warped;
  Manifold b = warped;
  double volumeA = 0.0;
  double volumeB = 0.0;
  std::thread t1([&] { volumeA = a.Volume(); });
  std::thread t2([&] { volumeB = b.Volume(); });
  t1.join();
  t2.join();
  TASK_CHECK(calls.load() == static_cast<int>(numVert));
  TASK_CHECK(std::abs(volumeA - volumeB) <= kTol);
  TASK_CHECK(std::abs(volumeA - warped.Volume()) <= kTol);
  return 0;
}

int ForksDoNotMutateBase() {
  auto root = MakeRoot();
  auto base = root.Warp(WarpFormula);
  (void)base.Status();
  const MeshGL64 baseMesh = base.GetMeshGL64();
  const double baseVolume = base.Volume();

  auto forkA = base.Warp([](vec3& v) { v.x += 5.0; });
  auto forkB = base.Warp([](vec3& v) { v.y -= 7.0; });
  (void)forkA.Status();
  (void)forkB.Volume();

  // Neither fork rewrites the shared base stage nor the original geometry.
  TASK_CHECK(std::abs(base.Volume() - baseVolume) <= kTol);
  const MeshGL64 baseAgain = base.GetMeshGL64();
  TASK_CHECK(PositionsNear(SortedPositions(baseMesh),
                           SortedPositions(baseAgain)));
  TASK_CHECK(baseMesh.runOriginalID == baseAgain.runOriginalID);
  TASK_CHECK(!PositionsNear(SortedPositions(forkA.GetMeshGL64()),
                            SortedPositions(forkB.GetMeshGL64())));
  return 0;
}

int AlternatingBooleanWarpChain() {
  constexpr int kSteps = 256;
  constexpr int kDeepUnobserved = 10000;
  auto rootA = MakeRoot();
  auto rootB = MakeRoot();

  // Eager reference: read and rebuild after every single step, and count the
  // per-stage vertex visits independently.
  Manifold eager = Snapshot(rootA);
  long expectedCalls = 0;
  double referenceVolume = 0.0;
  {
    int eagerCalls = 0;
    for (int i = 0; i < kSteps; ++i) {
      eager = Snapshot(eager.Warp([&](vec3& v) {
        ++eagerCalls;
        WarpFormula(v);
      }));
      expectedCalls += static_cast<long>(eager.NumVert());
      auto cube = Manifold::Cube({0.25, 0.25, 0.25})
                      .Translate({10.0 + static_cast<double>(i), 0, 0});
      eager = Snapshot(eager + cube);
    }
    referenceVolume = eager.Volume();
  }

  // Deferred chain of alternating warp and Boolean stages, observed once.
  std::atomic<int> calls{0};
  Manifold deferred = rootB;
  for (int i = 0; i < kSteps; ++i) {
    deferred = deferred.Warp([&](vec3& v) {
      calls.fetch_add(1, std::memory_order_relaxed);
      WarpFormula(v);
    });
    auto cube = Manifold::Cube({0.25, 0.25, 0.25})
                    .Translate({10.0 + static_cast<double>(i), 0, 0});
    deferred = deferred + cube;
  }
  TASK_CHECK(deferred.Status() == Manifold::Error::NoError);
  TASK_CHECK(calls.load() == expectedCalls);
  TASK_CHECK(std::abs(deferred.Volume() - referenceVolume) <= kTol);

  // A depth-10000 alternating Boolean/warp chain destroyed without any
  // observation must complete through the iterative teardown (no recursive
  // destructor chain) and must never run a callback.
  std::atomic<int> droppedCalls{0};
  {
    Manifold m = MakeRoot();
    for (int i = 0; i < kDeepUnobserved; ++i) {
      m = m.Warp([&](vec3& v) {
        droppedCalls.fetch_add(1, std::memory_order_relaxed);
        WarpFormula(v);
      });
      m = m + Manifold::Cube({0.25, 0.25, 0.25})
                      .Translate({20.0 + static_cast<double>(i), 0, 0});
    }
  }
  TASK_CHECK(droppedCalls.load() == 0);
  return 0;
}

}  // namespace

const task_case task_cases[] = {
    {"deferred", "scalar_runs_only_on_observation", ScalarDeferred},
    {"deferred", "batch_single_callback", BatchDeferred},
    {"deferred", "copies_before_observation", CopiesBeforeObservation},
    {"deferred", "chained_affines_do_not_rewarp", ChainedAffinesDoNotRewarp},
    {"deferred", "error_source_skips_callbacks", ErrorSourceSkipsCallbacks},
    {"deferred", "deep_chain_destroyed_unobserved", DeepChainDestroyedUnobserved},
    {"deferred", "deep_chain_observed", DeepChainObserved},
    {"ordering", "warp_after_boolean_matches_eager", WarpAfterBooleanMatchesEager},
    {"ordering", "boolean_stage_boundary", BooleanStageBoundary},
    {"ordering", "warp_affine_noncommuting", WarpAffineNonCommuting},
    {"ordering", "batch_matches_scalar", BatchMatchesScalar},
    {"ordering", "mixed_boolean_affine_warp", MixedBooleanAffineWarp},
    {"ordering", "properties_normals_tangents_preserved",
     PropertiesNormalsTangentsPreserved},
    {"sharing", "shared_predecessor_forks", SharedPredecessorForks},
    {"sharing", "concurrent_copies_evaluate_once", ConcurrentCopiesEvaluateOnce},
    {"sharing", "forks_do_not_mutate_base", ForksDoNotMutateBase},
    {"sharing", "alternating_boolean_warp_chain", AlternatingBooleanWarpChain},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
