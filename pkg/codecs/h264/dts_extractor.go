package h264

import (
	"bytes"
	"fmt"

	"github.com/bluenviron/mediacommon/v2/pkg/bits"
)

const (
	maxReorderedFrames = 10
	/*
		(max_size(first_mb_in_slice) + max_size(slice_type) + max_size(pic_parameter_set_id) +
		max_size(frame_num) + max_size(pic_order_cnt_lsb)) * 4 / 3 =
		(3 * max_size(golomb) + (max(Log2MaxFrameNumMinus4) + 4) / 8 + (max(Log2MaxPicOrderCntLsbMinus4) + 4) / 8) * 4 / 3 =
		(3 * 4 + 2 + 2) * 4 / 3 = 22
	*/
	maxBytesToGetPOC = 22
)

func getPictureOrderCount(buf []byte, sps *SPS, idr bool) (uint32, error) {
	buf = buf[1:]
	lb := min(len(buf), maxBytesToGetPOC)

	buf = EmulationPreventionRemove(buf[:lb])
	pos := 0

	_, err := bits.ReadGolombUnsigned(buf, &pos) // first_mb_in_slice
	if err != nil {
		return 0, err
	}

	_, err = bits.ReadGolombUnsigned(buf, &pos) // slice_type
	if err != nil {
		return 0, err
	}

	_, err = bits.ReadGolombUnsigned(buf, &pos) // pic_parameter_set_id
	if err != nil {
		return 0, err
	}

	_, err = bits.ReadBits(buf, &pos, int(sps.Log2MaxFrameNumMinus4+4)) // frame_num
	if err != nil {
		return 0, err
	}

	if idr {
		_, err = bits.ReadGolombUnsigned(buf, &pos) // idr_pic_id
		if err != nil {
			return 0, err
		}
	}

	picOrderCntLsb, err := bits.ReadBits(buf, &pos, int(sps.Log2MaxPicOrderCntLsbMinus4+4))
	if err != nil {
		return 0, err
	}

	return uint32(picOrderCntLsb), nil
}

func pictureOrderCountDiff(a uint32, b uint32, sps *SPS) int32 {
	maxVal := uint32(1 << (sps.Log2MaxPicOrderCntLsbMinus4 + 4))
	d := (a - b) & (maxVal - 1)
	if d > (maxVal / 2) {
		return int32(d) - int32(maxVal)
	}
	return int32(d)
}

type spsEntry struct {
	raw   []byte
	value SPS
}

type ppsEntry struct {
	raw   []byte
	value PPS
}

type sliceInfo struct {
	ppsID    uint32
	frameNum uint32
	idrPicID uint32
	pocLsb   uint32
	isIDR    bool
	hasPOC   bool
}

type parsedSliceIDs struct {
	ppsID uint32
	rbsp  []byte
	pos   int
	isIDR bool
}

type pictureKind uint8

const (
	pictureIDR pictureKind = iota
	pictureRecovery
	pictureNormal
)

// DTSExtractor computes DTS from PTS.
type DTSExtractor struct {
	sps             []byte
	spsp            *SPS
	prevDTSFilled   bool
	prevDTS         int64
	randomReceived  bool
	expectedPOC     uint32
	reorderedFrames int
	pause           int
	auCount         int
	pocIncrement    int
	prevPOC         uint32
	prevPrevPOC     uint32

	spsByID      map[uint32]*spsEntry
	ppsByID      map[uint32]*ppsEntry
	seenPPS      bool
	activeSPSID  uint32
	activeSPSRaw []byte
	activeSPS    *SPS
}

// Initialize initializes a DTSExtractor.
func (d *DTSExtractor) Initialize() {
	d.pocIncrement = 2
}

// NewDTSExtractor allocates a DTSExtractor.
//
// Deprecated: replaced by DTSExtractor.Initialize.
func NewDTSExtractor() *DTSExtractor {
	d := &DTSExtractor{}
	d.Initialize()
	return d
}

