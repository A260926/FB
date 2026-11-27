package astisub_test

import (
	"bytes"
	"context"
	"encoding/binary"
	"image/color"
	"image/png"
	"io"
	"os"
	"path/filepath"
	"reflect"
	"strconv"
	"testing"
	"time"

	"github.com/asticode/go-astisub"
	"github.com/asticode/go-astisub/task_support/taskcheck"
	astits "github.com/asticode/go-astits"
)

// ---- Hand-written segment encoder -----------------------------------------
//
// These helpers build DVB bitmap subtitle PES payloads byte by byte so that the
// tests do not rely on the decoder's own parser for their inputs.

func dvbSeg(typ byte, page uint16, payload []byte) []byte {
	b := []byte{0x0f, typ, 0, 0, 0, 0}
	binary.BigEndian.PutUint16(b[2:], page)
	binary.BigEndian.PutUint16(b[4:], uint16(len(payload)))
	return append(b, payload...)
}

func dvbPES(segments ...[]byte) []byte {
	b := []byte{0x20, 0x00}
	for _, s := range segments {
		b = append(b, s...)
	}
	return append(b, 0xff)
}

func dvbPCS(timeout, version, state byte, regions ...[2]uint16) []byte {
	p := []byte{timeout, version<<4 | state<<2}
	for _, r := range regions {
		p = append(p, byte(r[0]), 0, byte(r[1]>>8), byte(r[1]), 0, 0)
	}
	return dvbSeg(0x10, 1, p)
}

type dvbRCSRef struct {
	id   uint16
	x, y uint16
}

func dvbRCS(id, version byte, fill bool, w, h uint16, depth byte, clut, bg byte, refs ...dvbRCSRef) []byte {
	flags := version << 4
	if fill {
		flags |= 0x08
	}
	p := []byte{id, flags, byte(w >> 8), byte(w), byte(h >> 8), byte(h), depth << 2, clut, bg, 0}
	for _, r := range refs {
		p = append(p,
			byte(r.id>>8), byte(r.id),
			byte(r.x>>8&0x0f), byte(r.x),
			byte(r.y>>8&0x0f), byte(r.y))
	}
	return dvbSeg(0x11, 1, p)
}

type dvbCLUTEntry struct {
	id           byte
	y, cr, cb, t byte
}

func dvbCLUT(id, version byte, entries ...dvbCLUTEntry) []byte {
	p := []byte{id, version << 4}
	for _, e := range entries {
		p = append(p, e.id, 0x21, e.y, e.cr, e.cb, e.t) // 8-bit + full-range
	}
	return dvbSeg(0x12, 1, p)
}

// dvbObjPixels builds a single-field object data payload.
func dvbObjPixels(id uint16, version byte, nonMod bool, top, bottom []byte) []byte {
	flags := version << 4
	if nonMod {
		flags |= 0x02
	}
	p := []byte{byte(id >> 8), byte(id), flags, byte(len(top) >> 8), byte(len(top)), byte(len(bottom) >> 8), byte(len(bottom))}
	p = append(p, top...)
	p = append(p, bottom...)
	return dvbSeg(0x13, 1, p)
}

// dvb8 builds an 8-bit pixel sub-block: one 0x12-introduced string per row,
// rows separated by 0xf0 line breaks.
func dvb8(rows ...[]byte) []byte {
	var b []byte
	for i, r := range rows {
		if i > 0 {
			b = append(b, 0xf0)
		}
		b = append(b, 0x12)
		b = append(b, r...)
		b = append(b, 0, 0)
	}
	return b
}

func dvbEODS() []byte { return dvbSeg(0x80, 1, nil) }

func dvbClutGray(version byte, values map[byte]byte) []byte {
	var entries []dvbCLUTEntry
	for id, v := range values {
		entries = append(entries, dvbCLUTEntry{id: id, y: v, cr: 128, cb: 128, t: 0})
	}
	return dvbCLUT(1, version, entries...)
}

func readPES(t *testing.T, name string) []byte {
	t.Helper()
	b, err := os.ReadFile(filepath.Join("task_support", "fixtures", name))
	if err != nil {
		t.Fatalf("reading %s: %v", name, err)
	}
	return b
}

func openFixture(t *testing.T) io.ReadCloser {
	t.Helper()
	f, err := os.Open(filepath.Join("task_support", "fixtures", "basic.ts"))
	if err != nil {
		t.Fatalf("open fixture: %v", err)
	}
	return f
}

