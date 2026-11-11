package masktest_test

import (
	"image"
	"image/color"
	"testing"

	. "github.com/fogleman/gg"
)

// subMask carves a mask out of a larger parent image. Every parent byte
// outside the sub-rectangle is set to 77 so tests can detect writes that
// escape the mask's active region. v is called with absolute (parent)
// coordinates for each pixel inside the sub-rectangle.
func subMask(parent, sub image.Rectangle, v func(x, y int) uint8) (*image.Alpha, *image.Alpha) {
	p := image.NewAlpha(parent)
	for i := range p.Pix {
		p.Pix[i] = 77
	}
	m := p.SubImage(sub).(*image.Alpha)
	for y := sub.Min.Y; y < sub.Max.Y; y++ {
		for x := sub.Min.X; x < sub.Max.X; x++ {
			m.SetAlpha(x, y, color.Alpha{v(x, y)})
		}
	}
	return p, m
}

func solid(v uint8) func(x, y int) uint8 {
	return func(x, y int) uint8 { return v }
}

func fillRed(d *Context, w, h float64) {
	d.SetRGBA(1, 0, 0, 1)
	d.DrawRectangle(0, 0, w, h)
	d.Fill()
}

func wantPixel(t *testing.T, d *Context, x, y int, want color.RGBA) {
	t.Helper()
	got := d.Image().(*image.RGBA).RGBAAt(x, y)
	if got != want {
		t.Fatalf("pixel (%d,%d) = %v, want %v", x, y, got, want)
	}
}

var (
	opaqueRed = color.RGBA{255, 0, 0, 255}
	blank     = color.RGBA{0, 0, 0, 0}
)

// A mask whose Bounds.Min is negative in both axes must still have its
// top-left corner treated as device pixel (0,0).
func TestNegativeOriginMask(t *testing.T) {
	_, m := subMask(image.Rect(-9, -7, -1, -1), image.Rect(-6, -5, -4, -3), solid(255))
	d := NewContext(2, 2)
	if err := d.SetMask(m); err != nil {
		t.Fatal(err)
	}
	fillRed(d, 2, 2)
	for y := 0; y < 2; y++ {
		for x := 0; x < 2; x++ {
			wantPixel(t, d, x, y, opaqueRed)
		}
	}
}

// Non-trivial alpha values in a shifted sub-image must modulate the fill
// proportionally, using logical (device) coordinates.
func TestMaskAlphaValues(t *testing.T) {
	vals := map[image.Point]uint8{{4, 4}: 0, {5, 4}: 128, {4, 5}: 255, {5, 5}: 64}
	_, m := subMask(image.Rect(3, 3, 9, 9), image.Rect(4, 4, 6, 6),
		func(x, y int) uint8 { return vals[image.Point{x, y}] })
	d := NewContext(2, 2)
	if err := d.SetMask(m); err != nil {
		t.Fatal(err)
	}
	fillRed(d, 2, 2)
	wantPixel(t, d, 0, 0, color.RGBA{0, 0, 0, 0})
	wantPixel(t, d, 1, 0, color.RGBA{128, 0, 0, 128})
	wantPixel(t, d, 0, 1, color.RGBA{255, 0, 0, 255})
	wantPixel(t, d, 1, 1, color.RGBA{64, 0, 0, 64})
}

// The mask must be honored through the stroke path as well.
func TestStrokeWithSubMask(t *testing.T) {
	_, m := subMask(image.Rect(5, 5, 13, 13), image.Rect(6, 6, 10, 10),
		func(x, y int) uint8 {
			if x == 6 { // device column 0
				return 0
			}
			return 255
		})
	d := NewContext(4, 4)
	if err := d.SetMask(m); err != nil {
		t.Fatal(err)
	}
	d.SetRGBA(1, 0, 0, 1)
	d.SetLineCapButt()
	d.SetLineWidth(2)
	d.DrawLine(0, 2, 4, 2)
	d.Stroke()
	for _, y := range []int{1, 2} {
		wantPixel(t, d, 0, y, blank) // masked out
		for _, x := range []int{1, 2, 3} {
			wantPixel(t, d, x, y, opaqueRed)
		}
	}
	for _, y := range []int{0, 3} {
		for x := 0; x < 4; x++ {
			wantPixel(t, d, x, y, blank)
		}
	}
}

// The mask must be honored through the image drawing path.
func TestDrawImageWithSubMask(t *testing.T) {
	_, m := subMask(image.Rect(10, 10, 16, 16), image.Rect(11, 11, 13, 13),
		func(x, y int) uint8 {
			if x == y { // device (0,0) and (1,1)
				return 255
			}
			return 0
		})
	src := image.NewRGBA(image.Rect(0, 0, 2, 2))
	for i := 0; i < len(src.Pix); i += 4 {
		src.Pix[i+0] = 255
		src.Pix[i+3] = 255
	}
	d := NewContext(2, 2)
	if err := d.SetMask(m); err != nil {
		t.Fatal(err)
	}
	d.DrawImage(src, 0, 0)
	wantPixel(t, d, 0, 0, opaqueRed)
	wantPixel(t, d, 1, 1, opaqueRed)
	wantPixel(t, d, 1, 0, blank)
	wantPixel(t, d, 0, 1, blank)
}

