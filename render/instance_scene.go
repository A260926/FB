package render

import (
	"fmt"
	"math"

	"github.com/markphelps/go-trace/primitive"
)

// unitNormalTol is the tolerance on the length of a SurfaceRef local normal.
const unitNormalTol = 1e-6

// SphereProto is a reusable Lambertian sphere prototype. The surface of the
// prototype is parameterized by the unit sphere direction n, giving the local
// point Center + Radius*n.
type SphereProto struct {
	Center primitive.Vector
	Radius float64
	Albedo primitive.Color
}

// Instance places one prototype in the world through a row-major 3x4 affine
// matrix. The linear part occupies columns 0-2 of each row and the translation
// is column 3. Shear, non-uniform scale and mirroring are allowed; the matrix
// must be finite and invertible.
type Instance struct {
	ID     uint64
	Proto  int
	Matrix [12]float64
}

// SurfaceRef identifies a surface point across poses: the same instance ID
// with the same unit sphere direction is the same material point in every
// frame, independent of prototype indices or material pointers.
type SurfaceRef struct {
	Instance    uint64
	LocalNormal primitive.Vector
}

// SurfacePoint is the resolved world-space geometry of a SurfaceRef.
type SurfacePoint struct {
	Ref    SurfaceRef
	Point  primitive.Vector
	Normal primitive.Vector
	Albedo primitive.Color
}

// SurfaceHit is a ray hit that also carries the SurfaceRef of the hit point.
type SurfaceHit struct {
	Hit primitive.Hit
	Ref SurfaceRef
}

// instanceData is the immutable runtime form of one Instance.
type instanceData struct {
	proto int
	m     [3][3]float64
	t     primitive.Vector
	inv   [3][3]float64
	invT  [3][3]float64
	det   float64
}

// InstanceScene is an immutable collection of affinely transformed Lambertian
// sphere instances. It is safe for concurrent queries, including queries that
// run while a different pose of the same scene is being rendered.
type InstanceScene struct {
	protos    []SphereProto
	instances []instanceData
	ids       []uint64
	index     map[uint64]int
}

var _ primitive.Hitable = (*InstanceScene)(nil)

func finite64(v float64) bool { return !math.IsNaN(v) && !math.IsInf(v, 0) }

func finiteVec(v primitive.Vector) bool {
	return finite64(v.X) && finite64(v.Y) && finite64(v.Z)
}

// NewInstanceScene copies the prototype and instance descriptions into an
// immutable scene. Any invalid description makes the whole construction fail;
// later mutation of the input slices never affects the returned scene.
func NewInstanceScene(protos []SphereProto, instances []Instance) (*InstanceScene, error) {
	scene := &InstanceScene{
		protos: make([]SphereProto, len(protos)),
		index:  make(map[uint64]int, len(instances)),
	}
	for i, p := range protos {
		switch {
		case !finiteVec(p.Center):
			return nil, fmt.Errorf("render: prototype %d has non-finite center", i)
		case !finite64(p.Radius) || p.Radius <= 0:
			return nil, fmt.Errorf("render: prototype %d has invalid radius %v", i, p.Radius)
		case !finite64(p.Albedo.R) || !finite64(p.Albedo.G) || !finite64(p.Albedo.B):
			return nil, fmt.Errorf("render: prototype %d has non-finite albedo", i)
		case p.Albedo.R < 0 || p.Albedo.R > 1 ||
			p.Albedo.G < 0 || p.Albedo.G > 1 ||
			p.Albedo.B < 0 || p.Albedo.B > 1:
			return nil, fmt.Errorf("render: prototype %d albedo outside [0,1]", i)
		}
		scene.protos[i] = p
	}
	for i, inst := range instances {
		if inst.Proto < 0 || inst.Proto >= len(scene.protos) {
			return nil, fmt.Errorf("render: instance %d references invalid prototype %d", i, inst.Proto)
		}
		if _, dup := scene.index[inst.ID]; dup {
			return nil, fmt.Errorf("render: duplicate instance id %d", inst.ID)
		}
		var m [3][3]float64
		finite := true
		for r := 0; r < 3; r++ {
			for c := 0; c < 3; c++ {
				m[r][c] = inst.Matrix[r*4+c]
				finite = finite && finite64(m[r][c])
			}
		}
		t := primitive.Vector{X: inst.Matrix[3], Y: inst.Matrix[7], Z: inst.Matrix[11]}
		if !finite || !finiteVec(t) {
			return nil, fmt.Errorf("render: instance %d (id %d) has non-finite matrix", i, inst.ID)
		}
		inv, det, ok := invert3(m)
		if !ok {
			return nil, fmt.Errorf("render: instance %d (id %d) has singular matrix", i, inst.ID)
		}
		scene.index[inst.ID] = len(scene.instances)
		scene.ids = append(scene.ids, inst.ID)
		scene.instances = append(scene.instances, instanceData{
			proto: inst.Proto,
			m:     m,
			t:     t,
			inv:   inv,
			invT:  transpose3(inv),
			det:   det,
		})
	}
	return scene, nil
}

// Hit implements primitive.Hitable. The ray direction does not need to be
// normalized; T always refers to O+tD in the original ray parameterization.
func (s *InstanceScene) Hit(r primitive.Ray, tMin, tMax float64) (bool, primitive.Hit) {
	ok, hit := s.HitSurface(r, tMin, tMax)
	return ok, hit.Hit
}

