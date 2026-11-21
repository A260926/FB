package raytrace

import "github.com/go-gl/mathgl/mgl64"

// Hitable go
type Hitable interface {
	Hit(ray *Ray, tMin float64, tMax float64) *HitRecord
	GetBoundingBox(t0 float64, t1 float64) *AABB

	GetPdfValue(origin mgl64.Vec3, v mgl64.Vec3) float64
	Random(origin mgl64.Vec3) mgl64.Vec3
}
