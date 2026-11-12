//-----------------------------------------------------------------------------
/*

Bounded Dual Contouring

Like DualContouringV2, but every voxel vertex is the true minimizer of the
Hermite quadratic error function (QEF) *inside the closed voxel box*.

There is no center-push term and no post-hoc coordinate clamping:

  - the unconstrained QEF is solved in voxel-local coordinates (so results are
    invariant to world translation and scale),
  - if the unconstrained minimum leaves the voxel, the true constrained
    minimum on the box boundary is found by recursively solving the active
    faces, edges and corners,
  - rank-deficient systems keep every minimum-norm residual solution, then the
    minimizer closest to the mean Hermite point is chosen (a single plane keeps
    the tangential mean position),
  - only when the Hermite data carries no usable constraint at all does the
    vertex fall back to the voxel center.

*/
//-----------------------------------------------------------------------------

package dc

import (
	"fmt"
	"math"

	"github.com/deadsy/sdfx/render"
	"github.com/deadsy/sdfx/sdf"
	"github.com/deadsy/sdfx/vec/conv"
	v3 "github.com/deadsy/sdfx/vec/v3"
	"github.com/deadsy/sdfx/vec/v3i"
	"gonum.org/v1/gonum/mat"
)

// Compile-time guarantee that the bounded renderer is a render.Render3, so it
// can be handed to render.CollectTriangles, render.ToSTL and friends.
var _ render.Render3 = (*DualContouringBounded)(nil)

//-----------------------------------------------------------------------------
// PUBLIC INTERFACE
//-----------------------------------------------------------------------------

// DualContouringBounded renders an SDF3 with dual contouring, placing every
// voxel vertex at the QEF minimum constrained to the voxel's closed box.
// It implements render.Render3 and can be used with render.CollectTriangles
// and render.ToSTL.
type DualContouringBounded struct {
	meshCells int // number of cells on the longest axis of the bounding box

	// see sdf.Raycast3
	RaycastScaleAndSigmoid, RaycastStepScale, RaycastEpsilon float64
	// see sdf.Raycast3
	RaycastMaxSteps int
}

// NewDualContouringBounded returns a bounded dual contouring renderer.
// meshCells is the number of voxels on the longest bounding-box axis.
func NewDualContouringBounded(meshCells int) *DualContouringBounded {
	return &DualContouringBounded{
		meshCells:              meshCells,
		RaycastScaleAndSigmoid: 0,
		RaycastStepScale:       1,
		RaycastEpsilon:         1e-4,
		RaycastMaxSteps:        1000,
	}
}

// Info returns a string describing the rendered volume.
func (b *DualContouringBounded) Info(s sdf.SDF3) string {
	resolution, cells := b.getCells(s)
	return fmt.Sprintf("%dx%dx%d, resolution %.2f", cells.X, cells.Y, cells.Z, resolution)
}

// Render produces a 3d triangle mesh over the bounding volume of an sdf3.
func (b *DualContouringBounded) Render(sdf3 sdf.SDF3, output sdf.Triangle3Writer) {
	_, cells := b.getCells(sdf3)
	s := &dcSdf{sdf3, map[v3.Vec]float64{}}
	vertexBuffer, vertexVoxelInfo, vertexVoxelInfoIndexed := b.placeVertices(s, cells)
	b.generateTriangles(s, vertexBuffer, vertexVoxelInfo, vertexVoxelInfoIndexed, output)
	output.Close()
}

func (b *DualContouringBounded) getCells(s sdf.SDF3) (float64, v3i.Vec) {
	bbSize := s.BoundingBox().Size()
	resolution := bbSize.MaxComponent() / float64(b.meshCells)
	return resolution, conv.V3ToV3i(bbSize.DivScalar(resolution))
}

//-----------------------------------------------------------------------------
// MAIN ALGORITHM
//-----------------------------------------------------------------------------

