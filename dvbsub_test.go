package astisub_test

import (
	"bytes"
	"context"
	"encoding/binary"
	"encoding/json"
	"image/color"
	"image/png"
	"os"
	"os/exec"
	"path/filepath"
	"testing"
	"time"

	"github.com/asticode/go-astisub"
	"github.com/asticode/go-astisub/task_support/taskcheck"
	"github.com/asticode/go-astits"
)

// ---- segment / PES builders (hand-written bytes, no encoder round trip) ----

func dvbSeg(typ byte, page uint16, payload []byte) []byte {
	b := []byte{0x0f, typ, byte(page >> 8), byte(page), 0x00, 0x00}
	binary.BigEndian.PutUint16(b[4:], uint16(len(payload)))
	return append(b, payload...)
}

func dvbPES(segs ...[]byte) []byte {
	b := []byte{0x20, 0x00}
	for _, s := range segs {
		b = append(b, s...)
	}
	return append(b, 0xff)
}

func pcsPayload(timeout, version, state byte, regions ...[]byte) []byte {
	p := []byte{timeout, (version << 4) | (state << 2)}
	for _, r := range regions {
		p = append(p, r...)
	}
	return p
}

func pcsRegion(id byte, x, y uint16) []byte {
	b := []byte{id, 0x00, 0x00, 0x00, 0x00, 0x00}
	binary.BigEndian.PutUint16(b[2:], x)
	binary.BigEndian.PutUint16(b[4:], y)
	return b
}

// rcsPayload builds a region composition segment. depth 3 means 8-bit.
func rcsPayload(id, version byte, fill bool, w, h uint16, clutID, bg byte, refs ...[]byte) []byte {
	flags := version << 4
	if fill {
		flags |= 0x08
	}
	p := []byte{
		id, flags,
		byte(w >> 8), byte(w), byte(h >> 8), byte(h),
		0x0c, // compatibility 0, depth 3, reserved 0
		clutID, bg, 0x00,
	}
	for _, r := range refs {
		p = append(p, r...)
	}
	return p
}

func rcsRef(objectID uint16, x, y uint16) []byte {
	return []byte{
		byte(objectID >> 8), byte(objectID),
		byte(x >> 8), byte(x),
		byte(y >> 8), byte(y),
	}
}

func clutPayload(id, version byte, entries ...[]byte) []byte {
	p := []byte{id, version << 4}
	for _, e := range entries {
		p = append(p, e...)
	}
	return p
}

// clutEntry builds one full-range 8-bit entry: id, flags, Y, Cr, Cb, T.
func clutEntry(id, y, cr, cb, t byte) []byte {
	return []byte{id, 0x21, y, cr, cb, t}
}

func odsPayload(objectID uint16, version, method byte, nonmod bool, top, bottom []byte) []byte {
	flags := version<<4 | method<<2
	if nonmod {
		flags |= 0x02
	}
	p := []byte{
		byte(objectID >> 8), byte(objectID), flags,
		byte(len(top) >> 8), byte(len(top)),
		byte(len(bottom) >> 8), byte(len(bottom)),
	}
	p = append(p, top...)
	p = append(p, bottom...)
	return p
}

func fullDisplaySet(page uint16, timeout, pcsVersion, state byte, px, py uint16,
	regionID byte, rcsVersion byte, fill bool, rw, rh uint16, clutID, bg byte,
	objectID uint16, objectVersion, method byte, nonmod bool, top, bottom []byte,
	clutEntries ...[]byte) [][]byte {
	pcs := dvbSeg(0x10, page, pcsPayload(timeout, pcsVersion, state, pcsRegion(regionID, px, py)))
	rcs := dvbSeg(0x11, page, rcsPayload(regionID, rcsVersion, fill, rw, rh, clutID, bg, rcsRef(objectID, 0, 0)))
	clut := dvbSeg(0x12, page, clutPayload(clutID, 0, clutEntries...))
	ods := dvbSeg(0x13, page, odsPayload(objectID, objectVersion, method, nonmod, top, bottom))
	eods := dvbSeg(0x80, page, nil)
	return [][]byte{pcs, rcs, clut, ods, eods}
}

func gray(y byte) color.NRGBA {
	r, g, b := color.YCbCrToRGB(y, 128, 128)
	return color.NRGBA{R: r, G: g, B: b, A: 255}
}

func pushSet(t *testing.T, d *astisub.DVBSubtitleDecoder, at time.Duration, segs ...[]byte) []astisub.BitmapSubtitle {
	t.Helper()
	out, err := d.Push(dvbPES(segs...), at)
	if err != nil {
		t.Fatalf("Push at %s failed: %v", at, err)
	}
	return out
}

// pushPES feeds an already assembled PES payload.
func pushPES(t *testing.T, d *astisub.DVBSubtitleDecoder, at time.Duration, pes []byte) []astisub.BitmapSubtitle {
	t.Helper()
	out, err := d.Push(pes, at)
	if err != nil {
		t.Fatalf("Push at %s failed: %v", at, err)
	}
	return out
}

// drain runs every PES at the given successive times and returns every closed
// interval, including the one closed by a final Flush.
func drain(t *testing.T, d *astisub.DVBSubtitleDecoder, flushAt time.Duration, peses ...[]byte) []astisub.BitmapSubtitle {
	t.Helper()
	var all []astisub.BitmapSubtitle
	for i, p := range peses {
		frames, err := d.Push(p, time.Duration(i)*time.Second)
		if err != nil {
			t.Fatalf("Push %d failed: %v", i, err)
		}
		all = append(all, frames...)
	}
	frames, err := d.Flush(flushAt)
	if err != nil {
		t.Fatalf("Flush failed: %v", err)
	}
	return append(all, frames...)
}