func (d *DTSExtractor) ensureMaps() {
	if d.spsByID == nil {
		d.spsByID = make(map[uint32]*spsEntry)
	}
	if d.ppsByID == nil {
		d.ppsByID = make(map[uint32]*ppsEntry)
	}
}

func (d *DTSExtractor) storeSPS(nalu []byte) (*spsEntry, error) {
	var sps SPS
	err := sps.Unmarshal(nalu)
	if err != nil {
		return nil, fmt.Errorf("invalid SPS: %w", err)
	}

	if sps.ID > maxSPSID {
		return nil, fmt.Errorf("invalid SPS id: %d", sps.ID)
	}

	if sps.Log2MaxFrameNumMinus4 > 12 {
		return nil, fmt.Errorf("invalid log2_max_frame_num_minus4: %d", sps.Log2MaxFrameNumMinus4)
	}

	if sps.PicOrderCntType == 0 && sps.Log2MaxPicOrderCntLsbMinus4 > 12 {
		return nil, fmt.Errorf("invalid log2_max_pic_order_cnt_lsb_minus4: %d",
			sps.Log2MaxPicOrderCntLsbMinus4)
	}

	if e, ok := d.spsByID[sps.ID]; ok && bytes.Equal(e.raw, nalu) {
		return e, nil
	}

	raw := make([]byte, len(nalu))
	copy(raw, nalu)

	e := &spsEntry{
		raw:   raw,
		value: sps,
	}
	d.spsByID[sps.ID] = e

	return e, nil
}

func (d *DTSExtractor) storePPS(nalu []byte) (*ppsEntry, error) {
	var pps PPS
	err := pps.Unmarshal(nalu)
	if err != nil {
		return nil, fmt.Errorf("invalid PPS: %w", err)
	}

	if e, ok := d.ppsByID[pps.ID]; ok && bytes.Equal(e.raw, nalu) {
		return e, nil
	}

	raw := make([]byte, len(nalu))
	copy(raw, nalu)

	e := &ppsEntry{
		raw:   raw,
		value: pps,
	}
	d.ppsByID[pps.ID] = e

	return e, nil
}

func (d *DTSExtractor) spsSnapshot() map[uint32]*spsEntry {
	m := make(map[uint32]*spsEntry, len(d.spsByID))
	for id, e := range d.spsByID {
		m[id] = e
	}
	return m
}

func (d *DTSExtractor) ppsSnapshot() map[uint32]*ppsEntry {
	m := make(map[uint32]*ppsEntry, len(d.ppsByID))
	for id, e := range d.ppsByID {
		m[id] = e
	}
	return m
}

// useActiveSPS makes the SPS referenced by a picture the active one.
// A change of the active SPS is only allowed on IDR pictures.
func (d *DTSExtractor) useActiveSPS(entry *spsEntry, isIDR bool) error {
	changed := d.activeSPS == nil ||
		d.activeSPSID != entry.value.ID ||
		!bytes.Equal(d.activeSPSRaw, entry.raw)
	if !changed {
		return nil
	}

	if d.activeSPS != nil && !isIDR {
		return fmt.Errorf("SPS %d is referenced by a non-IDR picture, "+
			"but the active SPS is %d", entry.value.ID, d.activeSPSID)
	}

	spsCopy := entry.value
	d.activeSPSID = spsCopy.ID
	d.activeSPSRaw = entry.raw
	d.activeSPS = &spsCopy
	d.spsp = d.activeSPS

	// reset POC derivation for the new configuration,
	// but keep the DTS lower bound (prevDTS).
	resetPOCDerivation(d)

	return nil
}

func resetPOCDerivation(d *DTSExtractor) {
	d.randomReceived = false
	d.expectedPOC = 0
	d.reorderedFrames = 0
	d.pause = 0
	d.auCount = 0
	d.pocIncrement = 2
	d.prevPOC = 0
	d.prevPrevPOC = 0
}