func (b *DualContouringBounded) placeVertices(s *dcSdf, cells v3i.Vec) (buf []v3.Vec, bufMap []*dcVoxelInfo, bufMapIndexed map[v3i.Vec]*dcVoxelInfo) {
	buf = make([]v3.Vec, 0, dcMaxI(32, cells.X*cells.Y*cells.Z/100))
	bufMap = make([]*dcVoxelInfo, 0, dcMaxI(32, cells.X*cells.Y*cells.Z/100))
	bufMapIndexed = make(map[v3i.Vec]*dcVoxelInfo, dcMaxI(32, cells.X*cells.Y*cells.Z/100))
	// Hermite data buffers (all crossing edges are kept, not just the first six).
	points := make([]v3.Vec, 0, 12)
	normals := make([]v3.Vec, 0, 12)
	bb := s.BoundingBox()
	cellSize := bb.Size().Div(conv.V3iToV3(cells))
	cellIndex := v3i.Vec{}
	for cellIndex.X = 0; cellIndex.X < cells.X; cellIndex.X++ {
		for cellIndex.Y = 0; cellIndex.Y < cells.Y; cellIndex.Y++ {
			for cellIndex.Z = 0; cellIndex.Z < cells.Z; cellIndex.Z++ {
				cellStart := bb.Min.Add(cellSize.Mul(conv.V3iToV3(cellIndex)))
				vertexPos, active := b.placeVertex(s, cellStart, cellSize, points[:0], normals[:0])
				if !active {
					continue
				}
				bufIndex := len(buf)
				buf = append(buf, vertexPos)
				info := &dcVoxelInfo{
					cellIndex: cellIndex,
					bufIndex:  bufIndex,
					cellStart: cellStart,
					cellSize:  cellSize,
				}
				bufMapIndexed[cellIndex] = info
				bufMap = append(bufMap, info)
			}
		}
	}
	return
}

// placeVertex collects the Hermite data of every surface-crossing edge and
// solves the box-constrained QEF. It returns false for fully inside/outside
// voxels, where no vertex belongs.
func (b *DualContouringBounded) placeVertex(s *dcSdf, cellStart, cellSize v3.Vec, points []v3.Vec, normals []v3.Vec) (v3.Vec, bool) {
	inside := b.computeCornersInside(s, cellStart, cellSize)
	if inside == 0 || inside == math.MaxUint8 {
		return v3.Vec{}, false
	}

	for _, edge := range dcEdges {
		if ((inside >> edge.X) & 1) == ((inside >> edge.Y) & 1) {
			continue // not a crossing edge
		}
		cornerPos1 := cellStart.Add(dcCorners[edge.X].Mul(cellSize))
		cornerPos2 := cellStart.Add(dcCorners[edge.Y].Mul(cellSize))
		dir := cornerPos2.Sub(cornerPos1)
		dirLength := dir.Length()
		edgeSurfPos, t, _ := sdf.Raycast3(s, cornerPos1, dir, b.RaycastScaleAndSigmoid, b.RaycastStepScale,
			b.RaycastEpsilon, dirLength*2, b.RaycastMaxSteps)
		if t < 0 || t > dirLength {
			edgeSurfPos = dcApproximateZeroCrossingPosition(s, cornerPos1, cornerPos2)
		}
		edgeSurfNormal := sdf.Normal3(s, edgeSurfPos, 1e-3)
		points = append(points, edgeSurfPos)
		normals = append(normals, edgeSurfNormal)
	}

	return boundedQEFPosition(points, normals, cellStart, cellSize), true
}

func (b *DualContouringBounded) computeCornersInside(s *dcSdf, cellStart v3.Vec, cellSize v3.Vec) uint8 {
	inside := uint8(0)
	for i, corner := range dcCorners {
		isSolid := s.evaluateCached(cellStart.Add(corner.Mul(cellSize))) < 0
		if isSolid {
			inside = inside | (1 << i)
		}
	}
	return inside
}