// Clip rasterizes the path in device space (after the current transform) and
// intersects it with the borrowed mask; resetting the transform afterwards
// must not move the mask.
func TestClipAfterTransform(t *testing.T) {
	_, m := subMask(image.Rect(2, 8, 12, 18), image.Rect(4, 10, 8, 14), solid(255))
	d := NewContext(4, 4)
	if err := d.SetMask(m); err != nil {
		t.Fatal(err)
	}
	d.Translate(1, 1)
	d.DrawRectangle(0, 0, 2, 2)
	d.Clip()
	d.Identity()
	fillRed(d, 4, 4)
	for y := 0; y < 4; y++ {
		for x := 0; x < 4; x++ {
			want := blank
			if x >= 1 && x <= 2 && y >= 1 && y <= 2 {
				want = opaqueRed
			}
			wantPixel(t, d, x, y, want)
		}
	}
}

// A size mismatch must return an error and keep the previous mask; drawing
// must continue to work afterwards.
func TestSetMaskErrorKeepsMask(t *testing.T) {
	_, m := subMask(image.Rect(0, 0, 6, 6), image.Rect(2, 2, 4, 4), solid(255))
	d := NewContext(2, 2)
	if err := d.SetMask(m); err != nil {
		t.Fatal(err)
	}
	if err := d.SetMask(image.NewAlpha(image.Rect(0, 0, 3, 3))); err == nil {
		t.Fatal("expected error for mismatched mask size")
	}
	fillRed(d, 2, 2)
	for y := 0; y < 2; y++ {
		for x := 0; x < 2; x++ {
			wantPixel(t, d, x, y, opaqueRed)
		}
	}

	// with no prior mask, a failed SetMask leaves the context unclipped
	d2 := NewContext(2, 2)
	if err := d2.SetMask(image.NewAlpha(image.Rect(0, 0, 5, 5))); err == nil {
		t.Fatal("expected error for mismatched mask size")
	}
	fillRed(d2, 2, 2)
	for y := 0; y < 2; y++ {
		for x := 0; x < 2; x++ {
			wantPixel(t, d2, x, y, opaqueRed)
		}
	}
}

// Inverting a borrowed sub-image mask twice must restore the active bytes
// exactly and never touch the surrounding parent bytes.
func TestInvertRoundTrip(t *testing.T) {
	vals := []uint8{10, 20, 30, 40}
	p, m := subMask(image.Rect(10, 20, 16, 24), image.Rect(12, 21, 14, 23),
		func(x, y int) uint8 { return vals[(y-21)*2+(x-12)] })
	before := append([]byte(nil), p.Pix...)
	d := NewContext(2, 2)
	if err := d.SetMask(m); err != nil {
		t.Fatal(err)
	}
	d.InvertMask()
	d.InvertMask()
	for i, v := range p.Pix {
		if v != before[i] {
			t.Fatalf("parent byte %d changed after two inverts: %d -> %d", i, before[i], v)
		}
	}
}

// Inverting and then clipping must intersect against the inverted mask.
func TestInvertThenClip(t *testing.T) {
	_, m := subMask(image.Rect(0, 0, 6, 6), image.Rect(2, 2, 4, 4),
		func(x, y int) uint8 {
			if x == 3 && y == 3 { // device (1,1)
				return 0
			}
			return 255
		})
	d := NewContext(2, 2)
	if err := d.SetMask(m); err != nil {
		t.Fatal(err)
	}
	d.InvertMask() // only device (1,1) is opaque now
	d.DrawRectangle(0, 0, 1, 2)
	d.Clip() // left column ∩ {(1,1)} = empty
	fillRed(d, 2, 2)
	for y := 0; y < 2; y++ {
		for x := 0; x < 2; x++ {
			wantPixel(t, d, x, y, blank)
		}
	}
}

// SetMask borrows the caller's mask: edits made after SetMask must be
// visible to the next draw.
func TestMaskBorrowedNotCopied(t *testing.T) {
	_, m := subMask(image.Rect(0, 0, 6, 6), image.Rect(2, 2, 4, 4), solid(255))
	d := NewContext(2, 2)
	if err := d.SetMask(m); err != nil {
		t.Fatal(err)
	}
	m.SetAlpha(3, 2, color.Alpha{0}) // device (1,0)
	fillRed(d, 2, 2)
	wantPixel(t, d, 1, 0, blank)
	wantPixel(t, d, 0, 0, opaqueRed)
	wantPixel(t, d, 0, 1, opaqueRed)
	wantPixel(t, d, 1, 1, opaqueRed)
}

// A mask produced by Clip is owned by the context: later edits to the
// originally borrowed mask must not affect it.
func TestClipMaskOwnedByContext(t *testing.T) {
	_, m := subMask(image.Rect(0, 0, 6, 6), image.Rect(2, 2, 4, 4), solid(255))
	d := NewContext(2, 2)
	if err := d.SetMask(m); err != nil {
		t.Fatal(err)
	}
	d.DrawRectangle(0, 0, 1, 2)
	d.Clip() // context-owned mask: left column only
	for y := 2; y < 4; y++ {
		for x := 2; x < 4; x++ {
			m.SetAlpha(x, y, color.Alpha{0})
		}
	}
	fillRed(d, 2, 2)
	wantPixel(t, d, 0, 0, opaqueRed)
	wantPixel(t, d, 0, 1, opaqueRed)
	wantPixel(t, d, 1, 0, blank)
	wantPixel(t, d, 1, 1, blank)
}

// ResetClip must restore unclipped drawing.
func TestResetClipAfterSubMask(t *testing.T) {
	_, m := subMask(image.Rect(0, 0, 6, 6), image.Rect(2, 2, 4, 4), solid(0))
	d := NewContext(2, 2)
	if err := d.SetMask(m); err != nil {
		t.Fatal(err)
	}
	d.ResetClip()
	fillRed(d, 2, 2)
	for y := 0; y < 2; y++ {
		for x := 0; x < 2; x++ {
			wantPixel(t, d, x, y, opaqueRed)
		}
	}
}