// HitSurface is like Hit but also reports the SurfaceRef of the hit point.
func (s *InstanceScene) HitSurface(r primitive.Ray, tMin, tMax float64) (bool, SurfaceHit) {
	hitAnything := false
	closest := tMax
	var best SurfaceHit

	for i := range s.instances {
		inst := &s.instances[i]
		proto := &s.protos[inst.proto]

		// Transform the ray into the prototype's local frame. The affine
		// map preserves the ray parameter, so t still means O+tD in world.
		origin := mulMatVec(inst.inv, r.Origin.Subtract(inst.t))
		direction := mulMatVec(inst.inv, r.Direction)

		oc := origin.Subtract(proto.Center)
		a := direction.Dot(direction)
		b := oc.Dot(direction)
		c := oc.Dot(oc) - proto.Radius*proto.Radius
		discriminant := b*b - a*c
		if discriminant <= 0 {
			continue
		}
		root := math.Sqrt(discriminant)
		for _, t := range [2]float64{(-b - root) / a, (-b + root) / a} {
			if t <= tMin || t >= closest {
				continue
			}
			local := origin.Add(direction.MultiplyScalar(t))
			n := local.Subtract(proto.Center).DivideScalar(proto.Radius).Normalize()
			rec := primitive.Hit{
				T:        t,
				Point:    r.Point(t),
				Normal:   mulMatVec(inst.invT, n).Normalize(),
				Material: primitive.Lambertian{Attenuation: proto.Albedo},
			}
			best = SurfaceHit{Hit: rec, Ref: SurfaceRef{Instance: s.ids[i], LocalNormal: n}}
			closest = t
			hitAnything = true
			break
		}
	}
	return hitAnything, best
}

// Resolve maps a SurfaceRef to world-space geometry. The reference must name
// an existing instance and carry a finite unit vector.
func (s *InstanceScene) Resolve(ref SurfaceRef) (SurfacePoint, error) {
	inst, proto, n, err := s.locate(ref)
	if err != nil {
		return SurfacePoint{}, err
	}
	local := proto.Center.Add(n.MultiplyScalar(proto.Radius))
	return SurfacePoint{
		Ref:    SurfaceRef{Instance: ref.Instance, LocalNormal: n},
		Point:  mulMatVec(inst.m, local).Add(inst.t),
		Normal: mulMatVec(inst.invT, n).Normalize(),
		Albedo: proto.Albedo,
	}, nil
}

// AreaFactor returns the world surface area per unit of solid angle of the
// unit-sphere parameterization at ref: radius^2 * |det A| * |A^-T n|.
func (s *InstanceScene) AreaFactor(ref SurfaceRef) (float64, error) {
	inst, proto, n, err := s.locate(ref)
	if err != nil {
		return 0, err
	}
	return proto.Radius * proto.Radius * math.Abs(inst.det) * mulMatVec(inst.invT, n).Length(), nil
}

// locate validates a SurfaceRef and returns the referenced data with the
// renormalized local direction.
func (s *InstanceScene) locate(ref SurfaceRef) (*instanceData, *SphereProto, primitive.Vector, error) {
	idx, ok := s.index[ref.Instance]
	if !ok {
		return nil, nil, primitive.Vector{}, fmt.Errorf("render: unknown instance id %d", ref.Instance)
	}
	n := ref.LocalNormal
	if !finiteVec(n) {
		return nil, nil, primitive.Vector{}, fmt.Errorf("render: non-finite local normal for instance %d", ref.Instance)
	}
	l := n.Length()
	if math.Abs(l-1) > unitNormalTol {
		return nil, nil, primitive.Vector{}, fmt.Errorf("render: local normal for instance %d is not a unit vector (length %v)", ref.Instance, l)
	}
	inst := &s.instances[idx]
	return inst, &s.protos[inst.proto], n.DivideScalar(l), nil
}

// invert3 returns the inverse and determinant of a 3x3 matrix. ok is false
// when the matrix is singular or produces non-finite inverse entries.
func invert3(m [3][3]float64) (inv [3][3]float64, det float64, ok bool) {
	a, b, c := m[0][0], m[0][1], m[0][2]
	d, e, f := m[1][0], m[1][1], m[1][2]
	g, h, i := m[2][0], m[2][1], m[2][2]

	c00 := e*i - f*h
	c01 := -(d*i - f*g)
	c02 := d*h - e*g
	c10 := -(b*i - c*h)
	c11 := a*i - c*g
	c12 := -(a*h - b*g)
	c20 := b*f - c*e
	c21 := -(a*f - c*d)
	c22 := a*e - b*d

	det = a*c00 + b*c01 + c*c02
	if !finite64(det) || det == 0 {
		return inv, 0, false
	}
	s := 1 / det
	inv = [3][3]float64{
		{c00 * s, c10 * s, c20 * s},
		{c01 * s, c11 * s, c21 * s},
		{c02 * s, c12 * s, c22 * s},
	}
	for r := range inv {
		for col := range inv[r] {
			if !finite64(inv[r][col]) {
				return inv, 0, false
			}
		}
	}
	return inv, det, true
}

func transpose3(m [3][3]float64) [3][3]float64 {
	return [3][3]float64{
		{m[0][0], m[1][0], m[2][0]},
		{m[0][1], m[1][1], m[2][1]},
		{m[0][2], m[1][2], m[2][2]},
	}
}

func mulMatVec(m [3][3]float64, v primitive.Vector) primitive.Vector {
	return primitive.Vector{
		X: m[0][0]*v.X + m[0][1]*v.Y + m[0][2]*v.Z,
		Y: m[1][0]*v.X + m[1][1]*v.Y + m[1][2]*v.Z,
		Z: m[2][0]*v.X + m[2][1]*v.Y + m[2][2]*v.Z,
	}
}
