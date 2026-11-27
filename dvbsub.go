package astisub

import (
	"context"
	"encoding/binary"
	"fmt"
	"image"
	"io"
	"time"

	"github.com/asticode/go-astits"
)

// DVB bitmap subtitle segment types.
const (
	dvbDataIdentifier      = 0x20
	dvbSubtitleStreamID    = 0x00
	dvbSegmentSync         = 0x0f
	dvbSegmentPCS          = 0x10
	dvbSegmentRCS          = 0x11
	dvbSegmentCLUT         = 0x12
	dvbSegmentODS          = 0x13
	dvbSegmentEODS         = 0x80
	dvbMaxDimension        = 2048
	dvbMaxObjectEntryCount = 256
)

// BitmapSubtitle represents a DVB bitmap subtitle frame. Image is a full-frame
// non-premultiplied RGBA image.
type BitmapSubtitle struct {
	StartAt time.Duration
	EndAt   time.Duration
	Image   *image.NRGBA
}

// DVBSubtitleOptions represents DVB bitmap subtitle transport options.
type DVBSubtitleOptions struct {
	PID    uint16
	PageID uint16
	Width  int
	Height int
	EndAt  time.Duration
}

type dvbCLUTEntry struct {
	present      bool
	y, cr, cb, t uint8
}

type dvbCLUT struct {
	version uint8
	entries [dvbMaxObjectEntryCount]dvbCLUTEntry
}

type dvbPixelWrite struct {
	x, y  int
	index int16 // >= 0; non-modifying skips are not stored
}

type dvbObject struct {
	id      uint16
	version uint8
	nonMod  bool
	writes  []dvbPixelWrite
}

type dvbObjectRef struct {
	id uint16
	x  int
	y  int
}

type dvbRegion struct {
	version    uint8
	width      int
	height     int
	depth      uint8
	clutID     uint8
	background uint8
	refs       []dvbObjectRef
	indices    []uint8
}

type dvbPCSRegion struct {
	id uint8
	x  int
	y  int
}

type dvbPCS struct {
	version uint8
	state   uint8
	timeout uint8
	regions []dvbPCSRegion
}

type dvbPendingRegion struct {
	version    uint8
	fill       bool
	width      int
	height     int
	depth      uint8
	clutID     uint8
	background uint8
	refs       []dvbObjectRef
}

type dvbPendingCLUT struct {
	version uint8
	entries [dvbMaxObjectEntryCount]dvbCLUTEntry
}

type dvbPendingDisplaySet struct {
	at          time.Duration
	hasSelected bool
	pcs         *dvbPCS
	objects     map[uint16]*dvbObject
	regions     map[uint8]*dvbPendingRegion
	cluts       map[uint8]*dvbPendingCLUT
}

type dvbCurrentFrame struct {
	image   *image.NRGBA
	start   time.Duration
	timeout uint8
}

// DVBSubtitleDecoder is an incremental DVB bitmap subtitle decoder.
type DVBSubtitleDecoder struct {
	pageID uint16
	width  int
	height int

	objects map[uint16]*dvbObject
	regions map[uint8]*dvbRegion
	cluts   map[uint8]*dvbCLUT

	hasPCS     bool
	pcsVersion uint8
	pcsRegions []dvbPCSRegion
	timeout    uint8

	pending dvbPendingDisplaySet

	current *dvbCurrentFrame
	hasAt   bool
	lastAt  time.Duration
}

// NewDVBSubtitleDecoder creates an incremental DVB bitmap subtitle decoder for
// a single page and a fixed output canvas.
func NewDVBSubtitleDecoder(pageID uint16, width, height int) (*DVBSubtitleDecoder, error) {
	if width < 1 || width > dvbMaxDimension || height < 1 || height > dvbMaxDimension {
		return nil, fmt.Errorf("astisub: invalid DVB subtitle dimensions %dx%d", width, height)
	}
	return &DVBSubtitleDecoder{
		pageID:  pageID,
		width:   width,
		height:  height,
		objects: make(map[uint16]*dvbObject),
		regions: make(map[uint8]*dvbRegion),
		cluts:   make(map[uint8]*dvbCLUT),
	}, nil
}

type dvbSegment struct {
	typ     byte
	pageID  uint16
	payload []byte
}

