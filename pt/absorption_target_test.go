package pt

import (
	"fmt"
	"math"
	"math/rand"
	"os"
	"path/filepath"
	"testing"
)

// tolerance for the epsilon-shifted origins: well under the 5e-4 budget
const absorptionTol = 5e-4

// analytic transmittance over a world-space distance, per channel
func beerColor(sigma Color, distance float64) Color {
	return Color{
		math.Exp(-sigma.R * distance),
		math.Exp(-sigma.G * distance),
		math.Exp(-sigma.B * distance),
	}
}

// deterministic path tracer used by every analytical case: one sample per
// hit, direct lighting off, enough bounces for any internal reflection series
func absorptionSampler() *DefaultSampler {
	s := NewSampler(1, 8)
	s.DirectLighting = false
	return s
}

func whiteScene(shapes ...Shape) *Scene {
	s := &Scene{Color: White}
	for _, shape := range shapes {
		s.Add(shape)
	}
	s.Compile()
	return s
}

func absorbingClear(index float64, sigma Color) Material {
	m := ClearMaterial(index, 0)
	m.Absorption = sigma
	return m
}

func closeRGB(a, b Color, tol float64) bool {
	return math.Abs(a.R-b.R) <= tol &&
		math.Abs(a.G-b.G) <= tol &&
		math.Abs(a.B-b.B) <= tol
}

func report(t *testing.T, name string, got, want Color) {
	t.Helper()
	t.Logf("%-28s got=(%.6f,%.6f,%.6f) want=(%.6f,%.6f,%.6f)",
		name, got.R, got.G, got.B, want.R, want.G, want.B)
}

