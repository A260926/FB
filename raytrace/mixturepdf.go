package raytrace

import (
	"math/rand"

	"github.com/go-gl/mathgl/mgl64"
)

type MixturePdf struct {
	p0 Pdf
	p1 Pdf
}

func NewMixturePdf(p0 Pdf, p1 Pdf) Pdf {
	p := &MixturePdf{p0, p1}
	return p
}

func (p *MixturePdf) GetValue(direction mgl64.Vec3) float64 {
	return (0.5 * p.p0.GetValue(direction)) + (0.5 * p.p1.GetValue(direction))
}

func (p *MixturePdf) Generate() mgl64.Vec3 {
	if rand.Float64() < 0.5 {
		return p.p0.Generate()
	}
	return p.p1.Generate()
}
