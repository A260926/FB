package hitables

import (
	"math"

	"github.com/go-gl/mathgl/mgl64"
	"github.com/markzuber/zgotrace/raytrace"
	"github.com/markzuber/zgotrace/raytrace/vectorextensions"
)

type Sphere struct {
	center   mgl64.Vec3
	radius   float64
	material raytrace.Material
}

func NewSphere(center mgl64.Vec3, radius float64, material raytrace.Material) raytrace.Hitable {
	s := &Sphere{center, radius, material}
	return s
}

func (s *Sphere) Hit(ray *raytrace.Ray, tMin float64, tMax float64) *raytrace.HitRecord {
	oc := ray.Origin().Sub(s.center)
	a := ray.Direction().Dot(ray.Direction())
	b := oc.Dot(ray.Direction())
	c := oc.Dot(oc) - (s.radius * s.radius)
	discriminant := (b * b) - (a * c)
	if discriminant > 0.0 {
		temp := (-b - math.Sqrt((b*b)-(a*c))) / a
		if temp < tMax && temp > tMin {
			p := ray.GetPointAtParameter(temp)
			return raytrace.NewHitRecord(temp, p, vectorextensions.DivScalar(p.Sub(s.center), s.radius), getSphereUv(p), s.material)
		}

		temp = (-b + math.Sqrt((b*b)-(a*c))) / a
		if temp < tMax && temp > tMin {
			p := ray.GetPointAtParameter(temp)
			return raytrace.NewHitRecord(temp, p, vectorextensions.DivScalar(p.Sub(s.center), s.radius), getSphereUv(p), s.material)
		}
	}

	return nil
}

func (s *Sphere) GetBoundingBox(t0 float64, t1 float64) *raytrace.AABB {
	return raytrace.NewAABB(s.center.Sub(mgl64.Vec3{s.radius, s.radius, s.radius}), s.center.Add(mgl64.Vec3{s.radius, s.radius, s.radius}))
}

func (s *Sphere) GetPdfValue(origin mgl64.Vec3, v mgl64.Vec3) float64 {
	hr := s.Hit(raytrace.NewRay(origin, v), 0.001, math.MaxFloat64)
	if hr == nil {
		return 0.0
	}

	cosThetaMax := math.Sqrt(1.0 - (s.radius * s.radius / (s.center.Sub(origin).LenSqr())))
	solidAngle := 2.0 * math.Pi * (1.0 - cosThetaMax)
	return 1.0 / solidAngle
}

func (s *Sphere) Random(origin mgl64.Vec3) mgl64.Vec3 {
	direction := s.center.Sub(origin)
	distanceSquared := direction.LenSqr()
	uvw := raytrace.OrthoNormalBaseFromW(direction)
	return uvw.Local(raytrace.RandomToSphere(s.radius, distanceSquared))
}

func getSphereUv(p mgl64.Vec3) mgl64.Vec2 {
	punit := vectorextensions.ToUnitVector(p)
	phi := math.Atan2(punit.Z(), punit.X())
	theta := math.Asin(punit.Y())
	u := 1.0 - ((phi + math.Pi) / (2.0 * math.Pi))
	v := (theta + (math.Pi / 2.0)) / math.Pi
	return mgl64.Vec2{u, v}
}
