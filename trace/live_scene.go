package trace

import (
	"fmt"
	"math"

	"github.com/peterstace/grayt/scene"
	"github.com/peterstace/grayt/xmath"
)

// generation is a single published scene within an Instance. It bundles the
// camera, acceleration structure, accumulator and counters that make up one
// consistent view of the world. Generations are immutable once published
// (the accumulator and completed counter have their own synchronisation), so
// capturing a *generation yields a consistent snapshot.
type generation struct {
	id        uint64
	cam       camera
	accel     accelerationStructure
	accum     *accumulator
	completed int64 // atomic
}

// accelForObjects builds the production acceleration structure for a set of
// objects. Empty scenes (no placements, or all placements hidden) are given a
// structure that misses every ray rather than an empty grid, which cannot be
// constructed.
func accelForObjects(objs []object) accelerationStructure {
	if len(objs) == 0 {
		return newListAccelerationStructure(nil)
	}
	return newGrid(4, objs)
}

// UpdateScene compiles the given live scene and, if it is valid, publishes it
// as the current generation of the instance. It returns the generation
// identifier of the new scene. Generation identifiers are handed out in
// publish order: the scene loaded at startup is generation 0, and each
// successful update is one higher than the previously published generation.
//
// The scene is fully compiled before anything is published, and the published
// generation owns an independent copy of all submitted data: once UpdateScene
// returns, the caller may freely reuse or mutate the input slices. On error
// the current generation, camera, primitives, accumulation and worker
// settings are left untouched.
func (in *Instance) UpdateScene(live scene.LiveScene) (uint64, error) {
	cam, objs, err := compileLiveScene(live)
	if err != nil {
		return 0, err
	}
	gen := &generation{
		cam:   cam,
		accel: accelForObjects(objs),
		accum: newAccumulator(in.dim),
	}
	in.cond.L.Lock()
	gen.id = in.nextGenID
	in.nextGenID++
	in.current = gen
	in.cond.L.Unlock()
	in.cond.Broadcast()
	return gen.id, nil
}

// compileLiveScene validates a live scene and compiles it into a camera and a
// list of world space objects. Each prototype is compiled into local
// primitives exactly once; every visible placement wraps those shared
// primitives with its own transform, producing one object per (placement,
// primitive) pair without expanding the world geometry.
func compileLiveScene(live scene.LiveScene) (camera, []object, error) {
	if err := validateCamera(live.Camera); err != nil {
		return camera{}, nil, fmt.Errorf("invalid camera: %v", err)
	}

	locals := make([][]object, len(live.Prototypes))
	for i, proto := range live.Prototypes {
		if err := validateObjects(proto.Objects); err != nil {
			return camera{}, nil, fmt.Errorf("prototype %d: %v", i, err)
		}
		locals[i] = buildObjects(proto.Objects)
	}

	var objs []object
	for i, p := range live.Placements {
		if p.Prototype < 0 || p.Prototype >= len(live.Prototypes) {
			return camera{}, nil, fmt.Errorf(
				"placement %d: prototype index %d out of range", i, p.Prototype)
		}
		fwd, inv, nrm, err := newPlacementTransforms(p.Transform)
		if err != nil {
			return camera{}, nil, fmt.Errorf("placement %d: %v", i, err)
		}
		if !p.Visible {
			continue
		}
		for _, local := range locals[p.Prototype] {
			minBound, maxBound := local.Surface.bound()
			wMin, wMax := fwd.transformBounds(minBound, maxBound)
			objs = append(objs, object{
				Surface: &transformedSurface{
					local:    local.Surface,
					fwd:      fwd,
					inv:      inv,
					nrm:      nrm,
					minBound: wMin,
					maxBound: wMax,
				},
				Material: local.Material,
			})
		}
	}
	return newCamera(live.Camera), objs, nil
}

// affineTransform is a row major 3x4 affine matrix; the last column is the
// translation.
type affineTransform struct {
	m [12]float64
}

func (t affineTransform) apply(p xmath.Vector) xmath.Vector {
	return xmath.Vect(
		t.m[0]*p.X+t.m[1]*p.Y+t.m[2]*p.Z+t.m[3],
		t.m[4]*p.X+t.m[5]*p.Y+t.m[6]*p.Z+t.m[7],
		t.m[8]*p.X+t.m[9]*p.Y+t.m[10]*p.Z+t.m[11],
	)
}

func (t affineTransform) applyDir(d xmath.Vector) xmath.Vector {
	return xmath.Vect(
		t.m[0]*d.X+t.m[1]*d.Y+t.m[2]*d.Z,
		t.m[4]*d.X+t.m[5]*d.Y+t.m[6]*d.Z,
		t.m[8]*d.X+t.m[9]*d.Y+t.m[10]*d.Z,
	)
}

