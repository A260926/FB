package raytrace

import (
	"math"
	"math/rand"

	"github.com/markzuber/zgotrace/raytrace/vectorextensions"

	"github.com/go-gl/mathgl/mgl64"
)

var ranFloat [256]float64
var ranVector [256]mgl64.Vec3
var permX [256]int
var permY [256]int
var permZ [256]int

func perlinGenerate() [256]float64 {
	var p [256]float64
	for i := 0; i < 256; i++ {
		p[i] = rand.Float64()
	}
	return p
}

func perlinVectorGenerate() [256]mgl64.Vec3 {
	var p [256]mgl64.Vec3
	for i := 0; i < 256; i++ {
		p[i] = vectorextensions.ToUnitVector(mgl64.Vec3{-1.0 + (2.0 * rand.Float64()), -1.0 + (2.0 * rand.Float64()), -1.0 + (2.0 * rand.Float64())})
	}
	return p
}

func perlinGeneratePerm() [256]int {
	var p [256]int
	for i := 0; i < 256; i++ {
		p[i] = i
	}
	permute(p)
	return p
}

func permute(p [256]int) {
	for i := len(p) - 1; i > 0; i-- {
		target := int(rand.Float64() * float64(i+1))
		tmp := p[i]
		p[i] = p[target]
		p[target] = tmp
	}
}

func init() {
	ranFloat = perlinGenerate()
	ranVector = perlinVectorGenerate()
	permX = perlinGeneratePerm()
	permY = perlinGeneratePerm()
	permZ = perlinGeneratePerm()
}

func PerlinNoise(p mgl64.Vec3, interpolate bool) float64 {
	u := p.X() - math.Floor(p.X())
	v := p.Y() - math.Floor(p.Y())
	w := p.Z() - math.Floor(p.Z())

	if interpolate {
		i := int(math.Floor(p.X()))
		j := int(math.Floor(p.Y()))
		k := int(math.Floor(p.Z()))

		u = u * u * (3.0 - (2.0 * u))
		v = v * v * (3.0 - (2.0 * v))
		w = w * w * (3.0 - (2.0 * w))

		var o [2][2][2]float64

		for di := 0; di < 2; di++ {
			for dj := 0; dj < 2; dj++ {
				for dk := 0; dk < 2; dk++ {
					o[di][dj][dk] = ranFloat[permX[(i+di)&255]^permY[(j+dj)&255]^permZ[(k+dk)&255]]
				}
			}
		}

		return trilinearInterpolate(o, u, v, w)
	} else {
		i := int(4.0*p.X()) & 255
		j := int(4.0*p.Y()) & 255
		k := int(4.0*p.Z()) & 255
		return ranFloat[permX[i]^permY[j]^permZ[k]]
	}
}

func PerlinVectorNoise(p mgl64.Vec3) float64 {
	u := p.X() - math.Floor(p.X())
	v := p.Y() - math.Floor(p.Y())
	w := p.Z() - math.Floor(p.Z())
	i := int(math.Floor(p.X()))
	j := int(math.Floor(p.Y()))
	k := int(math.Floor(p.Z()))
	var c [2][2][2]mgl64.Vec3
	for di := 0; di < 2; di++ {
		for dj := 0; dj < 2; dj++ {
			for dk := 0; dk < 2; dk++ {
				c[di][dj][dk] = ranVector[permX[(i+di)&255]^permY[(j+dj)&255]^permZ[(k+dk)&255]]
			}
		}
	}

	return perlinVectorInterpolate(c, u, v, w)
}

func PerlinVectorTurbulence(p mgl64.Vec3) float64 {
	return PerlinVectorTurbulenceCustomDepth(p, 7)
}
func PerlinVectorTurbulenceCustomDepth(p mgl64.Vec3, depth int) float64 {
	accum := 0.0
	weight := 1.0
	for i := 0; i < depth; i++ {
		accum += weight * PerlinVectorNoise(p)
		weight *= 0.5
		p = vectorextensions.MulScalar(p, 2.0)
	}
	return math.Abs(accum)
}

func perlinVectorInterpolate(c [2][2][2]mgl64.Vec3, u float64, v float64, w float64) float64 {
	uu := u * u * (3.0 - (2.0 * u))
	vv := v * v * (3.0 - (2.0 * v))
	ww := w * w * (3.0 - (2.0 * w))
	accum := 0.0
	for i := 0; i < 2; i++ {
		dubi := float64(i)
		for j := 0; j < 2; j++ {
			dubj := float64(j)
			for k := 0; k < 2; k++ {
				dubk := float64(k)
				weightVec := mgl64.Vec3{u - dubi, v - dubj, w - dubk}
				accum += ((dubi * uu) + ((1.0 - dubi) * (1.0 - uu))) *
					((dubj * vv) + ((1.0 - dubj) * (1.0 - vv))) *
					((dubk * ww) + ((1.0 - dubk) * (1.0 - ww))) *
					c[i][j][k].Dot(weightVec)
			}
		}
	}

	return accum
}

func trilinearInterpolate(o [2][2][2]float64, u float64, v float64, w float64) float64 {
	accum := 0.0
	for i := 0; i < 2; i++ {
		dubi := float64(i)
		for j := 0; j < 2; j++ {
			dubj := float64(j)
			for k := 0; k < 2; k++ {
				dubk := float64(k)
				accum += ((dubi * u) + ((1.0 - dubi) * (1.0 - u))) *
					((dubj * v) + ((1.0 - dubj) * (1.0 - v))) *
					((dubk * w) + ((1.0 - dubk) * (1.0 - w))) * o[i][j][k]
			}
		}
	}

	return accum
}
