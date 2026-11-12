# Volume-preserving fairing (`volume_preserving_fairing`)

`cinolib/volume_preserving_fairing.h` adds a fairing path for closed triangle
surfaces that keeps a set of user-chosen vertices fixed and preserves the
signed volume of **each connected shell** (component), shell by shell.

It complements — and does not replace or alter — `tangential_smoothing`,
`mesh_smoother` and the remeshing entry points, whose behaviour is unchanged.

```c++
#include <cinolib/meshes/trimesh.h>
#include <cinolib/volume_preserving_fairing.h>

cinolib::Trimesh<> m("closed_mesh.obj");

std::vector<uint> fixed = { /* vertex ids that must not move */ };
cinolib::VolumePreservingFairingOptions opt;   // max_iterations=10, step=0.1, tol=1e-9
auto report = cinolib::volume_preserving_fairing(m, fixed, opt);
```

The `fixed_vertices` argument accepts `std::vector<uint>`, `std::set<uint>` or
`std::unordered_set<uint>`. Duplicate ids are ignored.

## Energy

The quantity minimised is the sum of squared **undirected** mesh edge lengths:

```
E(x) = Σ_{ {i,j} ∈ undirected edges } || x_i - x_j ||²
```

Connectivity is never modified, so the only variables are the coordinates of
the free vertices. Every accepted step strictly lowers `E`.

## Constraints

* **Fixed vertices** keep their exact input coordinates, component by
  component. Isolated vertices (no incident triangle) are preserved too.
  Different shells can have their own fixed vertices.
* **Per-shell signed volume**

  ```
  V_c(x) = Σ_{ (a,b,c) ∈ shell c } det(a,b,c) / 6
  ```

  is kept at its input value within `options.relative_volume_tolerance`
  (default `1e-9`) for *every* shell `c`. This is what makes the method
  different from a total-volume check: a shell cannot shrink merely because
  another one grows. There is no global rescaling of the model, and no
  "move the anchors and glue them back at the end" trick: anchors are never
  assigned, and volume is corrected continuously inside each step.
* **Face validity.** Before a step is accepted every triangle is checked:
  * `dot(n_old, n_new) > 0` where `n = (b−a)×(c−a)` is the oriented area
    vector (no face may flip); and
  * `||n_new|| ≥ 1e-6 · ||n_initial||` (no triangle may be squashed below a
    factor `1e-6` of its original area).

  This prevents a volume compensation from turning a thin triangle inside out.
* Global self-intersections between distant parts of the surface are **not**
  detected (the same limitation as classical tangential smoothing).
* Volumes are measured with a **shell-local reference point** (the shell
  centroid). Because the surface is closed, the signed volume is independent
  of that point, so the scale and the position of the model do not change the
  meaning of the problem: the solution is invariant under uniform scaling and
  translation.

## How a step is solved

The fairing direction and the volume correction are coupled, so a plain
per-point tangential move followed by a final global volume fix is not
sufficient. Each iteration works in the space of free vertices:

1. **Fairing direction** — for every free vertex `i`, the uniform-Laplacian
   descent direction

   ```
   d_i = (mean of neighbours of i) − x_i = −∇_i E / (2 · deg_i)
   ```

2. **Volume gradients** — for every shell, compute `g_i = ∂V_c / ∂x_i`
   for its free vertices, using the shell-local reference point.

3. **Coupled first-order correction** — with the natural vertex metric
   (`h_i = g_i / deg_i`), a displacement

   ```
   x_i(t, λ) = x_i + t · d_i − λ · h_i
   ```

   changes the shell volume at rate `k = Σ g_i·d_i` by the fairing part and
   at rate `denom = Σ ||g_i||²/deg_i` by the correction part, so
   `λ = t · k / denom` removes the volume change *inside the same step*.

4. **Exact (non-linear) shell solve** — `λ` is then refined with a scalar
   Newton iteration on the exact shell volume, evaluated on the trial
   geometry (80 iterations at most, targeting half the requested tolerance).

