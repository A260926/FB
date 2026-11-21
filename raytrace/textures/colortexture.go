package textures

import (
	"github.com/go-gl/mathgl/mgl64"
	"github.com/markzuber/zgotrace/raytrace"
)

type ColorTexture struct {
	color raytrace.ColorVector
}

func NewColorTexture(color raytrace.ColorVector) raytrace.Texture {
	t := &ColorTexture{color}
	return t
}

func (t *ColorTexture) GetValue(uvCoords mgl64.Vec2, p mgl64.Vec3) raytrace.ColorVector {
	return t.color
}
