package render

import (
	"fmt"
	"math"

	"github.com/markphelps/go-trace/primitive"
)

// SpherePrototype describes a diffuse sphere in the unit-sphere parameter
// space used by InstanceScene. The local surface point associated with a unit
// direction n is Center + Radius*n, and Color is the Lambertian albedo.
type SpherePrototype struct {
	Center primitive.Vector
	Radius float64
	Color  primitive.Color
}

// InstanceDesc places one sphere prototype into world space. Matrix is a
// row-major 3x4 affine transform: world = A*local + t. The 3x3 part may
// contain shear, non-uniform scale and mirroring, but must be finite and
// invertible.
type InstanceDesc struct {
	ID        uint64
	Prototype int
	Matrix    [3][4]float64
}

// Prototype is an alias kept for callers that prefer the shorter name.
type Prototype = SpherePrototype

// Instance is an alias kept for callers that prefer the shorter name.
type Instance = InstanceDesc

// SurfaceRef identifies a surface point independently of a particular pose.
// Instance is the stable uint64 instance identity. LocalNormal is the unit
// direction on the prototype's unit sphere, so the local surface point is
// prototypeCenter + prototypeRadius*LocalNormal in every frame.
type SurfaceRef struct {
	Instance    uint64
	LocalNormal primitive.Vector
}

// Surface is a resolved world-space surface sample.
type Surface struct {
	Ref    SurfaceRef
	Point  primitive.Vector
	Normal primitive.Vector
	Color  primitive.Color
}

type mat3 [3][3]float64

func (m mat3) mulVec(v primitive.Vector) primitive.Vector {
	return primitive.Vector{
		X: m[0][0]*v.X + m[0][1]*v.Y + m[0][2]*v.Z,
		Y: m[1][0]*v.X + m[1][1]*v.Y + m[1][2]*v.Z,
		Z: m[2][0]*v.X + m[2][1]*v.Y + m[2][2]*v.Z,
	}
}

type sceneInstance struct {
	id          uint64
	proto       int
	a           mat3
	translation primitive.Vector
	inv         mat3 // world -> local
	invT        mat3 // transpose of inv
	det         float64
}

// InstanceScene is an immutable collection of affine diffuse-sphere
// instances. It implements primitive.Hitable. Construction copies all input
// data; callers may freely mutate their prototype and instance slices
// afterwards (or while another goroutine queries this scene).
type InstanceScene struct {
	prototypes []SpherePrototype
	instances  []sceneInstance
	byID       map[uint64]int
}

// NewInstanceScene validates and copies the supplied descriptions. Empty
// scenes are valid and render as background sky.
func NewInstanceScene(prototypes []SpherePrototype, instances []InstanceDesc) (*InstanceScene, error) {
	scene := &InstanceScene{
		prototypes: append([]SpherePrototype(nil), prototypes...),
		instances:  make([]sceneInstance, 0, len(instances)),
		byID:       make(map[uint64]int, len(instances)),
	}

	for i, p := range scene.prototypes {
		if !finiteVec(p.Center) {
			return nil, fmt.Errorf("prototype %d: center is not finite", i)
		}
		if !finite(p.Radius) || p.Radius <= 0 {
			return nil, fmt.Errorf("prototype %d: radius must be finite and positive", i)
		}
		if !finite(p.Color.R) || !finite(p.Color.G) || !finite(p.Color.B) {
			return nil, fmt.Errorf("prototype %d: color is not finite", i)
		}
		if p.Color.R < 0 || p.Color.R > 1 ||
			p.Color.G < 0 || p.Color.G > 1 ||
			p.Color.B < 0 || p.Color.B > 1 {
			return nil, fmt.Errorf("prototype %d: color components must be in [0,1]", i)
		}
	}

	for i, d := range instances {
		if _, exists := scene.byID[d.ID]; exists {
			return nil, fmt.Errorf("instance %d: duplicate id %d", i, d.ID)
		}
		if d.Prototype < 0 || d.Prototype >= len(scene.prototypes) {
			return nil, fmt.Errorf("instance %d (id %d): prototype index %d out of range", i, d.ID, d.Prototype)
		}

		var a mat3
		for r := 0; r < 3; r++ {
			for c := 0; c < 4; c++ {
				if !finite(d.Matrix[r][c]) {
					return nil, fmt.Errorf("instance %d (id %d): matrix is not finite", i, d.ID)
				}
			}
			a[r][0] = d.Matrix[r][0]
			a[r][1] = d.Matrix[r][1]
			a[r][2] = d.Matrix[r][2]
		}

		inv, det, ok := invert3(a)
		if !ok {
			return nil, fmt.Errorf("instance %d (id %d): matrix is not finite or invertible", i, d.ID)
		}

		var invT mat3
		for r := 0; r < 3; r++ {
			for c := 0; c < 3; c++ {
				invT[r][c] = inv[c][r]
			}
		}

		scene.byID[d.ID] = len(scene.instances)
		scene.instances = append(scene.instances, sceneInstance{
			id:          d.ID,
			proto:       d.Prototype,
			a:           a,
			translation: primitive.Vector{X: d.Matrix[0][3], Y: d.Matrix[1][3], Z: d.Matrix[2][3]},
			inv:         inv,
			invT:        invT,
			det:         det,
		})
	}

	return scene, nil
}

