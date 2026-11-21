package raytrace

import (
	"github.com/go-gl/mathgl/mgl64"
)

type HitablePdf struct {
	hitable Hitable
	origin  mgl64.Vec3
}

func NewHitablePdf(hitable Hitable, origin mgl64.Vec3) Pdf {
	p := &HitablePdf{hitable, origin}
	return p
}

func (p *HitablePdf) GetValue(direction mgl64.Vec3) float64 {
	return p.hitable.GetPdfValue(p.origin, direction)
}

func (p *HitablePdf) Generate() mgl64.Vec3 {
	return p.hitable.Random(p.origin)
}