// generateTriangles reuses the V2 dual-grid connectivity: every crossing of a
// far edge emits the quad of the four surrounding voxel vertices, split into
// two consistently oriented triangles.
func (b *DualContouringBounded) generateTriangles(s *dcSdf, vertices []v3.Vec, info []*dcVoxelInfo, infoI map[v3i.Vec]*dcVoxelInfo, output sdf.Triangle3Writer) {
	for _, voxelInfo := range info {
		k0 := voxelInfo.bufIndex
		cellIndex := voxelInfo.cellIndex

		inside := b.computeCornersInside(s, voxelInfo.cellStart, voxelInfo.cellSize)

		for ai := 0; ai < 3; ai++ {
			edge := dcFarEdges[ai]
			if ((inside >> edge.X) & 1) == ((inside >> edge.Y) & 1) {
				continue // not a crossing
			}

			var k1, k2, k3 *dcVoxelInfo
			if ai == 0 {
				k1, _ = infoI[cellIndex.Add(v3i.Vec{X: 0, Y: 0, Z: 1})]
				k2, _ = infoI[cellIndex.Add(v3i.Vec{X: 0, Y: 1, Z: 0})]
				k3, _ = infoI[cellIndex.Add(v3i.Vec{X: 0, Y: 1, Z: 1})]
			} else if ai == 1 {
				k1, _ = infoI[cellIndex.Add(v3i.Vec{X: 0, Y: 0, Z: 1})]
				k2, _ = infoI[cellIndex.Add(v3i.Vec{X: 1, Y: 0, Z: 0})]
				k3, _ = infoI[cellIndex.Add(v3i.Vec{X: 1, Y: 0, Z: 1})]
			} else {
				k1, _ = infoI[cellIndex.Add(v3i.Vec{X: 0, Y: 1, Z: 0})]
				k2, _ = infoI[cellIndex.Add(v3i.Vec{X: 1, Y: 0, Z: 0})]
				k3, _ = infoI[cellIndex.Add(v3i.Vec{X: 1, Y: 1, Z: 0})]
			}

			if k1 == nil || k2 == nil || k3 == nil {
				continue
			}

			t := []*sdf.Triangle3{
				{vertices[k0], vertices[k1.bufIndex], vertices[k3.bufIndex]},
				{vertices[k0], vertices[k3.bufIndex], vertices[k2.bufIndex]},
			}

			// Get the normals right.
			if ((inside >> edge.X) & 1) != uint8(ai&1) {
				t[0] = dcFlip(t[0])
				t[1] = dcFlip(t[1])
			}
			if !t[0].Degenerate(0) && !t[1].Degenerate(0) {
				output.Write(t)
			}
		}
	}
}

//-----------------------------------------------------------------------------
// BOX-CONSTRAINED QEF SOLVER
//-----------------------------------------------------------------------------

// boundedQEFInput is the voxel-local Hermite data for one voxel.
// Points are expressed relative to the voxel origin, so the closed voxel is
// always the unit cube [0,1]^3 regardless of world translation or scale.
type boundedQEFInput struct {
	points  []v3.Vec // Hermite intersection points (voxel local)
	normals []v3.Vec // unit normals, one per point
}

// boundedQEFResult is the outcome of the constrained QEF solve.
type boundedQEFResult struct {
	local    v3.Vec  // minimizer in voxel-local coordinates [0,1]^3
	world    v3.Vec  // minimizer in world coordinates
	residual float64 // sum of squared signed plane distances at the minimizer
	rank     int     // numerical rank of the normal matrix
	mean     v3.Vec  // mean Hermite point (voxel local)
}

// boundedQEFSolve minimizes sum_i (n_i . x - n_i . p_i)^2 over the closed
// voxel box. It is package-private so the dc tests can exercise the solver
// directly without exposing a generic math library.
//
// cellStart and cellSize describe the voxel in world coordinates; points and
// normals are the Hermite intersections and their unit normals.
func boundedQEFSolve(points, normals []v3.Vec, cellStart, cellSize v3.Vec) boundedQEFResult {
	local := make([]v3.Vec, len(points))
	// A world plane n.(x-p) = 0 becomes (D n).(u - lp) = 0 under x = o + D u.
	// The local plane coefficients are therefore D n (componentwise), not n:
	// this keeps the local objective identical to the world objective even
	// when the voxel is not cubic, and makes it invariant to world scale.
	localNormals := make([]v3.Vec, len(normals))
	mean := v3.Vec{}
	for i, p := range points {
		lp := v3.Vec{
			X: (p.X - cellStart.X) / cellSize.X,
			Y: (p.Y - cellStart.Y) / cellSize.Y,
			Z: (p.Z - cellStart.Z) / cellSize.Z,
		}
		local[i] = lp
		mean = mean.Add(lp)
		localNormals[i] = v3.Vec{
			X: cellSize.X * normals[i].X,
			Y: cellSize.Y * normals[i].Y,
			Z: cellSize.Z * normals[i].Z,
		}
	}
	if len(local) != 0 {
		mean = mean.DivScalar(float64(len(local)))
	}

	in := boundedQEFInput{points: local, normals: localNormals}
	x, rank, residual := solveBoundedQEF(in)

	world := v3.Vec{
		X: cellStart.X + x.X*cellSize.X,
		Y: cellStart.Y + x.Y*cellSize.Y,
		Z: cellStart.Z + x.Z*cellSize.Z,
	}
	return boundedQEFResult{local: x, world: world, residual: residual, rank: rank, mean: mean}
}