// Count returns the number of instances.
func (s *InstanceScene) Count() int {
	if s == nil {
		return 0
	}
	return len(s.instances)
}

// PrototypeCount returns the number of copied prototypes.
func (s *InstanceScene) PrototypeCount() int {
	if s == nil {
		return 0
	}
	return len(s.prototypes)
}

// HasInstance reports whether id exists in this frame.
func (s *InstanceScene) HasInstance(id uint64) bool {
	if s == nil {
		return false
	}
	_, ok := s.byID[id]
	return ok
}

// IDs returns the instance IDs in insertion order.
func (s *InstanceScene) IDs() []uint64 {
	if s == nil {
		return nil
	}
	ids := make([]uint64, len(s.instances))
	for i := range s.instances {
		ids[i] = s.instances[i].id
	}
	return ids
}

// MapSurface returns ref unchanged when its instance exists in this scene.
// The unit-sphere direction, rather than a prototype index or material
// pointer, is what carries identity between frames.
func (s *InstanceScene) MapSurface(ref SurfaceRef) (SurfaceRef, bool) {
	if s == nil || !s.HasInstance(ref.Instance) {
		return SurfaceRef{}, false
	}
	return ref, true
}

// Surface resolves a SurfaceRef to its world-space point, outward unit normal
// and diffuse color. Invalid references return an error.
func (s *InstanceScene) Surface(ref SurfaceRef) (Surface, error) {
	if s == nil {
		return Surface{}, fmt.Errorf("nil instance scene")
	}
	idx, ok := s.byID[ref.Instance]
	if !ok {
		return Surface{}, fmt.Errorf("surface ref: unknown instance id %d", ref.Instance)
	}
	n := ref.LocalNormal
	if !finiteVec(n) {
		return Surface{}, fmt.Errorf("surface ref for instance %d: local normal is not finite", ref.Instance)
	}
	if math.Abs(n.Length()-1) > 1e-6 {
		return Surface{}, fmt.Errorf("surface ref for instance %d: local normal is not unit length", ref.Instance)
	}

	inst := &s.instances[idx]
	proto := &s.prototypes[inst.proto]
	local := proto.Center.Add(n.MultiplyScalar(proto.Radius))
	point := inst.apply(local)

	normal := inst.invT.mulVec(n)
	nl := normal.Length()
	if !(nl > 0) || !finite(nl) {
		return Surface{}, fmt.Errorf("surface ref for instance %d: degenerate normal", ref.Instance)
	}
	normal = normal.DivideScalar(nl)
	if !finiteVec(normal) {
		return Surface{}, fmt.Errorf("surface ref for instance %d: non-finite normal", ref.Instance)
	}

	return Surface{Ref: ref, Point: point, Normal: normal, Color: proto.Color}, nil
}

// areaFactor is d(world area)/d(unit-sphere solid angle) at ref:
//
//	radius^2 * |det A| * |A^-T n|
//
// The inverse-transpose term is essential for non-uniform scaling and shear.
func (s *InstanceScene) areaFactor(ref SurfaceRef) float64 {
	if s == nil {
		return 0
	}
	idx, ok := s.byID[ref.Instance]
	if !ok {
		return 0
	}
	inst := &s.instances[idx]
	proto := &s.prototypes[inst.proto]
	v := inst.invT.mulVec(ref.LocalNormal)
	l := v.Length()
	if !(l >= 0) || !finite(l) {
		return 0
	}
	return proto.Radius * proto.Radius * math.Abs(inst.det) * l
}

func (inst *sceneInstance) apply(local primitive.Vector) primitive.Vector {
	return primitive.Vector{
		X: inst.a[0][0]*local.X + inst.a[0][1]*local.Y + inst.a[0][2]*local.Z + inst.translation.X,
		Y: inst.a[1][0]*local.X + inst.a[1][1]*local.Y + inst.a[1][2]*local.Z + inst.translation.Y,
		Z: inst.a[2][0]*local.X + inst.a[2][1]*local.Y + inst.a[2][2]*local.Z + inst.translation.Z,
	}
}

