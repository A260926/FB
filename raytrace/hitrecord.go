package raytrace

import (
	"github.com/go-gl/mathgl/mgl64"
)

type HitRecord struct {
	t        float64
	p        mgl64.Vec3
	normal   mgl64.Vec3
	uvcoords mgl64.Vec2
	material Material
}

func NewHitRecord(t float64, p mgl64.Vec3, normal mgl64.Vec3, uvCoords mgl64.Vec2, material Material) *HitRecord {
	return &HitRecord{t, p, normal, uvCoords, material}
}

func (hr *HitRecord) Normal() mgl64.Vec3 {
	return hr.normal
}

func (hr *HitRecord) P() mgl64.Vec3 {
	return hr.p
}

func (hr *HitRecord) T() float64 {
	return hr.t
}

func (hr *HitRecord) UvCoords() mgl64.Vec2 {
	return hr.uvcoords
}

func (hr *HitRecord) Material() Material {
	return hr.material
}
