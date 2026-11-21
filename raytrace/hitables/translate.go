package hitables

import (
	"github.com/go-gl/mathgl/mgl64"
	"github.com/markzuber/zgotrace/raytrace"
	"github.com/markzuber/zgotrace/raytrace/vectorextensions"
)

type Translate struct {
	hitable      raytrace.Hitable
	displacement mgl64.Vec3
}

func NewTranslate(hitable raytrace.Hitable, displacement mgl64.Vec3) raytrace.Hitable {
	h := &Translate{hitable, displacement}
	return h
}

func (s *Translate) Hit(ray *raytrace.Ray, tMin float64, tMax float64) *raytrace.HitRecord {
	movedRay := raytrace.NewRay(ray.Origin().Sub(s.displacement), ray.Direction())
	hitRecord := s.hitable.Hit(movedRay, tMin, tMax)
	if hitRecord == nil {
		return nil
	}

	return raytrace.NewHitRecord(
		hitRecord.T(),
		hitRecord.P().Add(s.displacement),
		hitRecord.Normal(),
		hitRecord.UvCoords(),
		hitRecord.Material())
}

func (s *Translate) GetBoundingBox(t0 float64, t1 float64) *raytrace.AABB {
	box := s.hitable.GetBoundingBox(t0, t1)
	if box == nil {
		return nil
	}
	box = raytrace.NewAABB(box.Min().Add(s.displacement), box.Max().Add(s.displacement))
	return box
}

func (s *Translate) GetPdfValue(origin mgl64.Vec3, v mgl64.Vec3) float64 {
	return 1.0
}

func (s *Translate) Random(origin mgl64.Vec3) mgl64.Vec3 {
	return vectorextensions.UnitX()
}