func TestDVBDisplaySets(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// A display set split over two PES payloads commits only at EODS.
		d, err := astisub.NewDVBSubtitleDecoder(1, 8, 8)
		c.Check(err == nil, "decoder construction: %v", err)
		if err != nil {
			return
		}
		frames, err := d.Push(dvbPES(
			dvbPCS(0, 4, 1, [2]uint16{1, 0}),
			dvbRCS(1, 4, true, 4, 4, 3, 1, 0, dvbRCSRef{id: 1, x: 1, y: 0}),
		), 0)
		c.Check(err == nil, "push partial display set: %v", err)
		c.Check(len(frames) == 0, "partial display set must not publish, got %d", len(frames))
		frames, err = d.Push(dvbPES(
			dvbObjPixels(1, 4, false, dvb8([]byte{2, 2}, []byte{2, 2}), nil),
			dvbCLUT(1, 4, dvbCLUTEntry{2, 200, 128, 128, 0}),
			dvbEODS(),
		), 0)
		c.Check(err == nil, "push completing display set: %v", err)
		c.Check(len(frames) == 0, "EODS closes no visible frame on first publish")

		frames, err = d.Flush(1 * time.Second)
		c.Check(err == nil, "flush: %v", err)
		c.Check(len(frames) == 1, "flush must publish one interval, got %d", len(frames))
		if len(frames) == 1 {
			f := frames[0]
			c.Check(f.StartAt == 0 && f.EndAt == time.Second, "interval %v-%v", f.StartAt, f.EndAt)
			c.Check(f.Image.Bounds().Dx() == 8 && f.Image.Bounds().Dy() == 8, "dimensions")
			c.Check(f.Image.NRGBAAt(1, 1) == (color.NRGBA{200, 200, 200, 255}), "object pixel at (1,1): %v", f.Image.NRGBAAt(1, 1))
			c.Check(f.Image.NRGBAAt(2, 1) == (color.NRGBA{200, 200, 200, 255}), "object pixel at (2,1)")
			c.Check(f.Image.NRGBAAt(2, 3) == (color.NRGBA{200, 200, 200, 255}), "interleaved pixel at (2,3)")
			c.Check(f.Image.NRGBAAt(3, 2) == (color.NRGBA{0, 0, 0, 0}), "unwritten pixel transparent")
		}

		frames, err = d.Flush(2 * time.Second)
		c.Check(err == nil && len(frames) == 0, "second flush: frames=%d err=%v", len(frames), err)

		// Retransmitting the same PCS/RCS/ODS/CLUT versions must not execute the
		// region fill again and must not disturb the visible frame.
		d2, _ := astisub.NewDVBSubtitleDecoder(1, 8, 8)
		_, err = d2.Push(dvbPES(
			dvbPCS(0, 4, 1, [2]uint16{1, 0}),
			dvbRCS(1, 4, true, 4, 4, 3, 1, 0, dvbRCSRef{id: 1, x: 1, y: 0}),
			dvbObjPixels(1, 4, false, dvb8([]byte{2, 2}, []byte{2, 2}), nil),
			dvbCLUT(1, 4, dvbCLUTEntry{2, 200, 128, 128, 0}),
			dvbEODS(),
		), 0)
		c.Check(err == nil, "first retransmission setup: %v", err)
		_, err = d2.Push(dvbPES(
			dvbPCS(0, 4, 1, [2]uint16{1, 0}),
			dvbRCS(1, 4, true, 4, 4, 3, 1, 0, dvbRCSRef{id: 1, x: 1, y: 0}),
			dvbObjPixels(1, 4, false, dvb8([]byte{2, 2}, []byte{2, 2}), nil),
			dvbCLUT(1, 4, dvbCLUTEntry{2, 200, 128, 128, 0}),
			dvbEODS(),
		), 1*time.Second)
		c.Check(err == nil, "version retransmission: %v", err)
		frames, err = d2.Flush(2 * time.Second)
		c.Check(err == nil, "flush after retransmission: %v", err)
		if len(frames) == 1 {
			c.Check(frames[0].Image.NRGBAAt(2, 1) == (color.NRGBA{200, 200, 200, 255}), "retransmission keeps pixels")
		}

		// Version 15 -> 0 is a new version even though numerically smaller.
		d3, _ := astisub.NewDVBSubtitleDecoder(1, 8, 8)
		d3.Push(dvbPES(
			dvbPCS(0, 15, 1, [2]uint16{1, 0}),
			dvbRCS(1, 15, true, 2, 1, 3, 1, 0, dvbRCSRef{id: 1, x: 0, y: 0}),
			dvbObjPixels(1, 15, false, dvb8([]byte{2, 2}), nil),
			dvbClutGray(15, map[byte]byte{2: 100}),
			dvbEODS(),
		), 0)
		_, err = d3.Push(dvbPES(
			dvbPCS(0, 0, 0, [2]uint16{1, 0}),
			dvbObjPixels(1, 0, false, dvb8([]byte{3, 3}), nil),
			dvbClutGray(0, map[byte]byte{2: 100, 3: 60}),
			dvbEODS(),
		), 1*time.Second)
		c.Check(err == nil, "version 15->0: %v", err)
		frames, _ = d3.Flush(2 * time.Second)
		if len(frames) == 1 {
			c.Check(frames[0].Image.NRGBAAt(0, 0) == (color.NRGBA{60, 60, 60, 255}), "new version applied")
		}

		// Decreasing time is rejected; the prior state keeps working.
		_, err = d3.Push(dvbPES(dvbEODS()), 0)
		c.Check(err != nil, "decreasing time must error")

		// A malformed PES must not corrupt a legal pending display set.
		d4, _ := astisub.NewDVBSubtitleDecoder(1, 8, 8)
		_, err = d4.Push(dvbPES(dvbPCS(0, 5, 1, [2]uint16{1, 0}), dvbRCS(1, 5, true, 1, 1, 3, 1, 0, dvbRCSRef{id: 1, x: 0, y: 0})), 0)
		c.Check(err == nil, "pending display set")
		_, err = d4.Push([]byte{0x20, 0x00, 0x0f, 0x13, 0, 1, 0, 9, 0}, 0) // truncated ODS
		c.Check(err != nil, "malformed segment must error")
		frames, err = d4.Push(dvbPES(
			dvbObjPixels(1, 5, false, dvb8([]byte{2}), nil),
			dvbClutGray(5, map[byte]byte{2: 180}),
			dvbEODS(),
		), 0)
		c.Check(err == nil, "pending display set survives malformed push: %v", err)
		frames, err = d4.Flush(1 * time.Second)
		c.Check(err == nil && len(frames) == 1, "flush after recovery frames=%d err=%v", len(frames), err)

		// Unsupported region depth, object type/provider and coding method must
		// all error.
		for name, payload := range map[string][]byte{
			"depth":  dvbRCS(1, 1, true, 1, 1, 2, 1, 0, dvbRCSRef{id: 1, x: 0, y: 0}),
			"state3": dvbPES(dvbPCS(0, 1, 3), dvbEODS()),
		} {
			d5, _ := astisub.NewDVBSubtitleDecoder(1, 8, 8)
			_, err := d5.Push(dvbPES(payload), 0)
			c.Check(err != nil, "unsupported %s must error", name)
		}
		badRCS := dvbRCS(1, 1, true, 1, 1, 3, 1, 0, dvbRCSRef{id: 1, x: 0, y: 0})
		badRCS[6+12] = 0x40 // object type 1 in the first reference's high nibble
		d6, _ := astisub.NewDVBSubtitleDecoder(1, 8, 8)
		_, err = d6.Push(dvbPES(badRCS), 0)
		c.Check(err != nil, "unsupported object type must error")
		badODS := dvbObjPixels(1, 1, false, dvb8([]byte{2}), nil)
		badODS[6+2] = 0x04 // coding method 1
		_, err = d6.Push(dvbPES(badODS), 0)
		c.Check(err != nil, "unsupported coding method must error")

		// A missing object/CLUT reference fails at EODS and publishes nothing.
		d7, _ := astisub.NewDVBSubtitleDecoder(1, 8, 8)
		_, err = d7.Push(dvbPES(
			dvbPCS(0, 1, 1, [2]uint16{1, 0}),
			dvbRCS(1, 1, true, 1, 1, 3, 1, 0, dvbRCSRef{id: 99, x: 0, y: 0}),
			dvbClutGray(1, map[byte]byte{2: 200}),
			dvbEODS(),
		), 0)
		c.Check(err != nil, "missing object must error at EODS")
		_, err = d7.Push(dvbPES(dvbEODS()), 1*time.Second)
		c.Check(err == nil, "EODS after rollback: %v", err)
	})
}