// transformBounds maps a local axis aligned bound into the world by
// transforming all eight of its corners and taking their envelope.
func (t affineTransform) transformBounds(min, max xmath.Vector) (xmath.Vector, xmath.Vector) {
	lo := xmath.Vect(math.Inf(+1), math.Inf(+1), math.Inf(+1))
	hi := xmath.Vect(math.Inf(-1), math.Inf(-1), math.Inf(-1))
	for i := 0; i < 8; i++ {
		corner := xmath.Vect(
			pickComponent(i, 0, min.X, max.X),
			pickComponent(i, 1, min.Y, max.Y),
			pickComponent(i, 2, min.Z, max.Z),
		)
		world := t.apply(corner)
		lo = lo.Min(world)
		hi = hi.Max(world)
	}
	return lo, hi
}

func pickComponent(corner, axis int, min, max float64) float64 {
	if corner&(1<<uint(axis)) != 0 {
		return max
	}
	return min
}

// newPlacementTransforms validates a placement transform and derives the
// forward (local to world), inverse (world to local) and inverse transpose
// (local normal to world normal) transforms.
func newPlacementTransforms(m [12]float64) (fwd, inv, nrm affineTransform, err error) {
	for i, v := range m {
		if math.IsNaN(v) || math.IsInf(v, 0) {
			err = fmt.Errorf("transform element %d is not finite: %v", i, v)
			return
		}
	}
	fwd = affineTransform{m: m}

	// Invert the 3x3 linear part using the adjugate over the determinant.
	a, b, c := m[0], m[1], m[2]
	d, e, f := m[4], m[5], m[6]
	g, h, i := m[8], m[9], m[10]
	det := a*(e*i-f*h) - b*(d*i-f*g) + c*(d*h-e*g)
	if det == 0 || math.IsNaN(det) || math.IsInf(det, 0) {
		err = fmt.Errorf("transform is singular")
		return
	}
	inv3 := [9]float64{
		(e*i - f*h) / det,
		(c*h - b*i) / det,
		(b*f - c*e) / det,
		(f*g - d*i) / det,
		(a*i - c*g) / det,
		(c*d - a*f) / det,
		(d*h - e*g) / det,
		(b*g - a*h) / det,
		(a*e - b*d) / det,
	}
	for j, v := range inv3 {
		if math.IsNaN(v) || math.IsInf(v, 0) {
			err = fmt.Errorf("transform inverse element %d is not finite: %v", j, v)
			return
		}
	}

	// The inverse affine maps p to inv3*(p - t) = inv3*p - inv3*t.
	tx, ty, tz := m[3], m[7], m[11]
	inv = affineTransform{m: [12]float64{
		inv3[0], inv3[1], inv3[2], -(inv3[0]*tx + inv3[1]*ty + inv3[2]*tz),
		inv3[3], inv3[4], inv3[5], -(inv3[3]*tx + inv3[4]*ty + inv3[5]*tz),
		inv3[6], inv3[7], inv3[8], -(inv3[6]*tx + inv3[7]*ty + inv3[8]*tz),
	}}

	// Normals transform by the inverse transpose of the linear part.
	nrm = affineTransform{m: [12]float64{
		inv3[0], inv3[3], inv3[6], 0,
		inv3[1], inv3[4], inv3[7], 0,
		inv3[2], inv3[5], inv3[8], 0,
	}}
	return
}

// transformedSurface wraps a shared local primitive with a placement
// transform. Rays are mapped into local space (without normalising the local
// direction, so distances stay in the world ray's parameterisation) and
// normals are mapped back to world space with the inverse transpose.
type transformedSurface struct {
	local              surface
	fwd                affineTransform // local to world
	inv                affineTransform // world to local
	nrm                affineTransform // local normal to world normal
	minBound, maxBound xmath.Vector
}

func (s *transformedSurface) intersect(r xmath.Ray) (intersection, bool) {
	localRay := xmath.Ray{
		Start: s.inv.apply(r.Start),
		Dir:   s.inv.applyDir(r.Dir),
	}
	isect, hit := s.local.intersect(localRay)
	if !hit {
		return intersection{}, false
	}
	return intersection{
		unitNormal: s.nrm.applyDir(isect.unitNormal).Unit(),
		distance:   isect.distance,
	}, true
}

func (s *transformedSurface) bound() (xmath.Vector, xmath.Vector) {
	return s.minBound, s.maxBound
}