// TestAbsorptionTarget validates world-distance, per-channel Beer-Lambert
// absorption integrated into the real DefaultSampler recursion.
func TestAbsorptionTarget(t *testing.T) {
	const tol = absorptionTol
	axisRay := Ray{Vector{-4, 0, 0}, Vector{1, 0, 0}}
	sigma := Color{0.2, 0.2, 0.2}

	// diameter-2 sphere: chord length 2 -> exp(-2 sigma)
	t.Run("ThinSphereDiameter2", func(t *testing.T) {
		m := absorbingClear(1, sigma)
		scene := whiteScene(NewSphere(Vector{}, 1, m))
		got := absorptionSampler().Sample(scene, axisRay, rand.New(rand.NewSource(1)))
		want := beerColor(sigma, 2)
		report(t, "thin d=2", got, want)
		if !closeRGB(got, want, tol) {
			t.Fatalf("thin sphere mismatch: %v vs %v", got, want)
		}
	})

	// diameter-4 sphere: chord length 4 -> exp(-4 sigma)
	t.Run("ThickSphereDiameter4", func(t *testing.T) {
		m := absorbingClear(1, sigma)
		scene := whiteScene(NewSphere(Vector{}, 2, m))
		got := absorptionSampler().Sample(scene, axisRay, rand.New(rand.NewSource(2)))
		want := beerColor(sigma, 4)
		report(t, "thick d=4", got, want)
		if !closeRGB(got, want, tol) {
			t.Fatalf("thick sphere mismatch: %v vs %v", got, want)
		}
	})

	// concentric regions r=2 (outer) and r=1 (inner):
	// exp(-4 sigmaOuter - 2 sigmaInner)
	t.Run("ConcentricRegions", func(t *testing.T) {
		sigmaOuter := Color{0.1, 0.1, 0.1}
		sigmaInner := Color{0.15, 0.15, 0.15}
		outer := NewSphere(Vector{}, 2, absorbingClear(1, sigmaOuter))
		inner := NewSphere(Vector{}, 1, absorbingClear(1, sigmaInner))
		scene := whiteScene(outer, inner)
		got := absorptionSampler().Sample(scene, axisRay, rand.New(rand.NewSource(3)))
		want := beerColor(sigmaOuter, 4).Mul(beerColor(sigmaInner, 2))
		report(t, "concentric", got, want)
		if !closeRGB(got, want, tol) {
			t.Fatalf("concentric mismatch: %v vs %v", got, want)
		}
	})

	// eccentric, mutually intersecting spheres with separated crossings.
	// A: center (0,0,0) r=2 ; B: center (0.5,0,0) r=1
	// crossings at t = 2, 3.5, 5.5, 6 along the ray from x=-4.
	t.Run("EccentricOverlap", func(t *testing.T) {
		sigmaA := Color{0.2, 0.2, 0.2}
		sigmaB := Color{0.15, 0.25, 0.35} // non-uniform: per-channel check
		a := NewSphere(Vector{}, 2, absorbingClear(1, sigmaA))
		b := NewSphere(Vector{0.5, 0, 0}, 1, absorbingClear(1, sigmaB))
		scene := whiteScene(a, b)
		got := absorptionSampler().Sample(scene, axisRay, rand.New(rand.NewSource(4)))
		// A spans the whole 4-unit chord; B adds 2 units of its own sigma
		want := beerColor(sigmaA, 4).Mul(beerColor(sigmaB, 2))
		report(t, "eccentric overlap", got, want)
		if !closeRGB(got, want, tol) {
			t.Fatalf("eccentric overlap mismatch: %v vs %v", got, want)
		}
	})

	// one original Sphere referenced by two transforms: distinct regions, and
	// leaving one instance must not clear the other while they overlap.
	t.Run("SharedSphereTwoInstances", func(t *testing.T) {
		original := NewSphere(Vector{}, 1, absorbingClear(1, sigma))
		first := NewTransformedShape(original, Translate(Vector{-0.75, 0, 0}))
		second := NewTransformedShape(original, Translate(Vector{0.75, 0, 0}))
		scene := whiteScene(first, second)
		got := absorptionSampler().Sample(scene, axisRay, rand.New(rand.NewSource(5)))
		// each instance contributes a full 2-unit chord of sigma
		want := beerColor(sigma, 4)
		report(t, "shared instances", got, want)
		if !closeRGB(got, want, tol) {
			t.Fatalf("shared-instance mismatch: %v vs %v (regions not isolated?)", got, want)
		}
	})

	// non-uniform scale: world-space chord is 6 units even though the local
	// sphere has radius 1 and local hit distances differ
	t.Run("NonUniformScale", func(t *testing.T) {
		scaled := NewTransformedShape(
			NewSphere(Vector{}, 1, absorbingClear(1, sigma)),
			Scale(Vector{3, 1, 1}))
		scene := whiteScene(scaled)
		ray := Ray{Vector{-8, 0, 0}, Vector{1, 0, 0}}
		got := absorptionSampler().Sample(scene, ray, rand.New(rand.NewSource(6)))
		want := beerColor(sigma, 6)
		report(t, "scaled ellipsoid", got, want)
		if !closeRGB(got, want, tol) {
			t.Fatalf("non-uniform scale mismatch: %v vs %v", got, want)
		}
	})

	// shallow, off-axis chord and a ray that grazes without entering
	t.Run("ShallowChordAndGrazing", func(t *testing.T) {
		scene := whiteScene(NewSphere(Vector{}, 1, absorbingClear(1, sigma)))
		// impact parameter 0.9 -> chord 2*sqrt(1-0.81)
		shallow := Ray{Vector{-4, 0.9, 0}, Vector{1, 0, 0}}
		got := absorptionSampler().Sample(scene, shallow, rand.New(rand.NewSource(7)))
		chord := 2 * math.Sqrt(1-0.9*0.9)
		want := beerColor(sigma, chord)
		report(t, "shallow chord", got, want)
		if !closeRGB(got, want, tol) {
			t.Fatalf("shallow chord mismatch: %v vs %v", got, want)
		}
		// impact parameter beyond the radius: no intersection, no entry
		grazing := Ray{Vector{-4, 1.0005, 0}, Vector{1, 0, 0}}
		got = absorptionSampler().Sample(scene, grazing, rand.New(rand.NewSource(8)))
		report(t, "grazing miss", got, White)
		if !closeRGB(got, White, tol) {
			t.Fatalf("grazing ray should stay outside: %v", got)
		}
	})

	// empty scene: never inherits any medium
	t.Run("EmptyScene", func(t *testing.T) {
		scene := whiteScene()
		got := absorptionSampler().Sample(scene, axisRay, rand.New(rand.NewSource(9)))
		report(t, "empty scene", got, White)
		if !closeRGB(got, White, tol) {
			t.Fatalf("empty scene mismatch: %v", got)
		}
	})

	// non-unit index: reflected branch stays outside (unattenuated) while the
	// transmitted branch accumulates absorption, including internal bounces
	t.Run("ReflectRefractIsolation", func(t *testing.T) {
		const index = 1.5
		scene := whiteScene(NewSphere(Vector{}, 1, absorbingClear(index, sigma)))
		sampler := absorptionSampler()
		sampler.SpecularMode = SpecularModeAll // force both branches at every hit
		got := sampler.Sample(scene, axisRay, rand.New(rand.NewSource(10)))

		// normal-incidence Fresnel reflectance, identical at both surfaces
		p := math.Pow((index-1)/(index+1), 2)
		q := 1 - p
		transmit := math.Exp(-sigma.R * 2) // one diameter traversal
		want := p                          // immediate reflection: no absorption
		for m := 0; m < 40; m++ {
			// escape through the back after 2m+1 diameter traversals
			want += q * q * math.Pow(p, 2*float64(m)) * math.Pow(transmit, 2*float64(m)+1)
		}
		for m := 1; m < 40; m++ {
			// internal reflection(s), escape through the front after 2m traversals
			want += q * q * math.Pow(p, 2*float64(m)-1) * math.Pow(transmit, 2*float64(m))
		}
		wantColor := Color{want, want, want}
		report(t, "reflect/refract", got, wantColor)
		if !closeRGB(got, wantColor, tol) {
			t.Fatalf("reflection/transmission isolation mismatch: %v vs %v", got, wantColor)
		}
	})

	// fixed-seed determinism and no medium inheritance across Sample calls
	t.Run("FixedSeedRepeatAndNoInheritance", func(t *testing.T) {
		scene := whiteScene(NewSphere(Vector{}, 1, absorbingClear(1, sigma)))
		sampler := absorptionSampler()
		c1 := sampler.Sample(scene, axisRay, rand.New(rand.NewSource(123)))
		c2 := sampler.Sample(scene, axisRay, rand.New(rand.NewSource(123)))
		report(t, "seeded repeat A", c1, c2)
		if c1 != c2 {
			t.Fatalf("identical seeds gave different colors: %v vs %v", c1, c2)
		}
		// a later miss must be white: media from the previous call cannot leak
		miss := Ray{Vector{-4, 5, 0}, Vector{1, 0, 0}}
		c3 := sampler.Sample(scene, miss, rand.New(rand.NewSource(999)))
		report(t, "later miss", c3, White)
		if !closeRGB(c3, White, tol) {
			t.Fatalf("medium leaked across Sample calls: %v", c3)
		}
	})

	// render a small image entirely through DefaultSampler. Every pixel is a
	// real Sample() result; nothing is precomputed when filling the buffer
	// (the analytical chord is only used afterwards to assert correctness).
	t.Run("PPMRender", func(t *testing.T) {
		const w, h = 24, 12
		ppmSigma := Color{0.25, 0.2, 0.15}
		ppmScene := whiteScene(NewSphere(Vector{}, 1, absorbingClear(1, ppmSigma)))
		ppmSampler := absorptionSampler()
		ppmRnd := rand.New(rand.NewSource(77))
		pixels := make([]Color, w*h)
		rays := make([]Ray, w*h)
		for j := 0; j < h; j++ {
			for i := 0; i < w; i++ {
				y := (float64(i)/(w-1))*2.4 - 1.2
				z := (float64(j)/(h-1))*2.4 - 1.2
				ray := Ray{Vector{-4, y, z}, Vector{1, 0, 0}}
				rays[j*w+i] = ray
				pixels[j*w+i] = ppmSampler.Sample(ppmScene, ray, ppmRnd)
			}
		}

		// the center pixel comes straight from Sample; its analytical chord is
		// derived from that same ray rather than assuming a perfect center hit
		centerIdx := (h/2)*w + w/2
		center := pixels[centerIdx]
		impact := Vector{0, rays[centerIdx].Origin.Y, rays[centerIdx].Origin.Z}.Length()
		chord := 2 * math.Sqrt(math.Max(0, 1-impact*impact))
		wantCenter := beerColor(ppmSigma, chord)
		report(t, "ppm center", center, wantCenter)
		if !closeRGB(center, wantCenter, absorptionTol) {
			t.Fatalf("ppm center not driven by Sample: %v vs %v", center, wantCenter)
		}
		corner := pixels[0]
		report(t, "ppm corner", corner, White)
		if !closeRGB(corner, White, absorptionTol) {
			t.Fatalf("ppm corner should miss the sphere: %v", corner)
		}
		if center == corner {
			t.Fatal("ppm shows no thickness variation")
		}

		path := os.Getenv("PT_ABSORPTION_PPM")
		if path == "" {
			path = filepath.Join(os.TempDir(), "absorption.ppm")
		}
		if err := writePPM(path, w, h, pixels); err != nil {
			t.Fatal(err)
		}
		fmt.Printf("ABSORPTION_PPM %s (%dx%d)\n", path, w, h)
	})
}

func writePPM(path string, w, h int, pixels []Color) error {
	f, err := os.Create(path)
	if err != nil {
		return err
	}
	defer f.Close()
	if _, err := fmt.Fprintf(f, "P6\n%d %d\n255\n", w, h); err != nil {
		return err
	}
	buf := make([]byte, 0, w*h*3)
	clamp := func(x float64) byte {
		if x <= 0 {
			return 0
		}
		if x >= 1 {
			return 255
		}
		return byte(math.Round(x * 255))
	}
	for _, c := range pixels {
		buf = append(buf, clamp(c.R), clamp(c.G), clamp(c.B))
	}
	_, err = f.Write(buf)
	return err
}