func (d *DTSExtractor) computePOCDTS(
	kind pictureKind,
	poc uint32,
	sps *SPS,
	pts int64,
) (int64, bool, error) {
	var ptsDTSDiff int

	switch kind {
	case pictureIDR, pictureRecovery:
		d.expectedPOC = poc

		if d.pocIncrement == 2 {
			if (d.expectedPOC % 2) != 0 {
				d.pocIncrement = 1
			}

			d.auCount = 1
			d.prevPrevPOC = 0
			d.prevPOC = d.expectedPOC
		}

		ptsDTSDiff = 0

	case pictureNormal:
		if d.auCount < 5 && d.pocIncrement == 2 {
			if (poc % 2) != 0 {
				d.pocIncrement = 1
				d.expectedPOC /= 2

				if d.reorderedFrames != 0 {
					increase := d.reorderedFrames
					if (d.reorderedFrames + increase) > maxReorderedFrames {
						return 0, false, fmt.Errorf("too many reordered frames (%d)",
							d.reorderedFrames+increase)
					}

					d.reorderedFrames += increase
					d.pause += increase
				}
			} else if d.auCount >= 2 && (poc%4) == 0 &&
				poc == (d.prevPOC+4) && d.prevPOC == (d.prevPrevPOC+4) {
				d.pocIncrement = 4
				d.expectedPOC *= 2
			}

			d.auCount++
			d.prevPrevPOC = d.prevPOC
			d.prevPOC = poc
		}

		d.expectedPOC += uint32(d.pocIncrement)
		d.expectedPOC &= ((1 << (sps.Log2MaxPicOrderCntLsbMinus4 + 4)) - 1)

		ptsDTSDiff = int(pictureOrderCountDiff(poc, d.expectedPOC, sps)) / d.pocIncrement
	}

	d.randomReceived = true
	ptsDTSDiff += d.reorderedFrames

	switch {
	case ptsDTSDiff > (2*d.reorderedFrames + 1):
		increase := ptsDTSDiff - (2*d.reorderedFrames + 1)
		if (d.reorderedFrames + increase) > maxReorderedFrames {
			return 0, false, fmt.Errorf("too many reordered frames (%d)", d.reorderedFrames+increase)
		}

		d.reorderedFrames += increase
		d.pause += increase
		ptsDTSDiff += increase

	case ptsDTSDiff < 0:
		increase := -ptsDTSDiff
		if (d.reorderedFrames + increase) > maxReorderedFrames {
			return 0, false, fmt.Errorf("too many reordered frames (%d)", d.reorderedFrames+increase)
		}

		d.reorderedFrames += increase
		d.pause += increase
		ptsDTSDiff += increase
	}

	if d.pause > 0 {
		d.pause--
		if !d.prevDTSFilled {
			return pts, true, nil
		}
		return d.prevDTS + 90, true, nil
	}

	if !d.prevDTSFilled {
		return pts, true, nil
	}

	return d.prevDTS + (pts-d.prevDTS)/(int64(ptsDTSDiff)+1), false, nil
}

func parseSliceIDs(
	nalu []byte,
	isIDR bool,
	ppsMap map[uint32]*ppsEntry,
	spsMap map[uint32]*spsEntry,
) (parsedSliceIDs, *spsEntry, error) {
	p := parsedSliceIDs{
		isIDR: isIDR,
	}

	if len(nalu) < 1 {
		return p, nil, fmt.Errorf("not enough bits")
	}

	typ := NALUType(nalu[0] & 0x1F)
	if isIDR {
		if typ != NALUTypeIDR {
			return p, nil, fmt.Errorf("not an IDR NALU")
		}
	} else if typ != NALUTypeNonIDR {
		return p, nil, fmt.Errorf("not a non-IDR NALU")
	}

	p.rbsp = EmulationPreventionRemove(nalu[1:])

	var err error
	_, err = bits.ReadGolombUnsigned(p.rbsp, &p.pos) // first_mb_in_slice
	if err != nil {
		return p, nil, err
	}

	_, err = bits.ReadGolombUnsigned(p.rbsp, &p.pos) // slice_type
	if err != nil {
		return p, nil, err
	}

	p.ppsID, err = bits.ReadGolombUnsigned(p.rbsp, &p.pos) // pic_parameter_set_id
	if err != nil {
		return p, nil, err
	}

	if p.ppsID > maxPPSID {
		return p, nil, fmt.Errorf("invalid pic_parameter_set_id: %d", p.ppsID)
	}

	pps, ok := ppsMap[p.ppsID]
	if !ok {
		return p, nil, fmt.Errorf("PPS %d was not received yet", p.ppsID)
	}

	sps, ok := spsMap[pps.value.SPSID]
	if !ok {
		return p, nil, fmt.Errorf("SPS %d referenced by PPS %d was not received yet",
			pps.value.SPSID, p.ppsID)
	}

	return p, sps, nil
}

