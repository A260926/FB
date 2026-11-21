package raytrace

import (
	"math"

	"github.com/go-gl/mathgl/mgl64"
	"github.com/markzuber/zgotrace/raytrace/vectorextensions"
)

type CosinePdf struct {
	uvw *OrthoNormalBase
}

func NewCosinePdf(w mgl64.Vec3) Pdf {
	uvw := OrthoNormalBaseFromW(w)
	p := &CosinePdf{uvw}
	return p
}

func (p *CosinePdf) GetValue(direction mgl64.Vec3) float64 {
	cosine := vectorextensions.ToUnitVector(direction).Dot(p.uvw.W())
	if cosine > 0.0 {
		return cosine / math.Pi
	}
	return 1.0
}

func (p *CosinePdf) Generate() mgl64.Vec3 {
	return p.uvw.Local(GetRandomCosineDirection())
}