func dvbParseSegments(pes []byte) ([]dvbSegment, error) {
	if len(pes) < 2 {
		return nil, fmt.Errorf("astisub: DVB subtitle PES data too short")
	}
	if pes[0] != dvbDataIdentifier {
		return nil, fmt.Errorf("astisub: unsupported DVB data identifier 0x%02x", pes[0])
	}
	if pes[1] != dvbSubtitleStreamID {
		return nil, fmt.Errorf("astisub: unsupported DVB subtitle stream id 0x%02x", pes[1])
	}
	var segments []dvbSegment
	for i := 2; i < len(pes); {
		if pes[i] == 0xff {
			for j := i; j < len(pes); j++ {
				if pes[j] != 0xff {
					return nil, fmt.Errorf("astisub: invalid DVB stuffing byte at offset %d", j)
				}
			}
			break
		}
		if pes[i] != dvbSegmentSync {
			return nil, fmt.Errorf("astisub: invalid DVB segment sync 0x%02x", pes[i])
		}
		if i+6 > len(pes) {
			return nil, fmt.Errorf("astisub: truncated DVB segment header")
		}
		typ := pes[i+1]
		pageID := binary.BigEndian.Uint16(pes[i+2 : i+4])
		l := int(binary.BigEndian.Uint16(pes[i+4 : i+6]))
		start := i + 6
		if start+l > len(pes) {
			return nil, fmt.Errorf("astisub: truncated DVB segment payload")
		}
		segments = append(segments, dvbSegment{typ: typ, pageID: pageID, payload: pes[start : start+l]})
		i = start + l
	}
	return segments, nil
}

func dvbParsePCS(p []byte) (*dvbPCS, error) {
	if len(p) < 2 {
		return nil, fmt.Errorf("astisub: truncated DVB page composition segment")
	}
	if (len(p)-2)%6 != 0 {
		return nil, fmt.Errorf("astisub: invalid DVB page composition region list")
	}
	pcs := &dvbPCS{
		timeout: p[0],
		version: p[1] >> 4,
		state:   (p[1] >> 2) & 0x03,
	}
	if pcs.state == 3 {
		return nil, fmt.Errorf("astisub: unsupported DVB page state 3")
	}
	for i := 2; i < len(p); i += 6 {
		pcs.regions = append(pcs.regions, dvbPCSRegion{
			id: p[i],
			x:  int(binary.BigEndian.Uint16(p[i+2 : i+4])),
			y:  int(binary.BigEndian.Uint16(p[i+4 : i+6])),
		})
	}
	return pcs, nil
}

func dvbParseRCS(p []byte) (*dvbPendingRegion, error) {
	if len(p) < 10 {
		return nil, fmt.Errorf("astisub: truncated DVB region composition segment")
	}
	if (len(p)-10)%6 != 0 {
		return nil, fmt.Errorf("astisub: invalid DVB region object list")
	}
	r := &dvbPendingRegion{
		version:    p[1] >> 4,
		fill:       p[1]&0x08 != 0,
		width:      int(binary.BigEndian.Uint16(p[2:4])),
		height:     int(binary.BigEndian.Uint16(p[4:6])),
		depth:      (p[6] >> 2) & 0x07,
		clutID:     p[7],
		background: p[8],
	}
	if r.depth != 3 {
		return nil, fmt.Errorf("astisub: unsupported DVB region depth %d", r.depth)
	}
	for i := 10; i < len(p); i += 6 {
		typ := (p[i+2] >> 6) & 0x03
		provider := (p[i+2] >> 4) & 0x03
		if typ != 0 || provider != 0 {
			return nil, fmt.Errorf("astisub: unsupported DVB object type/provider %d/%d", typ, provider)
		}
		r.refs = append(r.refs, dvbObjectRef{
			id: binary.BigEndian.Uint16(p[i : i+2]),
			x:  int(p[i+2]&0x0f)<<8 | int(p[i+3]),
			y:  int(p[i+4]&0x0f)<<8 | int(p[i+5]),
		})
	}
	return r, nil
}

func dvbParseCLUT(p []byte) (*dvbPendingCLUT, error) {
	if len(p) < 2 {
		return nil, fmt.Errorf("astisub: truncated DVB CLUT definition segment")
	}
	if (len(p)-2)%6 != 0 {
		return nil, fmt.Errorf("astisub: invalid DVB CLUT entry list")
	}
	c := &dvbPendingCLUT{version: p[1] >> 4}
	for i := 2; i < len(p); i += 6 {
		entryID := p[i]
		flags := p[i+1]
		if flags&0x20 == 0 {
			return nil, fmt.Errorf("astisub: DVB CLUT entry %d has no 8-bit definition", entryID)
		}
		if flags&0x01 == 0 {
			return nil, fmt.Errorf("astisub: DVB CLUT entry %d is not full range", entryID)
		}
		c.entries[entryID] = dvbCLUTEntry{
			present: true,
			y:       p[i+2],
			cr:      p[i+3],
			cb:      p[i+4],
			t:       p[i+5],
		}
	}
	return c, nil
}

