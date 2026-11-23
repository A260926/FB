package astisub

import (
	"context"
	"errors"
	"fmt"
	"image"
	"io"
	"sort"
	"time"

	"github.com/asticode/go-astits"
)

// DVB subtitle segment constants (ETSI EN 300 743).
const (
	dvbDataIdentifier   = 0x20
	dvbSubtitleStreamID = 0x00
	dvbSegmentSync      = 0x0f
	dvbStuffing         = 0xff

	dvbSegmentPCS  = 0x10
	dvbSegmentRCS  = 0x11
	dvbSegmentCLUT = 0x12
	dvbSegmentODS  = 0x13
	dvbSegmentEODS = 0x80

	dvbSubBlock8Bit    = 0x12
	dvbSubBlockNewLine = 0xf0

	dvbStateNormalCase       = 0
	dvbStateAcquisitionPoint = 1
	dvbStateModeChange       = 2
)

// BitmapSubtitle is one rendered DVB bitmap subtitle interval.
type BitmapSubtitle struct {
	StartAt time.Duration
	EndAt   time.Duration
	Image   *image.NRGBA
}

// DVBSubtitleOptions configures ReadFromDVBSubtitles.
type DVBSubtitleOptions struct {
	PID    uint16
	PageID uint16
	Width  int
	Height int
	EndAt  time.Duration
}

// DVBSubtitleDecoder incrementally decodes the 8-bit DVB subtitle subset
// described in task_support/dvb-contract.md.
type DVBSubtitleDecoder struct {
	pageID uint16
	width  int
	height int

	// Persistent caches, only mutated at end-of-display-set.
	regions map[uint8]*dvbRegion
	objects map[uint16]*dvbObject
	cluts   map[uint8]*dvbCLUT

	lastPCS     *dvbPCS
	lastTimeout time.Duration

	// Currently visible frame.
	haveFrame    bool
	frameVisible bool
	frame        *image.NRGBA
	frameStart   time.Duration
	lastAt       time.Duration

	// Display set in progress, committed at end-of-display-set.
	dsActive  bool
	dsAt      time.Duration
	dsPCS     *dvbPCS
	dsRegions map[uint8]*dvbRCS
	dsObjects map[uint16]*dvbObject
	dsCLUTs   map[uint8]*dvbCLUTUpdate
}

// dvbPCSRegion is one region entry of a page composition segment.
type dvbPCSRegion struct {
	id   uint8
	x, y int
}

type dvbPCS struct {
	timeout uint8
	version uint8
	state   uint8
	regions []dvbPCSRegion
}

type dvbRCS struct {
	version uint8
	fill    bool
	w, h    int
	depth   uint8
	clutID  uint8
	bg      uint8
	refs    []dvbRegionRef
}

type dvbCLUTUpdate struct {
	version uint8
	entries map[uint8]dvbCLUTEntry
}

// NewDVBSubtitleDecoder creates a decoder pinned to one DVB subtitle page and
// one fixed frame size.
func NewDVBSubtitleDecoder(pageID uint16, width, height int) (*DVBSubtitleDecoder, error) {
	if width < 1 || width > 2048 || height < 1 || height > 2048 {
		return nil, dvbError("decoder dimensions must be between 1 and 2048, got %dx%d", width, height)
	}
	return &DVBSubtitleDecoder{
		pageID:  pageID,
		width:   width,
		height:  height,
		regions: make(map[uint8]*dvbRegion),
		objects: make(map[uint16]*dvbObject),
		cluts:   make(map[uint8]*dvbCLUT),
	}, nil
}

func dvbError(format string, args ...interface{}) error {
	return fmt.Errorf("astisub: dvb subtitle: "+format, args...)
}

