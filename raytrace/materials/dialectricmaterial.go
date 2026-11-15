package materials

import (
	"math"
	"math/rand"

	"github.com/go-gl/mathgl/mgl64"
	"github.com/markzuber/zgotrace/raytrace"
	"github.com/markzuber/zgotrace/raytrace/vectorextensions"
)

type DialectricMaterial struct {
	refractionIndex float64
}

func NewDialectricMaterial(refractionIndex float64) raytrace.Material {
	m := &DialectricMaterial{refractionIndex}
	return m
}

func calculateSchlickApproximation(cosine float64, refractionIndex float64) float64 {
	r0 := (1.0 - refractionIndex) / (1.0 + refractionIndex)
	r0 *= r0
	return r0 + ((1.0 - r0) * math.Pow(1.0-cosine, 5.0))
}

func (m *DialectricMaterial) Scatter(rayIn *raytrace.Ray, hitRecord *raytrace.HitRecord) *raytrace.ScatterResult {
	var reflected = vectorextensions.Reflect(rayIn.Direction(), hitRecord.Normal())
	var attenuation = raytrace.NewColorVector(1.0, 1.0, 1.0)
	var niOverNt float64
	var outwardNormal mgl64.Vec3
	var cosine float64

	if rayIn.Direction().Dot(hitRecord.Normal()) > 0.0 {
		outwardNormal = vectorextensions.Invert(hitRecord.Normal())
		niOverNt = m.refractionIndex
		cosine = m.refractionIndex * rayIn.Direction().Dot(hitRecord.Normal()) / rayIn.Direction().Len()
	} else {
		outwardNormal = hitRecord.Normal()
		niOverNt = 1.0 / m.refractionIndex
		cosine = -rayIn.Direction().Dot(hitRecord.Normal()) / rayIn.Direction().Len()
	}

	var reflectProbability float64
	var scattered *raytrace.Ray
	var refracted = vectorextensions.Refract(rayIn.Direction(), outwardNormal, niOverNt)
	if vectorextensions.IsVectorZero(refracted) {
		scattered = raytrace.NewRay(hitRecord.P(), reflected)
		reflectProbability = 1.0
	} else {
		reflectProbability = calculateSchlickApproximation(cosine, m.refractionIndex)
	}

	if rand.Float64() < reflectProbability {
		scattered = raytrace.NewRay(hitRecord.P(), reflected)
	} else {
		scattered = raytrace.NewRay(hitRecord.P(), refracted)
	}

	return raytrace.NewScatterResult(true, attenuation, scattered, nil)
}

func (m *DialectricMaterial) ScatteringPdf(rayIn *raytrace.Ray, hitRecord *raytrace.HitRecord, scattered *raytrace.Ray) float64 {
	return 0.0
}

func (m *DialectricMaterial) Emitted(rayIn *raytrace.Ray, hitRecord *raytrace.HitRecord, uvCoords mgl64.Vec2, p mgl64.Vec3) raytrace.ColorVector {
	return raytrace.NewColorVector(0, 0, 0)
}
