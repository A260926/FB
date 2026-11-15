package materials

import (
	"github.com/go-gl/mathgl/mgl64"
	"github.com/markzuber/zgotrace/raytrace"
	"github.com/markzuber/zgotrace/raytrace/vectorextensions"
)

type MetalMaterial struct {
	albedo raytrace.ColorVector
	fuzz   float64
}

func NewMetalMaterial(albedo raytrace.ColorVector, fuzz float64) raytrace.Material {
	m := &MetalMaterial{albedo, fuzz}
	return m
}

func (m *MetalMaterial) Scatter(rayIn *raytrace.Ray, hitRecord *raytrace.HitRecord) *raytrace.ScatterResult {
	reflected := vectorextensions.Reflect(vectorextensions.ToUnitVector(rayIn.Direction()), hitRecord.Normal())
	specularRay := raytrace.NewRay(hitRecord.P(), reflected.Add(vectorextensions.MulScalar(vectorextensions.GetRandomInUnitSphere(), m.fuzz)))
	attenuation := m.albedo
	return raytrace.NewScatterResult(true, attenuation, specularRay, nil)
}

func (m *MetalMaterial) ScatteringPdf(rayIn *raytrace.Ray, hitRecord *raytrace.HitRecord, scattered *raytrace.Ray) float64 {
	return 0.0
}

func (m *MetalMaterial) Emitted(rayIn *raytrace.Ray, hitRecord *raytrace.HitRecord, uvCoords mgl64.Vec2, p mgl64.Vec3) raytrace.ColorVector {
	return raytrace.NewColorVector(0, 0, 0)
}