// boundedQEFPosition is the convenience entry point used by the renderer:
// solve the constrained QEF and return the world-space vertex position. With
// no effective constraint the vertex lands on the voxel center.
func boundedQEFPosition(points, normals []v3.Vec, cellStart, cellSize v3.Vec) v3.Vec {
	res := boundedQEFSolve(points, normals, cellStart, cellSize)
	return res.world
}

// planeSystem holds the normal equations of a QEF restricted to an affine
// sub-box of the voxel (the cube itself, a face, an edge or a corner).
//
// On a face/edge some coordinates are fixed (in fixed[i]) and the free
// coordinates are minimized over their own [0,1] intervals. Substituting the
// fixed coordinates keeps the reduced system in the same least-squares form,
// so one code path handles every recursion level.
type planeSystem struct {
	ata   *mat.SymDense // 3x3 normal matrix A^T A
	atb   *mat.VecDense // A^T d, where d_i = n_i . p_i
	btb   float64       // d^T d
	np    int           // number of Hermite planes
	mean  v3.Vec        // mean Hermite point (local)
	fixed [3]bool       // coordinates pinned by an enclosing face/edge
	value [3]float64    // pinned coordinate values
}

// solveBoundedQEF returns the constrained minimizer, the numerical rank of the
// normal matrix and the minimum residual.
func solveBoundedQEF(in boundedQEFInput) (x v3.Vec, rank int, residual float64) {
	sys := buildPlaneSystem(in)

	// Unconstrained minimum (minimum residual; tie-break towards the mean).
	xUnc, r := sys.unconstrained()
	if inUnitBox(xUnc) {
		return xUnc, r, sys.residualAt(xUnc)
	}

	// The unconstrained minimum leaves the voxel: the constrained minimum lies
	// on the box boundary. Search faces, edges and corners rather than clamping
	// coordinates independently (clamping is generally not a minimizer).
	x = sys.constrainedMin()
	return x, r, sys.residualAt(x)
}

// buildPlaneSystem assembles the normal equations from the Hermite planes.
// The coefficient vectors need not be unit length: boundedQEFSolve passes
// D-scaled local normals, whose magnitude encodes the voxel scaling so the
// local objective matches the world objective exactly.
func buildPlaneSystem(in boundedQEFInput) *planeSystem {
	sys := &planeSystem{
		ata: mat.NewSymDense(3, nil),
		atb: mat.NewVecDense(3, nil),
	}
	ata := sys.ata
	atb := sys.atb
	for i, n := range in.normals {
		if n.Length() == 0 {
			continue // promised never to happen, but a zero row carries no constraint
		}
		d := n.Dot(in.points[i])
		ata.SetSym(0, 0, ata.At(0, 0)+n.X*n.X)
		ata.SetSym(0, 1, ata.At(0, 1)+n.X*n.Y)
		ata.SetSym(0, 2, ata.At(0, 2)+n.X*n.Z)
		ata.SetSym(1, 1, ata.At(1, 1)+n.Y*n.Y)
		ata.SetSym(1, 2, ata.At(1, 2)+n.Y*n.Z)
		ata.SetSym(2, 2, ata.At(2, 2)+n.Z*n.Z)
		atb.SetVec(0, atb.AtVec(0)+n.X*d)
		atb.SetVec(1, atb.AtVec(1)+n.Y*d)
		atb.SetVec(2, atb.AtVec(2)+n.Z*d)
		sys.btb += d * d
		sys.np++
		sys.mean = sys.mean.Add(in.points[i])
	}
	if sys.np > 0 {
		sys.mean = sys.mean.DivScalar(float64(sys.np))
	}
	return sys
}

// qefRankTolerance is the relative singular-value cutoff for the rank of the
// normal matrix. Singular values below this fraction of the largest are
// treated as rank deficiency (unconstrained directions), not as constraints.
const qefRankTolerance = 1e-12

