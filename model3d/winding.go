package model3d

import (
	"fmt"
	"math"
)

// Four pi, precomputed for the normalized winding number.
const fourPi = 4 * math.Pi

// WindingResult is the outcome of a WindingTree.Evaluate query.
type WindingResult struct {
	// Value is the generalized winding number at the query
	// point: the sum of the directed solid angles of all
	// triangles divided by 4*pi.
	//
	// Unlike parity-based containment, the value is never
	// folded into 0/1 and never clamped:
	//
	//   - A single outward-oriented closed shell contributes
	//     +1 to points inside it and 0 outside.
	//   - Two overlapping same-oriented shells contribute +2
	//     in their shared interior.
	//   - An inward-oriented inner shell cancels its outer
	//     shell in the cavity (contribution 0 there).
	//   - An open surface yields a genuine fractional value
	//     between the inside and outside limits: it is the
	//     fraction of the full view around the point that is
	//     covered by the oriented patch, signed by the
	//     patch orientation. Reversing every triangle's
	//     winding reverses the sign.
	Value float64

	// ErrorBound is a conservative bound on the geometric
	// approximation error in Value, i.e. the true winding
	// number lies in [Value-ErrorBound, Value+ErrorBound]
	// (apart from ordinary double-precision round-off, which
	// is bounded separately by 1e-10).
	//
	// It is zero when every triangle was evaluated exactly.
	// The bounds of all accepted aggregate nodes draw from
	// one shared tolerance budget; the total reported bound
	// never exceeds the absTol passed to Evaluate, rather
	// than each node consuming a full tolerance allowance.
	ErrorBound float64

	// ExactTriangles is the number of triangles for which
	// the exact directed solid angle was actually computed
	// during this query.
	ExactTriangles int

	// ApproximatedNodes is the number of hierarchy nodes
	// whose far-field aggregate was actually accepted during
	// this query.
	ApproximatedNodes int
}

// WindingTree is a read-only spatial hierarchy over a
// snapshot of a mesh's triangles that answers generalized
// winding number queries.
//
// Each node stores its bounding box together with
// far-field summaries of its triangle cluster: the total
// (unsigned) area, area-weighted center, bounding radius,
// net vector area, a dipole matrix, and the exact second
// area moment. A query walks the tree per query point:
// clusters that are far enough away are accepted with a
// conservative remainder bound drawn from a shared
// tolerance budget, while nearby clusters are expanded
// until individual triangles can be evaluated exactly.
//
// A WindingTree is safe for concurrent read-only use by
// multiple goroutines. All per-query bookkeeping lives in
// local state, never on the tree.
type WindingTree struct {
	root *windingNode

	// numTriangles is the size of the snapshot.
	numTriangles int
}

type windingNode struct {
	min Coord3D
	max Coord3D

	// leaf is non-nil for leaf nodes.
	leaf *Triangle

	children [2]*windingNode

	// area is the sum of unsigned triangle areas in the node.
	area float64

	// center is the area-weighted average triangle center.
	center Coord3D

	// radius bounds every triangle center (and, via the
	// triangle bounding boxes, every triangle point) by
	// this distance from center.
	radius float64

	// vectorArea is V = integral of the unit normal over
	// the cluster, i.e. the net oriented area vector.
	vectorArea Coord3D

	// dipole is the 3x3 matrix T with entries
	// T_{jk} = integral (x-center)_j n_k dA over the
	// cluster, stored row-wise.
	dipole [3]Coord3D

	// moment2 is integral |x-center|^2 dA over the
	// cluster. It is the exact second area moment, not a
	// radius-based estimate, and gives a tight remainder
	// bound for well-behaved clusters.
	moment2 float64
}

