package hitables

import (
	"math"
	"math/rand"

	"github.com/go-gl/mathgl/mgl64"
	"github.com/markzuber/zgotrace/raytrace"
	"github.com/markzuber/zgotrace/raytrace/vectorextensions"
)

type XzRect struct {
	x0       float64
	x1       float64
	z0       float64
	z1       float64
	k        float64
	material raytrace.Material
}

func NewXzRect(
	x0 float64,
	x1 float64,
	z0 float64,
	z1 float64,
	k float64,
	material raytrace.Material) raytrace.Hitable {
	s := &XzRect{x0, x1, z0, z1, k, material}
	return s
}

func (s *XzRect) Hit(ray *raytrace.Ray, tMin float64, tMax float64) *raytrace.HitRecord {
	t := (s.k - ray.Origin().Y()) / ray.Direction().Y()
	if t < tMin || t > tMax {
		return nil
	}

	x := ray.Origin().X() + (t * ray.Direction().X())
	z := ray.Origin().Z() + (t * ray.Direction().Z())
	if x < s.x0 || x > s.x1 || z < s.z0 || z > s.z1 {
		return nil
	}

	return raytrace.NewHitRecord(
		t,
		ray.GetPointAtParameter(t),
		vectorextensions.UnitY(),
		mgl64.Vec2{(x - s.x0) / (s.x1 - s.x0), (z - s.z0) / (s.z1 - s.z0)},
		s.material)
}

func (s *XzRect) GetBoundingBox(t0 float64, t1 float64) *raytrace.AABB {
	return raytrace.NewAABB(mgl64.Vec3{s.x0, s.k - 0.001, s.z0}, mgl64.Vec3{s.x1, s.k + 0.001, s.z1})
}

func (s *XzRect) GetPdfValue(origin mgl64.Vec3, v mgl64.Vec3) float64 {
	hr := s.Hit(raytrace.NewRay(origin, v), 0.001, math.MaxFloat64)
	if hr == nil {
		return 0.0
	}

	area := (s.x1 - s.x0) * (s.z1 - s.z0)
	distanceSquared := hr.T() * hr.T() * v.LenSqr()
	cosine := math.Abs(v.Dot(hr.Normal()) / v.Len())
	return distanceSquared / (cosine * area)

}

func (s *XzRect) Random(origin mgl64.Vec3) mgl64.Vec3 {
	randomPoint := mgl64.Vec3{s.x0 + (rand.Float64() * (s.x1 - s.x0)), s.k, s.z0 + (rand.Float64() * (s.z1 - s.z0))}
	return randomPoint.Sub(origin)
}