// dvbSnapshot is a deep copy of the decoder state used to roll a failed Push
// back completely.
type dvbSnapshot struct {
	regions      map[uint8]*dvbRegion
	objects      map[uint16]*dvbObject
	cluts        map[uint8]*dvbCLUT
	lastPCS      *dvbPCS
	lastTimeout  time.Duration
	haveFrame    bool
	frameVisible bool
	frame        *image.NRGBA
	frameStart   time.Duration
	lastAt       time.Duration
	dsActive     bool
	dsAt         time.Duration
	dsPCS        *dvbPCS
	dsRegions    map[uint8]*dvbRCS
	dsObjects    map[uint16]*dvbObject
	dsCLUTs      map[uint8]*dvbCLUTUpdate
}

func (d *DVBSubtitleDecoder) snapshot() *dvbSnapshot {
	s := &dvbSnapshot{
		regions:      make(map[uint8]*dvbRegion, len(d.regions)),
		objects:      make(map[uint16]*dvbObject, len(d.objects)),
		cluts:        make(map[uint8]*dvbCLUT, len(d.cluts)),
		lastTimeout:  d.lastTimeout,
		haveFrame:    d.haveFrame,
		frameVisible: d.frameVisible,
		frameStart:   d.frameStart,
		lastAt:       d.lastAt,
		dsActive:     d.dsActive,
		dsAt:         d.dsAt,
		dsRegions:    make(map[uint8]*dvbRCS, len(d.dsRegions)),
		dsObjects:    make(map[uint16]*dvbObject, len(d.dsObjects)),
		dsCLUTs:      make(map[uint8]*dvbCLUTUpdate, len(d.dsCLUTs)),
	}
	for id, r := range d.regions {
		s.regions[id] = cloneDVBRegion(r)
	}
	for id, o := range d.objects {
		s.objects[id] = cloneDVBObject(o)
	}
	for id, c := range d.cluts {
		s.cluts[id] = cloneDVBCLUT(c)
	}
	if d.lastPCS != nil {
		s.lastPCS = cloneDVBPCS(d.lastPCS)
	}
	if d.frame != nil {
		s.frame = cloneDVBImage(d.frame)
	}
	if d.dsPCS != nil {
		s.dsPCS = cloneDVBPCS(d.dsPCS)
	}
	for id, r := range d.dsRegions {
		s.dsRegions[id] = cloneDVBRCS(r)
	}
	for id, o := range d.dsObjects {
		s.dsObjects[id] = cloneDVBObject(o)
	}
	for id, c := range d.dsCLUTs {
		s.dsCLUTs[id] = cloneDVBCLUTUpdate(c)
	}
	return s
}

func (d *DVBSubtitleDecoder) restore(s *dvbSnapshot) {
	d.regions = s.regions
	d.objects = s.objects
	d.cluts = s.cluts
	d.lastPCS = s.lastPCS
	d.lastTimeout = s.lastTimeout
	d.haveFrame = s.haveFrame
	d.frameVisible = s.frameVisible
	d.frame = s.frame
	d.frameStart = s.frameStart
	d.lastAt = s.lastAt
	d.dsActive = s.dsActive
	d.dsAt = s.dsAt
	d.dsPCS = s.dsPCS
	d.dsRegions = s.dsRegions
	d.dsObjects = s.dsObjects
	d.dsCLUTs = s.dsCLUTs
}

func cloneDVBImage(img *image.NRGBA) *image.NRGBA {
	out := image.NewNRGBA(img.Rect)
	copy(out.Pix, img.Pix)
	return out
}

func cloneDVBRegion(r *dvbRegion) *dvbRegion {
	out := *r
	out.plane = append([]uint8(nil), r.plane...)
	out.refs = append([]dvbRegionRef(nil), r.refs...)
	return &out
}

func cloneDVBObject(o *dvbObject) *dvbObject {
	out := *o
	out.idx = append([]uint8(nil), o.idx...)
	out.skip = append([]bool(nil), o.skip...)
	return &out
}

func cloneDVBCLUT(c *dvbCLUT) *dvbCLUT {
	out := &dvbCLUT{version: c.version, entries: make(map[uint8]dvbCLUTEntry, len(c.entries))}
	for k, v := range c.entries {
		out.entries[k] = v
	}
	return out
}

