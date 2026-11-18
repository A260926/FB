// Multi-patch coarsening regression suite
//
// This file is compiled inside `mesh::octree::task_target` and therefore
// has access to the octree's private production state.  Every case uses the
// `Check` callback for its real comparisons; a case that never calls
// `check` fails the driver.

// The enclosing `task_target` module already does `use super::*`, so octree
// internals (`Octree`, `Settings`, `Leaf`, hermite types, ...) are in scope.
use super::super::multipatch;
use crate::{
    context::Tree,
    mesh::{
        cell::{CellBounds, CellVertex},
        types::Corner,
        Mesh,
    },
    render::{CancelToken, ThreadPool},
    shape::ShapeVars,
    vm::VmShape,
};
use nalgebra::{Vector3, Vector4};
use std::collections::{BTreeMap, BTreeSet};

/// Builds settings for the canonical [-1, +1] sampling cube
fn settings(depth: u8, eps: Option<f32>, threads: Option<&'static ThreadPool>) -> Settings<'static> {
    Settings {
        depth,
        threads,
        world_to_model: nalgebra::Matrix4::identity(),
        multi_patch_error: eps,
        ..Default::default()
    }
}

/// The deterministic two-plane fixture: two disconnected corner patches in
/// one unit cell, expressed in the tree's [-1, +1] cube
fn two_plane_tree() -> Tree {
    let (x, y, z) = Tree::axes();
    let s = (x + y + z + 3.0) * 0.5;
    (s.clone() - 0.75).min(2.25 - s)
}

/// Recursively collects every leaf below the root
fn walk_leaves(octree: &Octree) -> Vec<(CellIndex<3>, Leaf<3>)> {
    fn rec(
        octree: &Octree,
        cell: CellIndex<3>,
        out: &mut Vec<(CellIndex<3>, Leaf<3>)>,
    ) {
        match octree[cell] {
            Cell::Leaf(leaf) => out.push((cell, leaf)),
            Cell::Empty | Cell::Full => {}
            Cell::Branch { .. } => {
                for corner in Corner::iter() {
                    rec(octree, octree.child(cell, corner), out);
                }
            }
            Cell::Invalid => panic!(),
        }
    }
    let mut out = Vec::new();
    rec(octree, CellIndex::default(), &mut out);
    out
}

/// Total number of surface vertices (one per patch) across all leaves
fn surface_vertex_count(octree: &Octree) -> usize {
    walk_leaves(octree)
        .iter()
        .map(|(_, leaf)| leaf.patch_count as usize)
        .sum()
}

/// Builds a synthetic octree of eight child leaves from raw corner masks,
/// together with fresh hermite data whose intersections sit on edge
/// midpoints and whose normals point along the crossing axis.
///
/// Mask 0 / 255 produce Empty / Full cells; anything else a sampling leaf.
fn synthetic_octree(
    child_masks: [u8; 8],
    nan_grad_on: Option<usize>,
) -> (Octree, [LeafHermiteData; 8]) {
    let mut octree = Octree::new();
    octree.cells.push([Cell::Invalid; 8]);
    let mut hermite = [LeafHermiteData::default(); 8];

    for (child, &mask) in child_masks.iter().enumerate() {
        let cell = match mask {
            0 => Cell::Empty,
            255 => Cell::Full,
            _ => {
                let pieces = CELL_TO_VERT_TO_EDGES[mask as usize];
                let leaf_index = octree.verts.len();
                // One placeholder surface vertex per patch, then one entry
                // per crossing edge, mirroring the production layout.
                for _ in 0..pieces.len() {
                    octree.verts.push(CellVertex {
                        pos: Vector3::zeros(),
                    });
                }
                let mut local_edges = Vec::new();
                let diagonal = matches!(mask, 0x01 | 0x17 | 0x80 | 0xe8);
                let diagonal_normal = if mask & 1 != 0 {
                    Vector3::new(1.0, 1.0, 1.0).normalize()
                } else {
                    Vector3::new(-1.0, -1.0, -1.0).normalize()
                };
                for piece in pieces {
                    for edge in *piece {
                        let undirected = edge.to_undirected();
                        local_edges.push((edge, undirected.index()));
                        let (start, end) = (edge.start(), edge.end());
                        let world = |corner: Corner<3>| -> Vector3<f32> {
                            let c = corner.get();
                            Vector3::new(
                                -1.0 + (child & 1) as f32 + (c & 1) as f32,
                                -1.0 + ((child >> 1) & 1) as f32
                                    + ((c >> 1) & 1) as f32,
                                -1.0 + ((child >> 2) & 1) as f32
                                    + ((c >> 2) & 1) as f32,
                            )
                        };
                        let a = world(start);
                        let b = world(end);
                        let axis =
                            (start.get() ^ end.get()).trailing_zeros() as usize;
                        let mut mid = (a + b) * 0.5;
                        if diagonal {
                            // Place the crossing on the actual plane
                            // x + y + z = c, clamped to the edge interval.
                            let c_plane = if mask & 1 != 0 { -1.5 } else { 1.5 };
                            let fixed_sum = (0..3)
                                .filter(|k| *k != axis)
                                .map(|k| a[k])
                                .sum::<f32>();
                            let want = c_plane - fixed_sum;
                            let lo = a[axis].min(b[axis]);
                            let hi = a[axis].max(b[axis]);
                            mid[axis] = want.clamp(lo, hi);
                        }
                        let mut normal = diagonal_normal;
                        if !diagonal {
                            // Generic fallback: normal along the crossing
                            // axis, pointing from filled to empty.
                            normal = Vector3::zeros();
                            normal[axis] =
                                if end.get() & (1 << axis) != 0 { 1.0 } else { -1.0 };
                        }
                        let grad = if nan_grad_on == Some(undirected.index()) {
                            Vector4::new(f32::NAN, 0.0, 0.0, 0.0)
                        } else {
                            Vector4::new(normal.x, normal.y, normal.z, 0.0)
                        };
                        hermite[child].intersections[undirected.index()] =
                            LeafIntersection {
                                pos: Vector4::new(mid.x, mid.y, mid.z, 1.0),
                                grad,
                            };
                        octree.verts.push(CellVertex { pos: mid });
                    }
                }
                Cell::Leaf(Leaf {
                    mask: CellMask::new(mask),
                    index: leaf_index,
                    patch_count: pieces.len() as u8,
                    multi_index: NO_MULTI_PATCH,
                })
            }
        };
        octree.cells[0][child] = cell;
    }
    (octree, hermite)
}

/// Canonical two-plane masks: the diagonal children carry four-corner
/// patches while the other six are single-corner leaves
const TWO_PLANE_MASKS: [u8; 8] =
    [0x17, 0x01, 0x01, 0x80, 0x01, 0x80, 0x80, 0xe8];

/// Undirected edge count and two-face manifold check on a produced mesh
fn check_manifold(mesh: &Mesh, c: &mut Check) {
    let mut oriented: BTreeMap<(usize, usize), i32> = BTreeMap::new();
    let mut incidence: BTreeMap<(usize, usize), usize> = BTreeMap::new();
    let mut distinct_vertices = true;
    for triangle in &mesh.triangles {
        distinct_vertices &= triangle.x != triangle.y
            && triangle.y != triangle.z
            && triangle.z != triangle.x;
        for (a, b) in [
            (triangle.x, triangle.y),
            (triangle.y, triangle.z),
            (triangle.z, triangle.x),
        ] {
            *oriented.entry((a, b)).or_default() += 1;
            *incidence.entry((a.min(b), a.max(b))).or_default() += 1;
        }
    }
    c.check(distinct_vertices, "every triangle must have distinct vertices");
    c.check(
        oriented.values().all(|count| *count == 1),
        "every directed edge must be emitted exactly once",
    );
    c.check(
        incidence.values().all(|count| *count == 2),
        "every undirected mesh edge must have exactly two incident faces",
    );
    c.check(
        incidence
            .keys()
            .all(|(a, b)| oriented.contains_key(&(*b, *a))),
        "the two faces of each edge must use opposite orientations",
    );
    c.check(!incidence.is_empty(), "closed mesh must contain interior edges");
}

/// Number of connected vertex components in the mesh (surface components)
fn mesh_components(mesh: &Mesh) -> usize {
    let mut adjacency: BTreeMap<usize, BTreeSet<usize>> = BTreeMap::new();
    for id in 0..mesh.vertices.len() {
        adjacency.entry(id).or_default();
    }
    for triangle in &mesh.triangles {
        for (a, b) in [
            (triangle.x, triangle.y),
            (triangle.y, triangle.z),
            (triangle.z, triangle.x),
        ] {
            adjacency.entry(a).or_default().insert(b);
            adjacency.entry(b).or_default().insert(a);
        }
    }
    let mut seen = BTreeSet::new();
    let mut components = 0;
    for start in adjacency.keys().copied().collect::<Vec<_>>() {
        if !seen.insert(start) {
            continue;
        }
        components += 1;
        let mut stack = vec![start];
        while let Some(node) = stack.pop() {
            for &next in &adjacency[&node] {
                if seen.insert(next) {
                    stack.push(next);
                }
            }
        }
    }
    components
}

/// Two closed spherical lobes straddling the center planes of the root cube
///
/// At depth 4 the lobes meet inside individual octree cells as two separate
/// corner patches, so multi-patch coarsening can collapse them while the
/// neighboring cells stay at the finest level.
fn two_sphere_tree() -> Tree {
    let (x, y, z) = Tree::axes();
    let sphere = |cx: f32, cy: f32, cz: f32, r: f32| {
        ((x.clone() - cx).square()
            + (y.clone() - cy).square()
            + (z.clone() - cz).square())
        .sqrt()
            - r
    };
    sphere(-0.15, -0.15, -0.15, 0.24)
        .min(sphere(0.15, 0.15, 0.15, 0.24))
}

/// Error budget and depth at which the closed scene produces multi leaves
const CLOSED_SCENE_EPS: f32 = 0.2;
const CLOSED_SCENE_DEPTH: u8 = 4;

/// The deterministic two-plane fixture expressed in model coordinates
///
/// `center` and `scale` mirror a `View3::from_center_and_scale` transform:
/// the sampled world cube `p in [-1, 1]^3` is mapped to model coordinates
/// `q = center + scale * p`, and the field is defined so that the same two
/// diagonal planes appear in that transformed region.
fn two_plane_tree_model(center: Vector3<f32>, scale: f32) -> Tree {
    let (x, y, z) = Tree::axes();
    let sum = (x - center.x) + (y - center.y) + (z - center.z);
    let t = sum / scale;
    let s = (t + 3.0) * 0.5;
    (s.clone() - 0.75).min(2.25 - s)
}
fn break_high_component_normals(
    masks: [u8; 8],
    hermite: &mut [LeafHermiteData; 8],
) {
    for (child, h) in hermite.iter_mut().enumerate() {
        if masks[child] & 0x80 == 0 {
            continue;
        }
        for intersection in h.intersections.iter_mut() {
            if intersection.pos.w > 0.0 {
                intersection.grad = Vector4::new(1.0, 0.0, 0.0, 0.0);
            }
        }
    }
}

////////////////////////////////////////////////////////////////////////////////
// topology

fn two_plane_collapses(c: &mut Check) {
    let shape = VmShape::from(two_plane_tree());
    let octree =
        Octree::build(&shape, &settings(1, Some(1e-4), None)).unwrap();

    let leaves = walk_leaves(&octree);
    c.check(leaves.len() == 1, "eight children must collapse to one leaf");
    let (_, leaf) = leaves[0];
    c.check(leaf.patch_count == 2, "the collapsed leaf must hold two patches");
    c.check(
        leaf.multi_index != NO_MULTI_PATCH,
        "the collapsed leaf must anchor multi-patch records",
    );
    let records = [
        octree.multi_patches[leaf.multi_index as usize],
        octree.multi_patches[leaf.multi_index as usize + 1],
    ];
    // Undirected packing is 4*t + 1*u + 2*v with the frame of each edge.
    c.check(
        records[0].contains_edge(0)
            && records[0].contains_edge(4)
            && records[0].contains_edge(8),
        "first patch owns the three parent edges around corner 0",
    );
    c.check(
        records[1].contains_edge(3)
            && records[1].contains_edge(7)
            && records[1].contains_edge(11),
        "second patch owns the three parent edges around corner 7",
    );
    let disjoint = records.iter().all(|r| {
        (0..12).filter(|e| r.contains_edge(*e)).count() == 3
    }) && records[0].edge_set & records[1].edge_set == 0;
    c.check(disjoint, "the two patches must partition the parent edges");

    // Surface vertices of the canonical fixture: (0.25)^3 and (0.75)^3 in
    // unit-cell coordinates, i.e. +/- 0.5 in tree coordinates.
    let q0 = octree.verts[leaf.index].pos;
    let q1 = octree.verts[leaf.index + 1].pos;
    c.check(
        (q0 - Vector3::new(-0.5, -0.5, -0.5)).norm() < 1e-4,
        "first patch vertex must land near (-0.5, -0.5, -0.5)",
    );
    c.check(
        (q1 - Vector3::new(0.5, 0.5, 0.5)).norm() < 1e-4,
        "second patch vertex must land near (0.5, 0.5, 0.5)",
    );
    c.check(
        surface_vertex_count(&octree) == 2,
        "surface vertex count must drop from eight to two",
    );
}

fn legacy_path_unchanged(c: &mut Check) {
    let shape = VmShape::from(two_plane_tree());
    let legacy = Octree::build(&shape, &settings(1, None, None)).unwrap();
    c.check(
        matches!(legacy.root, Cell::Branch { .. }),
        "without the flag, the parent must stay split",
    );
    c.check(
        surface_vertex_count(&legacy) == 8,
        "legacy build keeps all eight fine surface vertices",
    );
    c.check(
        legacy.multi_patches.is_empty(),
        "legacy build must not allocate multi-patch records",
    );
}

/// The two-plane field with an extra closed bubble at the cell center
fn two_plane_with_bubble_tree() -> Tree {
    let (x, y, z) = Tree::axes();
    let s = (x.clone() + y.clone() + z.clone() + 3.0) * 0.5;
    let planes = (s.clone() - 0.75).min(2.25 - s);
    let bubble = (x.square() + y.square() + z.square()).sqrt() - 0.3;
    planes.min(bubble)
}

fn internal_shell_rejected(c: &mut Check) {
    // Field-based check: the central bubble makes every fine leaf carry an
    // extra patch that never reaches the parent boundary, so the parent has
    // two candidate patches while the fine level holds sixteen fragments.
    // The collapse must stay rejected even with a very loose budget.
    let shape = VmShape::from(two_plane_with_bubble_tree());
    let rejected =
        Octree::build(&shape, &settings(1, Some(1.0), None)).unwrap();
    c.check(
        matches!(rejected.root, Cell::Branch { .. }),
        "an interior closed bubble must keep the parent split",
    );
    // The same field without the flag also stays split, but the key point is
    // that a loose eps does not silently swallow the extra fine components.
    let fine_patch_count = walk_leaves(&rejected)
        .iter()
        .map(|(_, leaf)| leaf.patch_count as usize)
        .sum::<usize>();
    c.check(
        fine_patch_count >= 16,
        "the fine level must retain all bubble fragments",
    );

    // Unit-level check that single-patch cells are not routed through the
    // multi-patch path at all.
    let mut masks2 = [0u8; 8];
    masks2[0] = 0x01;
    let (octree2, hermite2) = synthetic_octree(masks2, None);
    let result2 = multipatch::try_multi_patch_collapse(
        &octree2,
        0,
        &hermite2,
        &CellBounds::default(),
        1e-4,
    );
    c.check(
        result2.is_none(),
        "single-patch cells are not routed to the multi-patch path",
    );
}

fn synthetic_two_patch_accepted(c: &mut Check) {
    let (octree, hermite) = synthetic_octree(TWO_PLANE_MASKS, None);
    let collapse = multipatch::try_multi_patch_collapse(
        &octree,
        0,
        &hermite,
        &CellBounds::default(),
        1e-4,
    )
    .expect("synthetic two-patch geometry must collapse");
    c.check(collapse.records.len() == 2, "two records must be produced");
    c.check(collapse.positions.len() == 2, "two vertices must be solved");
    c.check(
        collapse.intersections.len() == 6,
        "all six parent-edge intersections must be retained once each",
    );
    let unique: BTreeSet<(u32, u32, u32)> = collapse
        .intersections
        .iter()
        .map(|p| (p.x.to_bits(), p.y.to_bits(), p.z.to_bits()))
        .collect();
    c.check(
        unique.len() == collapse.intersections.len(),
        "intersection positions must not be duplicated",
    );

    // Constraint conservation: every fine crossing edge the children owned
    // must appear in exactly one patch record, with no overlap or loss.
    let mut expected: BTreeSet<usize> = BTreeSet::new();
    for (child, &mask) in TWO_PLANE_MASKS.iter().enumerate() {
        for piece in CELL_TO_VERT_TO_EDGES[mask as usize] {
            for edge in *piece {
                expected.insert(
                    multipatch::fine_edge_key(
                        child,
                        edge.to_undirected().index(),
                    ),
                );
            }
        }
    }
    let mut recorded = BTreeSet::new();
    let mut overlap = false;
    for record in &collapse.records {
        for key in 0..64usize {
            if (record.fine_edges >> key) & 1 != 0 {
                overlap |= !recorded.insert(key);
            }
        }
    }
    c.check(!overlap, "a fine constraint must not be assigned to two patches");
    c.check(
        recorded == expected,
        "all fine constraints must be conserved across the collapse",
    );
}

fn mismatched_components_rejected(c: &mut Check) {
    // Swapping two child masks keeps the parent mask (and therefore the
    // candidate parent patches and parent-edge crossing sets) unchanged, but
    // rewires the fine connectivity: the actual shared-face connections no
    // longer match the parent face pairings.  A sign-only comparison would
    // accept this; the connection-based proof must reject it.
    let swapped = [
        0x17u8, 0x01, 0x01, 0x80, 0x80, 0x01, 0x80, 0xe8,
    ];
    let (octree, hermite) = synthetic_octree(swapped, None);
    let result = multipatch::try_multi_patch_collapse(
        &octree,
        0,
        &hermite,
        &CellBounds::default(),
        1.0,
    );
    c.check(
        result.is_none(),
        "reconnected fine patches must reject even with a loose eps",
    );

    // The unswapped layout with identical parent signs is accepted, proving
    // the rejection comes from connectivity rather than the corner mask.
    let (good_octree, good_hermite) =
        synthetic_octree(TWO_PLANE_MASKS, None);
    let accepted = multipatch::try_multi_patch_collapse(
        &good_octree,
        0,
        &good_hermite,
        &CellBounds::default(),
        1.0,
    );
    c.check(
        accepted.is_some(),
        "the same parent mask with correct fine connectivity must collapse",
    );

    // A disconnected fine component that never reaches a parent edge is an
    // internal shell and is rejected before any QEF work.
    let mut shell = [0u8; 8];
    shell[0] = 0x01;
    shell[7] = 0x01;
    let (shell_octree, shell_hermite) = synthetic_octree(shell, None);
    let shell_result = multipatch::try_multi_patch_collapse(
        &shell_octree,
        0,
        &shell_hermite,
        &CellBounds::default(),
        1.0,
    );
    c.check(
        shell_result.is_none(),
        "a fine component without a parent-boundary crossing is an internal shell",
    );
}

////////////////////////////////////////////////////////////////////////////////
// qef

fn independent_patch_residuals(c: &mut Check) {
    let shape = VmShape::from(two_plane_tree());
    let eps = 1e-4f32;
    let octree =
        Octree::build(&shape, &settings(1, Some(eps), None)).unwrap();
    let (_, leaf) = walk_leaves(&octree)[0];
    c.check(leaf.patch_count == 2, "fixture must collapse with two patches");

    // Recompute each patch's mean squared residual from its own constraints,
    // sampling fresh normals at the recorded intersection positions.
    let mut eval = VmShape::new_grad_slice_eval();
    let tape = shape.grad_slice_tape(Default::default());
    let threshold = (eps * 2.0).powi(2); // parent edge length L = 2 in tree coords
    for patch in 0..2 {
        let record = octree.multi_patches[leaf.multi_index as usize + patch];
        let q = octree.verts[leaf.index + patch].pos;
        let mut xs = Vec::new();
        let mut ys = Vec::new();
        let mut zs = Vec::new();
        let mut points = Vec::new();
        for edge in 0..12 {
            if !record.contains_edge(edge) {
                continue;
            }
            let intersection = leaf
                .edge(crate::mesh::types::Edge::new(edge as u8))
                .expect("recorded edge must have an intersection");
            let p = octree.verts
                [leaf.index + intersection.edge.0 as usize]
                .pos;
            points.push(p);
            xs.push(crate::types::Grad::new(p.x, 1.0, 0.0, 0.0));
            ys.push(crate::types::Grad::new(p.y, 0.0, 1.0, 0.0));
            zs.push(crate::types::Grad::new(p.z, 0.0, 0.0, 1.0));
        }
        let grads = eval
            .eval_v(&tape, &xs, &ys, &zs, &ShapeVars::<f32>::new())
            .unwrap();
        let mut residual = 0.0f32;
        for (grad, p) in grads.iter().zip(points.iter()) {
            let g: Vector4<f32> = (*grad).into();
            let n = g.xyz().normalize();
            let d = n.dot(&(q - *p));
            residual += d * d;
        }
        let count = points.len();
        let mean = residual / count as f32;
        c.check(mean.is_finite(), "residual must be finite");
        c.check(
            mean <= threshold,
            "each patch must independently satisfy the error budget",
        );
        c.check(count == 3, "each plane patch keeps three constraints");
    }
}

fn curved_patch_cannot_hide(c: &mut Check) {
    // With coherent planar normals, both patches satisfy a tight budget.
    let (good_octree, good_hermite) = synthetic_octree(TWO_PLANE_MASKS, None);
    let good = multipatch::try_multi_patch_collapse(
        &good_octree,
        0,
        &good_hermite,
        &CellBounds::default(),
        1e-6,
    );
    c.check(
        good.is_some(),
        "two planar components must satisfy a tight budget",
    );

    // Corrupt only the high-corner component; the low one stays planar.  A
    // budget that the low component passes must still reject the whole
    // collapse, proving residuals cannot be shared across patches.
    let (bad_octree, mut bad_hermite) =
        synthetic_octree(TWO_PLANE_MASKS, None);
    break_high_component_normals(TWO_PLANE_MASKS, &mut bad_hermite);
    let rejected = multipatch::try_multi_patch_collapse(
        &bad_octree,
        0,
        &bad_hermite,
        &CellBounds::default(),
        1e-6,
    );
    c.check(
        rejected.is_none(),
        "one bad patch must reject the collapse despite the other passing",
    );

    // A generous budget still accepts the same topology, so the rejection is
    // due to the per-patch error, not the topology checks.
    let accepted = multipatch::try_multi_patch_collapse(
        &bad_octree,
        0,
        &bad_hermite,
        &CellBounds::default(),
        10.0,
    );
    c.check(
        accepted.is_some(),
        "the corrupted geometry must still collapse under a loose budget",
    );
}

fn invalid_gradient_rejected(c: &mut Check) {
    let (octree, hermite) = synthetic_octree(TWO_PLANE_MASKS, Some(0));
    let result = multipatch::try_multi_patch_collapse(
        &octree,
        0,
        &hermite,
        &CellBounds::default(),
        1.0,
    );
    c.check(
        result.is_none(),
        "a NaN gradient on any constraint must reject collapse",
    );
}

fn no_constraints_rejected(c: &mut Check) {
    // An all-empty configuration has no patches at all and must never be
    // treated as a multi-patch collapse.
    let (octree, hermite) = synthetic_octree([0u8; 8], None);
    let result = multipatch::try_multi_patch_collapse(
        &octree,
        0,
        &hermite,
        &CellBounds::default(),
        1.0,
    );
    c.check(result.is_none(), "a patchless cell cannot collapse");
}

////////////////////////////////////////////////////////////////////////////////
// seams

fn closed_scene_seams(c: &mut Check) {
    let shape = VmShape::from(two_sphere_tree());
    let octree = Octree::build(
        &shape,
        &settings(CLOSED_SCENE_DEPTH, Some(CLOSED_SCENE_EPS), None),
    )
    .unwrap();
    let leaves = walk_leaves(&octree);

    let multi: Vec<_> = leaves
        .iter()
        .filter(|(_, leaf)| leaf.multi_index != NO_MULTI_PATCH)
        .collect();
    c.check(
        !multi.is_empty(),
        "scene must contain a multi-patch coarse leaf",
    );
    let coarse_depth = multi[0].0.depth;
    c.check(
        leaves.iter().any(|(cell, _)| cell.depth > coarse_depth),
        "the multi-patch leaf must have finer neighbors",
    );

    // Coarsening must reduce both the reachable leaf count and the surface
    // vertex count relative to the legacy build.
    let legacy = Octree::build(
        &shape,
        &settings(CLOSED_SCENE_DEPTH, None, None),
    )
    .unwrap();
    let legacy_leaves = walk_leaves(&legacy);
    c.check(
        leaves.len() < legacy_leaves.len(),
        "reachable leaf count must drop under coarsening",
    );
    c.check(
        surface_vertex_count(&octree) < surface_vertex_count(&legacy),
        "surface vertex count must drop under coarsening",
    );

    // The two closed components must remain geometrically separate.
    let mut component_vertices = Vec::new();
    for (_, leaf) in &multi {
        for patch in 0..leaf.patch_count as usize {
            component_vertices
                .push(octree.verts[leaf.index + patch].pos);
        }
    }
    let distinct: BTreeSet<(u32, u32, u32)> = component_vertices
        .iter()
        .map(|p| (p.x.to_bits(), p.y.to_bits(), p.z.to_bits()))
        .collect();
    c.check(
        distinct.len() >= 2,
        "coarse leaf must keep distinct vertices per component",
    );

    let mesh = octree.walk_dual();
    c.check(!mesh.triangles.is_empty(), "mesh must contain triangles");
    check_manifold(&mesh, c);
    c.check(
        mesh_components(&mesh) == 2,
        "the two closed surfaces must stay disconnected in the mesh",
    );

    // Every coarse surface vertex must actually be consumed by the mesh.
    let all_consumed = component_vertices.iter().all(|position| {
        mesh.vertices
            .iter()
            .any(|v| (*v - position).norm() < 1e-6)
    });
    c.check(
        all_consumed,
        "every coarse patch vertex must be present in the output mesh",
    );
}

fn seam_edges_join_distinct_patches(c: &mut Check) {
    let shape = VmShape::from(two_sphere_tree());
    let octree = Octree::build(
        &shape,
        &settings(CLOSED_SCENE_DEPTH, Some(CLOSED_SCENE_EPS), None),
    )
    .unwrap();
    let mesh = octree.walk_dual();

    // The two coarse surface vertices must never be joined by a triangle:
    // a bridge between them would show up as a direct edge here.
    let (_, coarse_leaf) = walk_leaves(&octree)
        .into_iter()
        .find(|(_, leaf)| leaf.multi_index != NO_MULTI_PATCH)
        .expect("multi-patch leaf exists");
    let q: Vec<_> = (0..coarse_leaf.patch_count as usize)
        .map(|patch| octree.verts[coarse_leaf.index + patch].pos)
        .collect();
    let index_of = |point: Vector3<f32>| {
        mesh.vertices
            .iter()
            .position(|v| (*v - point).norm() < 1e-6)
    };
    let indices: Vec<_> = q.iter().map(|p| index_of(*p)).collect();
    c.check(
        indices.iter().all(Option::is_some),
        "both coarse vertices must be meshed",
    );
    let indices: BTreeSet<usize> =
        indices.into_iter().flatten().collect();
    let bridged = mesh.triangles.iter().any(|triangle| {
        [triangle.x, triangle.y, triangle.z]
            .iter()
            .filter(|v| indices.contains(v))
            .count()
            > 1
    });
    c.check(!bridged, "no triangle may bridge the two coarse patches");
    check_manifold(&mesh, c);
}

////////////////////////////////////////////////////////////////////////////////
// compat

fn none_matches_legacy_output(c: &mut Check) {
    let shape = VmShape::from(two_sphere_tree());
    let legacy = Octree::build(
        &shape,
        &settings(CLOSED_SCENE_DEPTH, None, None),
    )
    .unwrap()
    .walk_dual();
    let flagged = Octree::build(
        &shape,
        &settings(CLOSED_SCENE_DEPTH, Some(f32::INFINITY), None),
    )
    .unwrap()
    .walk_dual();
    // INFINITY is not a finite eps, so the new path must refuse to run and
    // the produced mesh must be identical to the legacy mesh.
    c.check(
        legacy.vertices.len() == flagged.vertices.len(),
        "non-finite eps must not change vertex output",
    );
    c.check(
        legacy.triangles.len() == flagged.triangles.len(),
        "non-finite eps must not change triangle output",
    );
}

fn single_and_multi_thread_match(c: &mut Check) {
    let shape = VmShape::from(two_sphere_tree());
    let single = Octree::build(
        &shape,
        &settings(CLOSED_SCENE_DEPTH, Some(CLOSED_SCENE_EPS), None),
    )
    .unwrap()
    .walk_dual();
    let pooled = Octree::build(
        &shape,
        &settings(
            CLOSED_SCENE_DEPTH,
            Some(CLOSED_SCENE_EPS),
            Some(&ThreadPool::Global),
        ),
    )
    .unwrap()
    .walk_dual();
    c.check(
        single.vertices.len() == pooled.vertices.len(),
        "threaded and single-threaded builds must agree on vertices",
    );
    c.check(
        single.triangles.len() == pooled.triangles.len(),
        "threaded and single-threaded builds must agree on triangles",
    );
    check_manifold(&single, c);
    check_manifold(&pooled, c);
}

fn world_to_model_still_works(c: &mut Check) {
    let center = Vector3::new(1.0, 2.0, 3.0);
    let scale = 0.5;
    let view = crate::render::View3::from_center_and_scale(center, scale);
    let shape = VmShape::from(two_plane_tree_model(center, scale));
    let settings = Settings {
        depth: 1,
        threads: None,
        world_to_model: view.world_to_model(),
        multi_patch_error: Some(1e-4),
        cancel: CancelToken::new(),
    };
    let octree = Octree::build(&shape, &settings).unwrap();
    let leaves = walk_leaves(&octree);
    c.check(
        leaves.iter().any(|(_, leaf)| leaf.patch_count == 2),
        "multi-patch collapse must still run under a world transform",
    );
    let mesh = octree.walk_dual();
    // At depth 1 the root is a single collapsed leaf, so walk_dual emits no
    // triangles; the important invariant is that every produced vertex is
    // finite after the transform.
    for vertex in &mesh.vertices {
        c.check(
            vertex.iter().all(|f| f.is_finite()),
            "transformed vertices must remain finite",
        );
    }

    // The output vertices live in world space; the two surface vertices must
    // land at the transformed canonical locations center +/- scale*0.5.
    let mut surface = Vec::new();
    for (_, leaf) in &leaves {
        for patch in 0..leaf.patch_count as usize {
            surface.push(octree.verts[leaf.index + patch].pos);
        }
    }
    let expected = [
        center + Vector3::new(-0.5, -0.5, -0.5) * scale,
        center + Vector3::new(0.5, 0.5, 0.5) * scale,
    ];
    for want in expected {
        c.check(
            surface.iter().any(|p| (*p - want).norm() < 1e-3),
            "transformed surface vertex must match the mapped canonical point",
        );
    }
}

fn negative_eps_is_legacy(c: &mut Check) {
    let shape = VmShape::from(two_plane_tree());
    let octree =
        Octree::build(&shape, &settings(1, Some(-1.0), None)).unwrap();
    c.check(
        matches!(octree.root, Cell::Branch { .. }),
        "a negative eps must be ignored in favor of the legacy path",
    );
}


pub const CASES: &[(&str, &str, fn(&mut Check))] = &[
    ("topology", "two_plane_collapses", two_plane_collapses),
    ("topology", "legacy_path_unchanged", legacy_path_unchanged),
    ("topology", "internal_shell_rejected", internal_shell_rejected),
    ("topology", "synthetic_two_patch_accepted", synthetic_two_patch_accepted),
    ("topology", "mismatched_components_rejected", mismatched_components_rejected),
    ("qef", "independent_patch_residuals", independent_patch_residuals),
    ("qef", "curved_patch_cannot_hide", curved_patch_cannot_hide),
    ("qef", "invalid_gradient_rejected", invalid_gradient_rejected),
    ("qef", "no_constraints_rejected", no_constraints_rejected),
    ("seams", "closed_scene_seams", closed_scene_seams),
    ("seams", "seam_edges_join_distinct_patches", seam_edges_join_distinct_patches),
    ("compat", "none_matches_legacy_output", none_matches_legacy_output),
    ("compat", "single_and_multi_thread_match", single_and_multi_thread_match),
    ("compat", "world_to_model_still_works", world_to_model_still_works),
    ("compat", "negative_eps_is_legacy", negative_eps_is_legacy),
];
