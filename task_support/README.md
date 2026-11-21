# Static light tree: interface and transport basis

This document describes the read-only light-tree snapshot added in
`src/lighttree.hxx`, how `PathTracer` and `VertexCM` (BPT/VCM) consume it, and
the density bookkeeping that keeps MIS correct when direct light selection is
no longer uniform.

The feature is **off by default**. Light sub-path emission always keeps the
uniform distribution `qE(light) = 1/N`; the tree only replaces the direct
illumination (next event estimation) light pick with a position-dependent
probability `qD(x, light)`.

## 1. Public interface of `LightTree`

### Construction

```cpp
bool LightTree::Build(const Scene& aScene);
bool LightTree::Build(const std::vector<AbstractLight*>& aLights,
                      const SceneSphere& aSceneSphere);
```

* Builds an immutable snapshot. Tests may assemble their own light table.
* The scene bounding-sphere radius `R` must be finite and strictly positive;
  otherwise the build fails.
* A non-finite or negative light intensity (or background radiance) makes the
  build fail and **the previous snapshot is retained**.
* An empty light table yields an empty, valid snapshot: `Sample` returns `-1`,
  `Pdf` returns `0`.
* A snapshot is read-only after a successful build and may be shared by many
  renderers through `const LightTree&`.

The scene wraps explicit rebuild semantics; the snapshot is never refreshed
implicitly when `mLights` changes:

```cpp
bool  Scene::RebuildLightTree();          // rebuild now from mLights
const LightTree& Scene::GetLightTree() const;
void  Scene::SetUseLightTree(bool);       // default false
bool  Scene::UseLightTree() const;         // false for an empty snapshot
int   Scene::GetBackgroundID() const;      // original table ID, -1 if none
```

Original light IDs are stable: tree nodes store the index the light had in the
input table; internal reordering never changes the returned IDs.

### Queries

```cpp
int    LightTree::Sample(const Vec3f& x, float u) const; // -> light ID or -1
double LightTree::Pdf   (const Vec3f& x, int lightID) const;
double LightTree::TreePdf(const Vec3f& x, int lightID) const; // pre-mixture
```

`Sample` takes a single `[0,1)` number. `Pdf` returns the **mixed** discrete
probability (including the uniform component); `Sample` and `Pdf` describe the
exact same distribution. Neither query scans the light table nor allocates an
N-entry temporary array: both walk the binary tree only.

### Inspection and statistics (for tests)

```cpp
int    LightTree::LightCount(), NodeCount(), Depth() const;
bool   LightTree::IsEmpty(), AllBlack() const;
double LightTree::Radius(), TotalWeight() const;
bool   LightTree::GetNodeInfo(int index, NodeInfo&) const;
int    LightTree::RootNodeIndex() const;
double LightTree::NodeScore(int index, const Vec3f& x) const;
int    LightTree::LastSampleNodeAccesses(), LastPdfNodeAccesses() const;
int    LightTree::MaxSampleNodeAccesses(),  MaxPdfNodeAccesses() const;
long long LightTree::SampleQueryCount(), PdfQueryCount() const;
static int LightTree::VisitBound(int lightCount);
```

`NodeInfo` exposes the world bounding box, finite/infinite weight sums, leaf
range, child indices, original light ID, depth and whether the subtree holds
finite lights.

## 2. Weights, scores and the hierarchy

All intermediate math is performed in `double`. With `R` the scene
bounding-sphere radius, the per-light scalar weights are:

| Light          | Weight |
|----------------|--------|
| Area           | `Luminance(intensity) * pi * area` |
| Point          | `Luminance(intensity) * 4*pi` |
| Directional    | `Luminance(intensity) * pi * R^2` |
| Background     | `Luminance(scale * backgroundColor) * 4*pi^2 * R^2` |

The hierarchy is an almost-complete binary tree with one leaf per light, built
by median splits on the largest centroid-extent axis; depth is `O(log N)` and
the node count is `2N - 1`. Each node stores the union world bounding box and
finite-weight sum of its finite lights, plus the infinite-weight sum of its
infinite lights. The score of a node at receiving point `x` is

```
score(x, node) = finiteWeight / (dist^2(x, box) + 1e-6 * R^2)
               + infiniteWeight / R^2,
```

with the finite term equal to zero when the subtree has no finite light. At
each internal node the left child is chosen with probability
`score(left) / (score(left) + score(right))`; if both scores are zero the
branch probability follows the subtree leaf counts.

### Mixture and single-number remapping

The tree distribution is mixed with a uniform component of fraction `1/16`:

```
qD(x, light) = (1/16) * (1/N) + (15/16) * treePdf(x, light).
```