func cloneDVBCLUTUpdate(c *dvbCLUTUpdate) *dvbCLUTUpdate {
	out := &dvbCLUTUpdate{version: c.version, entries: make(map[uint8]dvbCLUTEntry, len(c.entries))}
	for k, v := range c.entries {
		out.entries[k] = v
	}
	return out
}

func cloneDVBPCS(p *dvbPCS) *dvbPCS {
	out := *p
	out.regions = append([]dvbPCSRegion(nil), p.regions...)
	return &out
}

func cloneDVBRCS(r *dvbRCS) *dvbRCS {
	out := *r
	out.refs = append([]dvbRegionRef(nil), r.refs...)
	return &out
}

// Push feeds one complete PES payload (without the PES header) at relative
// time at. Frames are only emitted on end-of-display-set.
func (d *DVBSubtitleDecoder) Push(pes []byte, at time.Duration) (out []BitmapSubtitle, err error) {
	if at < 0 {
		return nil, dvbError("negative Push time %s", at)
	}
	if at < d.lastAt {
		return nil, dvbError("non-monotonic Push time %s after %s", at, d.lastAt)
	}
	if d.dsActive && at != d.dsAt {
		return nil, dvbError("display set spans PES with different times %s and %s", d.dsAt, at)
	}

	snap := d.snapshot()
	defer func() {
		if err != nil {
			d.restore(snap)
		}
	}()

	segments, err := parseDVBPES(pes, d.pageID)
	if err != nil {
		return nil, err
	}
	d.lastAt = at
	for _, seg := range segments {
		switch seg.typ {
		case dvbSegmentPCS:
			out, err = d.handlePCS(seg.payload, at, out)
		case dvbSegmentRCS:
			err = d.handleRCS(seg.payload)
		case dvbSegmentCLUT:
			err = d.handleCLUT(seg.payload)
		case dvbSegmentODS:
			err = d.handleODS(seg.payload)
		case dvbSegmentEODS:
			out, err = d.handleEODS(at, out)
		default:
			err = dvbError("unknown segment type 0x%02x", seg.typ)
		}
		if err != nil {
			return nil, err
		}
	}
	return out, nil
}

type dvbSegment struct {
	typ     byte
	payload []byte
}

// parseDVBPES validates the PES framing and returns the segments that belong
// to the selected page.
func parseDVBPES(pes []byte, pageID uint16) ([]dvbSegment, error) {
	if len(pes) < 2 {
		return nil, dvbError("PES payload too short for data identifier and stream id")
	}
	if pes[0] != dvbDataIdentifier {
		return nil, dvbError("unsupported data identifier 0x%02x, only 0x20", pes[0])
	}
	if pes[1] != dvbSubtitleStreamID {
		return nil, dvbError("unsupported subtitle stream id 0x%02x, only 0", pes[1])
	}
	var segments []dvbSegment
	i := 2
	for i < len(pes) {
		if pes[i] == dvbStuffing {
			i++
			continue
		}
		if i+6 > len(pes) {
			return nil, dvbError("truncated segment header")
		}
		if pes[i] != dvbSegmentSync {
			return nil, dvbError("invalid segment sync byte 0x%02x", pes[i])
		}
		typ := pes[i+1]
		segPage := uint16(pes[i+2])<<8 | uint16(pes[i+3])
		length := int(uint16(pes[i+4])<<8 | uint16(pes[i+5]))
		i += 6
		if i+length > len(pes) {
			return nil, dvbError("segment type 0x%02x payload truncated", typ)
		}
		if segPage == pageID {
			segments = append(segments, dvbSegment{typ: typ, payload: pes[i : i+length]})
		}
		i += length
	}
	return segments, nil
}

func (d *DVBSubtitleDecoder) beginDisplaySet(at time.Duration) {
	d.dsActive = true
	d.dsAt = at
	d.dsRegions = make(map[uint8]*dvbRCS)
	d.dsObjects = make(map[uint16]*dvbObject)
	d.dsCLUTs = make(map[uint8]*dvbCLUTUpdate)
}

