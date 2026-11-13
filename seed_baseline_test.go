package fauxgl

import (
	"bytes"
	"fmt"
	"math"
	"testing"
)

// Only exercises existing single-frame rendering, not temporal resolve.
func seedTriangle() *Triangle {
	return NewTriangleForPoints(V(-0.83, -0.71, 0.22), V(0.77, -0.53, 0.22), V(-0.19, 0.87, 0.22))
}
func TestSeedRaster(t *testing.T) {
	dc := NewContext(24, 20)
	dc.Cull = CullNone
	dc.AlphaBlend = false
	dc.Shader = NewSolidColorShader(Identity(), Color{0.8, 0.3, 0.1, 1})
	info := dc.DrawTriangle(seedTriangle())
	if info.UpdatedPixels == 0 {
		t.Fatal("triangle did not render")
	}
	n := 0
	for i, z := range dc.DepthBuffer {
		if z != math.MaxFloat64 {
			n++
			if math.Abs(z-0.61) > 1e-12 {
				t.Fatalf("depth[%d]=%g", i, z)
			}
		}
	}
	if n < 60 {
		t.Fatalf("only %d covered pixels", n)
	}
	fmt.Printf("SEED_RASTER_OK covered=%d updates=%d\n", n, info.UpdatedPixels)
}
func TestSeedClip(t *testing.T) {
	dc := NewContext(24, 20)
	dc.Cull = CullNone
	dc.AlphaBlend = false
	dc.Shader = NewSolidColorShader(Identity(), Color{0.2, 0.6, 0.9, 1})
	tri := NewTriangleForPoints(V(-1.6, -0.67, 0.1), V(0.73, -0.42, 0.1), V(0.14, 0.81, 0.1))
	info := dc.DrawTriangle(tri)
	if info.UpdatedPixels == 0 {
		t.Fatal("clipped triangle absent")
	}
	for _, z := range dc.DepthBuffer {
		if z != math.MaxFloat64 && math.Abs(z-0.55) > 1e-12 {
			t.Fatal("bad clipped depth", z)
		}
	}
	fmt.Println("SEED_CLIP_OK homogeneous-to-screen")
}
func TestSeedParallel(t *testing.T) {
	draw := func(batch bool) *Context {
		dc := NewContext(24, 20)
		dc.Cull = CullNone
		dc.AlphaBlend = false
		dc.Shader = NewSolidColorShader(Identity(), Color{0.3, 0.7, 0.2, 1})
		a := seedTriangle()
		b := seedTriangle()
		b.Transform(Translate(V(0.12, 0.04, 0.2)))
		if batch {
			dc.DrawMesh(NewTriangleMesh([]*Triangle{a, b}))
		} else {
			dc.DrawTriangle(a)
			dc.DrawTriangle(b)
		}
		return dc
	}
	a, b := draw(false), draw(true)
	if !bytes.Equal(a.ColorBuffer.Pix, b.ColorBuffer.Pix) {
		t.Fatal("parallel color differs")
	}
	for i, z := range a.DepthBuffer {
		if z != b.DepthBuffer[i] {
			t.Fatal("parallel depth differs", i)
		}
	}
	fmt.Println("SEED_PARALLEL_OK actual-DrawMesh")
}