// recompute derives the inverse transform, normal transform and world bound
// from the forward transform. It is used after in-place mutations; the
// transform is assumed to stay invertible.
func (s *transformedSurface) recompute() {
	fwd, inv, nrm, err := newPlacementTransforms(s.fwd.m)
	if err != nil {
		panic(fmt.Sprintf("transformed surface mutation made transform singular: %v", err))
	}
	s.fwd, s.inv, s.nrm = fwd, inv, nrm
	s.minBound, s.maxBound = s.fwd.transformBounds(s.local.bound())
}

func (s *transformedSurface) translate(v xmath.Vector) {
	s.fwd.m[3] += v.X
	s.fwd.m[7] += v.Y
	s.fwd.m[11] += v.Z
	s.recompute()
}

func (s *transformedSurface) rotate(u xmath.Vector, rads float64) {
	// Compose the forward transform with a rotation about the origin.
	rot := func(p xmath.Vector) xmath.Vector { return p.Rotate(u, rads) }
	s.fwd = composeRotation(rot, s.fwd)
	s.recompute()
}

func (s *transformedSurface) scale(f float64) {
	for i := range s.fwd.m {
		s.fwd.m[i] *= f
	}
	s.recompute()
}

// composeRotation returns rot ∘ t for a pure rotation rot about the origin.
func composeRotation(rot func(xmath.Vector) xmath.Vector, t affineTransform) affineTransform {
	var out affineTransform
	cols := [3]xmath.Vector{
		rot(xmath.Vect(t.m[0], t.m[4], t.m[8])),
		rot(xmath.Vect(t.m[1], t.m[5], t.m[9])),
		rot(xmath.Vect(t.m[2], t.m[6], t.m[10])),
	}
	tr := rot(xmath.Vect(t.m[3], t.m[7], t.m[11]))
	out.m = [12]float64{
		cols[0].X, cols[1].X, cols[2].X, tr.X,
		cols[0].Y, cols[1].Y, cols[2].Y, tr.Y,
		cols[0].Z, cols[1].Z, cols[2].Z, tr.Z,
	}
	return out
}

func validateCamera(conf scene.Camera) error {
	for name, v := range map[string]float64{
		"location.x":               conf.Location.X,
		"location.y":               conf.Location.Y,
		"location.z":               conf.Location.Z,
		"looking_at.x":             conf.LookingAt.X,
		"looking_at.y":             conf.LookingAt.Y,
		"looking_at.z":             conf.LookingAt.Z,
		"up_direction.x":           conf.UpDirection.X,
		"up_direction.y":           conf.UpDirection.Y,
		"up_direction.z":           conf.UpDirection.Z,
		"field_of_view_in_radians": conf.FieldOfViewInRadians,
		"focal_length":             conf.FocalLength,
		"focal_ratio":              conf.FocalRatio,
	} {
		if math.IsNaN(v) || math.IsInf(v, 0) {
			return fmt.Errorf("%v is not finite: %v", name, v)
		}
	}
	viewDir := conf.LookingAt.Sub(conf.Location)
	if viewDir.LengthSq() == 0 {
		return fmt.Errorf("location and looking_at coincide")
	}
	if conf.UpDirection.LengthSq() == 0 {
		return fmt.Errorf("up_direction is zero")
	}
	if viewDir.Cross(conf.UpDirection).LengthSq() == 0 {
		return fmt.Errorf("up_direction is parallel to the view direction")
	}
	if conf.FieldOfViewInRadians <= 0 || conf.FieldOfViewInRadians >= math.Pi {
		return fmt.Errorf("field_of_view_in_radians %v not in (0, pi)", conf.FieldOfViewInRadians)
	}
	if conf.FocalLength <= 0 {
		return fmt.Errorf("focal_length %v is not positive", conf.FocalLength)
	}
	if conf.FocalRatio <= 0 {
		return fmt.Errorf("focal_ratio %v is not positive", conf.FocalRatio)
	}
	if conf.AspectWide <= 0 || conf.AspectHigh <= 0 {
		return fmt.Errorf("aspect %vx%v is not positive", conf.AspectWide, conf.AspectHigh)
	}
	return nil
}

func validateObjects(objs []scene.Object) error {
	for i, o := range objs {
		if err := validateSurface(o.Surface); err != nil {
			return fmt.Errorf("object %d: %v", i, err)
		}
	}
	return nil
}