func (d *DVBSubtitleDecoder) handlePCS(payload []byte, at time.Duration, out []BitmapSubtitle) ([]BitmapSubtitle, error) {
	if len(payload) < 2 {
		return nil, dvbError("PCS payload too short")
	}
	if (len(payload)-2)%6 != 0 {
		return nil, dvbError("PCS payload has invalid region entry length")
	}
	pcs := &dvbPCS{
		timeout: payload[0],
		version: payload[1] >> 4,
		state:   (payload[1] >> 2) & 0x03,
	}
	if pcs.state == 3 {
		return nil, dvbError("PCS state 3 is reserved")
	}
	if d.dsActive {
		return nil, dvbError("new PCS before previous display set reached EODS")
	}
	// Same version PCS is ignored and never clears the cache or replaces the
	// composition, but its EODS still closes and reopens the interval and any
	// new resource segments in the same set are applied normally.
	if d.lastPCS != nil && d.lastPCS.version == pcs.version {
		d.beginDisplaySet(at)
		return out, nil
	}
	for i := 2; i+6 <= len(payload); i += 6 {
		pcs.regions = append(pcs.regions, dvbPCSRegion{
			id: payload[i],
			x:  int(uint16(payload[i+2])<<8 | uint16(payload[i+3])),
			y:  int(uint16(payload[i+4])<<8 | uint16(payload[i+5])),
		})
	}
	d.beginDisplaySet(at)
	d.dsPCS = pcs
	if pcs.state == dvbStateAcquisitionPoint || pcs.state == dvbStateModeChange {
		// A new epoch rebuilds every region, object and CLUT from this set.
		d.regions = make(map[uint8]*dvbRegion)
		d.objects = make(map[uint16]*dvbObject)
		d.cluts = make(map[uint8]*dvbCLUT)
	}
	return out, nil
}

func (d *DVBSubtitleDecoder) handleRCS(payload []byte) error {
	if len(payload) < 10 {
		return dvbError("RCS payload too short")
	}
	id := payload[0]
	version := payload[1] >> 4
	fill := payload[1]&0x08 != 0
	w := int(uint16(payload[2])<<8 | uint16(payload[3]))
	h := int(uint16(payload[4])<<8 | uint16(payload[5]))
	depth := (payload[6] >> 2) & 0x07
	clutID := payload[7]
	bg := payload[8]
	// payload[9] packs the 4-bit and 2-bit background indexes, unused in the
	// 8-bit subset.
	if depth != 3 {
		return dvbError("unsupported region depth %d, only 8-bit (depth 3)", depth)
	}
	if (len(payload)-10)%6 != 0 {
		return dvbError("RCS payload has invalid object entry length")
	}
	if cached, ok := d.regions[id]; ok && cached.version == version {
		return nil // same version retransmission: nothing redone, no fill
	}
	if _, staged := d.dsRegions[id]; staged {
		return dvbError("region %d appears twice with different versions in one display set", id)
	}
	rcs := &dvbRCS{version: version, fill: fill, w: w, h: h, depth: depth, clutID: clutID, bg: bg}
	for i := 10; i+6 <= len(payload); i += 6 {
		objID := uint16(payload[i])<<8 | uint16(payload[i+1])
		typ := payload[i+2] >> 6
		provider := (payload[i+2] >> 4) & 0x03
		if typ != 0 || provider != 0 {
			return dvbError("unsupported object type %d provider %d, only 0/0", typ, provider)
		}
		x := int((uint16(payload[i+2]&0x0f) << 8) | uint16(payload[i+3]))
		y := int((uint16(payload[i+4]&0x0f) << 8) | uint16(payload[i+5]))
		rcs.refs = append(rcs.refs, dvbRegionRef{objectID: objID, typ: typ, provider: provider, x: x, y: y})
	}
	if !d.dsActive {
		d.beginDisplaySet(d.lastAt)
	}
	d.dsRegions[id] = rcs
	return nil
}

