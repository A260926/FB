//! Optional multi-patch coarsening
//!
//! The default collapse rule only accepts fine cells whose corner masks
//! contain a single surface patch.  When [`Settings::multi_patch_error`] is
//! set, a parent cell may instead collapse around several disconnected
//! patches at once.
//!
//! The proof that such a collapse is valid is deliberately conservative:
//! - fine surface patches are connected only through *actual* shared fine
//!   edges, never through corner sign agreement;
//! - every fine connected component must be a disk fragment whose single
//!   boundary loop lies on the parent cell boundary;
//! - fine components and candidate parent patches must be in one-to-one
//!   correspondence, with identical crossing sets on the parent's twelve
//!   edges and identical arc pairings on every parent face;
//! - each patch keeps every fine edge constraint it represents (a shared
//!   fine edge is counted once), and its QEF is solved and error-checked
//!   independently against those constraints.
//!
//! Anything that cannot be proven leaves the eight children in place.

use super::{
    cell::{CellBounds, Leaf},
    codegen::CELL_TO_VERT_TO_EDGES,
    octree::{LeafHermiteData, MultiPatch, NO_MULTI_PATCH},
    qef::QuadraticErrorSolver,
    types::{CellMask, Corner, Edge},
    Octree,
};
use crate::mesh::cell::Cell;

/// Number of distinct fine edges inside a parent cell
///
/// The fine grid has coordinates `0, 1, 2` on each axis.  A fine edge is
/// uniquely identified by its varying axis (3), its two fixed coordinates
/// (3 × 3) and its low coordinate (2): `3 * 9 * 2 = 54`.
const FINE_EDGE_COUNT: usize = 54;

/// Vertex layout of one collapsed multi-patch leaf
pub(crate) struct MultiPatchCollapse {
    /// Parent corner mask
    pub mask: CellMask<3>,
    /// One record per surface patch, in parent-table patch order
    pub records: Vec<MultiPatch>,
    /// One solved surface vertex per patch, in the same order
    pub positions: Vec<nalgebra::Vector3<f32>>,
    /// Edge intersection points, in parent-table edge iteration order
    pub intersections: Vec<nalgebra::Vector3<f32>>,
}

/// Description of one surface patch within a fine leaf child
struct FinePatch {
    /// Which of the eight children owns this patch
    child: usize,
    /// Canonical fine edges (0-53) carrying this patch's crossings
    edges: Vec<usize>,
}

/// Fine edge classification relative to the parent cell
#[derive(Copy, Clone, Debug, Eq, PartialEq)]
enum EdgeClass {
    /// Lies on a parent cube edge (two fixed coords on the boundary)
    ParentEdge,
    /// Lies inside a parent face (one fixed coord on the boundary)
    FaceInterior,
    /// Fully inside the parent cell
    Interior,
}

/// Computes the canonical fine-edge key for a local edge of child `child`
///
/// Fine grid coordinates are `child_bit + local_corner_bit`, so each value
/// is in `0, 1, 2`: a child occupies one half of the parent and shares the
/// midpoint plane with its neighbor.
pub(crate) fn fine_edge_key(child: usize, local_edge: usize) -> usize {
    let (start, end) = Edge::new(local_edge as u8).corners();
    let axis = (start.get() ^ end.get()).trailing_zeros() as usize;
    let lift = |corner: Corner<3>| -> [u8; 3] {
        let c = corner.get() as usize;
        [
            ((child & 1) + (c & 1)) as u8,
            (((child >> 1) & 1) + ((c >> 1) & 1)) as u8,
            (((child >> 2) & 1) + ((c >> 2) & 1)) as u8,
        ]
    };
    let a = lift(start);
    let b = lift(end);
    let u = (axis + 1) % 3;
    let v = (axis + 2) % 3;
    debug_assert_eq!(a[u], b[u]);
    debug_assert_eq!(a[v], b[v]);
    debug_assert_eq!(a[axis].abs_diff(b[axis]), 1);
    let low = a[axis].min(b[axis]) as usize;
    axis * 18 + (a[u] as usize * 3 + a[v] as usize) * 2 + low
}

/// Decomposes a canonical fine edge key into its grid coordinates
fn key_coords(key: usize) -> (usize, [u8; 3], u8) {
    let axis = key / 18;
    let rest = key % 18;
    let fixed = rest / 2;
    let low = (rest % 2) as u8;
    let u = (axis + 1) % 3;
    let v = (axis + 2) % 3;
    let mut g = [0u8; 3];
    g[axis] = low;
    g[u] = (fixed / 3) as u8;
    g[v] = (fixed % 3) as u8;
    (axis, g, low)
}