func validateSurface(s scene.Surface) error {
	finite := func(name string, v float64) error {
		if math.IsNaN(v) || math.IsInf(v, 0) {
			return fmt.Errorf("%v is not finite: %v", name, v)
		}
		return nil
	}
	finiteVec := func(name string, v xmath.Vector) error {
		if err := finite(name+".x", v.X); err != nil {
			return err
		}
		if err := finite(name+".y", v.Y); err != nil {
			return err
		}
		return finite(name+".z", v.Z)
	}
	positive := func(name string, v float64) error {
		if err := finite(name, v); err != nil {
			return err
		}
		if v <= 0 {
			return fmt.Errorf("%v is not positive: %v", name, v)
		}
		return nil
	}

	for i, x := range s.Triangles {
		if err := finiteVec(fmt.Sprintf("triangles[%d].a", i), x.A); err != nil {
			return err
		}
		if err := finiteVec(fmt.Sprintf("triangles[%d].b", i), x.B); err != nil {
			return err
		}
		if err := finiteVec(fmt.Sprintf("triangles[%d].c", i), x.C); err != nil {
			return err
		}
		if x.B.Sub(x.A).Cross(x.C.Sub(x.A)).LengthSq() == 0 {
			return fmt.Errorf("triangles[%d] is degenerate", i)
		}
	}
	for i, x := range s.AlignedBoxes {
		if err := finiteVec(fmt.Sprintf("aligned_boxes[%d].a", i), x.CornerA); err != nil {
			return err
		}
		if err := finiteVec(fmt.Sprintf("aligned_boxes[%d].b", i), x.CornerB); err != nil {
			return err
		}
		if x.CornerA.X == x.CornerB.X || x.CornerA.Y == x.CornerB.Y || x.CornerA.Z == x.CornerB.Z {
			return fmt.Errorf("aligned_boxes[%d] does not strictly increase on every axis", i)
		}
	}
	for i, x := range s.Spheres {
		if err := finiteVec(fmt.Sprintf("spheres[%d].center", i), x.Center); err != nil {
			return err
		}
		if err := positive(fmt.Sprintf("spheres[%d].radius", i), x.Radius); err != nil {
			return err
		}
	}
	for i, x := range s.AlignXSquares {
		for name, v := range map[string]float64{"x": x.X, "y1": x.Y1, "y2": x.Y2, "z1": x.Z1, "z2": x.Z2} {
			if err := finite(fmt.Sprintf("align_x_squares[%d].%v", i, name), v); err != nil {
				return err
			}
		}
		if !(x.Y1 < x.Y2) || !(x.Z1 < x.Z2) {
			return fmt.Errorf("align_x_squares[%d] intervals do not strictly increase", i)
		}
	}
	for i, x := range s.AlignYSquares {
		for name, v := range map[string]float64{"x1": x.X1, "x2": x.X2, "y": x.Y, "z1": x.Z1, "z2": x.Z2} {
			if err := finite(fmt.Sprintf("align_y_squares[%d].%v", i, name), v); err != nil {
				return err
			}
		}
		if !(x.X1 < x.X2) || !(x.Z1 < x.Z2) {
			return fmt.Errorf("align_y_squares[%d] intervals do not strictly increase", i)
		}
	}
	for i, x := range s.AlignZSquares {
		for name, v := range map[string]float64{"x1": x.X1, "x2": x.X2, "y1": x.Y1, "y2": x.Y2, "z": x.Z} {
			if err := finite(fmt.Sprintf("align_z_squares[%d].%v", i, name), v); err != nil {
				return err
			}
		}
		if !(x.X1 < x.X2) || !(x.Y1 < x.Y2) {
			return fmt.Errorf("align_z_squares[%d] intervals do not strictly increase", i)
		}
	}
	for i, x := range s.Discs {
		if err := finiteVec(fmt.Sprintf("discs[%d].center", i), x.Center); err != nil {
			return err
		}
		if err := positive(fmt.Sprintf("discs[%d].radius", i), x.Radius); err != nil {
			return err
		}
		if err := finiteVec(fmt.Sprintf("discs[%d].unit_norm", i), x.UnitNorm); err != nil {
			return err
		}
		if l := x.UnitNorm.Length(); math.Abs(l-1) > 1e-9 {
			return fmt.Errorf("discs[%d].unit_norm length %v differs from 1 by more than 1e-9", i, l)
		}
	}
	for i, x := range s.Pipes {
		if err := finiteVec(fmt.Sprintf("pipes[%d].endpoint_a", i), x.EndpointA); err != nil {
			return err
		}
		if err := finiteVec(fmt.Sprintf("pipes[%d].endpoint_b", i), x.EndpointB); err != nil {
			return err
		}
		if err := positive(fmt.Sprintf("pipes[%d].radius", i), x.Radius); err != nil {
			return err
		}
		if x.EndpointB.Sub(x.EndpointA).LengthSq() == 0 {
			return fmt.Errorf("pipes[%d] endpoints coincide", i)
		}
	}
	return nil
}
