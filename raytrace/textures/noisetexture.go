package textures

import (
	"github.com/go-gl/mathgl/mgl64"
	"github.com/markzuber/zgotrace/raytrace"
	"github.com/markzuber/zgotrace/raytrace/vectorextensions"
)

type NoiseTexture struct {
	interpolate bool
	scale       float64
}

func NewNoiseTexture(interpolate bool, scale float64) raytrace.Texture {
	t := &NoiseTexture{interpolate, scale}
	return t
}

func (t *NoiseTexture) GetValue(uvCoords mgl64.Vec2, p mgl64.Vec3) raytrace.ColorVector {
	return raytrace.NewColorVector(1.0, 1.0, 1.0).MulScalar(raytrace.PerlinNoise(vectorextensions.MulScalar(p, t.scale), t.interpolate))
}
