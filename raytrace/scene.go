package raytrace

type BackgroundFunc func(ray *Ray) ColorVector

type Scene interface {
	GetCamera(width int, height int) Camera
	GetWorld() Hitable
	GetLightHitable() Hitable
	GetBackgroundFunc() BackgroundFunc
}

func NewScene(camera Camera, world Hitable, lighthitable Hitable, backgroundFunc BackgroundFunc) Scene {
	s := &scene{camera, world, lighthitable, backgroundFunc}
	return s
}

type scene struct {
	camera         Camera
	world          Hitable
	lighthitable   Hitable
	backgroundfunc BackgroundFunc
}

func (s *scene) GetCamera(width int, height int) Camera {
	return s.camera
}

func (s *scene) GetWorld() Hitable {
	return s.world
}

func (s *scene) GetLightHitable() Hitable {
	return s.lighthitable
}

func (s *scene) GetBackgroundFunc() BackgroundFunc {
	return s.backgroundfunc
}