func TestDVBIndexedPixels(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Real fixture: cross-PES commit, CLUT recolouring and snapshot isolation.
		d, err := astisub.NewDVBSubtitleDecoder(1, 8, 8)
		c.Check(err == nil, "decoder construction: %v", err)
		var all []astisub.BitmapSubtitle
		for i, at := range []time.Duration{0, 0, 2 * time.Second, 3 * time.Second} {
			frames, err := d.Push(readPES(t, "pes"+strconv.Itoa(i)+".bin"), at)
			c.Check(err == nil, "fixture push %d: %v", i, err)
			all = append(all, frames...)
		}
		frames, err := d.Flush(4 * time.Second)
		c.Check(err == nil, "fixture flush: %v", err)
		all = append(all, frames...)
		c.Check(len(all) == 2, "fixture intervals, got %d", len(all))
		if len(all) == 2 {
			c.Check(all[0].StartAt == 0 && all[0].EndAt == 2*time.Second, "first interval")
			c.Check(all[1].StartAt == 2*time.Second && all[1].EndAt == 3*time.Second, "second interval")
			c.Check(all[0].Image.NRGBAAt(1, 1) == (color.NRGBA{200, 200, 200, 255}), "original CLUT pixel")
			c.Check(all[1].Image.NRGBAAt(1, 1) == (color.NRGBA{80, 80, 80, 255}), "recoloured CLUT pixel")
			c.Check(all[0].Image.NRGBAAt(1, 1) != (color.NRGBA{80, 80, 80, 255}), "old snapshot isolation")
			// Mutating a delivered image must not affect the decoder output.
			all[0].Image.Pix[0] ^= 0xff
		}

		// A new CLUT version alone recolours already-placed indices, and does not
		// affect the previously delivered snapshot.
		d2, _ := astisub.NewDVBSubtitleDecoder(1, 8, 8)
		d2.Push(dvbPES(
			dvbPCS(0, 1, 1, [2]uint16{1, 0}),
			dvbRCS(1, 1, true, 1, 1, 3, 1, 0, dvbRCSRef{id: 1, x: 0, y: 0}),
			dvbObjPixels(1, 1, false, dvb8([]byte{2}), nil),
			dvbClutGray(1, map[byte]byte{2: 200}),
			dvbEODS(),
		), 0)
		first, _ := d2.Push(dvbPES(dvbClutGray(2, map[byte]byte{2: 20}), dvbEODS()), 1*time.Second)
		// EODS at 1s closes the first frame.
		c.Check(len(first) == 1, "first frame closed by next EODS, got %d", len(first))
		if len(first) == 1 {
			c.Check(first[0].StartAt == 0 && first[0].EndAt == time.Second, "first frame interval")
			c.Check(first[0].Image.NRGBAAt(0, 0) == (color.NRGBA{200, 200, 200, 255}), "first snapshot")
		}
		frames, _ = d2.Flush(2 * time.Second)
		if len(frames) == 1 {
			c.Check(frames[0].Image.NRGBAAt(0, 0) == (color.NRGBA{20, 20, 20, 255}), "CLUT-only update recolours old indices")
		}

		// Non-modifying object: index 1 keeps existing pixels; 0 clears.
		d3, _ := astisub.NewDVBSubtitleDecoder(1, 8, 8)
		d3.Push(dvbPES(
			dvbPCS(0, 1, 1, [2]uint16{1, 0}),
			dvbRCS(1, 1, true, 2, 2, 3, 1, 0, dvbRCSRef{id: 1, x: 0, y: 0}),
			dvbObjPixels(1, 1, false, dvb8([]byte{2, 2}, []byte{2, 2}), nil),
			dvbClutGray(1, map[byte]byte{2: 200, 3: 40}),
			dvbEODS(),
		), 0)
		_, err = d3.Push(dvbPES(
			dvbRCS(1, 2, false, 2, 2, 3, 1, 0, dvbRCSRef{id: 1, x: 0, y: 0}),
			dvbObjPixels(1, 2, true, dvb8([]byte{1, 3}, []byte{1, 3}), nil),
			dvbEODS(),
		), 1*time.Second)
		c.Check(err == nil, "non-modifying object: %v", err)
		frames, _ = d3.Flush(2 * time.Second)
		if len(frames) == 1 {
			im := frames[0].Image
			c.Check(im.NRGBAAt(0, 0) == (color.NRGBA{200, 200, 200, 255}), "non-mod index 1 keeps old pixel")
			c.Check(im.NRGBAAt(1, 0) == (color.NRGBA{40, 40, 40, 255}), "non-mod index 3 writes")
			c.Check(im.NRGBAAt(0, 1) == (color.NRGBA{200, 200, 200, 255}), "non-mod index 1 row 2 keeps")
			c.Check(im.NRGBAAt(1, 1) == (color.NRGBA{40, 40, 40, 255}), "non-mod index 3 row 2 writes")
		}

		// A new object version without a new RCS is redrawn at the original refs.
		d4, _ := astisub.NewDVBSubtitleDecoder(1, 8, 8)
		d4.Push(dvbPES(
			dvbPCS(0, 1, 1, [2]uint16{1, 0}),
			dvbRCS(1, 1, true, 2, 1, 3, 1, 0, dvbRCSRef{id: 1, x: 0, y: 0}),
			dvbObjPixels(1, 1, false, dvb8([]byte{2, 2}), nil),
			dvbClutGray(1, map[byte]byte{2: 200, 4: 90}),
			dvbEODS(),
		), 0)
		_, err = d4.Push(dvbPES(
			dvbObjPixels(1, 2, false, dvb8([]byte{4, 4}), nil),
			dvbEODS(),
		), 1*time.Second)
		c.Check(err == nil, "incremental object without RCS: %v", err)
		frames, _ = d4.Flush(2 * time.Second)
		if len(frames) == 1 {
			c.Check(frames[0].Image.NRGBAAt(0, 0) == (color.NRGBA{90, 90, 90, 255}), "incremental object repainted")
			c.Check(frames[0].Image.NRGBAAt(1, 0) == (color.NRGBA{90, 90, 90, 255}), "incremental object repainted 2")
		}

		// Top/bottom interleave: bottomLen=0 reuses the top field shifted by one row.
		d5, _ := astisub.NewDVBSubtitleDecoder(1, 8, 8)
		d5.Push(dvbPES(
			dvbPCS(0, 1, 1, [2]uint16{1, 0}),
			dvbRCS(1, 1, true, 4, 4, 3, 1, 0, dvbRCSRef{id: 1, x: 0, y: 0}),
			dvbObjPixels(1, 1, false, dvb8([]byte{2, 2}, []byte{2, 2}), nil),
			dvbClutGray(1, map[byte]byte{2: 200}),
			dvbEODS(),
		), 0)
		frames, _ = d5.Flush(1 * time.Second)
		if len(frames) == 1 {
			im := frames[0].Image
			for _, p := range [][2]int{{0, 0}, {1, 0}, {0, 2}, {1, 2}, {0, 1}, {1, 1}, {0, 3}, {1, 3}} {
				c.Check(im.NRGBAAt(p[0], p[1]) == (color.NRGBA{200, 200, 200, 255}), "interlace pixel %v", p)
			}
			c.Check(im.NRGBAAt(2, 0) == (color.NRGBA{0, 0, 0, 0}), "interlace untouched")
		}

		// Explicit bottom field: y starts at 1.
		d6, _ := astisub.NewDVBSubtitleDecoder(1, 8, 8)
		d6.Push(dvbPES(
			dvbPCS(0, 1, 1, [2]uint16{1, 0}),
			dvbRCS(1, 1, true, 2, 2, 3, 1, 0, dvbRCSRef{id: 1, x: 0, y: 0}),
			dvbObjPixels(1, 1, false, dvb8([]byte{2, 2}), dvb8([]byte{2, 2})),
			dvbClutGray(1, map[byte]byte{2: 200}),
			dvbEODS(),
		), 0)
		frames, _ = d6.Flush(1 * time.Second)
		if len(frames) == 1 {
			im := frames[0].Image
			c.Check(im.NRGBAAt(0, 0) == (color.NRGBA{200, 200, 200, 255}), "top row")
			c.Check(im.NRGBAAt(0, 1) == (color.NRGBA{200, 200, 200, 255}), "bottom row at y=1")
		}

		// Hand-written run-length pixel string: colour 5, then 3x colour 7, then 2x 0.
		d7, _ := astisub.NewDVBSubtitleDecoder(1, 8, 8)
		d7.Push(dvbPES(
			dvbPCS(0, 1, 1, [2]uint16{1, 0}),
			dvbRCS(1, 1, true, 6, 1, 3, 1, 0, dvbRCSRef{id: 1, x: 0, y: 0}),
			dvbObjPixels(1, 1, false, []byte{0x12, 5, 0, 0x83, 7, 0, 0x02, 0, 0}, nil),
			dvbClutGray(1, map[byte]byte{5: 200, 7: 50}),
			dvbEODS(),
		), 0)
		frames, err = d7.Flush(1 * time.Second)
		c.Check(err == nil, "run-length flush: %v", err)
		if len(frames) == 1 {
			im := frames[0].Image
			c.Check(im.NRGBAAt(0, 0) == (color.NRGBA{200, 200, 200, 255}), "run-length colour 5")
			for x := 1; x <= 3; x++ {
				c.Check(im.NRGBAAt(x, 0) == (color.NRGBA{50, 50, 50, 255}), "run-length colour 7 at %d", x)
			}
			c.Check(im.NRGBAAt(4, 0).A == 0 && im.NRGBAAt(5, 0).A == 0, "run-length zeros transparent")
		}

		// Transparency: Y=0 forces transparent, alpha=255-T.
		d8, _ := astisub.NewDVBSubtitleDecoder(1, 8, 8)
		d8.Push(dvbPES(
			dvbPCS(0, 1, 1, [2]uint16{1, 0}),
			dvbRCS(1, 1, true, 2, 1, 3, 1, 0, dvbRCSRef{id: 1, x: 0, y: 0}),
			dvbObjPixels(1, 1, false, dvb8([]byte{2, 3}), nil),
			dvbCLUT(1, 1,
				dvbCLUTEntry{2, 0, 128, 128, 0},
				dvbCLUTEntry{3, 100, 128, 128, 100},
			),
			dvbEODS(),
		), 0)
		frames, _ = d8.Flush(1 * time.Second)
		if len(frames) == 1 {
			im := frames[0].Image
			c.Check(im.NRGBAAt(0, 0) == (color.NRGBA{0, 0, 0, 0}), "Y=0 transparent")
			c.Check(im.NRGBAAt(1, 0).A == 155, "alpha 255-T, got %d", im.NRGBAAt(1, 0).A)
		}

		// Object reused by two regions: position and old pixels stay independent.
		d9, _ := astisub.NewDVBSubtitleDecoder(1, 8, 8)
		d9.Push(dvbPES(
			dvbPCS(0, 1, 1, [2]uint16{1, 0}, [2]uint16{2, 4}),
			dvbRCS(1, 1, true, 2, 2, 3, 1, 0, dvbRCSRef{id: 5, x: 0, y: 0}),
			dvbRCS(2, 1, true, 2, 2, 3, 1, 0, dvbRCSRef{id: 5, x: 0, y: 0}),
			dvbObjPixels(5, 1, false, dvb8([]byte{2, 2}, []byte{2, 2}), nil),
			dvbClutGray(1, map[byte]byte{2: 200}),
			dvbEODS(),
		), 0)
		frames, _ = d9.Flush(1 * time.Second)
		if len(frames) == 1 {
			im := frames[0].Image
			for _, p := range [][2]int{{0, 0}, {1, 0}, {0, 1}, {1, 1}} {
				c.Check(im.NRGBAAt(p[0], p[1]).A == 255, "region 1 pixel %v", p)
			}
			for _, p := range [][2]int{{4, 0}, {5, 0}, {4, 1}, {5, 1}} {
				c.Check(im.NRGBAAt(p[0], p[1]).A == 255, "region 2 pixel %v", p)
			}
			c.Check(im.NRGBAAt(2, 2).A == 0, "regions independent")
		}

		// Timeout closes a frame before the next EODS.
		d10, _ := astisub.NewDVBSubtitleDecoder(1, 8, 8)
		d10.Push(dvbPES(
			dvbPCS(1, 1, 1, [2]uint16{1, 0}),
			dvbRCS(1, 1, true, 1, 1, 3, 1, 0, dvbRCSRef{id: 1, x: 0, y: 0}),
			dvbObjPixels(1, 1, false, dvb8([]byte{2}), nil),
			dvbClutGray(1, map[byte]byte{2: 200}),
			dvbEODS(),
		), 0)
		frames, err = d10.Flush(5 * time.Second)
		c.Check(err == nil, "flush timeout: %v", err)
		if len(frames) == 1 {
			c.Check(frames[0].EndAt == 1*time.Second, "timeout clipped end, got %v", frames[0].EndAt)
			c.Check(frames[0].StartAt == 0, "timeout start")
		}

		// Empty region list is a clear: it closes the previous frame and does not
		// publish a blank interval.
		d11, _ := astisub.NewDVBSubtitleDecoder(1, 8, 8)
		d11.Push(dvbPES(
			dvbPCS(0, 1, 1, [2]uint16{1, 0}),
			dvbRCS(1, 1, true, 1, 1, 3, 1, 0, dvbRCSRef{id: 1, x: 0, y: 0}),
			dvbObjPixels(1, 1, false, dvb8([]byte{2}), nil),
			dvbClutGray(1, map[byte]byte{2: 200}),
			dvbEODS(),
		), 0)
		cleared, err := d11.Push(dvbPES(dvbPCS(0, 2, 0), dvbEODS()), 1*time.Second)
		c.Check(err == nil, "clear display set: %v", err)
		c.Check(len(cleared) == 1, "clear closes previous frame, got %d", len(cleared))
		frames, _ = d11.Flush(2 * time.Second)
		c.Check(len(frames) == 0, "clear publishes no blank interval")
	})
}

