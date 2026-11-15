package vectorextensions

import (
	"math"
	"math/rand"

	"github.com/go-gl/mathgl/mgl64"
)

func ToUnitVector(vec mgl64.Vec3) mgl64.Vec3 {
	return vec.Mul(1.0 / vec.Len())
}

func GetRandomInUnitSphere() mgl64.Vec3 {
	var pv mgl64.Vec3

	for {
		randVec := mgl64.Vec3{rand.Float64(), rand.Float64(), rand.Float64()}
		pv = randVec.Mul(2.0).Sub(mgl64.Vec3{1, 1, 1})
		if pv.LenSqr() < 1.0 {
			return pv
		}
	}
}

func AddScalar(vec mgl64.Vec3, val float64) mgl64.Vec3 {
	return mgl64.Vec3{vec.X() + val, vec.Y() + val, vec.Z() + val}
}

func MulScalar(vec mgl64.Vec3, val float64) mgl64.Vec3 {
	return mgl64.Vec3{vec.X() * val, vec.Y() * val, vec.Z() * val}
}

func DivScalar(vec mgl64.Vec3, val float64) mgl64.Vec3 {
	return mgl64.Vec3{vec.X() / val, vec.Y() / val, vec.Z() / val}
}

func UnitY() mgl64.Vec3 {
	return mgl64.Vec3{0.0, 1.0, 0.0}
}

func UnitX() mgl64.Vec3 {
	return mgl64.Vec3{1.0, 0.0, 0.0}
}

func UnitZ() mgl64.Vec3 {
	return mgl64.Vec3{0.0, 0.0, 1.0}
}

func Reflect(vec mgl64.Vec3, other mgl64.Vec3) mgl64.Vec3 {
	return vec.Sub(other.Mul(2.0 * vec.Dot(other)))
}

func Invert(vec mgl64.Vec3) mgl64.Vec3 {
	return mgl64.Vec3{-vec.X(), -vec.Y(), -vec.Z()}
}

func Refract(vec mgl64.Vec3, normal mgl64.Vec3, niOverNt float64) mgl64.Vec3 {
	unitVec := ToUnitVector(vec)
	dt := unitVec.Dot(normal)
	discriminant := 1.0 - (niOverNt * niOverNt * (1.0 - (dt * dt)))
	if discriminant > 0.0 {
		return MulScalar(unitVec.Sub(MulScalar(normal, dt)), niOverNt).Sub(MulScalar(normal, math.Sqrt(discriminant)))
	}

	return mgl64.Vec3{0.0, 0.0, 0.0}
}

func IsVectorZero(vec mgl64.Vec3) bool {
	return vec.X() == 0.0 && vec.Y() == 0.0 && vec.Z() == 0.0
}