func completeSliceHeader(p parsedSliceIDs, sps *SPS) (sliceInfo, error) {
	si := sliceInfo{
		ppsID: p.ppsID,
		isIDR: p.isIDR,
	}

	var err error
	var v uint64
	v, err = bits.ReadBits(p.rbsp, &p.pos, int(sps.Log2MaxFrameNumMinus4+4))
	if err != nil {
		return si, err
	}
	si.frameNum = uint32(v)

	if p.isIDR {
		si.idrPicID, err = bits.ReadGolombUnsigned(p.rbsp, &p.pos) // idr_pic_id
		if err != nil {
			return si, err
		}
	}

	if sps.PicOrderCntType == 0 {
		v, err = bits.ReadBits(p.rbsp, &p.pos, int(sps.Log2MaxPicOrderCntLsbMinus4+4))
		if err != nil {
			return si, err
		}
		si.pocLsb = uint32(v)

		si.hasPOC = true
	}

	return si, nil
}

func validateSlice(first sliceInfo, cur sliceInfo, naluIndex int) error {
	if cur.ppsID != first.ppsID {
		return fmt.Errorf("slice at NALU %d references PPS %d, "+
			"but the first slice of the picture references PPS %d",
			naluIndex, cur.ppsID, first.ppsID)
	}

	if cur.isIDR != first.isIDR {
		return fmt.Errorf("slice at NALU %d has a different IDR identity than the first slice "+
			"of the picture", naluIndex)
	}

	if cur.frameNum != first.frameNum {
		return fmt.Errorf("slice at NALU %d has frame_num %d, "+
			"but the first slice of the picture has frame_num %d",
			naluIndex, cur.frameNum, first.frameNum)
	}

	if cur.isIDR && cur.idrPicID != first.idrPicID {
		return fmt.Errorf("slice at NALU %d has idr_pic_id %d, "+
			"but the first slice of the picture has idr_pic_id %d",
			naluIndex, cur.idrPicID, first.idrPicID)
	}

	if first.hasPOC && cur.pocLsb != first.pocLsb {
		return fmt.Errorf("slice at NALU %d has pic_order_cnt_lsb %d, "+
			"but the first slice of the picture has pic_order_cnt_lsb %d",
			naluIndex, cur.pocLsb, first.pocLsb)
	}

	return nil
}

func hasPPSBeforeFirstPicture(au [][]byte) bool {
	for _, nalu := range au {
		if len(nalu) == 0 {
			continue
		}

		switch NALUType(nalu[0] & 0x1F) {
		case NALUTypePPS:
			return true

		case NALUTypeIDR, NALUTypeNonIDR:
			return false
		}
	}

	return false
}

