package hitables

import (
	"math"
	"math/rand"

	"github.com/go-gl/mathgl/mgl64"
	"github.com/markzuber/zgotrace/raytrace"
)

type HitableList struct {
	members []raytrace.Hitable
}

func NewHitableList(members []raytrace.Hitable) raytrace.Hitable {
	s := &HitableList{members}
	return s
}

func (s *HitableList) Hit(ray *raytrace.Ray, tMin float64, tMax float64) *raytrace.HitRecord {
	var hitRecord *raytrace.HitRecord

	closestSoFar := tMax
	for _, item := range s.members {
		hr := item.Hit(ray, tMin, closestSoFar)
		if hr == nil {
			continue
		}

		closestSoFar = hr.T()
		hitRecord = hr
	}

	return hitRecord
}

func (s *HitableList) GetBoundingBox(t0 float64, t1 float64) *raytrace.AABB {
	if len(s.members) == 0 {
		return nil
	}

	box := s.members[0].GetBoundingBox(t0, t1)
	if box == nil {
		return nil
	}

	for i := 1; i < len(s.members); i++ {
		tempBox := s.members[i].GetBoundingBox(t0, t1)
		if tempBox == nil {
			return nil
		}

		box = box.GetSurroundingBox(tempBox)
	}

	return box
}

func (s *HitableList) GetPdfValue(origin mgl64.Vec3, v mgl64.Vec3) float64 {
	if len(s.members) == 0 {
		return 0
	}

	weight := float64(1.0 / len(s.members))
	sum := 0.0
	for _, hitable := range s.members {
		sum += weight * hitable.GetPdfValue(origin, v)
	}
	return sum
}

func (s *HitableList) Random(origin mgl64.Vec3) mgl64.Vec3 {
	index := int(math.Floor(rand.Float64() * float64(len(s.members))))
	if index < len(s.members) {
		return s.members[index].Random(origin)
	}
	return origin
}
