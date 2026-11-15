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
	// portal is set when the configured world belongs to a PortalScene. When
	// non-nil, getRayColor dispatches into the portal transport path; the
	// ordinary path below is untouched.
	portal *PortalScene
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
	if portalWorld, ok := world.(*portalWorld); ok {
		t.portal = portalWorld.scene
	} else {
		t.portal = nil
	}
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

			if t.portal != nil {
				color = color.Add(t.portal.portalTracer.getRayColor(r, t.portal.root, 0, t.renderConfig.maxDepth))
			} else {
				color = color.Add(t.getRayColor(r, t.world, 0))
			}
		}

		color = color.DivScalar(perPixelFloat)
	} else {
		r := t.camera.GetRay(xfloat/widthFloat, yfloat/heightFloat)
		if t.portal != nil {
			color = t.portal.portalTracer.getRayColor(r, t.portal.root, 0, t.renderConfig.maxDepth)
		} else {
			color = t.getRayColor(r, t.world, 0)
		}
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
	return shadeRayColor(ordinaryEnv{tracer: t, world: world}, ray, depth, t.renderConfig.maxDepth)
}

// rayEnvironment is everything the shading recursion needs from the space a
// ray is currently travelling through: nearest-surface queries, the light
// table used for next-event estimation, and the miss color. The ordinary
// scene supplies one fixed environment; PortalScene supplies a new one for
// every region a hop lands in.
type rayEnvironment interface {
	hit(ray *Ray, tMin float64, tMax float64) (*HitRecord, rayEnvironment)
	lights() Hitable
	background(ray *Ray) ColorVector
}

// ordinaryEnv is the rayEnvironment of a non-portal Scene.
type ordinaryEnv struct {
	tracer *MonteCarloTracer
	world  Hitable
}

func (e ordinaryEnv) hit(ray *Ray, tMin float64, tMax float64) (*HitRecord, rayEnvironment) {
	return e.world.Hit(ray, tMin, tMax), e
}

func (e ordinaryEnv) lights() Hitable {
	return e.tracer.lightHitable
}

func (e ordinaryEnv) background(ray *Ray) ColorVector {
	return e.tracer.backgroundFunc(ray)
}

// shadeRayColor holds the production emission, scattering and recursion logic
// of MonteCarloTracer. Both the ordinary path and the portal path run through
// it, so weighting, PDF clamping and depth handling can never drift apart.
func shadeRayColor(env rayEnvironment, ray *Ray, depth int, maxDepth int) ColorVector {
	hitRecord, hitEnv := env.hit(ray, portalHitTMin, math.MaxFloat64)
	if hitRecord != nil {
		emitted := hitRecord.Material().Emitted(ray, hitRecord, hitRecord.UvCoords(), hitRecord.P())

		if depth < maxDepth {
			scatterResult := hitRecord.Material().Scatter(ray, hitRecord)
			if scatterResult.IsScattered() {
				if scatterResult.IsSpecular() {
					subcolor := shadeRayColor(hitEnv, scatterResult.SpecularRay(), depth+1, maxDepth)
					specularColor := scatterResult.Attenuation().Mul(subcolor)
					return specularColor
				}

				var p Pdf
				if lightHitable := hitEnv.lights(); lightHitable != nil {
					p0 := NewHitablePdf(lightHitable, hitRecord.P())
					p = NewMixturePdf(p0, scatterResult.Pdf())
				} else {
					// A region without lights proposes directions from the
					// material PDF alone: no empty light component, no
					// mixture wrapping a nil component.
					p = scatterResult.Pdf()
				}
				scattered := NewRay(hitRecord.P(), p.Generate())
				pdfValue := p.GetValue(scattered.Direction())

				scatteringPdf := hitRecord.Material().ScatteringPdf(ray, hitRecord, scattered)
				if scatteringPdf < 0.01 {
					scatteringPdf = 0.01
				}
				if pdfValue < 0.1 {
					pdfValue = 0.1
				}

				depthRayColor := shadeRayColor(hitEnv, scattered, depth+1, maxDepth)
				recurseColor := ((scatterResult.Attenuation().Mul(depthRayColor).MulScalar(scatteringPdf)).DivScalar(pdfValue))
				emittedPlusRecurseColor := emitted.Add(recurseColor)
				return emittedPlusRecurseColor
			}
		}

		return emitted
	}

	return hitEnv.background(ray)
}