func (d *DTSExtractor) extractInnerLegacy(au [][]byte, pts int64) (int64, bool, error) {
	var idr []byte
	var nonIDR []byte
	var seiRecoveryPoint bool
	var pictureBeforeSEIFound bool
	pictureFound := false

	for _, nalu := range au {
		if len(nalu) == 0 {
			return 0, false, fmt.Errorf("NALU is empty")
		}

		typ := NALUType(nalu[0] & 0x1F)

		switch typ {
		case NALUTypeSPS:
			entry, err := d.storeSPS(nalu)
			if err != nil {
				return 0, false, err
			}

			if pictureFound {
				// Parameter sets declared after the picture can only
				// affect subsequent pictures.
				continue
			}

			if !bytes.Equal(d.sps, nalu) {
				spsCopy := entry.value
				d.sps = entry.raw
				d.spsp = &spsCopy
				d.activeSPSID = spsCopy.ID
				d.activeSPSRaw = entry.raw
				d.activeSPS = d.spsp

				resetPOCDerivation(d)
			}

		case NALUTypePPS:
			// Once a PPS has been seen, parameter sets are resolved explicitly.
			d.seenPPS = true
			_, err := d.storePPS(nalu)
			if err != nil {
				return 0, false, err
			}

		case NALUTypeIDR:
			if !pictureFound {
				idr = nalu
				pictureFound = true
			}

		case NALUTypeNonIDR:
			if !pictureFound {
				nonIDR = nalu
				pictureFound = true
				pictureBeforeSEIFound = !seiRecoveryPoint
			}

		case NALUTypeSEI:
			if !pictureFound && !pictureBeforeSEIFound && isSEIRecoveryPoint(nalu) {
				seiRecoveryPoint = true
			}
		}
	}

	if d.spsp == nil {
		return 0, false, fmt.Errorf("SPS not received yet")
	}

	if d.spsp.PicOrderCntType == 2 || !d.spsp.FrameMbsOnlyFlag {
		return pts, false, nil
	}

	if d.spsp.PicOrderCntType == 1 {
		return 0, false, fmt.Errorf("pic_order_cnt_type = 1 is not supported yet")
	}

	if !d.randomReceived && idr == nil &&
		(pictureBeforeSEIFound || !seiRecoveryPoint || nonIDR == nil) {
		return 0, false, fmt.Errorf("random access frame not received yet")
	}

	switch {
	case idr != nil:
		poc, err := getPictureOrderCount(idr, d.spsp, true)
		if err != nil {
			return 0, false, err
		}

		return d.computePOCDTS(pictureIDR, poc, d.spsp, pts)

	case nonIDR != nil && seiRecoveryPoint && !pictureBeforeSEIFound:
		poc, err := getPictureOrderCount(nonIDR, d.spsp, false)
		if err != nil {
			return 0, false, err
		}

		return d.computePOCDTS(pictureRecovery, poc, d.spsp, pts)

	case nonIDR != nil:
		poc, err := getPictureOrderCount(nonIDR, d.spsp, false)
		if err != nil {
			return 0, false, err
		}

		return d.computePOCDTS(pictureNormal, poc, d.spsp, pts)

	default:
		return 0, false, fmt.Errorf("access unit doesn't contain an IDR or non-IDR NALU")
	}
}

