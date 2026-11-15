package raytrace

import "github.com/go-gl/mathgl/mgl64"

type Texture interface {
	GetValue(uvCoords mgl64.Vec2, p mgl64.Vec3) ColorVector
}
