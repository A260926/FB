package raytrace

import (
	"math"
	"math/rand"

	"github.com/go-gl/mathgl/mgl64"
)

func RandomToSphere(radius float64, distanceSquared float64) mgl64.Vec3 {
	r1 := rand.Float64()
	r2 := rand.Float64()
	z := 1.0 + (r2 * (math.Sqrt(1.0-(radius*radius/distanceSquared)) - 1.0))
	phi := 2.0 * math.Pi * r1
	x := math.Cos(phi) * math.Sqrt(1.0-(z*z))
	y := math.Sin(phi) * math.Sqrt(1.0-(z*z))
	return mgl64.Vec3{x, y, z}
}

func GetRandomCosineDirection() mgl64.Vec3 {
	r1 := rand.Float64()
	r2 := rand.Float64()
	z := math.Sqrt(1.0 - r2)
	phi := 2.0 * math.Pi * r1
	x := math.Cos(phi) * 2.0 * math.Sqrt(r2)
	y := math.Sin(phi) * 2.0 * math.Sqrt(r2)
	return mgl64.Vec3{x, y, z}
}
