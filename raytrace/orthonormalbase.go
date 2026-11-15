package raytrace

import (
	"math"

	"github.com/go-gl/mathgl/mgl64"
	"github.com/markzuber/zgotrace/raytrace/vectorextensions"
)

type OrthoNormalBase struct {
	u mgl64.Vec3
	v mgl64.Vec3
	w mgl64.Vec3
}

func OrthoNormalBaseFromW(n mgl64.Vec3) *OrthoNormalBase {
	w := vectorextensions.ToUnitVector(n)

	var a mgl64.Vec3
	if math.Abs(w.X()) > 0.9 {
		a = vectorextensions.UnitY()
	} else {
		a = vectorextensions.UnitX()
	}

	v := vectorextensions.ToUnitVector(w.Cross(a))
	u := w.Cross(v)
	return &OrthoNormalBase{u, v, w}
}

func (onb *OrthoNormalBase) Local(a mgl64.Vec3) mgl64.Vec3 {
	return vectorextensions.MulScalar(onb.u, a.X()).Add(vectorextensions.MulScalar(onb.v, a.Y())).Add(vectorextensions.MulScalar(onb.w, a.Z()))
}

func (onb *OrthoNormalBase) W() mgl64.Vec3 {
	return onb.w
}
