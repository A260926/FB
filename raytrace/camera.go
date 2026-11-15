package raytrace

// Camera todo
type Camera interface {
	GetRay(s float64, t float64) *Ray
}
