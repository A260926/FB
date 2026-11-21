package hitables

import (
	"github.com/go-gl/mathgl/mgl64"
	"github.com/markzuber/zgotrace/raytrace"
	"github.com/markzuber/zgotrace/raytrace/vectorextensions"
)

type Box struct {
	parts  raytrace.Hitable
	posmin mgl64.Vec3
	posmax mgl64.Vec3
}

func NewBox(p0 mgl64.Vec3, p1 mgl64.Vec3, material raytrace.Material) raytrace.Hitable {
	list := []raytrace.Hitable{
		NewXyRect(p0.X(), p1.X(), p0.Y(), p1.Y(), p1.Z(), material),
		NewFlipNormals(NewXyRect(p0.X(), p1.X(), p0.Y(), p1.Y(), p0.Z(), material)),
		NewXzRect(p0.X(), p1.X(), p0.Z(), p1.Z(), p1.Y(), material),
		NewFlipNormals(NewXzRect(p0.X(), p1.X(), p0.Z(), p1.Z(), p0.Y(), material)),
		NewYzRect(p0.Y(), p1.Y(), p0.Z(), p1.Z(), p1.X(), material),
		NewFlipNormals(NewYzRect(p0.Y(), p1.Y(), p0.Z(), p1.Z(), p0.X(), material)),
	}

	box := &Box{NewHitableList(list), p0, p1}
	return box
}

func (h *Box) Hit(ray *raytrace.Ray, tMin float64, tMax float64) *raytrace.HitRecord {
	return h.parts.Hit(ray, tMin, tMax)
}

func (h *Box) GetBoundingBox(t0 float64, t1 float64) *raytrace.AABB {
	return raytrace.NewAABB(h.posmin, h.posmax)
}

func (h *Box) GetPdfValue(origin mgl64.Vec3, v mgl64.Vec3) float64 {
	return 1.0
}

func (h *Box) Random(origin mgl64.Vec3) mgl64.Vec3 {
	return vectorextensions.UnitX()
}