fn classify(key: usize) -> EdgeClass {
    let (axis, g, _) = key_coords(key);
    let u = (axis + 1) % 3;
    let v = (axis + 2) % 3;
    let boundary = |g: u8| g == 0 || g == 2;
    match (boundary(g[u]), boundary(g[v])) {
        (true, true) => EdgeClass::ParentEdge,
        (false, false) => EdgeClass::Interior,
        _ => EdgeClass::FaceInterior,
    }
}

/// Returns the parent edge index (0-11) for a fine edge on a parent cube edge
fn parent_edge_of(key: usize) -> usize {
    let (axis, g, _) = key_coords(key);
    let u = (axis + 1) % 3;
    let v = (axis + 2) % 3;
    // Packing used by `DirectedEdge::to_undirected`: 4*t + 1*u + 2*v, where
    // boundary coordinate 2 maps to bit 1.
    axis * 4 + ((g[u] == 2) as usize) + ((g[v] == 2) as usize) * 2
}

/// Checks whether a fine edge lies in the given parent face
///
/// An edge lies in a face perpendicular to `face_axis` only when it does not
/// vary along that axis and its fixed coordinate on that axis is on the
/// boundary.
fn key_on_parent_face(key: usize, face_axis: usize, side: u8) -> bool {
    let (axis, g, _) = key_coords(key);
    axis != face_axis
        && g[face_axis] == if side == 0 { 0 } else { 2 }
}

/// Inverts [`fine_edge_key`] for an edge known to cross the given child
fn local_edge_for(child: usize, key: usize) -> Option<usize> {
    let (axis, mut g, low) = key_coords(key);
    let lower = |g: u8, bit: usize| -> Option<u8> {
        let bit_value = g.checked_sub(((child >> bit) & 1) as u8)?;
        (bit_value <= 1).then_some(bit_value)
    };
    let mut local = [0u8; 3];
    for bit in 0..3 {
        local[bit] = lower(g[bit], bit)?;
    }
    let start = Corner::new(
        local[0] | (local[1] << 1) | (local[2] << 2),
    );
    g[axis] = low + 1;
    for bit in 0..3 {
        local[bit] = lower(g[bit], bit)?;
    }
    let end = Corner::new(
        local[0] | (local[1] << 1) | (local[2] << 2),
    );
    let directed = super::types::DirectedEdge::new(start, end);
    Some(directed.to_undirected().index())
}