func dvbParseODS(p []byte) (*dvbObject, error) {
	if len(p) < 7 {
		return nil, fmt.Errorf("astisub: truncated DVB object data segment")
	}
	flags := p[2]
	method := (flags >> 2) & 0x03
	if method != 0 {
		return nil, fmt.Errorf("astisub: unsupported DVB object coding method %d", method)
	}
	topLen := int(binary.BigEndian.Uint16(p[3:5]))
	bottomLen := int(binary.BigEndian.Uint16(p[5:7]))
	data := p[7:]
	if len(data) < topLen+bottomLen {
		return nil, fmt.Errorf("astisub: truncated DVB object field data")
	}
	rest := data[topLen+bottomLen:]
	if len(rest) > 1 || (len(rest) == 1 && rest[0] != 0) {
		return nil, fmt.Errorf("astisub: invalid DVB object padding")
	}
	o := &dvbObject{
		id:      binary.BigEndian.Uint16(p[0:2]),
		version: flags >> 4,
		nonMod:  flags&0x02 != 0,
	}
	top := data[:topLen]
	bottom := data[topLen : topLen+bottomLen]
	if err := dvbDecodeObjectField(top, 0, o.nonMod, &o.writes); err != nil {
		return nil, err
	}
	if bottomLen == 0 {
		if err := dvbDecodeObjectField(top, 1, o.nonMod, &o.writes); err != nil {
			return nil, err
		}
	} else {
		if err := dvbDecodeObjectField(bottom, 1, o.nonMod, &o.writes); err != nil {
			return nil, err
		}
	}
	return o, nil
}

// pendingResetsEpoch reports whether the display set currently being
// accumulated starts a new epoch.
func (d *DVBSubtitleDecoder) pendingResetsEpoch() bool {
	return d.pending.pcs != nil && (d.pending.pcs.state == 1 || d.pending.pcs.state == 2)
}

func (d *DVBSubtitleDecoder) beginSelected(at time.Duration) {
	if !d.pending.hasSelected {
		d.pending.hasSelected = true
		d.pending.at = at
	}
	if d.pending.objects == nil {
		d.pending.objects = make(map[uint16]*dvbObject)
		d.pending.regions = make(map[uint8]*dvbPendingRegion)
		d.pending.cluts = make(map[uint8]*dvbPendingCLUT)
	}
}

func (d *DVBSubtitleDecoder) stageObject(o *dvbObject) error {
	if prev, ok := d.pending.objects[o.id]; ok {
		if prev.version == o.version {
			return nil
		}
		return fmt.Errorf("astisub: multiple DVB object versions for id %d in one display set", o.id)
	}
	if !d.pendingResetsEpoch() {
		if cached, ok := d.objects[o.id]; ok && cached.version == o.version {
			return nil
		}
	}
	d.pending.objects[o.id] = o
	return nil
}

func (d *DVBSubtitleDecoder) stageRegion(r *dvbPendingRegion, id uint8) error {
	if prev, ok := d.pending.regions[id]; ok {
		if prev.version == r.version {
			return nil
		}
		return fmt.Errorf("astisub: multiple DVB region versions for id %d in one display set", id)
	}
	if !d.pendingResetsEpoch() {
		if cached, ok := d.regions[id]; ok && cached.version == r.version {
			return nil
		}
	}
	d.pending.regions[id] = r
	return nil
}

func (d *DVBSubtitleDecoder) stageCLUT(c *dvbPendingCLUT, id uint8) error {
	if prev, ok := d.pending.cluts[id]; ok {
		if prev.version == c.version {
			return nil
		}
		return fmt.Errorf("astisub: multiple DVB CLUT versions for id %d in one display set", id)
	}
	if !d.pendingResetsEpoch() {
		if cached, ok := d.cluts[id]; ok && cached.version == c.version {
			return nil
		}
	}
	d.pending.cluts[id] = c
	return nil
}

