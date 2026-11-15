package materials

import (
	"github.com/go-gl/mathgl/mgl64"
	"github.com/markzuber/zgotrace/raytrace"
	"github.com/markzuber/zgotrace/raytrace/vectorextensions"
)

type IsotropicMaterial struct {
	albedo raytrace.Texture
}

func NewIsotropicMaterial(albedo raytrace.Texture) raytrace.Material {
	m := &IsotropicMaterial{albedo}
	return m
}

func (m *IsotropicMaterial) Scatter(rayIn *raytrace.Ray, hitRecord *raytrace.HitRecord) *raytrace.ScatterResult {
	scattered := raytrace.NewRay(hitRecord.P(), vectorextensions.GetRandomInUnitSphere())
	attenuation := m.albedo.GetValue(hitRecord.UvCoords(), hitRecord.P())
	return raytrace.NewScatterResult(true, attenuation, scattered, nil)
}

func (m *IsotropicMaterial) ScatteringPdf(rayIn *raytrace.Ray, hitRecord *raytrace.HitRecord, scattered *raytrace.Ray) float64 {
	return 0.0
}

func (m *IsotropicMaterial) Emitted(rayIn *raytrace.Ray, hitRecord *raytrace.HitRecord, uvCoords mgl64.Vec2, p mgl64.Vec3) raytrace.ColorVector {
	return raytrace.NewColorVector(0, 0, 0)
}