func (d *DVBSubtitleDecoder) handleCLUT(payload []byte) error {
	if len(payload) < 2 {
		return dvbError("CLUT payload too short")
	}
	id := payload[0]
	version := payload[1] >> 4
	if cached, ok := d.cluts[id]; ok && cached.version == version {
		return nil
	}
	if _, staged := d.dsCLUTs[id]; staged {
		return dvbError("CLUT %d appears twice with different versions in one display set", id)
	}
	if (len(payload)-2)%6 != 0 {
		return dvbError("CLUT payload has invalid entry length")
	}
	upd := &dvbCLUTUpdate{version: version, entries: make(map[uint8]dvbCLUTEntry)}
	for i := 2; i+6 <= len(payload); i += 6 {
		entryID := payload[i]
		flags := payload[i+1]
		if flags&0x01 == 0 {
			return dvbError("only full-range CLUT entries are supported")
		}
		if flags&0x20 == 0 {
			return dvbError("only 8-bit CLUT entries are supported")
		}
		upd.entries[entryID] = dvbCLUTEntry{
			y:       payload[i+2],
			cr:      payload[i+3],
			cb:      payload[i+4],
			t:       payload[i+5],
			defined: true,
		}
	}
	if !d.dsActive {
		d.beginDisplaySet(d.lastAt)
	}
	d.dsCLUTs[id] = upd
	return nil
}

func (d *DVBSubtitleDecoder) handleODS(payload []byte) error {
	if len(payload) < 7 {
		return dvbError("ODS payload too short")
	}
	id := uint16(payload[0])<<8 | uint16(payload[1])
	version := payload[2] >> 4
	method := (payload[2] >> 2) & 0x03
	nonmod := payload[2]&0x02 != 0
	if method != 0 {
		return dvbError("unsupported object coding method %d, only 0", method)
	}
	if cached, ok := d.objects[id]; ok && cached.version == version {
		return nil
	}
	if _, staged := d.dsObjects[id]; staged {
		return dvbError("object %d appears twice with different versions in one display set", id)
	}
	topLen := int(uint16(payload[3])<<8 | uint16(payload[4]))
	bottomLen := int(uint16(payload[5])<<8 | uint16(payload[6]))
	end := 7 + topLen + bottomLen
	if end > len(payload) {
		return dvbError("object %d field data truncated", id)
	}
	if len(payload) != end && len(payload) != end+1 {
		return dvbError("object %d has %d unexpected trailing bytes", id, len(payload)-end)
	}
	if len(payload) == end+1 && payload[end] != 0 {
		return dvbError("object %d padding byte must be zero", id)
	}
	top := payload[7 : 7+topLen]
	bottom := payload[7+topLen : end]
	obj, err := decodeDVBObject(top, bottom, nonmod)
	if err != nil {
		return err
	}
	obj.version = version
	if !d.dsActive {
		d.beginDisplaySet(d.lastAt)
	}
	d.dsObjects[id] = obj
	return nil
}

