package raytrace

import "github.com/go-gl/mathgl/mgl64"

type Pdf interface {
	GetValue(direction mgl64.Vec3) float64
	Generate() mgl64.Vec3
}