// NewWindingTree builds a WindingTree from a snapshot of
// the triangles currently in mesh.
//
// The tree owns its own copy of the geometry: adding or
// removing triangles from mesh afterwards, or mutating a
// Triangle that was passed in, does not affect the tree.
//
// Non-finite triangle coordinates and zero-area
// (degenerate) triangles are rejected with an error. An
// empty mesh produces a valid tree whose queries return
// the zero result.
func NewWindingTree(mesh *Mesh) (*WindingTree, error) {
	if mesh == nil {
		return nil, fmt.Errorf("NewWindingTree: mesh is nil")
	}
	return newWindingTreeTriangles(mesh.TriangleSlice())
}

func newWindingTreeTriangles(faces []*Triangle) (*WindingTree, error) {
	snapshot := make([]*Triangle, len(faces))
	for i, f := range faces {
		if f == nil {
			return nil, fmt.Errorf("NewWindingTree: triangle %d is nil", i)
		}
		t := *f
		for _, c := range t {
			if !isFiniteCoord(c) {
				return nil, fmt.Errorf("NewWindingTree: triangle %d contains non-finite coordinate %v", i, c)
			}
		}
		cross := t[1].Sub(t[0]).Cross(t[2].Sub(t[0]))
		if cross.NormSquared() == 0 {
			return nil, fmt.Errorf("NewWindingTree: triangle %d has zero area: %v", i, t)
		}
		snapshot[i] = &t
	}
	if len(snapshot) == 0 {
		return &WindingTree{}, nil
	}
	// Reuse the library's existing spatial ordering.
	GroupTriangles(snapshot)
	return &WindingTree{
		root:         newWindingNode(snapshot),
		numTriangles: len(snapshot),
	}, nil
}

func newWindingNode(faces []*Triangle) *windingNode {
	node := &windingNode{
		min: faces[0].Min(),
		max: faces[0].Max(),
	}
	if len(faces) == 1 {
		node.leaf = faces[0]
		node.accumulateTriangle(faces[0])
		node.radius = node.boundingRadius()
		return node
	}
	mid := len(faces) / 2
	node.children = [2]*windingNode{
		newWindingNode(faces[:mid]),
		newWindingNode(faces[mid:]),
	}
	for _, child := range node.children {
		node.min = node.min.Min(child.min)
		node.max = node.max.Max(child.max)
		node.area += child.area
		node.center = node.center.Add(child.center.Scale(child.area))
	}
	node.center = node.center.Scale(1 / node.area)
	node.associateChildren()
	node.radius = node.boundingRadius()
	return node
}

// accumulateTriangle fills the far-field summary of a leaf
// triangle, taking the node center (the triangle centroid)
// as the expansion origin.
func (n *windingNode) accumulateTriangle(t *Triangle) {
	cross := t[1].Sub(t[0]).Cross(t[2].Sub(t[0]))
	area := cross.Norm() / 2
	n.area = area
	n.center = t[0].Add(t[1]).Add(t[2]).Scale(1.0 / 3.0)
	n.vectorArea = cross.Scale(0.5)
	// Centered at the triangle centroid, the first moment
	// integral over the triangle vanishes.
	n.dipole = [3]Coord3D{}
	// Polar area moment about the centroid:
	// integral |x-c|^2 dA = A*(a^2+b^2+c^2)/36 for the
	// three side lengths a, b, c.
	side2 := t[0].SquaredDist(t[1]) + t[1].SquaredDist(t[2]) + t[2].SquaredDist(t[0])
	n.moment2 = area * side2 / 36
}

// associateChildren aggregates vector area, dipole
// moments, and second moments after the node center has
// been fixed. The parallel-axis shifts move each child's
// expansion origin from its own center to the node center.
func (n *windingNode) associateChildren() {
	for _, child := range n.children {
		n.vectorArea = n.vectorArea.Add(child.vectorArea)
		shift := child.center.Sub(n.center)
		for j := 0; j < 3; j++ {
			// T_child expressed about n.center:
			// T'_{jk} = T_{jk} + shift_j * V_k.
			row := child.dipole[j].Add(child.vectorArea.Scale(shift.Array()[j]))
			n.dipole[j] = n.dipole[j].Add(row)
		}
		// Parallel-axis theorem for the scalar moment.
		n.moment2 += child.moment2 + child.area*shift.NormSquared()
	}
}