/// Attempts a multi-patch collapse of the eight children at `root`.
///
/// Returns `None` whenever the topology cannot be proven equivalent or any
/// per-patch error check fails; callers must then keep the children.
pub(crate) fn try_multi_patch_collapse(
    octree: &Octree,
    root: usize,
    hermite_data: &[LeafHermiteData; 8],
    cell_bounds: &CellBounds<3>,
    eps: f32,
) -> Option<MultiPatchCollapse> {
    if !eps.is_finite() || eps.is_sign_negative() {
        return None;
    }
    let cells = octree.cells[root];

    // Assemble the parent occupancy mask from child occupancy.
    let mut mask = 0u8;
    for (i, &c) in cells.iter().enumerate() {
        let b = match c {
            Cell::Leaf(leaf) => {
                if leaf.patch_count == 0 {
                    return None;
                }
                (leaf.mask.index() & (1 << i) != 0) as u8
            }
            Cell::Empty => 0,
            Cell::Full => 1,
            Cell::Branch { .. } | Cell::Invalid => return None,
        };
        mask |= b << i;
    }
    if mask == 0 || mask == 255 {
        return None;
    }
    let mask = CellMask::new(mask);

    // Candidate parent patches come from the existing corner-mask table.
    let parent_pieces = CELL_TO_VERT_TO_EDGES[mask.index()];
    if parent_pieces.len() < 2 {
        // Single-patch configurations belong to the legacy collapse path.
        return None;
    }
    let parent_edge_sets: Vec<u16> = parent_pieces
        .iter()
        .map(|piece| {
            let mut set: u16 = 0;
            for e in *piece {
                set |= 1 << e.to_undirected().index();
            }
            set
        })
        .collect();

    // Collect every fine patch with the canonical fine edges it crosses.
    let mut fine: Vec<FinePatch> = Vec::new();
    // Incidence list for every canonical fine edge.
    let mut incidence: Vec<Vec<usize>> = vec![Vec::new(); FINE_EDGE_COUNT];
    for (child_index, &c) in cells.iter().enumerate() {
        let Cell::Leaf(leaf) = c else { continue };
        let pieces = CELL_TO_VERT_TO_EDGES[leaf.mask.index()];
        if pieces.len() != leaf.patch_count as usize {
            return None;
        }
        for piece in pieces.iter() {
            let mut edges = Vec::new();
            for e in *piece {
                edges.push(fine_edge_key(
                    child_index,
                    e.to_undirected().index(),
                ));
            }
            edges.sort_unstable();
            if edges.windows(2).any(|w| w[0] == w[1]) {
                return None;
            }
            let patch_id = fine.len();
            for &key in &edges {
                incidence[key].push(patch_id);
            }
            fine.push(FinePatch {
                child: child_index,
                edges,
            });
        }
    }

    // Connect fine patches that share an actual crossing fine edge.
    let mut adjacency = vec![Vec::new(); fine.len()];
    for ids in &incidence {
        for w in ids.windows(2) {
            adjacency[w[0]].push(w[1]);
            adjacency[w[1]].push(w[0]);
        }
    }
    for list in &mut adjacency {
        list.sort_unstable();
        list.dedup();
    }

    // Build connected components; every fine patch lands in exactly one.
    let mut component_of = vec![usize::MAX; fine.len()];
    let mut components: Vec<Vec<usize>> = Vec::new();
    for start in 0..fine.len() {
        if component_of[start] != usize::MAX {
            continue;
        }
        let id = components.len();
        let mut stack = vec![start];
        let mut members = Vec::new();
        component_of[start] = id;
        while let Some(n) = stack.pop() {
            members.push(n);
            for &m in &adjacency[n] {
                if component_of[m] == usize::MAX {
                    component_of[m] = id;
                    stack.push(m);
                }
            }
        }
        components.push(members);
    }

    // A shared fine edge may only join patches of one component; this is
    // guaranteed by construction but verified explicitly.
    for ids in &incidence {
        if ids
            .iter()
            .any(|&p| component_of[p] != component_of[ids[0]])
        {
            return None;
        }
    }

    // Component count must match the candidate parent patch count exactly:
    // this rejects swallowed separate patches and changed component counts.
    if components.len() != parent_pieces.len() {
        return None;
    }

    // Simply-connected topology is enforced structurally instead of by a
    // graph-tree test (a rounded disk fragment can have cyclic patch
    // adjacency without a hole): every component must reach the parent
    // boundary, every parent face graph must be a set of open arcs ending on
    // parent-edge crossings, and any in-face closed loop rejects below.

    // Per-component edge sets; every component must reach the parent boundary
    // (rejects internal closed shells).
    let mut fine_parent_edges: Vec<u16> = vec![0; components.len()];
    let mut fine_edge_sets: Vec<u64> = vec![0; components.len()];
    for (patch_id, patch) in fine.iter().enumerate() {
        let component = component_of[patch_id];
        for &key in &patch.edges {
            fine_edge_sets[component] |= 1 << key;
            if classify(key) == EdgeClass::ParentEdge {
                fine_parent_edges[component] |= 1 << parent_edge_of(key);
            }
        }
    }
    if fine_parent_edges.contains(&0) {
        return None;
    }

    // One-to-one matching between fine components and parent table patches.
    let mut component_to_parent = vec![usize::MAX; components.len()];
    let mut parent_used = vec![false; parent_pieces.len()];
    for (component, set) in fine_parent_edges.iter().enumerate() {
        let parent = parent_edge_sets.iter().position(|s| s == set)?;
        if parent_used[parent] {
            return None;
        }
        parent_used[parent] = true;
        component_to_parent[component] = parent;
    }
    if parent_used.iter().any(|u| !u) {
        return None;
    }

    // On every parent face, crossings must pair into simple arcs whose
    // endpoints are exactly the component's parent-edge crossings; any
    // in-face closed loop or dangling arc rejects the collapse.
    for component in 0..components.len() {
        if !face_pairings_match(
            component,
            &fine,
            &component_of,
            fine_parent_edges[component],
            parent_edge_sets[component_to_parent[component]],
        ) {
            return None;
        }
    }

    // Per-patch isolated constraints, QEF solve and independent residual.
    let threshold = (eps * parent_edge_length(cell_bounds)).powi(2);
    let mut solved: Vec<(usize, MultiPatch, nalgebra::Vector3<f32>)> =
        Vec::with_capacity(components.len());
    // Intersection positions keyed by parent edge (0-11) for the leaf vertex
    // layout.
    let mut parent_edge_pos: [Option<nalgebra::Vector3<f32>>; 12] =
        [None; 12];

    for component in 0..components.len() {
        let mut qef = QuadraticErrorSolver::new();
        // Fine edges owned by this component; each is counted exactly once.
        let mut owned_keys: Vec<usize> = Vec::new();
        let mut counted = [false; FINE_EDGE_COUNT];
        for &patch_id in &components[component] {
            for &key in &fine[patch_id].edges {
                if counted[key] {
                    continue;
                }
                counted[key] = true;
                owned_keys.push(key);
            }
        }
        if owned_keys.is_empty() {
            return None;
        }

        for &key in &owned_keys {
            // Deterministically pick one owning child patch for the data.
            let patch_id = *incidence[key]
                .iter()
                .find(|&&p| component_of[p] == component)?;
            let child = fine[patch_id].child;
            let local = local_edge_for(child, key)?;
            let intersection = hermite_data[child].intersections[local];
            if intersection.pos.w <= 0.0
                || intersection.grad.iter().any(|f| f.is_nan())
            {
                return None;
            }
            qef.add_intersection(
                intersection.pos.xyz(),
                intersection.grad,
            );
            if classify(key) == EdgeClass::ParentEdge {
                parent_edge_pos[parent_edge_of(key)] =
                    Some(intersection.pos.xyz());
            }
        }

        let (vertex, _solver_err) = qef.solve();
        if !vertex.pos.iter().all(|f| f.is_finite())
            || !cell_bounds.contains(vertex)
        {
            return None;
        }

        // Independent mean squared residual over this patch's own
        // constraints, using unit normals.
        let mut residual = 0.0f32;
        for &key in &owned_keys {
            let patch_id = *incidence[key]
                .iter()
                .find(|&&p| component_of[p] == component)?;
            let child = fine[patch_id].child;
            let local = local_edge_for(child, key)?;
            let intersection = hermite_data[child].intersections[local];
            let n = intersection.grad.xyz().normalize();
            let d = n.dot(&(vertex.pos - intersection.pos.xyz()));
            residual += d * d;
        }
        let mean_residual = residual / owned_keys.len() as f32;
        if !mean_residual.is_finite() || mean_residual > threshold {
            return None;
        }

        solved.push((
            component_to_parent[component],
            MultiPatch {
                edge_set: fine_parent_edges[component],
                fine_edges: fine_edge_sets[component],
            },
            vertex.pos,
        ));
    }

    // Emit records in parent-table patch order so that vertex offsets line up
    // with the mask table used by dual contouring.
    solved.sort_by_key(|(parent, _, _)| *parent);
    let records: Vec<MultiPatch> =
        solved.iter().map(|(_, r, _)| *r).collect();
    let positions: Vec<nalgebra::Vector3<f32>> =
        solved.iter().map(|(_, _, p)| *p).collect();

    // Intersection vertices follow, in parent-table edge iteration order.
    let mut intersections = Vec::new();
    for piece in parent_pieces {
        for e in *piece {
            let index = e.to_undirected().index();
            intersections.push(
                parent_edge_pos[index]
                    .expect("parent edge crossing must have a position"),
            );
        }
    }

    Some(MultiPatchCollapse {
        mask,
        records,
        positions,
        intersections,
    })
}

