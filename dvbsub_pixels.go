package astisub

import (
	"image"
	"image/color"
	"image/draw"
)

// dvbObject is a decoded object data segment kept in the decoder cache.
// idx stores region colour indexes (one per pixel) while skip marks the
// non-modifying pixels that must not overwrite the region plane.
type dvbObject struct {
	version uint8
	w, h    int
	idx     []uint8
	skip    []bool
}

// dvbRegionRef is one object reference carried by a region composition segment.
type dvbRegionRef struct {
	objectID      uint16
	typ, provider uint8
	x, y          int
}

// dvbRegion is the persistent indexed plane of one on-screen region.
type dvbRegion struct {
	version uint8
	w, h    int
	depth   uint8
	clutID  uint8
	bg      uint8
	refs    []dvbRegionRef
	plane   []uint8
}

// dvbCLUTEntry is one colour lookup table entry.
type dvbCLUTEntry struct {
	y, cr, cb, t uint8
	defined      bool
}

// dvbCLUT groups the entries of a colour lookup table.
type dvbCLUT struct {
	version uint8
	entries map[uint8]dvbCLUTEntry
}

// dvbObjectRow accumulates one decoded object field line.
type dvbObjectRow struct {
	idx  []uint8
	skip []bool
}

// decodeDVBObjectField decodes one interlaced field (top or bottom) into rows
// keyed by absolute plane row. The top field starts on row 0, the bottom field
// on row 1, and every 0xf0 new-line marker moves two rows down.
func decodeDVBObjectField(data []byte, nonmod bool, startY int) (map[int]*dvbObjectRow, error) {
	rows := make(map[int]*dvbObjectRow)
	x, y := 0, startY
	i := 0
	for i < len(data) {
		switch data[i] {
		case dvbSubBlock8Bit:
			i++
			for i < len(data) {
				b := data[i]
				i++
				if b != 0 {
					// A literal pixel code; 0x01 is non-modifying in nonmod mode.
					putDVBObjectPixel(rows, x, y, b, nonmod && b == 1)
					x++
					continue
				}
				if i >= len(data) {
					return nil, dvbError("object pixel string truncated after zero byte")
				}
				ctrl := data[i]
				i++
				if ctrl == 0 {
					break // end of pixel string
				}
				count := int(ctrl & 0x7f)
				if ctrl&0x80 == 0 {
					// Count background (index 0) pixels.
					for k := 0; k < count; k++ {
						putDVBObjectPixel(rows, x, y, 0, false)
						x++
					}
					continue
				}
				if i >= len(data) {
					return nil, dvbError("object pixel string truncated in colour run")
				}
				col := data[i]
				i++
				noWrite := nonmod && col == 1
				for k := 0; k < count; k++ {
					putDVBObjectPixel(rows, x, y, col, noWrite)
					x++
				}
			}
		case dvbSubBlockNewLine:
			i++
			x = 0
			y += 2
		default:
			return nil, dvbError("unsupported object pixel sub-block marker 0x%02x", data[i])
		}
	}
	return rows, nil
}

func putDVBObjectPixel(rows map[int]*dvbObjectRow, x, y int, idx uint8, skip bool) {
	row := rows[y]
	if row == nil {
		row = &dvbObjectRow{}
		rows[y] = row
	}
	if len(row.idx) <= x {
		grow := x + 1 - len(row.idx)
		row.idx = append(row.idx, make([]uint8, grow)...)
		row.skip = append(row.skip, make([]bool, grow)...)
	}
	row.idx[x] = idx
	row.skip[x] = skip
}