// TestDVBDisplaySets covers display-set commit boundaries, version handling,
// epochs, timeouts, clearing and failure rollback. Frames are only returned
// once their interval is closed.
func TestDVBDisplaySets(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		const page = uint16(1)

		// A display set split over two PES only becomes visible at EODS; the
		// first closed interval arrives with the following display set.
		d, err := astisub.NewDVBSubtitleDecoder(page, 8, 8)
		if err != nil {
			t.Fatal(err)
		}
		top := []byte{0x12, 0x02, 0x01, 0x00, 0x00}
		segs := fullDisplaySet(page, 0, 0, 1, 1, 1, 1, 0, true, 2, 2, 1, 0, 1, 0, 0, false, top, nil,
			clutEntry(1, 100, 128, 128, 0), clutEntry(2, 200, 128, 128, 0))
		first, err := d.Push(dvbPES(segs[0], segs[1], segs[2]), 0)
		c.Check(err == nil, "first half of split display set accepted: %v", err)
		c.Check(len(first) == 0, "no frame before EODS, got %d", len(first))
		second, err := d.Push(dvbPES(segs[3], segs[4]), 0)
		c.Check(err == nil, "second half of split display set accepted: %v", err)
		c.Check(len(second) == 0, "first EODS opens an interval but returns nothing yet")

		// A standalone CLUT update closes the first interval and recolours.
		clutOnly := dvbPES(
			dvbSeg(0x12, page, clutPayload(1, 1, clutEntry(2, 80, 128, 128, 0))),
			dvbSeg(0x80, page, nil),
		)
		recolored, err := d.Push(clutOnly, time.Second)
		c.Check(err == nil, "CLUT-only display set: %v", err)
		c.Check(len(recolored) == 1, "previous frame closed at new EODS")
		if len(recolored) == 1 {
			c.Check(recolored[0].StartAt == 0 && recolored[0].EndAt == time.Second, "first interval spans 0..1s")
			c.Check(recolored[0].Image.Bounds().Dx() == 8, "frame geometry")
			c.Check(recolored[0].Image.NRGBAAt(1, 1) == gray(200), "object index 2 pixel")
			c.Check(recolored[0].Image.NRGBAAt(2, 1) == gray(100), "object index 1 pixel")
		}
		flushed, err := d.Flush(2 * time.Second)
		c.Check(err == nil, "flush closes recolored frame: %v", err)
		c.Check(len(flushed) == 1, "flush returns the pending interval")
		if len(flushed) == 1 {
			c.Check(flushed[0].StartAt == time.Second && flushed[0].EndAt == 2*time.Second, "second interval spans 1..2s")
			c.Check(flushed[0].Image.NRGBAAt(1, 1) == gray(80), "new frame uses recolored CLUT")
			c.Check(flushed[0].Image.NRGBAAt(2, 1) == gray(100), "untouched CLUT entry preserved")
		}
		// Previously returned images stay frozen while later sets change colors.
		c.Check(recolored[0].Image.NRGBAAt(1, 1) == gray(200), "returned snapshot isolated from later CLUT changes")

		// Version wrap 15 -> 0 is a new version; identical versions are ignored.
		d2, _ := astisub.NewDVBSubtitleDecoder(page, 8, 8)
		base := fullDisplaySet(page, 0, 0, 1, 0, 0, 1, 0, true, 1, 2, 1, 0, 7, 15, 0, false,
			[]byte{0x12, 0x02, 0x00, 0x00}, nil,
			clutEntry(2, 200, 128, 128, 0), clutEntry(3, 90, 128, 128, 0))
		out := pushSet(t, d2, 0, base...)
		c.Check(len(out) == 0, "first display set opens without output")
		// Exact retransmission of every segment: nothing re-executes.
		out = pushSet(t, d2, time.Second, base...)
		c.Check(len(out) == 1, "retransmitted EODS closes the frame, got %d", len(out))
		c.Check(out[0].Image.NRGBAAt(0, 0) == gray(200), "same-version object retransmission ignored")
		// Version 0 after 15 must be treated as new.
		newObj := dvbPES(
			dvbSeg(0x13, page, odsPayload(7, 0, 0, false, []byte{0x12, 0x03, 0x00, 0x00}, nil)),
			dvbSeg(0x80, page, nil),
		)
		out = pushPES(t, d2, 2*time.Second, newObj)
		c.Check(len(out) == 1, "object 15->0 closes another interval, got %d", len(out))
		c.Check(out[0].Image.NRGBAAt(0, 0) == gray(200), "closed frame still shows the old object")
		// A no-fill region update keeps plane rows the new object did not draw.
		noFill := dvbPES(
			dvbSeg(0x11, page, rcsPayload(1, 1, false, 1, 2, 1, 0, rcsRef(7, 0, 0))),
			dvbSeg(0x80, page, nil),
		)
		out = pushPES(t, d2, 3*time.Second, noFill)
		c.Check(len(out) == 1 && out[0].Image.NRGBAAt(0, 0) == gray(90) && out[0].Image.NRGBAAt(0, 1) == gray(90),
			"new object visible in next interval")
		// Same-version RCS retransmission carrying fill must not run fill again.
		fillRetry := dvbPES(
			dvbSeg(0x10, page, pcsPayload(0, 0, 0, pcsRegion(1, 0, 0))),
			dvbSeg(0x11, page, rcsPayload(1, 1, true, 1, 2, 1, 0, rcsRef(7, 0, 0))),
			dvbSeg(0x80, page, nil),
		)
		out = pushPES(t, d2, 4*time.Second, fillRetry)
		c.Check(len(out) == 1 && out[0].Image.NRGBAAt(0, 1) == gray(90), "retransmitted fill does not rebuild the plane")
		flushed, err = d2.Flush(5 * time.Second)
		c.Check(err == nil && len(flushed) == 1, "final pending interval flushed")
		c.Check(flushed[0].Image.NRGBAAt(0, 1) == gray(90), "flushed frame keeps preserved pixels")

		// Timeout: frame at 0 with 1s timeout, next EODS at 3s closes it at 1s.
		d3, _ := astisub.NewDVBSubtitleDecoder(page, 8, 8)
		timed := fullDisplaySet(page, 1, 0, 1, 0, 0, 1, 0, true, 1, 1, 1, 0, 1, 0, 0, false,
			[]byte{0x12, 0x02, 0x00, 0x00}, nil, clutEntry(2, 200, 128, 128, 0))
		c.Check(len(pushSet(t, d3, 0, timed...)) == 0, "timed frame opens")
		out = pushSet(t, d3, 3*time.Second,
			dvbSeg(0x10, page, pcsPayload(1, 1, 0, pcsRegion(1, 0, 0))),
			dvbSeg(0x80, page, nil))
		c.Check(len(out) == 1 && out[0].StartAt == 0 && out[0].EndAt == time.Second, "interval cut by timeout")
		flushed, err = d3.Flush(5 * time.Second)
		c.Check(err == nil && len(flushed) == 1, "flush returns next frame")
		c.Check(flushed[0].StartAt == 3*time.Second && flushed[0].EndAt == 4*time.Second, "flush also honors timeout")
		flushed, err = d3.Flush(6 * time.Second)
		c.Check(err == nil && len(flushed) == 0, "repeated flush emits nothing")

		// Empty region list clears the screen without publishing a blank frame.
		d4, _ := astisub.NewDVBSubtitleDecoder(page, 8, 8)
		untimed := fullDisplaySet(page, 0, 0, 1, 0, 0, 1, 0, true, 1, 1, 1, 0, 1, 0, 0, false,
			[]byte{0x12, 0x02, 0x00, 0x00}, nil, clutEntry(2, 200, 128, 128, 0))
		c.Check(len(pushSet(t, d4, 0, untimed...)) == 0, "frame opens before clear")
		clear := dvbPES(
			dvbSeg(0x10, page, pcsPayload(0, 1, 0)),
			dvbSeg(0x80, page, nil),
		)
		out = pushPES(t, d4, 2*time.Second, clear)
		c.Check(len(out) == 1 && out[0].EndAt == 2*time.Second, "clearing EODS only closes the old interval")
		flushed, err = d4.Flush(3 * time.Second)
		c.Check(err == nil && len(flushed) == 0, "cleared screen has nothing to flush")

		// Epoch: a failed acquisition point rolls back and keeps the old cache.
		d5, _ := astisub.NewDVBSubtitleDecoder(page, 8, 8)
		c.Check(len(pushSet(t, d5, 0, timed...)) == 0, "pre-epoch frame opens")
		badEpoch := dvbPES(
			dvbSeg(0x10, page, pcsPayload(0, 1, 1, pcsRegion(1, 0, 0))),
			dvbSeg(0x11, page, rcsPayload(1, 0, true, 1, 1, 1, 0, rcsRef(42, 0, 0))),
			dvbSeg(0x80, page, nil),
		)
		_, err = d5.Push(badEpoch, time.Second)
		c.Check(err != nil, "missing object in epoch is an error")
		flushed, err = d5.Flush(2 * time.Second)
		c.Check(err == nil && len(flushed) == 1, "rolled-back epoch leaves previous frame usable")
		// A successful epoch really drops the old objects and CLUTs.
		goodEpoch := dvbPES(
			dvbSeg(0x10, page, pcsPayload(0, 2, 2, pcsRegion(2, 0, 0))),
			dvbSeg(0x11, page, rcsPayload(2, 0, true, 1, 1, 2, 0, rcsRef(9, 0, 0))),
			dvbSeg(0x12, page, clutPayload(2, 0, clutEntry(2, 50, 128, 128, 0))),
			dvbSeg(0x13, page, odsPayload(9, 0, 0, false, []byte{0x12, 0x02, 0x00, 0x00}, nil)),
			dvbSeg(0x80, page, nil),
		)
		out = pushPES(t, d5, 3*time.Second, goodEpoch)
		c.Check(len(out) == 0, "epoch EODS opens a new frame after the flush")
		ghost := dvbPES(
			dvbSeg(0x10, page, pcsPayload(0, 3, 0, pcsRegion(2, 0, 0))),
			dvbSeg(0x11, page, rcsPayload(2, 1, false, 1, 1, 2, 0, rcsRef(1, 0, 0))),
			dvbSeg(0x80, page, nil),
		)
		_, err = d5.Push(ghost, 4*time.Second)
		c.Check(err != nil, "object from previous epoch no longer resolves")
		flushed, err = d5.Flush(5 * time.Second)
		c.Check(err == nil && len(flushed) == 1 && flushed[0].Image.NRGBAAt(0, 0) == gray(50),
			"mode-change epoch frame survives failed later set")

		// Malformed input must error without changing decoder state.
		d6, _ := astisub.NewDVBSubtitleDecoder(page, 8, 8)
		c.Check(len(pushSet(t, d6, 0, timed...)) == 0, "frame opens before bad input")
		invalid := [][]byte{
			{0x21, 0x00, 0xff},                                                                          // bad data identifier
			{0x20, 0x00, 0x0f, 0x10, 0x00, 0x01, 0x00},                                                  // truncated header
			{0x20, 0x00, 0x0f, 0x10, 0x00, 0x01, 0x00, 0x02, 0x00, 0x0c, 0xff},                          // state 3
			dvbPES(dvbSeg(0x13, page, odsPayload(1, 0, 1, false, []byte{0x12, 0x02, 0x00, 0x00}, nil))), // compressed method
		}
		for i, raw := range invalid {
			_, err = d6.Push(raw, time.Second)
			c.Check(err != nil, "invalid input %d rejected", i)
		}
		flushed, err = d6.Flush(2 * time.Second)
		c.Check(err == nil && len(flushed) == 1, "state intact after rejected pushes")
		// Monotonic time is enforced independently of segment validity.
		d6b, _ := astisub.NewDVBSubtitleDecoder(page, 8, 8)
		pushSet(t, d6b, time.Second, timed...)
		_, err = d6b.Push(dvbPES(dvbSeg(0x80, page, nil)), 0)
		c.Check(err != nil, "backwards time rejected")
		// Flush while a display set is still open is an error.
		d7, _ := astisub.NewDVBSubtitleDecoder(page, 8, 8)
		_, err = d7.Push(dvbPES(dvbSeg(0x10, page, pcsPayload(0, 0, 1, pcsRegion(1, 0, 0)))), 0)
		c.Check(err == nil, "open display set started: %v", err)
		_, err = d7.Flush(time.Second)
		c.Check(err != nil, "flush refuses an unfinished display set")
		_, err = d7.Push(dvbPES(), 0)
		c.Check(err == nil, "empty PES while set is open accepted: %v", err)
		_, err = d7.Push(dvbPES(), time.Millisecond)
		c.Check(err != nil, "display set spanning different at rejected")

		// A same-version PCS must not swallow a genuinely new CLUT in the set.
		d8, _ := astisub.NewDVBSubtitleDecoder(page, 8, 8)
		baseSet := dvbPES(
			dvbSeg(0x10, page, pcsPayload(0, 0, 1, pcsRegion(1, 0, 0))),
			dvbSeg(0x11, page, rcsPayload(1, 0, true, 1, 1, 1, 0, rcsRef(1, 0, 0))),
			dvbSeg(0x12, page, clutPayload(1, 0, clutEntry(2, 200, 128, 128, 0))),
			dvbSeg(0x13, page, odsPayload(1, 0, 0, false, []byte{0x12, 0x02, 0x00, 0x00}, nil)),
			dvbSeg(0x80, page, nil),
		)
		c.Check(len(pushPES(t, d8, 0, baseSet)) == 0, "first set opens")
		mixedSet := dvbPES(
			dvbSeg(0x10, page, pcsPayload(0, 0, 1, pcsRegion(1, 0, 0))),
			dvbSeg(0x12, page, clutPayload(1, 1, clutEntry(2, 80, 128, 128, 0))),
			dvbSeg(0x80, page, nil),
		)
		out = pushPES(t, d8, time.Second, mixedSet)
		c.Check(len(out) == 1 && out[0].Image.NRGBAAt(0, 0) == gray(200), "closed interval keeps the pre-update snapshot")
		flushed, err = d8.Flush(2 * time.Second)
		c.Check(err == nil && len(flushed) == 1 && flushed[0].Image.NRGBAAt(0, 0) == gray(80),
			"new CLUT applied even when the PCS version repeats")

		// One Push may carry several complete display sets at the same at.
		d9, _ := astisub.NewDVBSubtitleDecoder(page, 8, 8)
		firstSet := dvbPES(
			dvbSeg(0x10, page, pcsPayload(0, 0, 1, pcsRegion(1, 0, 0))),
			dvbSeg(0x11, page, rcsPayload(1, 0, true, 1, 1, 1, 0, rcsRef(1, 0, 0))),
			dvbSeg(0x12, page, clutPayload(1, 0, clutEntry(2, 200, 128, 128, 0))),
			dvbSeg(0x13, page, odsPayload(1, 0, 0, false, []byte{0x12, 0x02, 0x00, 0x00}, nil)),
			dvbSeg(0x80, page, nil),
		)
		c.Check(len(pushPES(t, d9, 0, firstSet)) == 0, "first set opens")
		twoSets := dvbPES(
			dvbSeg(0x10, page, pcsPayload(0, 0, 1, pcsRegion(1, 0, 0))),
			dvbSeg(0x80, page, nil),
			dvbSeg(0x12, page, clutPayload(1, 1, clutEntry(2, 80, 128, 128, 0))),
			dvbSeg(0x80, page, nil),
		)
		out = pushPES(t, d9, time.Second, twoSets)
		c.Check(len(out) == 1 && out[0].EndAt == time.Second, "first set closed within a multi-set Push")
		flushed, err = d9.Flush(2 * time.Second)
		c.Check(err == nil && len(flushed) == 1 && flushed[0].Image.NRGBAAt(0, 0) == gray(80),
			"second set of the same Push recolours the frame")
		// After Flush the decoder keeps accepting Push and resumes time coding.
		c.Check(len(pushPES(t, d9, 3*time.Second, firstSet)) == 0, "Push after Flush reopens a frame")
		flushed, err = d9.Flush(4 * time.Second)
		c.Check(err == nil && len(flushed) == 1 && flushed[0].StartAt == 3*time.Second,
			"frame reopened after Flush is closed by the next Flush")
	})
}