/// Builds the fine-crossing graph on one parent face for one component and
/// checks that it is a set of simple arcs ending on parent-edge crossings.
fn face_pairings_match(
    component: usize,
    fine: &[FinePatch],
    component_of: &[usize],
    fine_parent_set: u16,
    parent_set: u16,
) -> bool {
    for face_axis in 0..3 {
        for side in [0u8, 1] {
            let Some(fine_pairs) = fine_face_pairs(
                component,
                fine,
                component_of,
                face_axis,
                side,
            ) else {
                return false;
            };
            let parent_pairs =
                parent_face_pairs(parent_set, face_axis, side);
            if fine_pairs != parent_pairs {
                return false;
            }
            // Endpoints must be exactly the component's parent edges on
            // this face.
            let mut endpoints = std::collections::BTreeSet::new();
            for (a, b) in &fine_pairs {
                endpoints.insert(*a);
                endpoints.insert(*b);
            }
            let expected: std::collections::BTreeSet<usize> = (0..12)
                .filter(|&e| {
                    (fine_parent_set >> e) & 1 != 0
                        && edge_on_parent_face(e, face_axis, side)
                })
                .collect();
            if endpoints != expected {
                return false;
            }
        }
    }
    true
}

/// Derives arc pairings on a parent face from fine patch fragments
///
/// Returns `None` if the face contains a closed loop, a dangling arc or an
/// ambiguous four-crossing fragment.
fn fine_face_pairs(
    component: usize,
    fine: &[FinePatch],
    component_of: &[usize],
    face_axis: usize,
    side: u8,
) -> Option<Vec<(usize, usize)>> {
    // Nodes are canonical fine edges lying in this parent face.
    let mut nodes = std::collections::BTreeSet::new();
    // Links connect two nodes whose crossings are joined by a surface arc
    // inside a child patch.
    let mut links: std::collections::BTreeMap<usize, Vec<usize>> =
        std::collections::BTreeMap::new();

    for (patch_id, patch) in fine.iter().enumerate() {
        if component_of[patch_id] != component {
            continue;
        }
        let face_nodes: Vec<usize> = patch
            .edges
            .iter()
            .copied()
            .filter(|&key| key_on_parent_face(key, face_axis, side))
            .collect();
        // A cube face has four edges; a disk fragment with four crossings on
        // one face is an ambiguous configuration we refuse to prove.
        if face_nodes.len() >= 4 {
            return None;
        }
        for &key in &face_nodes {
            nodes.insert(key);
            links.entry(key).or_default();
        }
        if face_nodes.len() == 2 {
            let (a, b) = (face_nodes[0], face_nodes[1]);
            links.get_mut(&a).unwrap().push(b);
            links.get_mut(&b).unwrap().push(a);
        }
    }

    if nodes.is_empty() {
        return Some(Vec::new());
    }

    // Every face-interior node must have degree two (one arc from each side
    // of the shared child face); every parent-edge node degree one.
    for list in links.values_mut() {
        list.sort_unstable();
        list.dedup();
    }
    for &key in &nodes {
        let degree = links[&key].len();
        match classify(key) {
            EdgeClass::ParentEdge => {
                if degree != 1 {
                    return None;
                }
            }
            EdgeClass::FaceInterior => {
                if degree != 2 {
                    return None;
                }
            }
            EdgeClass::Interior => unreachable!(),
        }
    }

    // Walk paths from each parent-edge endpoint, rejecting any cycle.
    let mut pairs = Vec::new();
    let mut visited = std::collections::BTreeSet::new();
    for &start in &nodes {
        if classify(start) != EdgeClass::ParentEdge
            || visited.contains(&start)
        {
            continue;
        }
        let mut prev = usize::MAX;
        let mut cur = start;
        loop {
            if !visited.insert(cur) {
                return None;
            }
            let neighbors: Vec<usize> = links[&cur]
                .iter()
                .copied()
                .filter(|&n| n != prev)
                .collect();
            if neighbors.len() != 1 {
                return None;
            }
            prev = cur;
            cur = neighbors[0];
            if classify(cur) == EdgeClass::ParentEdge {
                if !visited.insert(cur) {
                    return None;
                }
                break;
            }
        }
        pairs.push((parent_edge_of(start), parent_edge_of(cur)));
    }
    if visited.len() != nodes.len() {
        // Unvisited nodes form a closed loop with no parent-edge endpoints,
        // or an interior fragment with no route to the boundary.
        return None;
    }
    pairs.sort();
    Some(pairs)
}