func (d *DTSExtractor) extractInnerWithParameterSets(au [][]byte, pts int64) (int64, bool, error) {
	d.ensureMaps()

	var (
		first            sliceInfo
		firstSPS         *spsEntry
		haveFirst        bool
		idrFound         bool
		nonIDRFound      bool
		seiRecoveryPoint bool
		pictureBeforeSEI bool
		returnPTS        bool
		spsSnapshot      map[uint32]*spsEntry
		ppsSnapshot      map[uint32]*ppsEntry
	)

	for i, nalu := range au {
		if len(nalu) == 0 {
			return 0, false, fmt.Errorf("NALU %d is empty", i)
		}

		switch NALUType(nalu[0] & 0x1F) {
		case NALUTypeSPS:
			_, err := d.storeSPS(nalu)
			if err != nil {
				return 0, false, err
			}

		case NALUTypePPS:
			d.seenPPS = true
			_, err := d.storePPS(nalu)
			if err != nil {
				return 0, false, err
			}

		case NALUTypeSEI:
			if !pictureBeforeSEI && isSEIRecoveryPoint(nalu) {
				seiRecoveryPoint = true
			}

		case NALUTypeIDR, NALUTypeNonIDR:
			isIDR := NALUType(nalu[0]&0x1F) == NALUTypeIDR

			if !haveFirst {
				spsSnapshot = d.spsSnapshot()
				ppsSnapshot = d.ppsSnapshot()

				p, spsEntry, err := parseSliceIDs(nalu, isIDR, ppsSnapshot, spsSnapshot)
				if err != nil {
					return 0, false, fmt.Errorf("slice at NALU %d: %w", i, err)
				}

				err = d.useActiveSPS(spsEntry, isIDR)
				if err != nil {
					return 0, false, fmt.Errorf("slice at NALU %d: %w", i, err)
				}

				sps := &spsEntry.value

				if sps.SeparateColourPlaneFlag {
					return 0, false, fmt.Errorf("separate colour planes are not supported")
				}

				if !sps.FrameMbsOnlyFlag {
					return pts, false, nil
				}

				if sps.PicOrderCntType == 1 {
					return 0, false, fmt.Errorf("pic_order_cnt_type = 1 is not supported yet")
				}

				returnPTS = sps.PicOrderCntType == 2

				si, err := completeSliceHeader(p, sps)
				if err != nil {
					return 0, false, fmt.Errorf("slice at NALU %d: %w", i, err)
				}

				first = si
				firstSPS = spsEntry
				haveFirst = true
				pictureBeforeSEI = !seiRecoveryPoint
			} else {
				p, _, err := parseSliceIDs(nalu, isIDR, ppsSnapshot, spsSnapshot)
				if err != nil {
					return 0, false, fmt.Errorf("slice at NALU %d: %w", i, err)
				}

				si, err := completeSliceHeader(p, &firstSPS.value)
				if err != nil {
					return 0, false, fmt.Errorf("slice at NALU %d: %w", i, err)
				}

				err = validateSlice(first, si, i)
				if err != nil {
					return 0, false, err
				}
			}

			if isIDR {
				idrFound = true
			} else {
				nonIDRFound = true
			}
		}
	}

	if !haveFirst {
		if len(d.spsByID) == 0 && d.activeSPS == nil {
			return 0, false, fmt.Errorf("SPS not received yet")
		}

		if !d.randomReceived {
			return 0, false, fmt.Errorf("random access frame not received yet")
		}

		return 0, false, fmt.Errorf("access unit doesn't contain an IDR or non-IDR NALU")
	}

	if returnPTS {
		return pts, false, nil
	}

	if !d.randomReceived && !idrFound &&
		(pictureBeforeSEI || !seiRecoveryPoint || !nonIDRFound) {
		return 0, false, fmt.Errorf("random access frame not received yet")
	}

	var kind pictureKind
	switch {
	case idrFound:
		kind = pictureIDR
	case seiRecoveryPoint && !pictureBeforeSEI:
		kind = pictureRecovery
	default:
		kind = pictureNormal
	}

	return d.computePOCDTS(kind, first.pocLsb, &firstSPS.value, pts)
}

func (d *DTSExtractor) extractInner(au [][]byte, pts int64) (int64, bool, error) {
	d.ensureMaps()

	if d.seenPPS || hasPPSBeforeFirstPicture(au) {
		return d.extractInnerWithParameterSets(au, pts)
	}

	return d.extractInnerLegacy(au, pts)
}

// Extract extracts the DTS of an access unit.
// AU is assumed to contain byte slices each with at least 1 byte.
func (d *DTSExtractor) Extract(au [][]byte, pts int64) (int64, error) {
	dts, skipChecks, err := d.extractInner(au, pts)
	if err != nil {
		return 0, err
	}

	if !skipChecks && dts > pts {
		return 0, fmt.Errorf("DTS is greater than PTS")
	}

	if d.prevDTSFilled && dts < d.prevDTS {
		return 0, fmt.Errorf("DTS is not monotonically increasing, was %v, now is %v",
			d.prevDTS, dts)
	}

	d.prevDTSFilled = true
	d.prevDTS = dts

	return dts, err
}