// TestDVBIndexedPixels covers the indexed plane: interlaced fields,
// incremental updates, fill, non-modifying codes, CLUT recolouring,
// transparency, object sharing and clipping.
func TestDVBIndexedPixels(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		const page = uint16(1)
		entries := [][]byte{
			clutEntry(1, 100, 128, 128, 0),
			clutEntry(2, 200, 128, 128, 0),
			clutEntry(3, 90, 128, 128, 0),
			clutEntry(4, 40, 128, 128, 0),
			clutEntry(5, 60, 128, 128, 0),
		}

		// Hand-written 8-bit string: 0x12, index2, index1, zero, end-of-string.
		d, _ := astisub.NewDVBSubtitleDecoder(page, 4, 4)
		segs := fullDisplaySet(page, 0, 0, 1, 0, 0, 1, 0, true, 2, 2, 1, 0, 1, 0, 0, false,
			[]byte{0x12, 0x02, 0x01, 0x00, 0x00}, nil, entries...)
		got := drain(t, d, time.Second, dvbPES(segs...))
		c.Check(len(got) == 1, "hand-written string decoded into one interval")
		img := got[0].Image
		c.Check(img.NRGBAAt(0, 0) == gray(200), "top field first pixel index 2")
		c.Check(img.NRGBAAt(1, 0) == gray(100), "top field second pixel index 1")
		c.Check(img.NRGBAAt(0, 1) == gray(200), "missing bottom field reuses top field")
		c.Check(img.NRGBAAt(1, 1) == gray(100), "reused bottom field second pixel")
		c.Check(img.NRGBAAt(2, 0) == (color.NRGBA{}), "pixels outside region stay transparent")
		c.Check(img.NRGBAAt(0, 2) == (color.NRGBA{}), "rows outside object stay at background 0")

		// Explicit bottom field and 0xf0 line advances; each pixel string ends
		// before the new-line marker.
		d2, _ := astisub.NewDVBSubtitleDecoder(page, 4, 4)
		top := []byte{0x12, 0x02, 0x00, 0x00, 0xf0, 0x12, 0x03, 0x00, 0x00}
		bottom := []byte{0x12, 0x04, 0x00, 0x00, 0xf0, 0x12, 0x05, 0x00, 0x00}
		segs = fullDisplaySet(page, 0, 0, 1, 0, 0, 1, 0, true, 1, 4, 1, 0, 1, 0, 0, false, top, bottom, entries...)
		img = drain(t, d2, time.Second, dvbPES(segs...))[0].Image
		c.Check(img.NRGBAAt(0, 0) == gray(200), "top field row 0")
		c.Check(img.NRGBAAt(0, 1) == gray(40), "bottom field row 1")
		c.Check(img.NRGBAAt(0, 2) == gray(90), "top field row 2 after new-line")
		c.Check(img.NRGBAAt(0, 3) == gray(60), "bottom field row 3 after new-line")

		// Run control bytes: 0x00 + low-7 control fills background indexes,
		// 0x00 + high-bit control starts a colour run.
		d3, _ := astisub.NewDVBSubtitleDecoder(page, 8, 2)
		str := []byte{0x12, 0x02, 0x00, 0x02, 0x00, 0x82, 0x03, 0x00, 0x00} // index2, 2 bg, 2x index3
		segs = fullDisplaySet(page, 0, 0, 1, 0, 0, 1, 0, true, 5, 2, 1, 0, 1, 0, 0, false, str, nil, entries...)
		img = drain(t, d3, time.Second, dvbPES(segs...))[0].Image
		c.Check(img.NRGBAAt(0, 0) == gray(200), "literal pixel before the zero run")
		c.Check(img.NRGBAAt(1, 0) == (color.NRGBA{}) && img.NRGBAAt(2, 0) == (color.NRGBA{}), "zero run writes background index 0")
		c.Check(img.NRGBAAt(3, 0) == gray(90) && img.NRGBAAt(4, 0) == gray(90), "colour run pixels")

		// Normal (no-fill) update preserves pixels the new object does not draw.
		d4, _ := astisub.NewDVBSubtitleDecoder(page, 4, 4)
		segs = fullDisplaySet(page, 0, 0, 1, 0, 0, 1, 0, true, 2, 2, 1, 0, 1, 0, 0, false,
			[]byte{0x12, 0x02, 0x02, 0x00, 0x00}, nil, entries...)
		initial := dvbPES(segs...)
		inc := dvbPES(
			dvbSeg(0x11, page, rcsPayload(1, 1, false, 2, 2, 1, 0, rcsRef(1, 0, 0))),
			dvbSeg(0x13, page, odsPayload(1, 1, 0, false, []byte{0x12, 0x03, 0x00, 0x00}, nil)),
			dvbSeg(0x80, page, nil),
		)
		fill := dvbPES(
			dvbSeg(0x11, page, rcsPayload(1, 2, true, 2, 2, 1, 1, rcsRef(1, 0, 0))),
			dvbSeg(0x80, page, nil),
		)
		got = drain(t, d4, 3*time.Second, initial, inc, fill)
		c.Check(len(got) == 3, "three closed intervals")
		c.Check(got[0].Image.NRGBAAt(1, 0) == gray(200), "initial plane filled with index 2")
		c.Check(got[1].Image.NRGBAAt(0, 0) == gray(90) && got[1].Image.NRGBAAt(0, 1) == gray(90), "incremental object repaints its column")
		c.Check(got[1].Image.NRGBAAt(1, 0) == gray(200) && got[1].Image.NRGBAAt(1, 1) == gray(200), "undrawn column keeps old indexes")
		c.Check(got[2].Image.NRGBAAt(1, 0) == gray(100), "fill restores background index 1")
		c.Check(got[2].Image.NRGBAAt(0, 0) == gray(90), "fill then redraws referenced objects")

		// Non-modifying colour code 1 leaves old pixels in place.
		d5, _ := astisub.NewDVBSubtitleDecoder(page, 4, 2)
		segs = fullDisplaySet(page, 0, 0, 1, 0, 0, 1, 0, true, 2, 2, 1, 0, 1, 0, 0, false,
			[]byte{0x12, 0x02, 0x02, 0x00, 0x00}, nil, entries...)
		initial = dvbPES(segs...)
		nonmod := dvbPES(
			dvbSeg(0x13, page, odsPayload(1, 1, 0, true,
				[]byte{0x12, 0x00, 0x81, 0x01, 0x03, 0x00, 0x00}, nil)), // skip one, then index 3
			dvbSeg(0x80, page, nil),
		)
		got = drain(t, d5, 2*time.Second, initial, nonmod)
		c.Check(len(got) == 2, "non-modifying update closes two intervals")
		c.Check(got[0].Image.NRGBAAt(0, 0) == gray(200), "first frame carries the old pixel")
		c.Check(got[1].Image.NRGBAAt(0, 0) == gray(200), "non-modifying run keeps old pixel")
		c.Check(got[1].Image.NRGBAAt(1, 0) == gray(90), "normal code after it still writes")

		// CLUT recolouring reaches pixels already in the plane, while the closed
		// snapshot keeps its original colours.
		d6, _ := astisub.NewDVBSubtitleDecoder(page, 4, 2)
		segs = fullDisplaySet(page, 0, 0, 1, 0, 0, 1, 0, true, 1, 1, 1, 0, 1, 0, 0, false,
			[]byte{0x12, 0x02, 0x00, 0x00}, nil, entries...)
		initial = dvbPES(segs...)
		recolor := dvbPES(
			dvbSeg(0x12, page, clutPayload(1, 1, clutEntry(2, 70, 128, 128, 0))),
			dvbSeg(0x80, page, nil),
		)
		got = drain(t, d6, 2*time.Second, initial, recolor)
		c.Check(len(got) == 2, "recolor update closes two intervals")
		c.Check(got[0].Image.NRGBAAt(0, 0) == gray(200), "closed snapshot keeps original CLUT color")
		c.Check(got[1].Image.NRGBAAt(0, 0) == gray(70), "existing index recoloured by standalone CLUT set")

		// Transparency: T=255 is fully transparent; Y=0 forces transparent black.
		// A visible third pixel keeps the frame publishable.
		d7, _ := astisub.NewDVBSubtitleDecoder(page, 4, 2)
		segs = fullDisplaySet(page, 0, 0, 1, 0, 0, 1, 0, true, 3, 1, 1, 0, 1, 0, 0, false,
			[]byte{0x12, 0x02, 0x03, 0x01, 0x00, 0x00}, nil,
			clutEntry(1, 100, 128, 128, 0),
			clutEntry(2, 200, 128, 128, 255),
			clutEntry(3, 0, 200, 100, 0),
		)
		img = drain(t, d7, time.Second, dvbPES(segs...))[0].Image
		transparent := img.NRGBAAt(0, 0)
		c.Check(transparent.A == 0, "T=255 gives zero alpha")
		c.Check(img.NRGBAAt(1, 0) == (color.NRGBA{}), "Y=0 forces transparent black")
		c.Check(img.NRGBAAt(2, 0) == gray(100), "opaque pixel keeps the frame visible")

		// One object shared by two regions: independent planes and positions.
		d8, _ := astisub.NewDVBSubtitleDecoder(page, 8, 4)
		shared := dvbPES(
			dvbSeg(0x10, page, pcsPayload(0, 0, 1, pcsRegion(1, 0, 0), pcsRegion(2, 4, 0))),
			dvbSeg(0x11, page, rcsPayload(1, 0, true, 2, 2, 1, 0, rcsRef(1, 0, 0))),
			dvbSeg(0x11, page, rcsPayload(2, 0, true, 2, 2, 1, 0, rcsRef(1, 0, 0))),
			dvbSeg(0x12, page, clutPayload(1, 0, entries...)),
			dvbSeg(0x13, page, odsPayload(1, 0, 0, false, []byte{0x12, 0x02, 0x02, 0x00, 0x00}, nil)),
			dvbSeg(0x80, page, nil),
		)
		sharedUpdate := dvbPES(
			dvbSeg(0x13, page, odsPayload(1, 1, 0, false, []byte{0x12, 0x03, 0x00, 0x00}, nil)),
			dvbSeg(0x80, page, nil),
		)
		region1Fill := dvbPES(
			dvbSeg(0x11, page, rcsPayload(1, 1, true, 2, 2, 1, 0, rcsRef(1, 0, 0))),
			dvbSeg(0x80, page, nil),
		)
		got = drain(t, d8, 3*time.Second, shared, sharedUpdate, region1Fill)
		c.Check(len(got) == 3, "shared object scenario closes three intervals")
		c.Check(got[0].Image.NRGBAAt(0, 0) == gray(200) && got[0].Image.NRGBAAt(4, 0) == gray(200), "object drawn in both regions")
		c.Check(got[1].Image.NRGBAAt(0, 0) == gray(90) && got[1].Image.NRGBAAt(4, 0) == gray(90), "updated object repaints both regions")
		c.Check(got[1].Image.NRGBAAt(1, 0) == gray(200) && got[1].Image.NRGBAAt(5, 0) == gray(200), "each region keeps its own old pixels")
		c.Check(got[2].Image.NRGBAAt(1, 0) == (color.NRGBA{}), "region 1 fill clears its plane")
		c.Check(got[2].Image.NRGBAAt(5, 0) == gray(200), "region 2 plane stays independent")

		// Clipping: an object fully outside its region is cut off; a region
		// crossing the frame edge is cut to the frame; a region fully outside
		// contributes nothing.
		d9, _ := astisub.NewDVBSubtitleDecoder(page, 8, 8)
		clipped := dvbPES(
			dvbSeg(0x10, page, pcsPayload(0, 0, 1, pcsRegion(1, 6, 6))),
			dvbSeg(0x11, page, rcsPayload(1, 0, true, 4, 4, 1, 1, rcsRef(1, 0xfff, 0xfff))),
			dvbSeg(0x12, page, clutPayload(1, 0, entries...)),
			dvbSeg(0x13, page, odsPayload(1, 0, 0, false, []byte{0x12, 0x02, 0x02, 0x00, 0x00}, nil)),
			dvbSeg(0x80, page, nil),
		)
		c.Check(len(pushPES(t, d9, 0, clipped)) == 0, "clipped object display set opens")
		offScreen := dvbPES(
			dvbSeg(0x10, page, pcsPayload(0, 1, 0, pcsRegion(1, 8, 8))),
			dvbSeg(0x80, page, nil),
		)
		got = pushPES(t, d9, time.Second, offScreen)
		c.Check(len(got) == 1, "off-screen region closes the previous interval")
		c.Check(got[0].Image.NRGBAAt(7, 7) == gray(100), "region background visible while object is clipped out")
		c.Check(got[0].Image.NRGBAAt(6, 6) == gray(100), "region crossing the frame edge is clipped to it")
		flushed9, err9 := d9.Flush(2 * time.Second)
		c.Check(err9 == nil && len(flushed9) == 0, "fully off-screen region publishes nothing")

		// Unknown sub-block marker errors out instead of silently decoding.
		d10, _ := astisub.NewDVBSubtitleDecoder(page, 4, 2)
		badObj := dvbPES(
			dvbSeg(0x10, page, pcsPayload(0, 0, 1, pcsRegion(1, 0, 0))),
			dvbSeg(0x11, page, rcsPayload(1, 0, true, 2, 2, 1, 0, rcsRef(1, 0, 0))),
			dvbSeg(0x12, page, clutPayload(1, 0, entries...)),
			dvbSeg(0x13, page, odsPayload(1, 0, 0, false, []byte{0x13, 0x02, 0x00, 0x00}, nil)),
			dvbSeg(0x80, page, nil),
		)
		_, err := d10.Push(badObj, 0)
		c.Check(err != nil, "unsupported pixel sub-block rejected")
	})
}

