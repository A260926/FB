package hitables

import (
	"github.com/go-gl/mathgl/mgl64"
	"github.com/markzuber/zgotrace/raytrace"
	"github.com/markzuber/zgotrace/raytrace/vectorextensions"
)

type FlipNormals struct {
	hitable raytrace.Hitable
}

func NewFlipNormals(hitable raytrace.Hitable) raytrace.Hitable {
	h := &FlipNormals{hitable}
	return h
}

func (h *FlipNormals) Hit(ray *raytrace.Ray, tMin float64, tMax float64) *raytrace.HitRecord {
	hitrec := h.hitable.Hit(ray, tMin, tMax)
	if hitrec != nil {
		return raytrace.NewHitRecord(hitrec.T(), hitrec.P(), vectorextensions.Invert(hitrec.Normal()), hitrec.UvCoords(), hitrec.Material())
	}
	return nil
}

func (h *FlipNormals) GetBoundingBox(t0 float64, t1 float64) *raytrace.AABB {
	return h.hitable.GetBoundingBox(t0, t1)
}

func (h *FlipNormals) GetPdfValue(origin mgl64.Vec3, v mgl64.Vec3) float64 {
	return 1.0
}

func (h *FlipNormals) Random(origin mgl64.Vec3) mgl64.Vec3 {
	return vectorextensions.UnitX()
}