func TestDVBTransport(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		f := openFixture(t)
		defer f.Close()
		transported, err := astisub.ReadFromDVBSubtitles(f, astisub.DVBSubtitleOptions{
			PID: 256, PageID: 1, Width: 8, Height: 8, EndAt: 4 * time.Second,
		})
		c.Check(err == nil, "transport read: %v", err)
		c.Check(len(transported) == 2, "transport intervals, got %d", len(transported))

		d, _ := astisub.NewDVBSubtitleDecoder(1, 8, 8)
		var incremental []astisub.BitmapSubtitle
		for i, at := range []time.Duration{0, 0, 2 * time.Second, 3 * time.Second} {
			frames, err := d.Push(readPES(t, "pes"+strconv.Itoa(i)+".bin"), at)
			c.Check(err == nil, "incremental push %d: %v", i, err)
			incremental = append(incremental, frames...)
		}
		frames, err := d.Flush(4 * time.Second)
		c.Check(err == nil, "incremental flush: %v", err)
		incremental = append(incremental, frames...)

		c.Check(len(transported) == len(incremental), "transport/incremental count")
		for i := 0; i < len(transported) && i < len(incremental); i++ {
			c.Check(transported[i].StartAt == incremental[i].StartAt && transported[i].EndAt == incremental[i].EndAt, "interval %d", i)
			c.Check(reflect.DeepEqual(transported[i].Image.Pix, incremental[i].Image.Pix), "pixel equality %d", i)
		}

		// A muxed stream where a PES without PTS is skipped, and where two PES
		// bytes of one display set share a PTS.
		var buf bytes.Buffer
		m := astits.NewMuxer(context.Background(), &buf)
		c.Check(m.AddElementaryStream(astits.PMTElementaryStream{ElementaryPID: 300, StreamType: astits.StreamTypePrivateData}) == nil, "add stream")
		m.SetPCRPID(300)
		writePESAt := func(payload []byte, base int64, withPTS bool) {
			h := &astits.PESHeader{StreamID: astits.StreamIDPrivateStream1, OptionalHeader: &astits.PESOptionalHeader{MarkerBits: 2}}
			if withPTS {
				h.OptionalHeader.PTSDTSIndicator = astits.PTSDTSIndicatorOnlyPTS
				h.OptionalHeader.PTS = &astits.ClockReference{Base: base}
			} else {
				h.OptionalHeader.PTSDTSIndicator = astits.PTSDTSIndicatorNoPTSOrDTS
			}
			_, err := m.WriteData(&astits.MuxerData{PID: 300, PES: &astits.PESData{Header: h, Data: payload}})
			c.Check(err == nil, "mux write: %v", err)
		}
		writePESAt(dvbPES(dvbPCS(0, 1, 1, [2]uint16{1, 0}), dvbRCS(1, 1, true, 1, 1, 3, 1, 0, dvbRCSRef{id: 1, x: 0, y: 0})), 90000, true)
		writePESAt(dvbPES(dvbObjPixels(1, 1, false, dvb8([]byte{2}), nil), dvbClutGray(1, map[byte]byte{2: 200}), dvbEODS()), 0, false) // skipped: no PTS
		writePESAt(dvbPES(dvbObjPixels(1, 1, false, dvb8([]byte{2}), nil), dvbClutGray(1, map[byte]byte{2: 200}), dvbEODS()), 90000, true)
		writePESAt(dvbPES(dvbPCS(0, 2, 0), dvbEODS()), 180000, true)

		got, err := astisub.ReadFromDVBSubtitles(bytes.NewReader(buf.Bytes()), astisub.DVBSubtitleOptions{
			PID: 300, PageID: 1, Width: 4, Height: 4, EndAt: 3 * time.Second,
		})
		c.Check(err == nil, "muxed transport: %v", err)
		c.Check(len(got) == 1, "muxed transport intervals, got %d", len(got))
		if len(got) == 1 {
			c.Check(got[0].StartAt == 0 && got[0].EndAt == time.Second, "muxed interval %v-%v", got[0].StartAt, got[0].EndAt)
			c.Check(got[0].Image.NRGBAAt(0, 0) == (color.NRGBA{200, 200, 200, 255}), "muxed pixel")
		}

		// A decreasing PTS sequence must error without returning partial results.
		var bad bytes.Buffer
		mb := astits.NewMuxer(context.Background(), &bad)
		mb.AddElementaryStream(astits.PMTElementaryStream{ElementaryPID: 310, StreamType: astits.StreamTypePrivateData})
		mb.SetPCRPID(310)
		writeBad := func(payload []byte, base int64) {
			h := &astits.PESHeader{StreamID: astits.StreamIDPrivateStream1, OptionalHeader: &astits.PESOptionalHeader{MarkerBits: 2, PTSDTSIndicator: astits.PTSDTSIndicatorOnlyPTS, PTS: &astits.ClockReference{Base: base}}}
			_, err := mb.WriteData(&astits.MuxerData{PID: 310, PES: &astits.PESData{Header: h, Data: payload}})
			c.Check(err == nil, "bad mux write: %v", err)
		}
		writeBad(dvbPES(dvbPCS(0, 1, 1)), 180000)
		writeBad(dvbPES(dvbEODS()), 90000)
		_, err = astisub.ReadFromDVBSubtitles(bytes.NewReader(bad.Bytes()), astisub.DVBSubtitleOptions{
			PID: 310, PageID: 1, Width: 4, Height: 4, EndAt: 2 * time.Second,
		})
		c.Check(err != nil, "decreasing PTS must error")

		// Old interfaces keep working.
		tf := openFixture(t)
		defer tf.Close()
		_, err = astisub.ReadFromTeletext(tf, astisub.TeletextOptions{Page: 888, PID: -1})
		c.Check(err == astisub.ErrNoValidTeletextPID, "teletext still reports missing PID, got %v", err)
		sub, err := astisub.ReadFromSRT(bytes.NewReader([]byte("1\n00:00:01.000 --> 00:00:02.000\nHi\n")))
		c.Check(err == nil && len(sub.Items) == 1, "SRT unchanged")
	})
}

// TestDVBExample exercises the same library path the example uses and verifies
// the PNG round-trips.
func TestDVBExample(t *testing.T) {
	out := t.TempDir()
	f := openFixture(t)
	frames, err := astisub.ReadFromDVBSubtitles(f, astisub.DVBSubtitleOptions{PID: 256, PageID: 1, Width: 8, Height: 8, EndAt: 4 * time.Second})
	f.Close()
	if err != nil {
		t.Fatal(err)
	}
	if len(frames) == 0 {
		t.Fatal("no frames")
	}
	name := filepath.Join(out, "frame.png")
	pf, err := os.Create(name)
	if err != nil {
		t.Fatal(err)
	}
	if err := png.Encode(pf, frames[0].Image); err != nil {
		t.Fatal(err)
	}
	pf.Close()
	rf, err := os.Open(name)
	if err != nil {
		t.Fatal(err)
	}
	im, err := png.Decode(rf)
	rf.Close()
	if err != nil {
		t.Fatal(err)
	}
	if im.Bounds().Dx() != 8 || im.Bounds().Dy() != 8 {
		t.Fatal("bad PNG dimensions")
	}
}