// muxPES writes one private_stream_1 PES, with or without a PTS.
func muxPES(m *astits.Muxer, payload []byte, pts *int64) error {
	hdr := &astits.PESHeader{
		StreamID:       astits.StreamIDPrivateStream1,
		OptionalHeader: &astits.PESOptionalHeader{MarkerBits: 2},
	}
	if pts != nil {
		hdr.OptionalHeader.PTSDTSIndicator = astits.PTSDTSIndicatorOnlyPTS
		hdr.OptionalHeader.PTS = &astits.ClockReference{Base: *pts}
	}
	_, err := m.WriteData(&astits.MuxerData{PID: 256, PES: &astits.PESData{Header: hdr, Data: payload}})
	return err
}

// TestDVBTransport covers the TS entry point, PTS zeroing, skipped PES and
// pixel-identical output between the transport and incremental paths, plus the
// example program output.
func TestDVBTransport(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		const page = uint16(1)
		top := []byte{0x12, 0x02, 0x01, 0x00, 0x00}
		entries := [][]byte{
			clutEntry(1, 100, 128, 128, 0),
			clutEntry(2, 200, 128, 128, 0),
		}
		first := fullDisplaySet(page, 0, 0, 1, 1, 1, 1, 0, true, 2, 2, 1, 0, 1, 0, 0, false, top, nil, entries...)
		recolor := dvbPES(
			dvbSeg(0x12, page, clutPayload(1, 1, clutEntry(2, 80, 128, 128, 0))),
			dvbSeg(0x80, page, nil),
		)
		clear := dvbPES(
			dvbSeg(0x10, page, pcsPayload(0, 1, 0)),
			dvbSeg(0x80, page, nil),
		)

		// Mux the same payloads: a PTS-less PES must be skipped.
		var buf bytes.Buffer
		m := astits.NewMuxer(context.Background(), &buf)
		if err := m.AddElementaryStream(astits.PMTElementaryStream{
			ElementaryPID: 256, StreamType: astits.StreamTypePrivateData,
		}); err != nil {
			t.Fatal(err)
		}
		m.SetPCRPID(256)
		pts0, pts1, pts2 := int64(90000), int64(180000), int64(270000)
		c.Check(muxPES(m, dvbPES(first...), nil) == nil, "mux PTS-less PES")
		c.Check(muxPES(m, dvbPES(first...), &pts0) == nil, "mux first PTS PES")
		c.Check(muxPES(m, recolor, &pts1) == nil, "mux recolor PES")
		c.Check(muxPES(m, clear, &pts2) == nil, "mux clear PES")

		// Incremental path over the payloads of the PTS-carrying PES only,
		// using the same relative times as the transport PTS.
		inc, _ := astisub.NewDVBSubtitleDecoder(page, 8, 8)
		var incremental []astisub.BitmapSubtitle
		for i, payload := range [][]byte{dvbPES(first...), recolor, clear} {
			frames, err := inc.Push(payload, time.Duration(i)*time.Second)
			if err != nil {
				t.Fatal(err)
			}
			incremental = append(incremental, frames...)
		}
		flushed, err := inc.Flush(3 * time.Second)
		c.Check(err == nil && len(flushed) == 0, "cleared screen flushes nothing: %v", err)

		transported, err := astisub.ReadFromDVBSubtitles(&buf, astisub.DVBSubtitleOptions{
			PID: 256, PageID: page, Width: 8, Height: 8, EndAt: 3 * time.Second,
		})
		c.Check(err == nil, "transport decode: %v", err)
		c.Check(len(transported) == len(incremental) && len(transported) == 2, "two intervals through transport, got %d", len(transported))
		for i := range transported {
			c.Check(transported[i].StartAt == incremental[i].StartAt, "interval %d start matches", i)
			c.Check(transported[i].EndAt == incremental[i].EndAt, "interval %d end matches", i)
			c.Check(bytes.Equal(transported[i].Image.Pix, incremental[i].Image.Pix), "interval %d pixels match", i)
		}
		if len(transported) == 2 {
			c.Check(transported[0].StartAt == 0 && transported[0].EndAt == time.Second, "zero point is first PTS PES")
			c.Check(transported[0].Image.NRGBAAt(1, 1) == gray(200), "transport original color")
			c.Check(transported[1].Image.NRGBAAt(1, 1) == gray(80), "transport recolored")
		}

		// The shipped fixture must produce the same intervals.
		fixture, err := os.Open(filepath.Join("task_support", "fixtures", "basic.ts"))
		if err != nil {
			t.Fatal(err)
		}
		fromFixture, err := astisub.ReadFromDVBSubtitles(fixture, astisub.DVBSubtitleOptions{
			PID: 256, PageID: 1, Width: 8, Height: 8, EndAt: 4 * time.Second,
		})
		c.Check(err == nil, "fixture decode: %v", err)
		c.Check(len(fromFixture) == 2, "fixture frame count")
		if len(fromFixture) == 2 {
			c.Check(fromFixture[0].StartAt == 0 && fromFixture[0].EndAt == 2*time.Second, "fixture first interval")
			c.Check(fromFixture[1].StartAt == 2*time.Second && fromFixture[1].EndAt == 3*time.Second, "fixture second interval")
		}
		c.Check(fixture.Close() == nil, "fixture closed")

		// Backwards PTS is rejected without returning partial results.
		var back bytes.Buffer
		mb := astits.NewMuxer(context.Background(), &back)
		c.Check(mb.AddElementaryStream(astits.PMTElementaryStream{
			ElementaryPID: 256, StreamType: astits.StreamTypePrivateData,
		}) == nil, "backwards stream muxer setup")
		mb.SetPCRPID(256)
		later, earlier := int64(180000), int64(90000)
		c.Check(muxPES(mb, dvbPES(first...), &later) == nil, "mux later PTS")
		c.Check(muxPES(mb, recolor, &earlier) == nil, "mux earlier PTS")
		part, err := astisub.ReadFromDVBSubtitles(&back, astisub.DVBSubtitleOptions{
			PID: 256, PageID: page, Width: 8, Height: 8, EndAt: 3 * time.Second,
		})
		c.Check(err != nil, "backwards PTS rejected")
		c.Check(part == nil, "no partial results on transport error")

		// Unknown PID yields no intervals but no error.
		empty, err := astisub.ReadFromDVBSubtitles(bytes.NewReader(buf.Bytes()), astisub.DVBSubtitleOptions{
			PID: 999, PageID: page, Width: 8, Height: 8, EndAt: 3 * time.Second,
		})
		c.Check(err == nil && len(empty) == 0, "unselected PID gives empty output")

		// The example program must run and its outputs must read back correctly.
		outDir := t.TempDir()
		cmd := exec.Command("go", "run", "./examples/dvbsub",
			filepath.Join("task_support", "fixtures", "basic.ts"), outDir)
		cmd.Stderr = os.Stderr
		c.Check(cmd.Run() == nil, "example program runs")
		indexBytes, err := os.ReadFile(filepath.Join(outDir, "index.json"))
		c.Check(err == nil, "index.json readable: %v", err)
		var index []struct {
			File    string `json:"file"`
			StartAt int64  `json:"start_at"`
			EndAt   int64  `json:"end_at"`
		}
		c.Check(json.Unmarshal(indexBytes, &index) == nil, "index.json parses")
		c.Check(len(index) == 2, "example indexes two frames")
		if len(index) == 2 {
			c.Check(index[0].File == "frame-0001.png" && index[0].StartAt == 0 && index[0].EndAt == 2e9, "first index entry")
			c.Check(index[1].File == "frame-0002.png" && index[1].StartAt == 2e9 && index[1].EndAt == 3e9, "second index entry")
		}
		pf, err := os.Open(filepath.Join(outDir, "frame-0001.png"))
		c.Check(err == nil, "frame png readable: %v", err)
		if pf != nil {
			decoded, perr := png.Decode(pf)
			c.Check(perr == nil, "frame png decodes")
			c.Check(decoded.Bounds().Dx() == 8 && decoded.Bounds().Dy() == 8, "frame png is 8x8")
			var visible bool
			for y := 0; y < 8; y++ {
				for x := 0; x < 8; x++ {
					if _, _, _, a := decoded.At(x, y).RGBA(); a != 0 {
						visible = true
					}
				}
			}
			c.Check(visible, "frame png has visible pixels")
			c.Check(pf.Close() == nil, "png closed")
		}
	})
}
