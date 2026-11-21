package raytrace

type RayTracer interface {
	Configure(
		imageWidth int,
		imageHeight int,
		camera Camera,
		world Hitable,
		lightHitable Hitable,
		renderConfig *RenderConfig,
		backgroundFunc BackgroundFunc)
	GetPixelColor(x int, y int) ColorVector
}