// unconstrained returns the minimum-residual point of the full 3D problem,
// choosing among all minimum-residual points the one closest to the mean
// Hermite point, plus the numerical rank of the normal matrix.
func (s *planeSystem) unconstrained() (v3.Vec, int) {
	if s.np == 0 {
		// No constraint at all: the center is the documented fallback and is
		// the mean-closest point of the whole voxel only by convention.
		return v3.Vec{X: 0.5, Y: 0.5, Z: 0.5}, 0
	}
	var svd mat.SVD
	if !svd.Factorize(s.ata, mat.SVDThin) {
		return v3.Vec{X: 0.5, Y: 0.5, Z: 0.5}, 0
	}
	rank := svd.Rank(qefRankTolerance)
	if rank == 0 {
		// Every point is an equivalent minimizer: choose the mean Hermite
		// point (a convex combination of in-voxel points, hence in the box).
		return s.mean, 0
	}

	// Minimum-norm solution of A^T A x = A^T d.
	var x0 mat.VecDense
	svd.SolveVecTo(&x0, s.atb, rank)
	base := vecFromDense(&x0)

	if rank == 3 {
		return base, rank
	}
	// Rank deficient: every minimizer is base + v with v in the null space.
	// Pick the one closest to the mean Hermite point.
	return projectOntoNull(base, svdNullDirections(&svd, rank), s.mean), rank
}

// svdNullDirections returns the null-space basis vectors of the normal matrix
// (columns of V beyond the numerical rank).
func svdNullDirections(svd *mat.SVD, rank int) []v3.Vec {
	var v mat.Dense
	svd.VTo(&v)
	dirs := make([]v3.Vec, 0, 3-rank)
	for j := rank; j < 3; j++ {
		dirs = append(dirs, v3.Vec{X: v.At(0, j), Y: v.At(1, j), Z: v.At(2, j)})
	}
	return dirs
}

// projectOntoNull returns the point base + null-space component that lies
// closest to mean. The null directions are orthonormalized before projection.
func projectOntoNull(base v3.Vec, nullDirs []v3.Vec, mean v3.Vec) v3.Vec {
	ortho := make([]v3.Vec, 0, len(nullDirs))
	for _, d := range nullDirs {
		for _, e := range ortho {
			d = d.Sub(e.MulScalar(d.Dot(e)))
		}
		l := d.Length()
		if l <= 1e-15 {
			continue
		}
		ortho = append(ortho, d.DivScalar(l))
	}
	x := base
	delta := mean.Sub(base)
	for _, e := range ortho {
		x = x.Add(e.MulScalar(delta.Dot(e)))
	}
	return x
}

// constrainedMin finds the true minimum over the closed unit cube once the
// unconstrained minimum is known to be outside it. Every boundary stratum
// (6 faces, 12 edges, 8 corners) is solved in its free coordinates.
func (s *planeSystem) constrainedMin() v3.Vec {
	best := v3.Vec{X: 0.5, Y: 0.5, Z: 0.5}
	bestErr := math.Inf(1)
	consider := func(cand v3.Vec) {
		if !inUnitBox(cand) {
			return
		}
		err := s.residualAt(cand)
		if err < bestErr || (err == bestErr && cand.Sub(s.mean).Length2() < best.Sub(s.mean).Length2()) {
			bestErr = err
			best = cand
		}
	}

	// Faces: pin one coordinate to 0 or 1.
	for axis := 0; axis < 3; axis++ {
		for _, val := range [2]float64{0, 1} {
			face := s.restrict([3]bool{}, [3]float64{}, axis, val)
			xf, ok := face.solveAffine()
			if ok {
				consider(xf)
			}
			// Edges of this face: pin a second coordinate.
			for axis2 := axis + 1; axis2 < 3; axis2++ {
				for _, val2 := range [2]float64{0, 1} {
					edge := s.restrict(face.fixed, face.value, axis2, val2)
					xe, ok := edge.solveAffine()
					if ok {
						consider(xe)
					}
				}
			}
		}
	}
	// Corners: pin all three coordinates.
	for cx := 0; cx < 2; cx++ {
		for cy := 0; cy < 2; cy++ {
			for cz := 0; cz < 2; cz++ {
				consider(v3.Vec{X: float64(cx), Y: float64(cy), Z: float64(cz)})
			}
		}
	}
	return best
}

