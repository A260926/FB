package materials

import (
	"math"

	"github.com/go-gl/mathgl/mgl64"
	"github.com/markzuber/zgotrace/raytrace"
	"github.com/markzuber/zgotrace/raytrace/vectorextensions"
)

type LambertianMaterial struct {
	albedo raytrace.Texture
}

func NewLambertianMaterial(albedo raytrace.Texture) raytrace.Material {
	m := &LambertianMaterial{albedo}
	return m
}

func (m *LambertianMaterial) Scatter(rayIn *raytrace.Ray, hitRecord *raytrace.HitRecord) *raytrace.ScatterResult {
	attenuation := m.albedo.GetValue(hitRecord.UvCoords(), hitRecord.P())
	return raytrace.NewScatterResult(true, attenuation, nil, raytrace.NewCosinePdf(hitRecord.Normal()))
}

func (m *LambertianMaterial) ScatteringPdf(rayIn *raytrace.Ray, hitRecord *raytrace.HitRecord, scattered *raytrace.Ray) float64 {
	cosine := hitRecord.Normal().Dot(vectorextensions.ToUnitVector(scattered.Direction()))
	if cosine < 0.0 {
		cosine = 0.0
	}

	return cosine / math.Pi
}

func (m *LambertianMaterial) Emitted(rayIn *raytrace.Ray, hitRecord *raytrace.HitRecord, uvCoords mgl64.Vec2, p mgl64.Vec3) raytrace.ColorVector {
	return raytrace.NewColorVector(0, 0, 0)
}
