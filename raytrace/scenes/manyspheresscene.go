package scenes

import (
	"math/rand"

	"github.com/go-gl/mathgl/mgl64"
	"github.com/markzuber/zgotrace/raytrace"
	"github.com/markzuber/zgotrace/raytrace/cameras"
	"github.com/markzuber/zgotrace/raytrace/hitables"
	"github.com/markzuber/zgotrace/raytrace/materials"
	"github.com/markzuber/zgotrace/raytrace/textures"
	"github.com/markzuber/zgotrace/raytrace/vectorextensions"
)

func CreateManySpheresScene() raytrace.Scene {
	lightMat := materials.NewDiffuseLight(textures.NewColorTexture(raytrace.NewColorVector(15.0, 15.0, 15.0)))
	glassMat := materials.NewDialectricMaterial(1.5)

	light := hitables.NewXzRect(213, 343, 227, 332, 554, lightMat)
	glassSphere := hitables.NewSphere(mgl64.Vec3{190, 90, 190}, 90, glassMat)

	var s = &manySpheresScene{light, glassSphere}
	return s
}

type manySpheresScene struct {
	light       raytrace.Hitable
	glassSphere raytrace.Hitable
}

func (s *manySpheresScene) GetCamera(width int, height int) raytrace.Camera {
	lookFrom := mgl64.Vec3{24.0, 2.0, 6.0}
	lookAt := vectorextensions.UnitY()
	distToFocus := (lookFrom.Sub(lookAt)).Len()
	aperture := 0.01

	return cameras.NewNormalCamera(
		lookFrom,
		lookAt,
		vectorextensions.UnitY(),
		15.0,
		float64(width)/float64(height),
		aperture,
		distToFocus)
}

func (s *manySpheresScene) GetWorld() raytrace.Hitable {

	checkerTex := textures.NewCheckerTexture(
		textures.NewColorTexture(raytrace.NewColorVector(0.2, 0.3, 0.1)),
		textures.NewColorTexture(raytrace.NewColorVector(0.9, 0.9, 0.9)),
		mgl64.Vec3{10.0, 10.0, 10.0})

	list := []raytrace.Hitable{}

	list = append(list, hitables.NewSphere(mgl64.Vec3{0.0, -1000.0, 0.0}, 1000.0, materials.NewLambertianMaterial(checkerTex)))

	for a := -11; a < 11; a++ {
		for b := -11; b < 11; b++ {
			chooseMat := rand.Float64()
			center := mgl64.Vec3{float64(a) * rand.Float64(), 0.2, float64(b) + (0.9 * rand.Float64())}

			if center.Sub(mgl64.Vec3{4.0, 0.2, 0.0}).Len() > 0.9 {
				if chooseMat < 0.8 {
					// diffuse
					list = append(list, hitables.NewSphere(center, 0.2, materials.NewLambertianMaterial(textures.NewColorTexture(raytrace.NewColorVector(rand.Float64()*rand.Float64(), rand.Float64()*rand.Float64(), rand.Float64()*rand.Float64())))))
				} else if chooseMat < 0.95 {
					// metal
					list = append(list, hitables.NewSphere(center, 0.2, materials.NewMetalMaterial(raytrace.NewColorVector(0.5*(1.0+rand.Float64()), 0.5*(1.0+rand.Float64()), 0.5*(1.0+rand.Float64())), 0.5*rand.Float64())))
				} else {
					// glass
					list = append(list, hitables.NewSphere(center, 0.2, materials.NewDialectricMaterial(1.5)))
				}
			}
		}
	}

	list = append(list, hitables.NewSphere(mgl64.Vec3{0.0, 1.0, 0.0}, 1.0, materials.NewDialectricMaterial(1.5)))
	list = append(list, hitables.NewSphere(mgl64.Vec3{-4.0, 1.0, 0.0}, 1.0, materials.NewLambertianMaterial(textures.NewColorTexture(raytrace.NewColorVector(0.4, 0.2, 0.1)))))
	list = append(list, hitables.NewSphere(mgl64.Vec3{4.0, 1.0, 0.0}, 1.0, materials.NewMetalMaterial(raytrace.NewColorVector(0.7, 0.6, 0.5), 0.0)))

	return hitables.NewBvhNode(list, 0.0, 1.0)
}

func (s *manySpheresScene) GetLightHitable() raytrace.Hitable {
	return hitables.NewHitableList([]raytrace.Hitable{})
}

func (s *manySpheresScene) GetBackgroundFunc() raytrace.BackgroundFunc {
	return func(ray *raytrace.Ray) raytrace.ColorVector {
		return raytrace.NewColorVector(0.1, 0.1, 0.1)
	}
}
