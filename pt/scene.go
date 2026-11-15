package pt

import (
	"math"
	"sync/atomic"
)

type Scene struct {
	Color        Color
	Texture      Texture
	TextureAngle float64
	Shapes       []Shape
	Lights       []Shape
	tree         *Tree
	rays         uint64
}

// Compile prepares every shape and rebuilds the acceleration structure and
// light list from the current scene state. Calling it repeatedly after
// SetTransform/SetLinks changes never keeps stale bounds, stale lights or
// duplicated lights: both structures are rebuilt from Shapes each time.
func (s *Scene) Compile() {
	for _, shape := range s.Shapes {
		shape.Compile()
	}
	lights := make([]Shape, 0, len(s.Shapes))
	for _, shape := range s.Shapes {
		if shape.MaterialAt(Vector{}).Emittance > 0 {
			lights = append(lights, shape)
		}
	}
	s.Lights = lights
	s.tree = NewTree(s.Shapes)
}

func (s *Scene) Add(shape Shape) {
	s.Shapes = append(s.Shapes, shape)
	if shape.MaterialAt(Vector{}).Emittance > 0 {
		s.Lights = append(s.Lights, shape)
	}
}

func (s *Scene) RayCount() uint64 {
	return atomic.LoadUint64(&s.rays)
}

// Intersect returns the nearest hit from visible shapes only. Hidden
// LinkedInstances are skipped, so a hidden near surface cannot block a
// visible object behind it; this is the query used by camera rays and all
// recursive reflection, refraction and diffuse bounces.
func (s *Scene) Intersect(r Ray) Hit {
	atomic.AddUint64(&s.rays, 1)
	return s.tree.IntersectQuery(r, &intersectQuery{mode: intersectVisible})
}

// intersectShape intersects only the named shape, ignoring every other
// shape in the scene. It is used to confirm that a light itself is reached.
func (s *Scene) intersectShape(r Ray, shape Shape) Hit {
	atomic.AddUint64(&s.rays, 1)
	return s.tree.IntersectQuery(r, &intersectQuery{mode: intersectSingle, target: shape})
}

// OccludedForLight reports whether a compatible occluder blocks the unit
// direction ray before it reaches a light. A shape blocks the ray only when
// its CastMask shares a bit with the light's EmitMask; Visible is ignored.
// Unwrapped occluders and lights carry every bit. The target light is never
// its own occluder, but a different instance of the same source is treated
// as a separate shape. Only hits with EPS < t < maxDistance-EPS count.
//
// maxDistance must be finite and greater than 2*EPS; light must be an
// emissive shape currently in the scene.
func (s *Scene) OccludedForLight(ray Ray, maxDistance float64, light Shape) bool {
	if math.IsNaN(maxDistance) || math.IsInf(maxDistance, 0) || maxDistance <= 2*EPS {
		return false
	}
	atomic.AddUint64(&s.rays, 1)
	query := &intersectQuery{
		mode:      intersectOccluders,
		target:    light,
		lightMask: emitMask(light),
	}
	hit := s.tree.IntersectQuery(ray, query)
	return hit.Ok() && hit.T > EPS && hit.T < maxDistance-EPS
}