func (d *DVBSubtitleDecoder) handleEODS(at time.Duration, out []BitmapSubtitle) ([]BitmapSubtitle, error) {
	// A stray EODS with no open display set has nothing to do.
	if !d.dsActive {
		return out, nil
	}
	if err := d.applyDisplaySet(); err != nil {
		return nil, err
	}

	pcs := d.dsPCS
	if pcs == nil {
		// Resource-only display set (e.g. a standalone CLUT update) keeps
		// composing the last page composition.
		pcs = d.lastPCS
	}
	if pcs == nil {
		return nil, dvbError("display set has no page composition")
	}

	// Close the previously visible frame using its own timeout.
	if d.haveFrame && d.frameVisible {
		end := at
		if d.lastTimeout > 0 {
			if limit := d.frameStart + d.lastTimeout; limit < end {
				end = limit
			}
		}
		if end > d.frameStart {
			out = append(out, BitmapSubtitle{
				StartAt: d.frameStart,
				EndAt:   end,
				Image:   cloneDVBImage(d.frame),
			})
		}
	}

	// Build the new visible frame. An empty region list clears the screen.
	if len(pcs.regions) > 0 {
		layers := make([]dvbComposedLayer, 0, len(pcs.regions))
		for _, pr := range pcs.regions {
			r, ok := d.regions[pr.id]
			if !ok {
				return nil, dvbError("PCS references unknown region %d", pr.id)
			}
			c, ok := d.cluts[r.clutID]
			if !ok {
				return nil, dvbError("region %d references unknown CLUT %d", pr.id, r.clutID)
			}
			img, err := renderRegion(r, c)
			if err != nil {
				return nil, err
			}
			layers = append(layers, dvbComposedLayer{x: pr.x, y: pr.y, img: img})
		}
		d.frame = composeDVBFrame(d.width, d.height, layers)
		d.frameVisible = hasVisiblePixel(d.frame)
	} else {
		d.frame = nil
		d.frameVisible = false
	}
	d.haveFrame = true
	d.frameStart = at
	if d.dsPCS != nil {
		d.lastPCS = cloneDVBPCS(d.dsPCS)
		d.lastTimeout = time.Duration(pcs.timeout) * time.Second
	}

	d.dsActive = false
	d.dsPCS = nil
	d.dsRegions = nil
	d.dsObjects = nil
	d.dsCLUTs = nil
	return out, nil
}

// applyDisplaySet validates and merges the staged segments into the caches.
func (d *DVBSubtitleDecoder) applyDisplaySet() error {
	// CLUT updates first so recolouring applies to already indexed pixels.
	clutIDs := make([]uint8, 0, len(d.dsCLUTs))
	for id := range d.dsCLUTs {
		clutIDs = append(clutIDs, id)
	}
	sort.Slice(clutIDs, func(i, j int) bool { return clutIDs[i] < clutIDs[j] })
	for _, id := range clutIDs {
		upd := d.dsCLUTs[id]
		c, ok := d.cluts[id]
		if !ok {
			c = &dvbCLUT{version: upd.version, entries: make(map[uint8]dvbCLUTEntry)}
		}
		c.version = upd.version
		for entryID, e := range upd.entries {
			c.entries[entryID] = e
		}
		d.cluts[id] = c
	}

	// Objects may arrive before the region that references them.
	objIDs := make([]uint16, 0, len(d.dsObjects))
	for id := range d.dsObjects {
		objIDs = append(objIDs, id)
	}
	sort.Slice(objIDs, func(i, j int) bool { return objIDs[i] < objIDs[j] })
	changedObjects := make(map[uint16]bool)
	for _, id := range objIDs {
		d.objects[id] = d.dsObjects[id]
		changedObjects[id] = true
	}

	// New region versions rebuild the indexed plane, honouring fill.
	regionIDs := make([]uint8, 0, len(d.dsRegions))
	for id := range d.dsRegions {
		regionIDs = append(regionIDs, id)
	}
	sort.Slice(regionIDs, func(i, j int) bool { return regionIDs[i] < regionIDs[j] })
	newRegions := make(map[uint8]bool)
	for _, id := range regionIDs {
		rcs := d.dsRegions[id]
		for _, ref := range rcs.refs {
			if _, ok := d.objects[ref.objectID]; !ok {
				return dvbError("region %d references unknown object %d", id, ref.objectID)
			}
		}
		if _, ok := d.cluts[rcs.clutID]; !ok {
			return dvbError("region %d references unknown CLUT %d", id, rcs.clutID)
		}
		plane := make([]uint8, rcs.w*rcs.h)
		old := d.regions[id]
		if !rcs.fill && old != nil && old.w == rcs.w && old.h == rcs.h {
			copy(plane, old.plane) // normal case: untouched pixels survive
		} else {
			for i := range plane {
				plane[i] = rcs.bg
			}
		}
		r := &dvbRegion{
			version: rcs.version,
			w:       rcs.w,
			h:       rcs.h,
			depth:   rcs.depth,
			clutID:  rcs.clutID,
			bg:      rcs.bg,
			refs:    append([]dvbRegionRef(nil), rcs.refs...),
			plane:   plane,
		}
		for _, ref := range rcs.refs {
			r.drawObject(d.objects[ref.objectID], ref.x, ref.y)
		}
		d.regions[id] = r
		newRegions[id] = true
	}

	// Objects changed without a new RCS: repaint them at the old reference
	// positions, in each region's original reference order.
	allRegionIDs := make([]uint8, 0, len(d.regions))
	for id := range d.regions {
		allRegionIDs = append(allRegionIDs, id)
	}
	sort.Slice(allRegionIDs, func(i, j int) bool { return allRegionIDs[i] < allRegionIDs[j] })
	for _, id := range allRegionIDs {
		if newRegions[id] {
			continue
		}
		r := d.regions[id]
		for _, ref := range r.refs {
			if changedObjects[ref.objectID] {
				r.drawObject(d.objects[ref.objectID], ref.x, ref.y)
			}
		}
	}
	return nil
}