func (d *DVBSubtitleDecoder) push(pes []byte, at time.Duration) ([]BitmapSubtitle, error) {
	segments, err := dvbParseSegments(pes)
	if err != nil {
		return nil, err
	}
	var frames []BitmapSubtitle
	for _, s := range segments {
		if s.pageID != d.pageID {
			continue
		}
		switch s.typ {
		case dvbSegmentPCS:
			if d.pending.hasSelected {
				return nil, fmt.Errorf("astisub: DVB page composition segment is not first in display set")
			}
			pcs, err := dvbParsePCS(s.payload)
			if err != nil {
				return nil, err
			}
			d.beginSelected(at)
			if d.hasPCS && pcs.version == d.pcsVersion {
				// A retransmitted page composition must not reset caches.
				d.pending.pcs = nil
			} else {
				d.pending.pcs = pcs
			}
		case dvbSegmentRCS:
			d.beginSelected(at)
			r, err := dvbParseRCS(s.payload)
			if err != nil {
				return nil, err
			}
			if err := d.stageRegion(r, s.payload[0]); err != nil {
				return nil, err
			}
		case dvbSegmentCLUT:
			d.beginSelected(at)
			c, err := dvbParseCLUT(s.payload)
			if err != nil {
				return nil, err
			}
			if err := d.stageCLUT(c, s.payload[0]); err != nil {
				return nil, err
			}
		case dvbSegmentODS:
			d.beginSelected(at)
			o, err := dvbParseODS(s.payload)
			if err != nil {
				return nil, err
			}
			if err := d.stageObject(o); err != nil {
				return nil, err
			}
		case dvbSegmentEODS:
			if len(s.payload) != 0 {
				return nil, fmt.Errorf("astisub: DVB end of display set segment has a payload")
			}
			d.beginSelected(at)
			closed, err := d.commitDisplaySet()
			if err != nil {
				return nil, err
			}
			frames = append(frames, closed...)
			d.pending = dvbPendingDisplaySet{}
		default:
			return nil, fmt.Errorf("astisub: unsupported DVB segment type 0x%02x", s.typ)
		}
	}
	return frames, nil
}

func (d *DVBSubtitleDecoder) commitDisplaySet() ([]BitmapSubtitle, error) {
	if _, err := d.applyDisplaySet(); err != nil {
		return nil, err
	}
	frame, visible, err := d.composeFrame()
	if err != nil {
		return nil, err
	}
	frames := d.closeCurrent(d.pending.at)
	if visible {
		d.current = &dvbCurrentFrame{image: frame, start: d.pending.at, timeout: d.timeout}
	} else {
		d.current = nil
	}
	return frames, nil
}

// applyDisplaySet merges the pending display set into the decoder caches. It
// returns the set of object ids that were freshly installed.
func (d *DVBSubtitleDecoder) applyDisplaySet() (map[uint16]bool, error) {
	p := &d.pending
	if p.pcs != nil {
		if p.pcs.state == 1 || p.pcs.state == 2 {
			d.objects = make(map[uint16]*dvbObject)
			d.regions = make(map[uint8]*dvbRegion)
			d.cluts = make(map[uint8]*dvbCLUT)
		}
		d.hasPCS = true
		d.pcsVersion = p.pcs.version
		d.pcsRegions = append([]dvbPCSRegion(nil), p.pcs.regions...)
		d.timeout = p.pcs.timeout
	}

	changed := make(map[uint16]bool, len(p.objects))
	for id, o := range p.objects {
		d.objects[id] = o
		changed[id] = true
	}
	for id, pc := range p.cluts {
		if c, ok := d.cluts[id]; ok {
			nc := *c
			nc.version = pc.version
			for i := range nc.entries {
				if pc.entries[i].present {
					nc.entries[i] = pc.entries[i]
				}
			}
			d.cluts[id] = &nc
		} else {
			nc := &dvbCLUT{version: pc.version}
			for i := range nc.entries {
				if pc.entries[i].present {
					nc.entries[i] = pc.entries[i]
				}
			}
			d.cluts[id] = nc
		}
	}

	stagedRegions := make(map[uint8]bool, len(p.regions))
	for id, pr := range p.regions {
		if r, ok := d.regions[id]; ok {
			if r.width != pr.width || r.height != pr.height || r.depth != pr.depth || r.clutID != pr.clutID {
				return nil, fmt.Errorf("astisub: DVB region %d changed its fixed attributes", id)
			}
			r.version = pr.version
			r.background = pr.background
			r.refs = append([]dvbObjectRef(nil), pr.refs...)
			if pr.fill {
				for i := range r.indices {
					r.indices[i] = pr.background
				}
			}
		} else {
			r := &dvbRegion{
				version:    pr.version,
				width:      pr.width,
				height:     pr.height,
				depth:      pr.depth,
				clutID:     pr.clutID,
				background: pr.background,
				refs:       append([]dvbObjectRef(nil), pr.refs...),
			}
			r.indices = make([]uint8, r.width*r.height)
			for i := range r.indices {
				r.indices[i] = pr.background
			}
			d.regions[id] = r
		}
		stagedRegions[id] = true
	}

	// A new or updated region composition repaints its whole object list.
	for id := range stagedRegions {
		if err := d.drawRegion(d.regions[id], nil); err != nil {
			return nil, err
		}
	}
	// An object update without a new region composition repaints only that
	// object, at its original reference positions.
	if len(changed) > 0 {
		for id, r := range d.regions {
			if stagedRegions[id] {
				continue
			}
			if err := d.drawRegion(r, changed); err != nil {
				return nil, err
			}
		}
	}
	// Every reference introduced by this display set, and every region shown by
	// the current page composition, must resolve at EODS. Cached regions that
	// are neither updated nor displayed keep the state validated earlier.
	check := make(map[uint8]bool, len(stagedRegions)+len(d.pcsRegions))
	for id := range stagedRegions {
		check[id] = true
	}
	for _, pc := range d.pcsRegions {
		check[pc.id] = true
	}
	for id := range check {
		r, ok := d.regions[id]
		if !ok {
			return nil, fmt.Errorf("astisub: DVB page references missing region %d", id)
		}
		for _, ref := range r.refs {
			if _, ok := d.objects[ref.id]; !ok {
				return nil, fmt.Errorf("astisub: DVB region %d references missing object %d", id, ref.id)
			}
		}
		if err := d.validateRegionColors(r); err != nil {
			return nil, err
		}
	}
	return changed, nil
}