5. **Backtracking / acceptance** — the step length `t` starts at
   `options.step` and is halved until the trial configuration is finite,
   strictly lowers `E`, keeps every shell volume within tolerance, keeps every
   face orientation positive and every face above the `1e-6` area floor.
   Rejected trials live in local buffers only: mesh coordinates, normals, the
   bounding box and any cache are never modified by an attempt that is not
   accepted.

Accepted coordinates are committed, and `update_normals()` / `update_bbox()`
are called so derived data agrees with the final geometry. Vertex, edge and
polygon indices and all custom per-element attributes are left untouched.

## Stopping criteria

The report always contains the real number of accepted steps and a stop
reason:

| reason              | meaning |
|---------------------|---------|
| `max_iterations`    | `options.max_iterations` accepted steps were performed |
| `stationary`        | the volume-constrained descent direction vanished (e.g. an already symmetric shell) |
| `no_free_vertices`  | every vertex is fixed (or the mesh has no triangles) |
| `no_legal_step`     | backtracking found no step satisfying volume, face validity and strict energy decrease |

## Report

`VolumePreservingFairingReport` provides:

* `accepted_steps` / `completed_steps`, `attempted_steps`;
* `initial_energy`, `final_energy`, and `energies` (after each accepted step);
* `steps[i]`: iteration, accepted step index, number of backtracked trials,
  step length, energy before/after, and the **constraint residual** (the
  maximum relative per-shell volume error achieved);
* `shells`: for each shell (numbered by increasing minimum vertex id) the
  min vertex id, its vertices, and the **initial and final signed volume**;
* `stop` (enum) plus stable `stop_reason` / `stop_message` strings.

This makes every step's energy, constraint residual and stop cause
traceable.

## Supported input

The input must be a finite, non-degenerate, consistently oriented,
boundary-less 2-manifold triangle surface. It may consist of several disjoint
closed shells. The following are rejected with `std::invalid_argument`
**before any coordinate is rewritten**:

* open meshes (an edge with valence ≠ 2), non-manifold edges or vertices;
* inconsistent face orientation;
* degenerate triangles (collinear / repeated vertices, non-finite area);
* zero-volume shells;
* non-finite vertex coordinates;
* out-of-range vertex indices in polygons or in `fixed_vertices`;
* invalid options (`step ∉ (0,1]`, non-positive/non-finite volume tolerance).

The option `max_iterations` is a non-negative step count; `0` simply requests
no steps.

## Scale and translation

Both the internal volume evaluations and the per-shell gradients use `long
double` arithmetic after subtracting the shell-local reference point. As a
consequence the result (normalised by scale) is the same for models placed at
`0`, `10³` or `10⁶` units from the origin, and for models scaled by `0.01`,
`1` or `100`. Small and large models are handled identically.

## Testing

`devcheck/fairing.cpp` is a standalone C++17 program that generates all its
models in code (no file, no online dependency; only the in-repo Eigen is on
the include path). It runs the following groups:

* `constrained-descent` — strict decrease of the independently recomputed
  energy at every accepted step, backtracking on a thin configuration whose
  naive step would flip faces;
* `component-volumes` — two disjoint shells of different size/orientation/
  volume, each preserving its own signed volume, with unchanged adjacency and
  fixed points;
* `geometry-state` — fixed points exactly untouched, connectivity and custom
  attributes preserved, no face flips, derived data consistent, scale/translation
  invariance (scale `0.01`, `1`, `100` × translation);
* `stopping-errors` — all-fixed and symmetric zero-step stops, and each
  invalid input/option rejected without touching the mesh.

Volumes and energies are recomputed independently with long-double triple
products from the triangle list; the library report is never used as the
reference value.

Run the whole target with:

```
bash devcheck/run-target.sh
```

or the fairing suite alone with `bash devcheck/check-fairing.sh`.
