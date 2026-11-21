package scenes

import (
	"github.com/go-gl/mathgl/mgl64"
	"github.com/markzuber/zgotrace/raytrace"
	"github.com/markzuber/zgotrace/raytrace/cameras"
	"github.com/markzuber/zgotrace/raytrace/hitables"
	"github.com/markzuber/zgotrace/raytrace/materials"
	"github.com/markzuber/zgotrace/raytrace/textures"
	"github.com/markzuber/zgotrace/raytrace/vectorextensions"
)

func CreateCornellBoxScene() raytrace.Scene {
	lightMat := materials.NewDiffuseLight(textures.NewColorTexture(raytrace.NewColorVector(15.0, 15.0, 15.0)))
	glassMat := materials.NewDialectricMaterial(1.5)

	light := hitables.NewXzRect(213, 343, 227, 332, 554, lightMat)
	glassSphere := hitables.NewSphere(mgl64.Vec3{190, 90, 190}, 90, glassMat)

	var s = &cornellBoxScene{light, glassSphere}
	return s
}

type cornellBoxScene struct {
	light       raytrace.Hitable
	glassSphere raytrace.Hitable
}

func (s *cornellBoxScene) GetCamera(width int, height int) raytrace.Camera {
	lookFrom := mgl64.Vec3{278.0, 278.0, -800.0}
	lookAt := mgl64.Vec3{278.0, 278.0, 0.0}
	distToFocus := 10.0
	aperture := 0.0

	return cameras.NewNormalCamera(
		lookFrom,
		lookAt,
		vectorextensions.UnitY(),
		40.0,
		float64(width)/float64(height),
		aperture,
		distToFocus)
}

func (s *cornellBoxScene) GetWorld() raytrace.Hitable {
	red := materials.NewLambertianMaterial(textures.NewColorTexture(raytrace.NewColorVector(0.65, 0.05, 0.05)))
	white := materials.NewLambertianMaterial(textures.NewColorTexture(raytrace.NewColorVector(0.73, 0.73, 0.73)))
	green := materials.NewLambertianMaterial(textures.NewColorTexture(raytrace.NewColorVector(0.12, 0.45, 0.15)))
	// aluminum := materials.NewMetalMaterial(raytrace.NewColorVector(0.8, 0.85, 0.88), 0.0)

	list := []raytrace.Hitable{
		hitables.NewFlipNormals(hitables.NewYzRect(0.0, 555.0, 0.0, 555.0, 555.0, green)),
		hitables.NewYzRect(0.0, 555.0, 0.0, 555.0, 0.0, red),
		hitables.NewFlipNormals(s.light),
		hitables.NewFlipNormals(hitables.NewXzRect(0.0, 555.0, 0.0, 555.0, 555.0, white)),
		hitables.NewXzRect(0.0, 555.0, 0.0, 555.0, 0.0, white),
		hitables.NewFlipNormals(hitables.NewXyRect(0, 555, 0, 555, 555, white)),
		hitables.NewTranslate(hitables.NewRotateY(hitables.NewBox(mgl64.Vec3{0.0, 0.0, 0.0}, mgl64.Vec3{165.0, 330.0, 165.0}, white), 15.0), mgl64.Vec3{265.0, 0.0, 295.0}),
		s.glassSphere,
	}

	return hitables.NewBvhNode(list, 0, 1)
}

func (s *cornellBoxScene) GetLightHitable() raytrace.Hitable {
	// return hitables.NewHitableList([]raytrace.Hitable{s.light, s.glassSphere})
	return hitables.NewHitableList([]raytrace.Hitable{s.light})
}

func (s *cornellBoxScene) GetBackgroundFunc() raytrace.BackgroundFunc {
	return func(ray *raytrace.Ray) raytrace.ColorVector {
		return raytrace.NewColorVector(0.3, 0.3, 0.3)
		// return raytrace.NewColorVector(0.0, 0.0, 0.0)
	}
}