// Push decodes one PES.Data payload (without PES header) at the provided
// relative time.
func (d *DVBSubtitleDecoder) Push(pes []byte, at time.Duration) ([]BitmapSubtitle, error) {
	if at < 0 {
		return nil, fmt.Errorf("astisub: negative DVB subtitle time %s", at)
	}
	if d.hasAt && at < d.lastAt {
		return nil, fmt.Errorf("astisub: decreasing DVB subtitle time %s < %s", at, d.lastAt)
	}
	if d.pending.hasSelected && at != d.pending.at {
		return nil, fmt.Errorf("astisub: DVB display set spans times %s and %s", d.pending.at, at)
	}
	saved := d.clone()
	frames, err := d.push(pes, at)
	if err != nil {
		d.restore(saved)
		return nil, err
	}
	d.hasAt = true
	d.lastAt = at
	return frames, nil
}

// Flush closes the current visible frame at the provided relative time.
func (d *DVBSubtitleDecoder) Flush(at time.Duration) ([]BitmapSubtitle, error) {
	if d.pending.hasSelected {
		return nil, fmt.Errorf("astisub: cannot flush DVB subtitles with an uncommitted display set")
	}
	if at < 0 {
		return nil, fmt.Errorf("astisub: negative DVB subtitle time %s", at)
	}
	if d.hasAt && at < d.lastAt {
		return nil, fmt.Errorf("astisub: decreasing DVB subtitle time %s < %s", at, d.lastAt)
	}
	frames := d.closeCurrent(at)
	d.hasAt = true
	d.lastAt = at
	return frames, nil
}

func (d *DVBSubtitleDecoder) closeCurrent(at time.Duration) []BitmapSubtitle {
	if d.current == nil {
		return nil
	}
	end := at
	if d.current.timeout > 0 {
		expiry := d.current.start + time.Duration(d.current.timeout)*time.Second
		if expiry < end {
			end = expiry
		}
	}
	var frames []BitmapSubtitle
	if end > d.current.start {
		frames = append(frames, BitmapSubtitle{
			StartAt: d.current.start,
			EndAt:   end,
			Image:   dvbCloneNRGBA(d.current.image),
		})
	}
	d.current = nil
	return frames
}