/// Arc pairings implied by a parent table patch on one parent face
fn parent_face_pairs(
    parent_set: u16,
    face_axis: usize,
    side: u8,
) -> Vec<(usize, usize)> {
    let edges: Vec<usize> = (0..12)
        .filter(|&e| {
            (parent_set >> e) & 1 != 0
                && edge_on_parent_face(e, face_axis, side)
        })
        .collect();
    // Parent table patches on a single face always carry 0 or 2 crossing
    // edges; anything else cannot be matched and yields no valid pairing.
    if edges.is_empty() {
        Vec::new()
    } else if edges.len() == 2 {
        vec![(edges[0], edges[1])]
    } else {
        // Force a mismatch with any fine-derived pairing.
        vec![(usize::MAX, usize::MAX)]
    }
}

/// Checks whether parent edge `e` lies on the given parent face
fn edge_on_parent_face(e: usize, face_axis: usize, side: u8) -> bool {
    let (start, end) = Edge::new(e as u8).corners();
    let bit = 1 << face_axis;
    let want = side == 1;
    let on = |c: Corner<3>| (c.get() & bit != 0) == want;
    on(start) && on(end)
}

/// Edge length of a cell in tree coordinates
fn parent_edge_length(bounds: &CellBounds<3>) -> f32 {
    bounds[crate::mesh::types::X].upper()
        - bounds[crate::mesh::types::X].lower()
}

/// Constructs the leaf descriptor for a successful collapse
pub(crate) fn leaf_of(
    collapse: &MultiPatchCollapse,
    index: usize,
) -> Leaf<3> {
    Leaf {
        mask: collapse.mask,
        index,
        patch_count: collapse.records.len() as u8,
        multi_index: NO_MULTI_PATCH,
    }
}
