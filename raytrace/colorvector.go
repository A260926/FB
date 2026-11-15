package raytrace

import (
	"math"

	"github.com/go-gl/mathgl/mgl64"
)

type ColorVector struct {
	colors mgl64.Vec3
}

func NewColorVector(r float64, g float64, b float64) ColorVector {
	return ColorVector{mgl64.Vec3{r, g, b}}
}

func NewColorVectorFromRGBA(r uint32, g uint32, b uint32, a uint32) ColorVector {
	return NewColorVector(float64(r)/float64(0xffff), float64(g)/float64(0xffff), float64(b)/float64(0xffff))
}

func (c ColorVector) RGBA() (r, g, b, a uint32) {
	c2 := c.Clamp()
	return uint32(c2.R() * 0xffff), uint32(c2.G() * 0xffff), uint32(c2.B() * 0xffff), 0xffff
}

func (c ColorVector) R() float64 {
	return c.colors.X()
}

func (c ColorVector) G() float64 {
	return c.colors.Y()
}

func (c ColorVector) B() float64 {
	return c.colors.Z()
}

func (c ColorVector) Mul(other ColorVector) ColorVector {
	return NewColorVector(c.R()*other.R(), c.G()*other.G(), c.B()*other.B())
}

func (c ColorVector) MulScalar(other float64) ColorVector {
	return NewColorVector(c.R()*other, c.G()*other, c.B()*other)
}

func (c ColorVector) DivScalar(other float64) ColorVector {
	return NewColorVector(c.R()/other, c.G()/other, c.B()/other)
}

func (c ColorVector) Add(other ColorVector) ColorVector {
	return NewColorVector(c.R()+other.R(), c.G()+other.G(), c.B()+other.B())
}

func (c ColorVector) ApplyGamma2() ColorVector {
	return NewColorVector(math.Sqrt(c.R()), math.Sqrt(c.G()), math.Sqrt(c.B()))
}

func (c ColorVector) Clamp() ColorVector {
	r := c.R()
	g := c.G()
	b := c.B()

	if r < 0.0 {
		r = 0.0
	}
	if r > 1.0 {
		r = 1.0
	}

	if g < 0.0 {
		g = 0.0
	}
	if g > 1.0 {
		g = 1.0
	}

	if b < 0.0 {
		b = 0.0
	}
	if b > 1.0 {
		b = 1.0
	}

	return NewColorVector(r, g, b)
}