// boundingRadius returns the maximum distance from the
// node center to any corner of its bounding box. Every
// point of every contained triangle lies within it.
func (n *windingNode) boundingRadius() float64 {
	var r2 float64
	corners := [8]Coord3D{
		XYZ(n.min.X, n.min.Y, n.min.Z),
		XYZ(n.min.X, n.min.Y, n.max.Z),
		XYZ(n.min.X, n.max.Y, n.min.Z),
		XYZ(n.min.X, n.max.Y, n.max.Z),
		XYZ(n.max.X, n.min.Y, n.min.Z),
		XYZ(n.max.X, n.min.Y, n.max.Z),
		XYZ(n.max.X, n.max.Y, n.min.Z),
		XYZ(n.max.X, n.max.Y, n.max.Z),
	}
	for _, c := range corners {
		if d2 := c.SquaredDist(n.center); d2 > r2 {
			r2 = d2
		}
	}
	return math.Sqrt(r2)
}

// Evaluate computes the generalized winding number of the
// snapshot at point.
//
// If absTol is zero, every triangle is evaluated with the
// exact solid angle formula and ErrorBound is zero.
//
// If absTol is positive, distant triangle clusters may be
// replaced by their monopole-plus-dipole far-field
// aggregate. Every accepted cluster contributes a
// conservative remainder bound, and the total reported
// ErrorBound never exceeds absTol: the budget is shared
// across the whole query rather than consumed per node.
// Nearby geometry always falls back to per-triangle exact
// evaluation.
//
// Negative or non-finite absTol values, non-finite query
// points, and points lying exactly on a triangle produce
// errors.
func (w *WindingTree) Evaluate(point Coord3D, absTol float64) (WindingResult, error) {
	if !isFiniteCoord(point) {
		return WindingResult{}, fmt.Errorf("WindingTree.Evaluate: query point must be finite, got %v", point)
	}
	if math.IsNaN(absTol) || math.IsInf(absTol, 0) {
		return WindingResult{}, fmt.Errorf("WindingTree.Evaluate: absTol must be finite, got %g", absTol)
	}
	if absTol < 0 {
		return WindingResult{}, fmt.Errorf("WindingTree.Evaluate: absTol must be non-negative, got %g", absTol)
	}
	res := WindingResult{}
	if w.root == nil {
		return res, nil
	}
	q := &windingQuery{
		point: point,
		exact: absTol == 0,
	}
	value, err := q.evalNode(w.root, absTol)
	if err != nil {
		return WindingResult{}, err
	}
	res.Value = value
	res.ExactTriangles = q.exactTriangles
	res.ApproximatedNodes = q.approximatedNodes
	if absTol == 0 {
		res.ErrorBound = 0
	} else {
		// Every acceptance was individually within budget,
		// so the sum cannot exceed absTol; clamp away
		// summation round-off.
		res.ErrorBound = math.Min(q.errorBound, absTol)
	}
	return res, nil
}

type windingQuery struct {
	point Coord3D
	exact bool

	errorBound        float64
	exactTriangles    int
	approximatedNodes int
}

// evalNode returns the (possibly approximate) winding
// contribution of a node, whose share of the total
// tolerance budget is budget.
//
// A node that cannot be accepted within its share splits
// that share between its children proportionally to their
// second area moments, since the remainder bound scales
// with that moment. Hence the bounds of all accepted nodes
// never exceed the root budget (absTol), while far-away
// clusters are accepted as high in the tree as the global
// error target allows rather than starving behind earlier
// subtrees.
func (q *windingQuery) evalNode(n *windingNode, budget float64) (float64, error) {
	if n.leaf != nil {
		if err := checkPointNotOnTriangle(q.point, n.leaf); err != nil {
			return 0, err
		}
		q.exactTriangles++
		return triangleSolidAngle(q.point, n.leaf) / fourPi, nil
	}
	if !q.exact {
		if bound := n.remainderBound(q.point); bound <= budget {
			q.errorBound += bound
			q.approximatedNodes++
			return n.farField(q.point), nil
		}
	}
	m0 := n.children[0].moment2
	m1 := n.children[1].moment2
	b0 := budget * m0 / (m0 + m1)
	b1 := budget - b0
	v1, err := q.evalNode(n.children[0], b0)
	if err != nil {
		return 0, err
	}
	v2, err := q.evalNode(n.children[1], b1)
	if err != nil {
		return 0, err
	}
	return v1 + v2, nil
}