// Flush closes the currently visible frame at at. An unfinished display set is
// an error; calling Flush again afterwards produces nothing.
func (d *DVBSubtitleDecoder) Flush(at time.Duration) ([]BitmapSubtitle, error) {
	if at < 0 {
		return nil, dvbError("negative Flush time %s", at)
	}
	if at < d.lastAt {
		return nil, dvbError("non-monotonic Flush time %s after %s", at, d.lastAt)
	}
	if d.dsActive {
		return nil, dvbError("Flush with an unfinished display set")
	}
	var out []BitmapSubtitle
	if d.haveFrame && d.frameVisible {
		end := at
		if d.lastTimeout > 0 {
			if limit := d.frameStart + d.lastTimeout; limit < end {
				end = limit
			}
		}
		if end > d.frameStart {
			out = append(out, BitmapSubtitle{
				StartAt: d.frameStart,
				EndAt:   end,
				Image:   cloneDVBImage(d.frame),
			})
		}
	}
	d.haveFrame = false
	d.frameVisible = false
	d.frame = nil
	d.lastAt = at
	return out, nil
}

// ReadFromDVBSubtitles demuxes the selected private_stream_1 PID of a transport
// stream and feeds it through the incremental DVBSubtitleDecoder. The first
// selected PES carrying a PTS is the time zero.
func ReadFromDVBSubtitles(r io.Reader, o DVBSubtitleOptions) ([]BitmapSubtitle, error) {
	dec, err := NewDVBSubtitleDecoder(o.PageID, o.Width, o.Height)
	if err != nil {
		return nil, err
	}
	dmx := astits.NewDemuxer(context.Background(), r)
	var all []BitmapSubtitle
	var zeroSet bool
	var zeroPTS int64
	for {
		data, err := dmx.NextData()
		if err != nil {
			if errors.Is(err, astits.ErrNoMorePackets) || errors.Is(err, io.EOF) {
				break
			}
			return nil, dvbError("demuxing transport stream failed: %w", err)
		}
		if data.PES == nil || data.PID != o.PID {
			continue
		}
		if data.PES.Header == nil || data.PES.Header.StreamID != astits.StreamIDPrivateStream1 {
			continue
		}
		hdr := data.PES.Header.OptionalHeader
		if hdr == nil || hdr.PTS == nil {
			continue // PES without PTS are skipped
		}
		pts := hdr.PTS.Base
		if !zeroSet {
			zeroSet = true
			zeroPTS = pts
		}
		rel := pts - zeroPTS
		if rel < 0 {
			return nil, dvbError("PTS wrapped or went backwards from %d to %d", zeroPTS, pts)
		}
		at := time.Duration(rel * 1e9 / 90000)
		frames, err := dec.Push(data.PES.Data, at)
		if err != nil {
			return nil, err
		}
		all = append(all, frames...)
	}
	frames, err := dec.Flush(o.EndAt)
	if err != nil {
		return nil, err
	}
	all = append(all, frames...)
	return all, nil
}
