package textures

import (
	"math"

	"github.com/go-gl/mathgl/mgl64"
	"github.com/markzuber/zgotrace/raytrace"
)

type CheckerTexture struct {
	t1    raytrace.Texture
	t2    raytrace.Texture
	scale mgl64.Vec3
}

func NewCheckerTexture(t1 raytrace.Texture, t2 raytrace.Texture, scale mgl64.Vec3) raytrace.Texture {
	t := &CheckerTexture{t1, t2, scale}
	return t
}

func (t *CheckerTexture) GetValue(uvCoords mgl64.Vec2, p mgl64.Vec3) raytrace.ColorVector {
	sines := math.Sin(t.scale.X()*p.X()) * math.Sin(t.scale.Y()*p.Y()) * math.Sin(t.scale.Z()*p.Z())
	if sines < 0.0 {
		return t.t1.GetValue(uvCoords, p)
	} else {
		return t.t2.GetValue(uvCoords, p)
	}
}