// farField evaluates the monopole-plus-dipole
// approximation of the cluster.
//
// For g(x) = (x-p)/|x-p|^3 the solid angle is
// integral g(x).n dA; expanding g about the node center
// gives g(c).V + tr(J*T), with V the net vector area, T
// the dipole matrix, and J the Jacobian of g at c.
func (n *windingNode) farField(p Coord3D) float64 {
	rho := n.center.Sub(p)
	d2 := rho.NormSquared()
	if d2 == 0 {
		return 0
	}
	d := math.Sqrt(d2)
	d3 := d2 * d
	d5 := d3 * d2

	// Monopole: g(c) . V.
	mono := rho.Dot(n.vectorArea) / d3

	// Dipole: tr(T)/d^3 - 3 (rho^T T rho)/d^5, using
	// J_ij = delta_ij/d^3 - 3 rho_i rho_j/d^5.
	var trT, rhoTrho float64
	for j := 0; j < 3; j++ {
		trT += n.dipole[j].Array()[j]
		for k := 0; k < 3; k++ {
			rhoTrho += rho.Array()[j] * n.dipole[j].Array()[k] * rho.Array()[k]
		}
	}
	dipole := trT/d3 - 3*rhoTrho/d5
	return (mono + dipole) / fourPi
}

// remainderBound returns a conservative upper bound on the
// absolute error of farField for this node as seen from p.
//
// d is the distance from p to the node's bounding box; if
// p is inside (or on) the box, d is zero and no finite
// bound exists, forcing expansion.
//
// When the cluster is well separated (its radius s satisfies
// s <= d/4), the Taylor expansion of the kernel
// g(x) = (x-p)/|x-p|^3 about the node center converges, and
// the monopole and dipole terms are evaluated exactly, so
// the remainder starts at second order. For the kernel
// derivatives, |d^k g| <= (2k+1)!!/r^(k+2); every point of
// the Taylor segment stays inside the bounding box, so
// r >= d throughout. Summing the second-order term and the
// geometric higher-order tail (ratio at most 1/4) against
// the exact second area moment gives the constant 20:
//
//	|error| <= 20 * moment2 / d^4.
//
// The moment uses unsigned area internally, so mutually
// cancelling opposite-facing patches only loosen the bound,
// never invalidate it; slender triangles carry their true
// tiny second moment.
//
// Otherwise a regime-independent bound is used: the exact
// solid angle of any planar patch beyond distance d is at
// most its area over d^2, the monopole at most A/d^2, and
// the dipole at most 12*A*s/d^3, giving
//
//	|error| <= 3*A/d^2 + 16*A*s/d^3.
//
// That loose value normally exceeds the tolerance budget,
// so the node is expanded, which is the correct fallback.
func (n *windingNode) remainderBound(p Coord3D) float64 {
	d2 := pointToBoundsDistSquared(p, n.min, n.max)
	if d2 == 0 {
		return math.Inf(1)
	}
	d := math.Sqrt(d2)
	s := n.radius
	if s <= d/4 {
		d4 := d2 * d2
		return 20 * n.moment2 / d4
	}
	d3 := d2 * d
	return 3*n.area/d2 + 16*n.area*s/d3
}

