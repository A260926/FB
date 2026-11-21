package raytrace

import "github.com/go-gl/mathgl/mgl64"

// Ray todo
type Ray struct {
	origin    mgl64.Vec3
	direction mgl64.Vec3
}

// NewRay todo
func NewRay(origin mgl64.Vec3, direction mgl64.Vec3) *Ray {
	return &Ray{origin, direction}
}

// Origin todo
func (r *Ray) Origin() mgl64.Vec3 {
	return r.origin
}

// Direction todo
func (r *Ray) Direction() mgl64.Vec3 {
	return r.direction
}

// GetPointAtParameter todo
func (r *Ray) GetPointAtParameter(t float64) mgl64.Vec3 {
	return r.Origin().Add((r.Direction().Mul(t)))
}
