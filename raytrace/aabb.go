package raytrace

import (
	"math"

	"github.com/go-gl/mathgl/mgl64"
)

type AABB struct {
	min mgl64.Vec3
	max mgl64.Vec3
}

func NewAABB(min mgl64.Vec3, max mgl64.Vec3) *AABB {
	return &AABB{min, max}
}

func (aabb *AABB) Hit(ray *Ray, tMin float64, tMax float64) bool {
	minvec := [3]float64{aabb.min.X(), aabb.min.Y(), aabb.min.Z()}
	maxvec := [3]float64{aabb.max.X(), aabb.max.Y(), aabb.max.Z()}
	originvec := [3]float64{ray.Origin().X(), ray.Origin().Y(), ray.Origin().Z()}
	dirvec := [3]float64{ray.Direction().X(), ray.Direction().Y(), ray.Direction().Z()}

	for a := 0; a < 3; a++ {
		invD := 1.0 / dirvec[a]
		t0 := (minvec[a] - originvec[a]) * invD
		t1 := (maxvec[a] - originvec[a]) * invD
		if invD < 0.0 {
			temp := t0
			t0 = t1
			t1 = temp
		}

		if t0 > tMin {
			tMin = t0
		}
		if t1 < tMax {
			tMax = t1
		}
		if tMax <= tMin {
			return false
		}
	}

	return true
}

func (aabb *AABB) GetSurroundingBox(other *AABB) *AABB {
	small := mgl64.Vec3{math.Min(aabb.min.X(), other.min.X()), math.Min(aabb.min.Y(), other.min.Y()), math.Min(aabb.min.Z(), other.min.Z())}
	big := mgl64.Vec3{math.Max(aabb.max.X(), other.max.X()), math.Max(aabb.max.Y(), other.max.Y()), math.Max(aabb.max.Z(), other.max.Z())}
	return NewAABB(small, big)
}

func (aabb *AABB) Min() mgl64.Vec3 {
	return aabb.min
}

func (aabb *AABB) Max() mgl64.Vec3 {
	return aabb.max
}
