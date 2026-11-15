package cameras

import (
	"math"

	"github.com/go-gl/mathgl/mgl64"
	"github.com/markzuber/zgotrace/raytrace"
	"github.com/markzuber/zgotrace/raytrace/vectorextensions"
)

type normalCamera struct {
	origin          mgl64.Vec3
	lowerLeftCorner mgl64.Vec3
	horizontal      mgl64.Vec3
	vertical        mgl64.Vec3
	u               mgl64.Vec3
	v               mgl64.Vec3
	w               mgl64.Vec3
	lensRadius      float64
}

func NewNormalCamera(
	lookFrom mgl64.Vec3,
	lookAt mgl64.Vec3,
	up mgl64.Vec3,
	verticalFov float64,
	aspect float64,
	aperture float64,
	focusDistance float64) raytrace.Camera {

	lensRadius := aperture / 2.0
	theta := verticalFov * math.Pi / 180.0
	halfHeight := math.Tan(theta / 2.0)
	halfWidth := aspect * halfHeight
	origin := lookFrom
	w := vectorextensions.ToUnitVector(lookFrom.Sub(lookAt))
	u := vectorextensions.ToUnitVector(up.Cross(w))
	v := w.Cross(u)
	lowerLeftCorner := origin.Sub(u.Mul(halfWidth * focusDistance)).Sub(v.Mul(halfHeight * focusDistance)).Sub(w.Mul(focusDistance))
	horizontal := u.Mul(2.0 * halfWidth * focusDistance)
	vertical := v.Mul(2.0 * halfHeight * focusDistance)

	c := &normalCamera{origin, lowerLeftCorner, horizontal, vertical, u, v, w, lensRadius}
	return c
}

func (c *normalCamera) GetRay(u float64, v float64) *raytrace.Ray {
	rd := vectorextensions.GetRandomInUnitSphere().Mul(c.lensRadius)
	offset := (u * rd.X()) + (v * rd.Y())
	return raytrace.NewRay(
		vectorextensions.AddScalar(c.origin, offset),
		vectorextensions.AddScalar(c.lowerLeftCorner.Add(c.horizontal.Mul(u)).Add(c.vertical.Mul(v)).Sub(c.origin), -offset))
}
