package hitables

import (
	"math"
	"math/rand"

	"github.com/go-gl/mathgl/mgl64"
	"github.com/markzuber/zgotrace/raytrace"
	"github.com/markzuber/zgotrace/raytrace/vectorextensions"
)

type YzRect struct {
	y0       float64
	y1       float64
	z0       float64
	z1       float64
	k        float64
	material raytrace.Material
}

func NewYzRect(
	y0 float64,
	y1 float64,
	z0 float64,
	z1 float64,
	k float64,
	material raytrace.Material) raytrace.Hitable {
	s := &YzRect{y0, y1, z0, z1, k, material}
	return s
}

func (s *YzRect) Hit(ray *raytrace.Ray, tMin float64, tMax float64) *raytrace.HitRecord {
	t := (s.k - ray.Origin().X()) / ray.Direction().X()
	if t < tMin || t > tMax {
		return nil
	}

	y := ray.Origin().Y() + (t * ray.Direction().Y())
	z := ray.Origin().Z() + (t * ray.Direction().Z())
	if y < s.y0 || y > s.y1 || z < s.z0 || z > s.z1 {
		return nil
	}

	return raytrace.NewHitRecord(
		t,
		ray.GetPointAtParameter(t),
		vectorextensions.UnitY(),
		mgl64.Vec2{(y - s.y0) / (s.y1 - s.y0), (z - s.z0) / (s.z1 - s.z0)},
		s.material)
}

func (s *YzRect) GetBoundingBox(t0 float64, t1 float64) *raytrace.AABB {
	return raytrace.NewAABB(mgl64.Vec3{s.k - 0.001, s.y0, s.z0}, mgl64.Vec3{s.k + 0.001, s.y1, s.z1})
}

func (s *YzRect) GetPdfValue(origin mgl64.Vec3, v mgl64.Vec3) float64 {
	hr := s.Hit(raytrace.NewRay(origin, v), 0.001, math.MaxFloat64)
	if hr == nil {
		return 0.0
	}

	area := (s.y1 - s.y0) * (s.z1 - s.z0)
	distanceSquared := hr.T() * hr.T() * v.LenSqr()
	cosine := math.Abs(v.Dot(hr.Normal()) / v.Len())
	return distanceSquared / (cosine * area)

}

func (s *YzRect) Random(origin mgl64.Vec3) mgl64.Vec3 {
	randomPoint := mgl64.Vec3{s.k, s.y0 + (rand.Float64() * (s.y1 - s.y0)), s.z0 + (rand.Float64() * (s.z1 - s.z0))}
	return randomPoint.Sub(origin)
}
