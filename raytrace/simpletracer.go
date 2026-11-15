package raytrace

type simpleTracer struct {
}

func NewSimpleTracer() RayTracer {
	return &simpleTracer{}
}

func (t *simpleTracer) GetPixelColor(x int, y int) ColorVector {
	return NewColorVector(0.5, 0.6, 0.7)
}

func (t *simpleTracer) Configure(
	imageWidth int,
	imageHeight int,
	camera Camera,
	world Hitable,
	lightHitable Hitable,
	renderConfig *RenderConfig,
	backgroundFunc BackgroundFunc) {

}
