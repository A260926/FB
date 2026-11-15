package textures

import (
	"math"

	"github.com/go-gl/mathgl/mgl64"
	"github.com/markzuber/zgotrace/raytrace"
	"github.com/markzuber/zgotrace/raytrace/vectorextensions"
)

type VectorNoiseMode int

const (
	DarkNoise      VectorNoiseMode = iota
	DarkTurbulence                 = iota
	Soft                           = iota
	Marble                         = iota
)

type VectorNoiseTexture struct {
	mode  VectorNoiseMode
	scale float64
}

func NewVectorNoiseTexture(mode VectorNoiseMode, scale float64) raytrace.Texture {
	t := &VectorNoiseTexture{mode, scale}
	return t
}

func (t *VectorNoiseTexture) GetValue(uvCoords mgl64.Vec2, p mgl64.Vec3) raytrace.ColorVector {
	switch t.mode {
	case Soft:
		return raytrace.NewColorVector(1.0, 1.0, 1.0).MulScalar(0.5 * (1.0 + raytrace.PerlinVectorTurbulence(vectorextensions.MulScalar(p, t.scale))))
	case DarkNoise:
		return raytrace.NewColorVector(1.0, 1.0, 1.0).MulScalar(raytrace.PerlinVectorNoise(vectorextensions.MulScalar(p, t.scale)))
	case DarkTurbulence:
		return raytrace.NewColorVector(1.0, 1.0, 1.0).MulScalar(raytrace.PerlinVectorTurbulence(vectorextensions.MulScalar(p, t.scale)))
	case Marble:
		return raytrace.NewColorVector(1.0, 1.0, 1.0).MulScalar(0.5 * (1.0 + math.Sin((t.scale*p.Z())+(10.0*raytrace.PerlinVectorTurbulence(p)))))
	default:
		return raytrace.NewColorVector(0.0, 0.0, 0.0)
	}
}
