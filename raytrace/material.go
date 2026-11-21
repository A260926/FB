package raytrace

import "github.com/go-gl/mathgl/mgl64"

type Material interface {
	Scatter(rayIn *Ray, hitRecord *HitRecord) *ScatterResult
	ScatteringPdf(rayIn *Ray, hitRecord *HitRecord, scattered *Ray) float64
	Emitted(rayIn *Ray, hitRecord *HitRecord, uvCoords mgl64.Vec2, p mgl64.Vec3) ColorVector
}