One input number drives everything: if `u < 1/16` the uniform branch is taken
after remapping `u' = 16 u`; otherwise `u' = (u - 1/16) / (15/16)` feeds the
tree walk, and at every level the same number is remapped through the chosen
child probability (`u' /= pLeft` or `u' = (u' - pLeft)/(1-pLeft)`). If every
light is black the whole distribution falls back to uniform `1/N`.

The observed visit count at 4096 lights is 13, well below the required bound
`8 * ceil(log2(N)) + 16 = 112`.

## 3. Endpoint densities and the MIS recurrence

Let a full path be written with light-side vertices `x0..x_{s-1}` (x0 the
emitter) and camera-side vertices `y0..y_{t-1}` (y0 the camera). Bidirectional
techniques are indexed by `s`, the number of light-side vertices.

Two distinct discrete light probabilities appear:

* **Emission** `qE(light) = 1/N`. It multiplies every technique whose emitter
  is produced by the light sub-path: light tracing, vertex connection, vertex
  merging, and BSDF-sampled light/background hits. The light's own `Emit` and
  `Illuminate` conditional distributions are untouched.
* **Direct selection** `qD(x, light)`. Only the single next-event-estimation
  (NEE) technique uses it: it picks one light from a receiving surface point.

In the original uniform code `qD == qE == 1/N`, so the discrete picks cancel
inside the iterative partial-weight formulas of the VCM technical report. With
the tree they differ, and the cancellation must be undone for exactly one
competitor per full path.

### Which point evaluates `qD` for the competing NEE technique

* **Connection, merging and light tracing.** For any full path assembled from
  a given light sub-path, the competing NEE technique traces the eye sub-path
  to `x1`, the first surface vertex adjacent to the emitter `x0`, and
  illuminates `x0` from there. Hence `r = qD(x1, light)/qE(light)` is constant
  for the whole light sub-path. It is computed once at the first light
  surface hit, stored on every light vertex as `mR`, and reused.
* **Camera ray hitting a light or escaping to the background (s = 0 paths).**
  The competing NEE technique would have sampled that light from the
  *immediately preceding* camera surface vertex, so `qD` is evaluated there
  (the path tracer uses the same previous-shading-point rule).
* **Direct illumination itself.** `qD` is evaluated at the current camera
  hit point, which is precisely the NEE receiving point.

### Isolated NEE monomial and the additive correction

The production recurrence keeps the original `dVCM/dVC/dVM` quantities and, on
light sub-paths only, additionally propagates `mNeeDVCM/mNeeDVC/mNeeDVM`.
These obey the same recurrence as the main quantities but with every
"newer-technique birth" constant removed, so they isolate the single NEE
technique. At a connection/merge/light-trace site the balance-heuristic
denominator is corrected by

```
D_new = D_uniform + (r - 1) * neeMonomial,
```

where `neeMonomial` uses the same local area-pdf factor as the full light
partial weight. When the tree is disabled `r = 1`, the additive term is zero
and every code branch is skipped, so PT/BPT/VCM results and the RNG stream are
bit-identical to the original implementation.

For the camera-hit and background strategies (`GetLightRadiance`) the same
idea is applied directly: the emission pdf keeps the uniform `qE`, while the
direct (NEE) pdf is multiplied by the actual `qD` evaluated at the previous
shading point. Delta events keep their original strategy support — delta
lights cannot be hit by BSDF sampling, so their BSDF-hit density stays zero.

The path tracer uses `qD` consistently at three places for the same receiving
point: next event estimation, a BSDF successor hitting a light, and escape to
the background. With the switch off it consumes exactly the same random
numbers as before.

## 4. Tests and demo

* `tests/lighttree_transport.cpp` (no `main`, registered through
  `task_cases/task_case_count`) contains three groups:
  * **distribution** — empty and all-black tables, Sample/Pdf identity and
    normalization, empirical frequencies with printed sample size and
    `3*sigma` standard-error bands, mixed light types with stable original IDs,
    rejection of illegal intensities while retaining the old snapshot, and the
    4096-light visit budget.
  * **mis** — independent short-path enumeration of feasible generation
    strategies in area measure, balance weights summing to one, production NEE
    terms matching the hand enumeration under both selection modes, the stored
    `mR` recurrence against the tree, diffuse and specular paths, light hits and
    background. A friend adapter (`src/vertexcm_adapter.hxx`) reads the real
    private MIS state.
  * **render** — linear radiance agreement between the two modes with printed
    sample counts and an error basis, regional pick distribution, multi-round
    rendering, and reading the PFM written by this demo run.
* `examples/lighttree_demo.cpp` runs real BPT and VCM with the tree enabled,
  writes `<dir>/lighttree.pfm` and prints the work counts (iterations, total
  samples, tree query counts and node-visit maxima).

Run everything with:

```
bash task_support/run.sh target
```
