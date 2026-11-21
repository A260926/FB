package raytrace

import (
	"math"
	"math/rand"
)

type MonteCarloTracer struct {
	imageWidth     int
	imageHeight    int
	camera         Camera
	world          Hitable
	lightHitable   Hitable
	renderConfig   *RenderConfig
	backgroundFunc BackgroundFunc
}

func NewMonteCarloTracer() RayTracer {
	return &MonteCarloTracer{}
}

func (t *MonteCarloTracer) Configure(
	imageWidth int,
	imageHeight int,
	camera Camera,
	world Hitable,
	lightHitable Hitable,
	renderConfig *RenderConfig,
	backgroundFunc BackgroundFunc) {
	t.imageWidth = imageWidth
	t.imageHeight = imageHeight
	t.camera = camera
	t.world = world
	t.lightHitable = lightHitable
	t.renderConfig = renderConfig
	t.backgroundFunc = backgroundFunc
}

func (t *MonteCarloTracer) GetPixelColor(x int, y int) ColorVector {

	// fmt.Printf("IN GetPixelColor(%v, %v)\n", x, y)
	color := NewColorVector(0, 0, 0)
	xfloat := float64(x)
	yfloat := float64(y)

	widthFloat := float64(t.imageWidth)
	heightFloat := float64(t.imageHeight)
	perPixelFloat := float64(t.renderConfig.samplesPerPixel)

	if t.renderConfig.samplesPerPixel > 1 {
		for sample := 0; sample < t.renderConfig.samplesPerPixel; sample++ {
			u := (xfloat + rand.Float64()) / widthFloat
			v := (yfloat + rand.Float64()) / heightFloat
			r := t.camera.GetRay(u, v)

			color = color.Add(t.getRayColor(r, t.world, 0))
		}

		color = color.DivScalar(perPixelFloat)
	} else {
		color = t.getRayColor(t.camera.GetRay(xfloat/widthFloat, yfloat/heightFloat), t.world, 0)
	}

	if color.R() > 1.0 || color.G() > 1.0 || color.B() > 1.0 {
		// fmt.Printf("color will need clamp: (%v, %v) -> %v\n", x, y, color)
	}

	color = color.ApplyGamma2()
	// fmt.Printf("FINAL COLOR: (%v, %v) -> %v\n", x, y, color)
	// fmt.Println("--------------------------------------------------")

	return color
}

func (t *MonteCarloTracer) getRayColor(ray *Ray, world Hitable, depth int) ColorVector {
	// spew.Dump(ray)

	hr := world.Hit(ray, 0.001, math.MaxFloat64)
	if hr != nil {
		// fmt.Printf("WE GOT A HIT!\n")
		emitted := hr.Material().Emitted(ray, hr, hr.UvCoords(), hr.P())
		// fmt.Printf("emitted: %v\n", emitted)

		if depth < t.renderConfig.maxDepth {
			scatterResult := hr.Material().Scatter(ray, hr)
			if scatterResult.IsScattered() {
				if scatterResult.IsSpecular() {

					subcolor := t.getRayColor(scatterResult.SpecularRay(), world, depth+1)
					specularColor := scatterResult.Attenuation().Mul(subcolor)
					// fmt.Printf("subcolor: %v  spec color: %v\n", subcolor, specularColor)
					return specularColor
				}
				p0 := NewHitablePdf(t.lightHitable, hr.P())
				p := NewMixturePdf(p0, scatterResult.Pdf())
				scattered := NewRay(hr.P(), p.Generate())
				pdfValue := p.GetValue(scattered.Direction())

				scatteringPdf := hr.Material().ScatteringPdf(ray, hr, scattered)
				if scatteringPdf < 0.01 {
					scatteringPdf = 0.01
				}
				if pdfValue < 0.1 {
					pdfValue = 0.1
				}

				depthRayColor := t.getRayColor(scattered, world, depth+1)
				recurseColor := ((scatterResult.Attenuation().Mul(depthRayColor).MulScalar(scatteringPdf)).DivScalar(pdfValue))
				emittedPlusRecurseColor := emitted.Add(recurseColor)
				// fmt.Printf("depthRayColor: %v recurseColor: %v  attenuation: %v  scatteringPdf: %v  pdfValue: %v emittedPlusRecurseColor: %v\n", depthRayColor, recurseColor, scatterResult.Attenuation(), scatteringPdf, pdfValue, emittedPlusRecurseColor)
				return emittedPlusRecurseColor
			}
		}

		return emitted
	}

	return t.backgroundFunc(ray)
}
