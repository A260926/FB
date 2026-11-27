package astisub

import (
	"fmt"
	"image"
	"image/color"
	"image/draw"
)

// dvbDecodeObjectField decodes one field (top or bottom) of an 8-bit object
// data payload into the object's pixel writes, starting at pixel row y.
func dvbDecodeObjectField(b []byte, y int, nonMod bool, writes *[]dvbPixelWrite) error {
	x := 0
	for i := 0; i < len(b); {
		c := b[i]
		if c == 0x12 {
			i++
			var pending []uint8
			for {
				if i >= len(b) {
					return fmt.Errorf("astisub: unterminated DVB 8-bit pixel string")
				}
				c := b[i]
				i++
				if c != 0 {
					pending = append(pending, c)
					continue
				}
				if i >= len(b) {
					return fmt.Errorf("astisub: truncated DVB 8-bit pixel string control")
				}
				c = b[i]
				i++
				if c == 0 {
					break
				}
				if c&0x80 == 0 {
					n := int(c & 0x7f)
					for j := 0; j < n; j++ {
						pending = append(pending, 0)
					}
				} else {
					if i >= len(b) {
						return fmt.Errorf("astisub: truncated DVB 8-bit repeat run")
					}
					v := b[i]
					i++
					n := int(c & 0x7f)
					for j := 0; j < n; j++ {
						pending = append(pending, v)
					}
				}
			}
			if err := dvbEmitPixels(&x, &y, pending, nonMod, writes); err != nil {
				return err
			}
			continue
		}
		if c == 0xf0 {
			x = 0
			y += 2
			i++
			continue
		}
		return fmt.Errorf("astisub: unsupported DVB pixel sub-block marker 0x%02x", c)
	}
	return nil
}

// dvbEmitPixels writes a decoded run of pixel colour indices. In non-modifying
// mode index 1 does not write an index.
func dvbEmitPixels(x, y *int, values []uint8, nonMod bool, writes *[]dvbPixelWrite) error {
	for _, v := range values {
		if nonMod && v == 1 {
			// Non-modifying objects leave the existing index untouched.
			*x++
			continue
		}
		*writes = append(*writes, dvbPixelWrite{x: *x, y: *y, index: int16(v)})
		*x++
	}
	return nil
}

// drawRegion applies the region's referenced objects, in order, onto its
// persistent index plane. When only is nil every reference is redrawn;
// otherwise only references whose object id is present in only are redrawn.
func (d *DVBSubtitleDecoder) drawRegion(r *dvbRegion, only map[uint16]bool) error {
	for _, ref := range r.refs {
		if only != nil && !only[ref.id] {
			continue
		}
		o, ok := d.objects[ref.id]
		if !ok {
			return fmt.Errorf("astisub: DVB region references missing object %d", ref.id)
		}
		for _, w := range o.writes {
			x := ref.x + w.x
			y := ref.y + w.y
			if x < 0 || x >= r.width || y < 0 || y >= r.height {
				continue
			}
			r.indices[y*r.width+x] = uint8(w.index)
		}
	}
	return nil
}

// validateRegionColors ensures every index used by the region's plane has a
// colour in the region's CLUT (index 0 is implicitly transparent).
func (d *DVBSubtitleDecoder) validateRegionColors(r *dvbRegion) error {
	c, ok := d.cluts[r.clutID]
	if !ok {
		return fmt.Errorf("astisub: DVB region references missing CLUT %d", r.clutID)
	}
	for _, v := range r.indices {
		if v == 0 {
			continue
		}
		if !c.entries[v].present {
			return fmt.Errorf("astisub: DVB region uses undefined CLUT %d entry %d", r.clutID, v)
		}
	}
	return nil
}

func dvbEntryColor(e dvbCLUTEntry) color.NRGBA {
	r, g, b := color.YCbCrToRGB(e.y, e.cb, e.cr)
	a := 255 - e.t
	if e.y == 0 || a == 0 {
		return color.NRGBA{}
	}
	return color.NRGBA{R: r, G: g, B: b, A: a}
}

// composeFrame composites the current page composition on a transparent frame
// using source-over (image/draw's NRGBA operator), in region list order.
func (d *DVBSubtitleDecoder) composeFrame() (*image.NRGBA, bool, error) {
	out := image.NewNRGBA(image.Rect(0, 0, d.width, d.height))
	for _, pc := range d.pcsRegions {
		r, ok := d.regions[pc.id]
		if !ok {
			return nil, false, fmt.Errorf("astisub: DVB page references missing region %d", pc.id)
		}
		c, ok := d.cluts[r.clutID]
		if !ok {
			return nil, false, fmt.Errorf("astisub: DVB region references missing CLUT %d", r.clutID)
		}
		var palette [dvbMaxObjectEntryCount]color.NRGBA
		for i := 1; i < len(palette); i++ {
			if c.entries[i].present {
				palette[i] = dvbEntryColor(c.entries[i])
			}
		}
		layer := image.NewNRGBA(image.Rect(0, 0, r.width, r.height))
		for i, v := range r.indices {
			if v == 0 {
				continue // index 0 is implicitly transparent
			}
			col := palette[v]
			if col.A == 0 {
				continue
			}
			layer.SetNRGBA(i%r.width, i/r.width, col)
		}
		draw.Draw(out, image.Rect(pc.x, pc.y, pc.x+r.width, pc.y+r.height), layer, image.Point{}, draw.Over)
	}
	// A frame is only published when it has at least one visible pixel after
	// clipping to the canvas.
	for i := 3; i < len(out.Pix); i += 4 {
		if out.Pix[i] != 0 {
			return out, true, nil
		}
	}
	return out, false, nil
}
