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

func CreateImageTextureScene(globeImagePath string) raytrace.Scene {
	var s = &imageTextureScene{globeImagePath}
	return s
}

type imageTextureScene struct {
	globeImagePath string
}

func (s *imageTextureScene) GetCamera(width int, height int) raytrace.Camera {
	lookFrom := mgl64.Vec3{13.0, 2.0, 3.0}
	lookAt := mgl64.Vec3{0.0, 0.0, 0.0}
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

func (s *imageTextureScene) GetWorld() raytrace.Hitable {

	globeImage := raytrace.LoadPixelBuffer(s.globeImagePath)

	list := []raytrace.Hitable{}

	list = append(list, hitables.NewSphere(mgl64.Vec3{0.0, -1000.0, 0.0}, 1000.0, materials.NewLambertianMaterial(textures.NewVectorNoiseTexture(textures.Soft, 3.0))))
	list = append(list, hitables.NewSphere(mgl64.Vec3{0.0, 2.0, 0.0}, 2.0, materials.NewLambertianMaterial(textures.NewImageTexture(globeImage))))

	return hitables.NewBvhNode(list, 0.0, 1.0)
}

func (s *imageTextureScene) GetLightHitable() raytrace.Hitable {
	return hitables.NewHitableList([]raytrace.Hitable{})
}

func (s *imageTextureScene) GetBackgroundFunc() raytrace.BackgroundFunc {
	return func(ray *raytrace.Ray) raytrace.ColorVector {
		unitDirection := vectorextensions.ToUnitVector(ray.Direction())
		t := 0.5 * (unitDirection.Y() + 1.0)
		clr := raytrace.NewColorVector(1.0, 1.0, 1.0).MulScalar(1.0 - t).Add(raytrace.NewColorVector(0.5, 0.7, 1.0).MulScalar(t)).MulScalar(0.5)
		return clr
	}
}
