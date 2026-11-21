package hitables

import (
	"math"
	"math/rand"

	"github.com/go-gl/mathgl/mgl64"
	"github.com/markzuber/zgotrace/raytrace"
	"github.com/markzuber/zgotrace/raytrace/vectorextensions"
)

type XyRect struct {
	x0       float64
	x1       float64
	y0       float64
	y1       float64
	k        float64
	material raytrace.Material
}

func NewXyRect(
	x0 float64,
	x1 float64,
	y0 float64,
	y1 float64,
	k float64,
	material raytrace.Material) raytrace.Hitable {
	s := &XyRect{x0, x1, y0, y1, k, material}
	return s
}

func (s *XyRect) Hit(ray *raytrace.Ray, tMin float64, tMax float64) *raytrace.HitRecord {
	t := (s.k - ray.Origin().Z()) / ray.Direction().Z()
	if t < tMin || t > tMax {
		return nil
	}

	x := ray.Origin().X() + (t * ray.Direction().X())
	y := ray.Origin().Y() + (t * ray.Direction().Y())
	if x < s.x0 || x > s.x1 || y < s.y0 || y > s.y1 {
		return nil
	}

	return raytrace.NewHitRecord(
		t,
		ray.GetPointAtParameter(t),
		vectorextensions.UnitZ(),
		mgl64.Vec2{(x - s.x0) / (s.x1 - s.x0), (y - s.y0) / (s.y1 - s.y0)},
		s.material)
}

func (s *XyRect) GetBoundingBox(t0 float64, t1 float64) *raytrace.AABB {
	return raytrace.NewAABB(mgl64.Vec3{s.x0, s.y0, s.k - 0.001}, mgl64.Vec3{s.x1, s.y1, s.k + 0.001})
}

func (s *XyRect) GetPdfValue(origin mgl64.Vec3, v mgl64.Vec3) float64 {
	hr := s.Hit(raytrace.NewRay(origin, v), 0.001, math.MaxFloat64)
	if hr == nil {
		return 0.0
	}

	area := (s.x1 - s.x0) * (s.y1 - s.y0)
	distanceSquared := hr.T() * hr.T() * v.LenSqr()
	cosine := math.Abs(v.Dot(hr.Normal()) / v.Len())
	return distanceSquared / (cosine * area)

}

func (s *XyRect) Random(origin mgl64.Vec3) mgl64.Vec3 {
	randomPoint := mgl64.Vec3{s.x0 + (rand.Float64() * (s.x1 - s.x0)), s.y0 + (rand.Float64() * (s.y1 - s.y0)), s.k}
	return randomPoint.Sub(origin)
}
