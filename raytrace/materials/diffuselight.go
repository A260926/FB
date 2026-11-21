package materials

import (
	"github.com/go-gl/mathgl/mgl64"
	"github.com/markzuber/zgotrace/raytrace"
)

type DiffuseLight struct {
	texture raytrace.Texture
}

func NewDiffuseLight(texture raytrace.Texture) raytrace.Material {
	l := &DiffuseLight{texture}
	return l
}

func (l *DiffuseLight) Scatter(rayIn *raytrace.Ray, hitRecord *raytrace.HitRecord) *raytrace.ScatterResult {
	return raytrace.NewFalseScatterResult()
}

func (l *DiffuseLight) ScatteringPdf(rayIn *raytrace.Ray, hitRecord *raytrace.HitRecord, scattered *raytrace.Ray) float64 {
	return 0.0
}

func (l *DiffuseLight) Emitted(rayIn *raytrace.Ray, hitRecord *raytrace.HitRecord, uvCoords mgl64.Vec2, p mgl64.Vec3) raytrace.ColorVector {
	// return hitRecord.Normal().Dot(rayIn.Direction()) < 0.0 ?
	return l.texture.GetValue(uvCoords, p)
}