// ReadFromDVBSubtitles reads DVB bitmap subtitles from a transport stream. It
// feeds every selected PES to the incremental decoder.
func ReadFromDVBSubtitles(r io.Reader, o DVBSubtitleOptions) ([]BitmapSubtitle, error) {
	d, err := NewDVBSubtitleDecoder(o.PageID, o.Width, o.Height)
	if err != nil {
		return nil, err
	}
	dmx := astits.NewDemuxer(context.Background(), r)
	var out []BitmapSubtitle
	var zero, lastPTS time.Duration
	var haveZero, havePTS bool
	for {
		data, err := dmx.NextData()
		if err != nil {
			if err == astits.ErrNoMorePackets || err == io.EOF {
				break
			}
			return nil, fmt.Errorf("astisub: fetching DVB subtitle data failed: %w", err)
		}
		if data.PES == nil || data.PID != o.PID {
			continue
		}
		h := data.PES.Header
		if h == nil || h.StreamID != astits.StreamIDPrivateStream1 {
			continue
		}
		if h.OptionalHeader == nil || h.OptionalHeader.PTS == nil {
			continue
		}
		base := h.OptionalHeader.PTS.Base
		if base < 0 {
			return nil, fmt.Errorf("astisub: negative DVB subtitle PTS")
		}
		// 90 kHz to nanoseconds, truncated, computed without overflowing int64.
		pts := time.Duration(base/90000)*time.Second + time.Duration(base%90000)*time.Second/90000
		if !haveZero {
			zero = pts
			haveZero = true
		}
		if havePTS && pts < lastPTS {
			return nil, fmt.Errorf("astisub: decreasing DVB subtitle PTS")
		}
		lastPTS = pts
		havePTS = true
		at := pts - zero
		if at < 0 {
			return nil, fmt.Errorf("astisub: negative relative DVB subtitle PTS")
		}
		frames, err := d.Push(data.PES.Data, at)
		if err != nil {
			return nil, err
		}
		out = append(out, frames...)
	}
	frames, err := d.Flush(o.EndAt)
	if err != nil {
		return nil, err
	}
	out = append(out, frames...)
	return out, nil
}

func dvbCloneNRGBA(m *image.NRGBA) *image.NRGBA {
	if m == nil {
		return nil
	}
	c := image.NewNRGBA(image.Rect(0, 0, m.Bounds().Dx(), m.Bounds().Dy()))
	copy(c.Pix, m.Pix)
	return c
}

func (d *DVBSubtitleDecoder) clone() *DVBSubtitleDecoder {
	c := *d
	c.objects = make(map[uint16]*dvbObject, len(d.objects))
	for id, o := range d.objects {
		oc := *o
		oc.writes = append([]dvbPixelWrite(nil), o.writes...)
		c.objects[id] = &oc
	}
	c.regions = make(map[uint8]*dvbRegion, len(d.regions))
	for id, r := range d.regions {
		rc := *r
		rc.refs = append([]dvbObjectRef(nil), r.refs...)
		rc.indices = append([]uint8(nil), r.indices...)
		c.regions[id] = &rc
	}
	c.cluts = make(map[uint8]*dvbCLUT, len(d.cluts))
	for id, cl := range d.cluts {
		cc := *cl
		c.cluts[id] = &cc
	}
	c.pcsRegions = append([]dvbPCSRegion(nil), d.pcsRegions...)
	c.pending = d.pending.clone()
	c.current = nil
	if d.current != nil {
		c.current = &dvbCurrentFrame{
			image:   dvbCloneNRGBA(d.current.image),
			start:   d.current.start,
			timeout: d.current.timeout,
		}
	}
	return &c
}

func (d *DVBSubtitleDecoder) restore(saved *DVBSubtitleDecoder) {
	*d = *saved
}

func (p dvbPendingDisplaySet) clone() dvbPendingDisplaySet {
	c := p
	if p.pcs != nil {
		pc := *p.pcs
		pc.regions = append([]dvbPCSRegion(nil), p.pcs.regions...)
		c.pcs = &pc
	}
	if p.objects != nil {
		c.objects = make(map[uint16]*dvbObject, len(p.objects))
		for id, o := range p.objects {
			oc := *o
			oc.writes = append([]dvbPixelWrite(nil), o.writes...)
			c.objects[id] = &oc
		}
	}
	if p.regions != nil {
		c.regions = make(map[uint8]*dvbPendingRegion, len(p.regions))
		for id, r := range p.regions {
			rc := *r
			rc.refs = append([]dvbObjectRef(nil), r.refs...)
			c.regions[id] = &rc
		}
	}
	if p.cluts != nil {
		c.cluts = make(map[uint8]*dvbPendingCLUT, len(p.cluts))
		for id, cl := range p.cluts {
			cc := *cl
			c.cluts[id] = &cc
		}
	}
	return c
}