// restrict returns a copy of the system with one more coordinate pinned.
func (s *planeSystem) restrict(fixed [3]bool, value [3]float64, axis int, val float64) *planeSystem {
	fixed[axis] = true
	value[axis] = val
	return &planeSystem{
		ata:   s.ata,
		atb:   s.atb,
		btb:   s.btb,
		np:    s.np,
		mean:  s.mean,
		fixed: fixed,
		value: value,
	}
}

// solveAffine minimizes the QEF on the affine subspace defined by the pinned
// coordinates, over the remaining [0,1] intervals. The free-coordinate normal
// equations are obtained by substituting the fixed coordinates.
func (s *planeSystem) solveAffine() (v3.Vec, bool) {
	free := make([]int, 0, 3)
	for i := 0; i < 3; i++ {
		if !s.fixed[i] {
			free = append(free, i)
		}
	}
	x := v3.Vec{X: s.value[0], Y: s.value[1], Z: s.value[2]}
	if len(free) == 0 {
		return x, true
	}

	nf := len(free)
	red := mat.NewSymDense(nf, nil)
	rhs := mat.NewVecDense(nf, nil)
	for a := 0; a < nf; a++ {
		ia := free[a]
		// rhs_a = (A^T d)_a - sum_j fixed (A^T A)_{a,j} value_j
		rhs.SetVec(a, s.atb.AtVec(ia))
		for j := 0; j < 3; j++ {
			if s.fixed[j] {
				rhs.SetVec(a, rhs.AtVec(a)-s.ata.At(ia, j)*s.value[j])
			}
		}
		for b := 0; b < nf; b++ {
			red.SetSym(a, b, s.ata.At(ia, free[b]))
		}
	}

	var svd mat.SVD
	if !svd.Factorize(red, mat.SVDThin) {
		return v3.Vec{}, false
	}
	rank := svd.Rank(qefRankTolerance)

	xf := make([]float64, nf)
	if rank == 0 {
		// No constraint in the free directions: keep the mean Hermite position
		// (projected onto the affine subspace), never the voxel center.
		for a, ia := range free {
			xf[a] = s.mean.Get(ia)
		}
	} else {
		var sol mat.VecDense
		svd.SolveVecTo(&sol, rhs, rank)
		for a := range free {
			xf[a] = sol.AtVec(a)
		}
		if rank < nf {
			// Tie-break the free null directions towards the mean position.
			base := v3.Vec{X: s.value[0], Y: s.value[1], Z: s.value[2]}
			for a, ia := range free {
				base.Set(ia, sol.AtVec(a))
			}
			var v mat.Dense
			svd.VTo(&v)
			dirs := make([]v3.Vec, 0, nf-rank)
			for j := rank; j < nf; j++ {
				d := v3.Vec{}
				for a, ia := range free {
					d.Set(ia, v.At(a, j))
				}
				dirs = append(dirs, d)
			}
			closest := projectOntoNull(base, dirs, s.mean)
			for a, ia := range free {
				xf[a] = closest.Get(ia)
			}
		}
	}

	for a, ia := range free {
		x.Set(ia, xf[a])
	}
	return x, true
}

// residualAt evaluates the QEF objective sum_i (n_i . x - d_i)^2 directly from
// the normal equations: x^T A^T A x - 2 x^T A^T d + d^T d.
func (s *planeSystem) residualAt(x v3.Vec) float64 {
	xv := mat.NewVecDense(3, []float64{x.X, x.Y, x.Z})
	var ax mat.VecDense
	ax.MulVec(s.ata, xv)
	q := mat.Dot(&ax, xv)
	l := mat.Dot(s.atb, xv)
	return q - 2*l + s.btb
}

// inUnitBox reports whether x lies in the closed unit cube, with a small
// tolerance for solver round-off.
func inUnitBox(x v3.Vec) bool {
	const eps = 1e-10
	return x.X >= -eps && x.X <= 1+eps &&
		x.Y >= -eps && x.Y <= 1+eps &&
		x.Z >= -eps && x.Z <= 1+eps &&
		!math.IsNaN(x.X) && !math.IsInf(x.X, 0)
}

func vecFromDense(v *mat.VecDense) v3.Vec {
	return v3.Vec{X: v.AtVec(0), Y: v.AtVec(1), Z: v.AtVec(2)}
}