// decodeDVBObject turns the top/bottom field byte strings of an object data
// segment into a flat index plane. A missing bottom field reuses the top field
// rows for the odd lines.
func decodeDVBObject(top, bottom []byte, nonmod bool) (*dvbObject, error) {
	topRows, err := decodeDVBObjectField(top, nonmod, 0)
	if err != nil {
		return nil, err
	}
	if len(topRows) == 0 {
		return nil, dvbError("object contains no pixel data")
	}
	var bottomRows map[int]*dvbObjectRow
	if len(bottom) > 0 {
		bottomRows, err = decodeDVBObjectField(bottom, nonmod, 1)
		if err != nil {
			return nil, err
		}
	} else {
		// bottomLen == 0: the bottom field is a copy of the top field.
		bottomRows = make(map[int]*dvbObjectRow)
		for y, row := range topRows {
			bottomRows[y+1] = &dvbObjectRow{
				idx:  append([]uint8(nil), row.idx...),
				skip: append([]bool(nil), row.skip...),
			}
		}
	}

	maxY := -1
	w := 0
	for y, row := range topRows {
		if y > maxY {
			maxY = y
		}
		if len(row.idx) > w {
			w = len(row.idx)
		}
	}
	for y, row := range bottomRows {
		if y > maxY {
			maxY = y
		}
		if len(row.idx) > w {
			w = len(row.idx)
		}
	}
	if w == 0 {
		return nil, dvbError("object contains no pixel data")
	}
	h := maxY + 1

	obj := &dvbObject{w: w, h: h, idx: make([]uint8, w*h), skip: make([]bool, w*h)}
	writeRow := func(y int, row *dvbObjectRow) {
		off := y * w
		for x := 0; x < len(row.idx); x++ {
			obj.idx[off+x] = row.idx[x]
			obj.skip[off+x] = row.skip[x]
		}
	}
	for y := 0; y < h; y++ {
		if row := topRows[y]; row != nil {
			writeRow(y, row)
		}
		if row := bottomRows[y]; row != nil {
			writeRow(y, row)
		}
	}
	return obj, nil
}

// drawObject paints an object into the region plane at (ox, oy), clipping the
// part that falls outside of the region. Non-modifying pixels keep the old
// index untouched.
func (r *dvbRegion) drawObject(obj *dvbObject, ox, oy int) {
	for y := 0; y < obj.h; y++ {
		ty := oy + y
		if ty < 0 || ty >= r.h {
			continue
		}
		for x := 0; x < obj.w; x++ {
			tx := ox + x
			if tx < 0 || tx >= r.w {
				continue
			}
			o := y*obj.w + x
			if obj.skip[o] {
				continue
			}
			r.plane[ty*r.w+tx] = obj.idx[o]
		}
	}
}

// dvbPixelColor resolves one index through a CLUT. Index 0 is fully
// transparent when the table does not define it; Y=0 always forces
// transparency with black RGB.
func dvbPixelColor(c *dvbCLUT, idx uint8) (color.NRGBA, bool) {
	if idx == 0 && (c == nil || !c.entries[0].defined) {
		return color.NRGBA{}, true
	}
	e := c.entries[idx]
	if !e.defined {
		return color.NRGBA{}, false
	}
	if e.y == 0 {
		return color.NRGBA{}, true
	}
	r, g, b := color.YCbCrToRGB(e.y, e.cb, e.cr)
	return color.NRGBA{R: r, G: g, B: b, A: 255 - e.t}, true
}

// renderRegion converts an indexed region into an NRGBA image.
func renderRegion(r *dvbRegion, c *dvbCLUT) (*image.NRGBA, error) {
	img := image.NewNRGBA(image.Rect(0, 0, r.w, r.h))
	for i, idx := range r.plane {
		col, ok := dvbPixelColor(c, idx)
		if !ok {
			return nil, dvbError("CLUT %d has no entry for index %d", r.clutID, idx)
		}
		img.Pix[4*i] = col.R
		img.Pix[4*i+1] = col.G
		img.Pix[4*i+2] = col.B
		img.Pix[4*i+3] = col.A
	}
	return img, nil
}

// composeDVBFrame draws the given region images onto a transparent frame using
// NRGBA source-over, clipping each region to the frame bounds.
func composeDVBFrame(width, height int, layers []dvbComposedLayer) *image.NRGBA {
	frame := image.NewNRGBA(image.Rect(0, 0, width, height))
	for _, layer := range layers {
		target := image.Rect(layer.x, layer.y, layer.x+layer.img.Bounds().Dx(), layer.y+layer.img.Bounds().Dy())
		target = target.Intersect(frame.Bounds())
		if target.Empty() {
			continue
		}
		source := image.Pt(target.Min.X-layer.x, target.Min.Y-layer.y)
		draw.Draw(frame, target, layer.img, source, draw.Over)
	}
	return frame
}

type dvbComposedLayer struct {
	x, y int
	img  *image.NRGBA
}

// hasVisiblePixel reports whether the image contains any non-transparent pixel.
func hasVisiblePixel(img *image.NRGBA) bool {
	for i := 3; i < len(img.Pix); i += 4 {
		if img.Pix[i] != 0 {
			return true
		}
	}
	return false
}
