package hitables

import (
	"math"

	"github.com/go-gl/mathgl/mgl64"
	"github.com/markzuber/zgotrace/raytrace"
	"github.com/markzuber/zgotrace/raytrace/vectorextensions"
)

type RotateY struct {
	hitable     raytrace.Hitable
	angle       float64
	sintheta    float64
	costheta    float64
	boundingbox *raytrace.AABB
}

func NewRotateY(hitable raytrace.Hitable, angle float64) raytrace.Hitable {
	radians := math.Pi / 180.0 * angle
	sinTheta := math.Sin(radians)
	cosTheta := math.Cos(radians)
	box := hitable.GetBoundingBox(0.0, 1.0)
	min := []float64{math.MaxFloat64, math.MaxFloat64, math.MaxFloat64}
	max := []float64{-math.MaxFloat64, -math.MaxFloat64, -math.MaxFloat64}

	for i := 0; i < 2; i++ {
		dubi := float64(i)
		for j := 0; j < 2; j++ {
			dubj := float64(j)
			for k := 0; k < 2; k++ {
				dubk := float64(k)
				x := (dubi * box.Max().X()) + ((1.0 - dubi) * box.Min().X())
				y := (dubj * box.Max().Y()) + ((1.0 - dubj) * box.Min().Y())
				z := (dubk * box.Max().Z()) + ((1.0 - dubk) * box.Min().Z())
				newx := (cosTheta * x) + (sinTheta * z)
				newz := (-sinTheta * x) + (cosTheta * z)
				tester := []float64{newx, y, newz}
				for c := 0; c < 3; c++ {
					if tester[c] > max[c] {
						max[c] = tester[c]
					}
					if tester[c] < min[c] {
						min[c] = tester[c]
					}
				}
			}
		}
	}

	boundingBox := raytrace.NewAABB(mgl64.Vec3{min[0], min[1], min[2]}, mgl64.Vec3{max[0], max[1], max[2]})

	r := &RotateY{hitable, angle, sinTheta, cosTheta, boundingBox}
	return r
}

func (s *RotateY) Hit(ray *raytrace.Ray, tMin float64, tMax float64) *raytrace.HitRecord {
	origin := []float64{ray.Origin().X(), ray.Origin().Y(), ray.Origin().Z()}
	dir := []float64{ray.Direction().X(), ray.Direction().Y(), ray.Direction().Z()}
	origin[0] = (s.costheta * ray.Origin().X()) - (s.sintheta * ray.Origin().Z())
	origin[2] = (s.sintheta * ray.Origin().X()) + (s.costheta * ray.Origin().Z())
	dir[0] = (s.costheta * ray.Direction().X()) - (s.sintheta * ray.Direction().Z())
	dir[2] = (s.sintheta * ray.Direction().X()) + (s.costheta * ray.Direction().Z())
	rotatedRay := raytrace.NewRay(mgl64.Vec3{origin[0], origin[1], origin[2]}, mgl64.Vec3{dir[0], dir[1], dir[2]})
	hitRecord := s.hitable.Hit(rotatedRay, tMin, tMax)
	if hitRecord == nil {
		return nil
	}

	p := []float64{hitRecord.P().X(), hitRecord.P().Y(), hitRecord.P().Z()}
	normal := []float64{hitRecord.Normal().X(), hitRecord.Normal().Y(), hitRecord.Normal().Z()}
	p[0] = (s.costheta * hitRecord.P().X()) + (s.sintheta * hitRecord.P().Z())
	p[2] = (-s.sintheta * hitRecord.P().X()) + (s.costheta * hitRecord.P().Z())
	normal[0] = (s.costheta * hitRecord.Normal().X()) + (s.sintheta * hitRecord.Normal().Z())
	normal[2] = (-s.sintheta * hitRecord.Normal().X()) + (s.costheta * hitRecord.Normal().Z())
	return raytrace.NewHitRecord(hitRecord.T(), mgl64.Vec3{p[0], p[1], p[2]}, mgl64.Vec3{normal[0], normal[1], normal[2]}, hitRecord.UvCoords(), hitRecord.Material())
}

func (s *RotateY) GetBoundingBox(t0 float64, t1 float64) *raytrace.AABB {
	return s.boundingbox
}

func (s *RotateY) GetPdfValue(origin mgl64.Vec3, v mgl64.Vec3) float64 {
	return 1.0
}

func (s *RotateY) Random(origin mgl64.Vec3) mgl64.Vec3 {
	return vectorextensions.UnitX()
}
