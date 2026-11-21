package hitables

import (
	"math"
	"math/rand"

	"github.com/markzuber/zgotrace/raytrace/materials"

	"github.com/go-gl/mathgl/mgl64"
	"github.com/markzuber/zgotrace/raytrace"
	"github.com/markzuber/zgotrace/raytrace/vectorextensions"
)

type ConstantMedium struct {
	boundary      raytrace.Hitable
	density       float64
	phaseFunction raytrace.Material
}

func NewConstantMedium(boundary raytrace.Hitable, density float64, phaseFunction raytrace.Texture) raytrace.Hitable {
	s := &ConstantMedium{boundary, density, materials.NewIsotropicMaterial(phaseFunction)}
	return s
}

func (s *ConstantMedium) Hit(ray *raytrace.Ray, tMin float64, tMax float64) *raytrace.HitRecord {
	hitRecord1 := s.boundary.Hit(ray, -math.MaxFloat64, math.MaxFloat64)
	if hitRecord1 == nil {
		return nil
	}

	hitRecord2 := s.boundary.Hit(ray, hitRecord1.T()+0.0001, math.MaxFloat64)
	if hitRecord2 == nil {
		return nil
	}

	rec1T := hitRecord1.T()
	rec2T := hitRecord2.T()

	if rec1T < tMin {
		rec1T = tMin
	}
	if rec2T > tMax {
		rec2T = tMax
	}
	if rec1T >= rec2T {
		return nil
	}
	if rec1T < 0.0 {
		rec1T = 0.0
	}

	distanceInsideBoundary := vectorextensions.MulScalar(ray.Direction(), (rec2T - rec1T)).Len()
	hitDistance := -(1.0 / s.density) * math.Log(rand.Float64())
	if hitDistance < distanceInsideBoundary {
		recT := rec1T + (hitDistance / ray.Direction().Len())

		return raytrace.NewHitRecord(
			recT,
			ray.GetPointAtParameter(recT),
			vectorextensions.UnitX(), // arbitrary
			mgl64.Vec2{0.0, 0.0},     // don't need u/v since PhaseFunction is a calculation
			s.phaseFunction)
	}

	return nil
}

func (s *ConstantMedium) GetBoundingBox(t0 float64, t1 float64) *raytrace.AABB {
	return s.boundary.GetBoundingBox(t0, t1)
}

func (s *ConstantMedium) GetPdfValue(origin mgl64.Vec3, v mgl64.Vec3) float64 {
	return 1.0
}

func (s *ConstantMedium) Random(origin mgl64.Vec3) mgl64.Vec3 {
	return vectorextensions.UnitX()
}