// intersect solves the prototype sphere in the instance's local frame. The
// ray parameter t is unchanged by the affine transform, so the returned T
// always satisfies world point = Origin + T*Direction even when Direction is
// not normalized.
func (inst *sceneInstance) intersect(r primitive.Ray, center primitive.Vector, radius, tMin, tMax float64) (float64, primitive.Vector, bool) {
	ol := inst.inv.mulVec(r.Origin.Subtract(inst.translation))
	dl := inst.inv.mulVec(r.Direction)

	oc := ol.Subtract(center)
	a := dl.Dot(dl)
	if !(a > 0) || !finite(a) {
		return 0, primitive.Vector{}, false
	}
	b := oc.Dot(dl)
	c := oc.Dot(oc) - radius*radius
	disc := b*b - a*c
	if !(disc > 0) || !finite(disc) {
		return 0, primitive.Vector{}, false
	}
	root := math.Sqrt(disc)

	t0 := (-b - root) / a
	t1 := (-b + root) / a
	for _, t := range [2]float64{t0, t1} {
		if !finite(t) || t <= tMin || t >= tMax {
			continue
		}
		localPoint := ol.Add(dl.MultiplyScalar(t))
		n := localPoint.Subtract(center)
		l := n.Length()
		if !(l > 0) || !finite(l) {
			continue
		}
		n = n.DivideScalar(l)
		return t, n, true
	}
	return 0, primitive.Vector{}, false
}

// HitRef is the SurfaceRef-returning intersection entry point. It returns the
// closest hit across all instances.
func (s *InstanceScene) HitRef(r primitive.Ray, tMin, tMax float64) (bool, primitive.Hit, SurfaceRef) {
	if s == nil {
		return false, primitive.Hit{}, SurfaceRef{}
	}

	found := false
	closest := tMax
	var record primitive.Hit
	var ref SurfaceRef

	for i := range s.instances {
		inst := &s.instances[i]
		proto := &s.prototypes[inst.proto]

		t, localNormal, ok := inst.intersect(r, proto.Center, proto.Radius, tMin, closest)
		if !ok {
			continue
		}

		normal := inst.invT.mulVec(localNormal)
		nl := normal.Length()
		if !(nl > 0) || !finite(nl) {
			continue
		}
		normal = normal.DivideScalar(nl)
		if !finiteVec(normal) {
			continue
		}

		found = true
		closest = t
		record = primitive.Hit{
			T:        t,
			Point:    r.Point(t),
			Normal:   normal,
			Material: primitive.Lambertian{Attenuation: proto.Color},
		}
		ref = SurfaceRef{Instance: inst.id, LocalNormal: localNormal}
	}

	if !found {
		return false, primitive.Hit{}, SurfaceRef{}
	}
	return true, record, ref
}

// HitSurface is an alias for HitRef.
func (s *InstanceScene) HitSurface(r primitive.Ray, tMin, tMax float64) (bool, primitive.Hit, SurfaceRef) {
	return s.HitRef(r, tMin, tMax)
}

// Hit implements primitive.Hitable.
func (s *InstanceScene) Hit(r primitive.Ray, tMin, tMax float64) (bool, primitive.Hit) {
	ok, hit, _ := s.HitRef(r, tMin, tMax)
	return ok, hit
}

func invert3(m mat3) (mat3, float64, bool) {
	a00, a01, a02 := m[0][0], m[0][1], m[0][2]
	a10, a11, a12 := m[1][0], m[1][1], m[1][2]
	a20, a21, a22 := m[2][0], m[2][1], m[2][2]

	det := a00*(a11*a22-a12*a21) - a01*(a10*a22-a12*a20) + a02*(a10*a21-a11*a20)
	if !finite(det) || det == 0 {
		return mat3{}, 0, false
	}

	var inv mat3
	inv[0][0] = (a11*a22 - a12*a21) / det
	inv[0][1] = (a02*a21 - a01*a22) / det
	inv[0][2] = (a01*a12 - a02*a11) / det
	inv[1][0] = (a12*a20 - a10*a22) / det
	inv[1][1] = (a00*a22 - a02*a20) / det
	inv[1][2] = (a02*a10 - a00*a12) / det
	inv[2][0] = (a10*a21 - a11*a20) / det
	inv[2][1] = (a01*a20 - a00*a21) / det
	inv[2][2] = (a00*a11 - a01*a10) / det

	for r := 0; r < 3; r++ {
		for c := 0; c < 3; c++ {
			if !finite(inv[r][c]) {
				return mat3{}, 0, false
			}
		}
	}
	return inv, det, true
}

func finite(f float64) bool {
	return !math.IsNaN(f) && !math.IsInf(f, 0)
}

func finiteVec(v primitive.Vector) bool {
	return finite(v.X) && finite(v.Y) && finite(v.Z)
}