// triangleSolidAngle computes the exact directed solid
// angle subtended by triangle t at point p using the
// Van Oosterom-Strackee arctangent formula.
func triangleSolidAngle(p Coord3D, t *Triangle) float64 {
	a := t[0].Sub(p)
	b := t[1].Sub(p)
	c := t[2].Sub(p)
	na := a.Norm()
	nb := b.Norm()
	nc := c.Norm()
	num := a.Dot(b.Cross(c))
	den := na*nb*nc + a.Dot(b)*nc + b.Dot(c)*na + c.Dot(a)*nb
	return 2 * math.Atan2(num, den)
}

// checkPointNotOnTriangle reports an error when p lies on
// the closed triangle, including its edges and vertices.
func checkPointNotOnTriangle(p Coord3D, t *Triangle) error {
	v1 := t[1].Sub(t[0])
	v2 := t[2].Sub(t[0])
	w := p.Sub(t[0])
	normal := v1.Cross(v2)
	nn := normal.NormSquared()
	if nn == 0 {
		// Defensive: the constructor rejects such faces.
		return fmt.Errorf("winding query encountered degenerate triangle %v", t)
	}
	// Least-squares barycentric coordinates of the
	// projection of p onto the triangle plane.
	d00 := v1.Dot(v1)
	d01 := v1.Dot(v2)
	d11 := v2.Dot(v2)
	det := d00*d11 - d01*d01
	b1 := (d11*w.Dot(v1) - d01*w.Dot(v2)) / det
	b2 := (d00*w.Dot(v2) - d01*w.Dot(v1)) / det
	b0 := 1 - b1 - b2

	size := math.Max(math.Max(v1.Norm(), v2.Norm()), v1.Sub(v2).Norm())
	planarTol := 1e-12 * math.Max(1, size)
	baryTol := 1e-10
	if b0 >= -baryTol && b1 >= -baryTol && b2 >= -baryTol {
		planarDist := math.Abs(w.Dot(normal)) / math.Sqrt(nn)
		if planarDist <= planarTol {
			return fmt.Errorf("WindingTree.Evaluate: point %v lies on triangle %v", p, t)
		}
	}
	return nil
}

func isFiniteCoord(c Coord3D) bool {
	return !math.IsNaN(c.X) && !math.IsInf(c.X, 0) &&
		!math.IsNaN(c.Y) && !math.IsInf(c.Y, 0) &&
		!math.IsNaN(c.Z) && !math.IsInf(c.Z, 0)
}

// windingSDF implements FaceSDF with the true geometric
// distance of the mesh distance hierarchy and a sign taken
// from the generalized winding number instead of ray
// parity.
type windingSDF struct {
	tree      *WindingTree
	mdf       *meshDistFunc
	threshold float64
}

// NewWindingSDF creates a FaceSDF whose distance magnitude,
// nearest point, and nearest face come from the library's
// exact geometric distance path, while the sign is decided
// by the generalized winding number:
//
//	sign = +1 (inside) if |winding| > threshold
//	sign = -1 (outside) otherwise
//
// A zero distance is reported as zero regardless of the
// winding value (the winding number itself is undefined on
// the surface).
//
// threshold must be a finite positive number. An empty
// mesh is rejected.
//
// The SDF keeps its own snapshot of the geometry together
// with its own WindingTree, so subsequent mesh edits do not
// affect it; the face returned by FaceSDF always belongs to
// that same snapshot, and the winding sign can never be
// computed from a different mesh than the distance.
//
// On an open surface the winding number is fractional: it
// is the oriented fraction of the view around a point that
// the patch covers, approaching +1 deep inside the intended
// solid and 0 far outside, with intermediate values only
// near the rim. threshold therefore selects how much
// oriented coverage makes a point count as inside; 0.5
// treats a closed shell (winding 1 inside) as solid, keeps
// overlapping shells solid in their common interior, and
// only classifies a thin neighborhood of an open boundary
// ambiguously.
//
// Approximate winding intervals never decide a
// threshold-crossing sign: whenever the error interval
// reaches the threshold, the query is tightened, falling
// back to an exact per-triangle summation if necessary.
//
// Example:
//
//	mesh := model3d.NewMeshRect(model3d.XYZ(-1, -1, -1),
//	    model3d.XYZ(1, 1, 1))
//	sdf, err := model3d.NewWindingSDF(mesh, 0.5)
//	if err != nil {
//	    log.Fatal(err)
//	}
//	fmt.Println(sdf.SDF(model3d.Coord3D{}))  // +1: inside
//	fmt.Println(sdf.SDF(model3d.XYZ(2, 0, 0))) // -1: outside
func NewWindingSDF(mesh *Mesh, threshold float64) (FaceSDF, error) {
	if math.IsNaN(threshold) || math.IsInf(threshold, 0) || threshold <= 0 {
		return nil, fmt.Errorf("NewWindingSDF: threshold must be a finite positive number, got %g", threshold)
	}
	if mesh == nil || mesh.NumTriangles() == 0 {
		return nil, fmt.Errorf("NewWindingSDF: mesh must contain at least one triangle")
	}
	tree, err := NewWindingTree(mesh)
	if err != nil {
		return nil, err
	}
	faces := tree.snapshotFaces()
	GroupTriangles(faces)
	return &windingSDF{
		tree:      tree,
		mdf:       newMeshDistFunc(faces),
		threshold: threshold,
	}, nil
}

