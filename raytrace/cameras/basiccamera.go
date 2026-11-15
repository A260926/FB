package cameras

import (
	"github.com/go-gl/mathgl/mgl64"
	"github.com/markzuber/zgotrace/raytrace"
)

type basicCamera struct {
	origin          mgl64.Vec3
	lowerLeftCorner mgl64.Vec3
	horizontal      mgl64.Vec3
	vertical        mgl64.Vec3
}

func NewBasicCamera(origin mgl64.Vec3, lowerLeftCorner mgl64.Vec3, horizontal mgl64.Vec3, vertical mgl64.Vec3) raytrace.Camera {
	c := &basicCamera{origin, lowerLeftCorner, horizontal, vertical}
	return c
}

func (c *basicCamera) Origin() mgl64.Vec3 {
	return c.origin
}

func (c *basicCamera) LowerLeftCorner() mgl64.Vec3 {
	return c.lowerLeftCorner
}

func (c *basicCamera) Horizontal() mgl64.Vec3 {
	return c.horizontal
}

func (c *basicCamera) Vertical() mgl64.Vec3 {
	return c.vertical
}

func (c *basicCamera) GetRay(u float64, v float64) *raytrace.Ray {
	return raytrace.NewRay(c.Origin(), c.LowerLeftCorner().Add(c.Horizontal().Mul(u)).Add(c.Vertical().Mul(v)).Sub(c.Origin()))
}