// snapshotFaces collects the triangles owned by the tree.
func (w *WindingTree) snapshotFaces() []*Triangle {
	if w.root == nil {
		return nil
	}
	faces := make([]*Triangle, 0, w.numTriangles)
	var collect func(*windingNode)
	collect = func(n *windingNode) {
		if n.leaf != nil {
			faces = append(faces, n.leaf)
			return
		}
		collect(n.children[0])
		collect(n.children[1])
	}
	collect(w.root)
	return faces
}

func (s *windingSDF) Min() Coord3D {
	return s.mdf.min
}

func (s *windingSDF) Max() Coord3D {
	return s.mdf.max
}

func (s *windingSDF) SDF(c Coord3D) float64 {
	dist := math.Inf(1)
	s.mdf.Dist(c, &dist, nil, nil)
	if dist == 0 {
		return 0
	}
	if s.inside(c) {
		return dist
	}
	return -dist
}

func (s *windingSDF) PointSDF(c Coord3D) (Coord3D, float64) {
	dist := math.Inf(1)
	point := Coord3D{}
	s.mdf.Dist(c, &dist, &point, nil)
	if dist == 0 {
		return point, 0
	}
	if !s.inside(c) {
		dist = -dist
	}
	return point, dist
}

func (s *windingSDF) NormalSDF(c Coord3D) (Coord3D, float64) {
	face, _, dist := s.FaceSDF(c)
	return face.Normal(), dist
}

func (s *windingSDF) FaceSDF(c Coord3D) (*Triangle, Coord3D, float64) {
	dist := math.Inf(1)
	point := Coord3D{}
	var face *Triangle
	s.mdf.Dist(c, &dist, &point, &face)
	if dist == 0 {
		return face, point, 0
	}
	if !s.inside(c) {
		dist = -dist
	}
	return face, point, dist
}

// inside decides the sign with the winding number,
// tightening the approximation until the error interval no
// longer reaches the threshold. An interval that still
// straddles the threshold eventually falls back to an exact
// summation, so an approximate interval alone never decides
// a threshold-crossing sign. Equality at an exact summation
// counts as outside, matching the strict
// |winding| > threshold rule.
func (s *windingSDF) inside(c Coord3D) bool {
	tol := math.Max(s.threshold/4, 1e-6)
	for {
		res, err := s.tree.Evaluate(c, tol)
		if err != nil {
			// Only reachable for invalid inputs; surface
			// points are caught by the zero-distance path.
			return false
		}
		magnitude := math.Abs(res.Value)
		if magnitude-res.ErrorBound > s.threshold {
			return true
		}
		if magnitude+res.ErrorBound < s.threshold {
			return false
		}
		if res.ErrorBound == 0 {
			// Exact summation: settle equality definitively.
			return magnitude > s.threshold
		}
		tol /= 2
	}
}
