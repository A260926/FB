// compact_tables.go holds the per-track analysis and output table
// construction for CompactEditedFile. Everything here is pure computation:
// it reads the decoded input box tree and builds new boxes, without touching
// the input tree, the input bytes, or any writer.

package mp4

import (
	"fmt"
	"math"
	"sort"
)

// compactTrack is the validated analysis of one source track plus the
// computed output track content.
type compactTrack struct {
	src         *TrakBox
	handlerType string
	timescale   uint32

	nrSamples uint32
	dts       []uint64 // decode time per sample
	dur       []uint32 // duration per sample
	cto       []int32  // composition offset per sample (zeros without ctts)
	sync      []bool
	size      []uint32
	sdid      []uint32 // sample description index per sample
	offset    []uint64 // absolute input file offset per sample

	hasElst        bool
	elstVersion    byte
	elstFlags      uint32
	outElstEntries []ElstEntry // nil when the source track has no elst
	outElstVersion byte

	components    []compactComponent
	mediaDuration uint64 // output media duration (mdhd)

	stts *SttsBox
	ctts *CttsBox // nil when the source track has no ctts
	stss *StssBox // nil when the source track has no stss
	stsc *StscBox
	stsz *StszBox

	chunks      []compactChunk
	copies      []payloadCopy
	payloadSize uint64

	outStbl *StblBox // set by buildTrak, used to fill chunk offsets
}

// compactComponent is one maximal run of consecutive source samples that
// ends up in the output, the union of overlapping or adjacent edit runs.
type compactComponent struct {
	first, last uint32 // source sample numbers, 1-based inclusive
	newDTS      uint64 // decode time of the component in the output track
}

// compactChunk is one output chunk: consecutive output samples sharing one
// sample description, never crossing a component boundary.
type compactChunk struct {
	nrSamples uint32
	sdid      uint32
	size      uint64
	offset    uint64 // absolute output file offset, set during layout
}

// compactEdit is one parsed media segment of an edit list.
type compactEdit struct {
	entryIndex int    // index in the source elst entries
	start, end uint64 // media interval [start, end) in media ticks
	runFirst   uint32
	runLast    uint32
}

// stblKeep lists the stbl children that CompactEditedFile regenerates or
// carries over; anything else that holds per-sample indices would silently
// go stale, so it is rejected instead. free and skip are padding and simply
// dropped from the output.
var stblKeep = map[string]bool{
	"stsd": true, "stts": true, "ctts": true, "stss": true,
	"stsz": true, "stsc": true, "stco": true, "co64": true,
	"free": true, "skip": true,
}

// analyzeCompactTrack validates trak and computes everything needed for the
// output track: the kept samples, the new sample tables, the chunk layout,
// the payload copies and the rewritten edit list.
func analyzeCompactTrack(trak *TrakBox, movieTimescale uint32, mdatPayloadStart, mdatPayloadEnd uint64) (*compactTrack, error) {
	if trak.Tkhd == nil {
		return nil, fmt.Errorf("no tkhd box")
	}
	if trak.Mdia == nil || trak.Mdia.Mdhd == nil || trak.Mdia.Hdlr == nil {
		return nil, fmt.Errorf("incomplete mdia box")
	}
	if trak.Mdia.Minf == nil || trak.Mdia.Minf.Stbl == nil {
		return nil, fmt.Errorf("incomplete minf box")
	}
	mdhd := trak.Mdia.Mdhd
	if mdhd.Timescale == 0 {
		return nil, fmt.Errorf("media timescale is zero")
	}
	stbl := trak.Mdia.Minf.Stbl
	for _, child := range stbl.Children {
		if !stblKeep[child.Type()] {
			return nil, fmt.Errorf("unsupported per-sample table %s in stbl", child.Type())
		}
	}
	if stbl.Stsd == nil || stbl.Stts == nil || stbl.Stsc == nil || stbl.Stsz == nil {
		return nil, fmt.Errorf("stbl misses stsd, stts, stsc or stsz")
	}
	if stbl.Stco == nil && stbl.Co64 == nil {
		return nil, fmt.Errorf("stbl has neither stco nor co64")
	}
	handlerType := trak.Mdia.Hdlr.HandlerType
	if handlerType != "vide" && handlerType != "soun" {
		return nil, fmt.Errorf("unsupported handler type %q", handlerType)
	}
	if err := trak.CheckDataIsSelfContained(); err != nil {
		return nil, err
	}

	ct := &compactTrack{
		src:         trak,
		handlerType: handlerType,
		timescale:   mdhd.Timescale,
	}
	if err := ct.readSampleTables(stbl); err != nil {
		return nil, err
	}
	if err := ct.readSampleLocations(stbl, mdatPayloadStart, mdatPayloadEnd); err != nil {
		return nil, err
	}
	if err := ct.selectAndLayout(movieTimescale); err != nil {
		return nil, err
	}
	if err := ct.buildTables(stbl); err != nil {
		return nil, err
	}
	return ct, nil
}

// readSampleTables reads and validates the per-sample timing, sync and size
// tables, checking that they cover exactly the declared sample count.
func (ct *compactTrack) readSampleTables(stbl *StblBox) error {
	n := stbl.Stsz.GetNrSamples()
	ct.nrSamples = n
	if stbl.Stsz.SampleUniformSize == 0 && uint32(len(stbl.Stsz.SampleSize)) != n {
		return fmt.Errorf("stsz lists %d sizes for %d samples", len(stbl.Stsz.SampleSize), n)
	}
	durs, err := stbl.Stts.SampleDurations(n)
	if err != nil {
		return fmt.Errorf("inconsistent stts: %w", err)
	}
	ct.dur = durs
	ct.dts = make([]uint64, n)
	var acc uint64
	for i := range durs {
		if acc > math.MaxInt64 {
			return fmt.Errorf("decode time overflow")
		}
		ct.dts[i] = acc
		acc += uint64(durs[i])
	}
	ct.cto = make([]int32, n)
	if stbl.Ctts != nil {
		ctos, err := stbl.Ctts.CompositionTimeOffsets(n)
		if err != nil {
			return fmt.Errorf("inconsistent ctts: %w", err)
		}
		ct.cto = ctos
	}
	ct.sync = make([]bool, n)
	if stbl.Stss != nil {
		var prev uint32
		for _, nr := range stbl.Stss.SampleNumber {
			if nr == 0 || nr > n {
				return fmt.Errorf("stss sample number %d outside 1-%d", nr, n)
			}
			if nr <= prev {
				return fmt.Errorf("stss sample numbers not strictly increasing")
			}
			prev = nr
			ct.sync[nr-1] = true
		}
	} else {
		for i := range ct.sync {
			ct.sync[i] = true
		}
	}
	ct.size = make([]uint32, n)
	for i := uint32(1); i <= n; i++ {
		ct.size[i-1] = stbl.Stsz.GetSampleSize(int(i))
	}
	return nil
}

// readSampleLocations walks stsc and the chunk offset table to compute the
// sample description index and the absolute input offset of every sample,
// validating coverage and that all sample data lies inside the mdat payload.
func (ct *compactTrack) readSampleLocations(stbl *StblBox, mdatPayloadStart, mdatPayloadEnd uint64) error {
	var chunkOffsets []uint64
	if stbl.Stco != nil {
		for _, o := range stbl.Stco.ChunkOffset {
			chunkOffsets = append(chunkOffsets, uint64(o))
		}
	} else {
		chunkOffsets = append(chunkOffsets, stbl.Co64.ChunkOffset...)
	}
	nrChunks := uint32(len(chunkOffsets))
	n := ct.nrSamples
	ct.sdid = make([]uint32, n)
	ct.offset = make([]uint64, n)
	entries := stbl.Stsc.Entries
	if n == 0 {
		if len(entries) != 0 || nrChunks != 0 {
			return fmt.Errorf("stsc/stco declare chunks but stsz declares no samples")
		}
		return nil
	}
	if len(entries) == 0 {
		return fmt.Errorf("stsc has no entries for %d samples", n)
	}
	if entries[0].FirstChunk != 1 {
		return fmt.Errorf("first stsc entry has firstChunk %d, not 1", entries[0].FirstChunk)
	}
	nrDescriptions := uint32(len(stbl.Stsd.Children))
	sample := uint32(1)
	for i, e := range entries {
		if e.SamplesPerChunk == 0 {
			return fmt.Errorf("stsc entry %d has zero samples per chunk", i+1)
		}
		if e.FirstChunk == 0 || e.FirstChunk > nrChunks {
			return fmt.Errorf("stsc entry %d has firstChunk %d outside 1-%d", i+1, e.FirstChunk, nrChunks)
		}
		nextFirst := nrChunks + 1
		if i+1 < len(entries) {
			nextFirst = entries[i+1].FirstChunk
			if nextFirst <= e.FirstChunk {
				return fmt.Errorf("stsc firstChunk values not increasing")
			}
		}
		sdid := stbl.Stsc.GetSampleDescriptionID(int(e.FirstChunk))
		if sdid == 0 || sdid > nrDescriptions {
			return fmt.Errorf("stsc entry %d refers to sample description %d of %d", i+1, sdid, nrDescriptions)
		}
		for chunkNr := e.FirstChunk; chunkNr < nextFirst; chunkNr++ {
			off := chunkOffsets[chunkNr-1]
			for s := uint32(0); s < e.SamplesPerChunk; s++ {
				if sample > n {
					return fmt.Errorf("stsc declares more samples than the %d in stsz", n)
				}
				end, err := addUint64(off, uint64(ct.size[sample-1]))
				if err != nil {
					return err
				}
				if off < mdatPayloadStart || end > mdatPayloadEnd {
					return fmt.Errorf("sample %d data range [%d, %d) outside mdat payload [%d, %d)",
						sample, off, end, mdatPayloadStart, mdatPayloadEnd)
				}
				ct.sdid[sample-1] = sdid
				ct.offset[sample-1] = off
				off = end
				sample++
			}
		}
	}
	if sample != n+1 {
		return fmt.Errorf("stsc covers %d samples, stsz declares %d", sample-1, n)
	}
	return nil
}

// mediaTimelineEnd returns the end of the media timeline that edits may
// reference: the latest presentation end for video, the total duration for
// audio.
func (ct *compactTrack) mediaTimelineEnd() int64 {
	if ct.handlerType == "vide" {
		var end int64
		for i := range ct.dts {
			pe := int64(ct.dts[i]) + int64(ct.cto[i]) + int64(ct.dur[i])
			if pe > end {
				end = pe
			}
		}
		return end
	}
	if len(ct.dts) == 0 {
		return 0
	}
	last := len(ct.dts) - 1
	return int64(ct.dts[last]) + int64(ct.dur[last])
}

// selectAndLayout parses the edit list, selects the kept samples, merges the
// kept runs into components and computes the output edit list entries.
func (ct *compactTrack) selectAndLayout(movieTimescale uint32) error {
	trak := ct.src
	var elst *ElstBox
	if trak.Edts != nil && len(trak.Edts.Elst) > 0 && len(trak.Edts.Elst[0].Entries) > 0 {
		elst = trak.Edts.Elst[0]
	}
	if elst == nil {
		// No edit list: the whole track is one kept interval and the
		// presentation timeline is kept as it is.
		if ct.nrSamples > 0 {
			ct.components = []compactComponent{{first: 1, last: ct.nrSamples}}
		}
		return ct.layoutComponents()
	}
	ct.hasElst = true
	ct.elstVersion = elst.Version
	ct.elstFlags = elst.Flags
	ct.outElstEntries = make([]ElstEntry, len(elst.Entries))
	copy(ct.outElstEntries, elst.Entries)

	mediaEnd := ct.mediaTimelineEnd()
	var edits []compactEdit
	for i, entry := range elst.Entries {
		if entry.MediaTime == -1 {
			continue // empty edit: no media
		}
		if entry.MediaTime < 0 {
			return fmt.Errorf("elst entry %d has invalid media_time %d", i+1, entry.MediaTime)
		}
		if entry.MediaRateInteger != 1 || entry.MediaRateFraction != 0 {
			return fmt.Errorf("elst entry %d has non-unit media rate", i+1)
		}
		if entry.SegmentDuration == 0 {
			return fmt.Errorf("elst entry %d is a media segment with zero duration", i+1)
		}
		segDurMedia, err := mulDivExact(entry.SegmentDuration, uint64(ct.timescale), uint64(movieTimescale))
		if err != nil {
			return fmt.Errorf("elst entry %d: %w", i+1, err)
		}
		start := uint64(entry.MediaTime)
		end, err := addUint64(start, segDurMedia)
		if err != nil {
			return fmt.Errorf("elst entry %d: %w", i+1, err)
		}
		if end > math.MaxInt64 {
			return fmt.Errorf("elst entry %d: media interval overflow", i+1)
		}
		if int64(start) >= mediaEnd || int64(end) > mediaEnd {
			return fmt.Errorf("elst entry %d: media interval [%d, %d) outside media timeline [0, %d)",
				i+1, start, end, mediaEnd)
		}
		edits = append(edits, compactEdit{entryIndex: i, start: start, end: end})
	}

	runs := make([]compactComponent, 0, len(edits))
	for i := range edits {
		first, last, err := ct.selectRun(edits[i])
		if err != nil {
			return fmt.Errorf("elst entry %d: %w", edits[i].entryIndex+1, err)
		}
		edits[i].runFirst, edits[i].runLast = first, last
		runs = append(runs, compactComponent{first: first, last: last})
	}
	ct.components = mergeRuns(runs)
	if err := ct.layoutComponents(); err != nil {
		return err
	}

	// Point every media edit at its own component with the component's shift.
	for _, edit := range edits {
		ci := -1
		for i, c := range ct.components {
			if c.first <= edit.runFirst && edit.runLast <= c.last {
				ci = i
				break
			}
		}
		if ci < 0 {
			return fmt.Errorf("elst entry %d: internal error, no component holds the edit run",
				edit.entryIndex+1)
		}
		comp := ct.components[ci]
		shift := int64(comp.newDTS) - int64(ct.dts[comp.first-1])
		newMediaTime := int64(edit.start) + shift
		if newMediaTime < 0 {
			return fmt.Errorf("elst entry %d: shifted media_time %d is negative",
				edit.entryIndex+1, newMediaTime)
		}
		ct.outElstEntries[edit.entryIndex].MediaTime = newMediaTime
	}
	ct.outElstVersion = ct.elstVersion
	for _, e := range ct.outElstEntries {
		if e.SegmentDuration > math.MaxUint32 || e.MediaTime > math.MaxInt32 || e.MediaTime < math.MinInt32 {
			if ct.outElstVersion < 1 {
				ct.outElstVersion = 1
			}
		}
	}
	return nil
}

// selectRun computes the contiguous decode-order run of source samples that
// one media edit needs, as [first, last] 1-based sample numbers.
func (ct *compactTrack) selectRun(edit compactEdit) (uint32, uint32, error) {
	if ct.handlerType == "vide" {
		return ct.selectVideoRun(edit)
	}
	return ct.selectAudioRun(edit)
}

// selectVideoRun finds the samples whose signed presentation interval
// [dts+cto, dts+cto+dur) intersects the edit, requires that those intervals
// tile the edit exactly, and extends the run to the nearest sync sample at
// or before the first visible sample.
func (ct *compactTrack) selectVideoRun(edit compactEdit) (uint32, uint32, error) {
	type visibleSample struct {
		nr         uint32
		start, end int64
	}
	editStart, editEnd := int64(edit.start), int64(edit.end)
	var visible []visibleSample
	for i := uint32(0); i < ct.nrSamples; i++ {
		pStart := int64(ct.dts[i]) + int64(ct.cto[i])
		pEnd := pStart + int64(ct.dur[i])
		if pStart < editEnd && pEnd > editStart {
			visible = append(visible, visibleSample{nr: i + 1, start: pStart, end: pEnd})
		}
	}
	if len(visible) == 0 {
		return 0, 0, fmt.Errorf("edit selects no samples")
	}
	sort.Slice(visible, func(i, j int) bool { return visible[i].start < visible[j].start })
	cur := editStart
	for _, v := range visible {
		if v.start != cur {
			return 0, 0, fmt.Errorf("presentation intervals do not exactly cover the edit (gap or overlap at %d, want %d)",
				v.start, cur)
		}
		cur = v.end
	}
	if cur != editEnd {
		return 0, 0, fmt.Errorf("presentation intervals end at %d, edit ends at %d", cur, editEnd)
	}
	firstVisible, lastVisible := uint32(math.MaxUint32), uint32(0)
	for _, v := range visible {
		if v.nr < firstVisible {
			firstVisible = v.nr
		}
		if v.nr > lastVisible {
			lastVisible = v.nr
		}
	}
	syncNr := uint32(0)
	for s := firstVisible; s >= 1; s-- {
		if ct.sync[s-1] {
			syncNr = s
			break
		}
	}
	if syncNr == 0 {
		return 0, 0, fmt.Errorf("no sync sample at or before sample %d", firstVisible)
	}
	return syncNr, lastVisible, nil
}

// selectAudioRun keeps every packet whose interval intersects the edit; the
// edit keeps masking the head and tail of the intersected packets.
func (ct *compactTrack) selectAudioRun(edit compactEdit) (uint32, uint32, error) {
	first, last := uint32(0), uint32(0)
	for i := uint32(1); i <= ct.nrSamples; i++ {
		aStart := ct.dts[i-1]
		aEnd := aStart + uint64(ct.dur[i-1])
		if aStart < edit.end && aEnd > edit.start {
			if first == 0 {
				first = i
			}
			last = i
		}
	}
	if first == 0 {
		return 0, 0, fmt.Errorf("edit selects no samples")
	}
	return first, last, nil
}

// mergeRuns merges overlapping or adjacent decode-order runs into sorted
// components.
func mergeRuns(runs []compactComponent) []compactComponent {
	if len(runs) == 0 {
		return nil
	}
	sorted := make([]compactComponent, len(runs))
	copy(sorted, runs)
	sort.Slice(sorted, func(i, j int) bool {
		if sorted[i].first != sorted[j].first {
			return sorted[i].first < sorted[j].first
		}
		return sorted[i].last < sorted[j].last
	})
	components := []compactComponent{sorted[0]}
	for _, r := range sorted[1:] {
		last := &components[len(components)-1]
		if r.first <= last.last+1 {
			if r.last > last.last {
				last.last = r.last
			}
		} else {
			components = append(components, r)
		}
	}
	return components
}

// layoutComponents assigns output decode times to the components and the
// total output media duration.
func (ct *compactTrack) layoutComponents() error {
	var acc uint64
	for i := range ct.components {
		c := &ct.components[i]
		c.newDTS = acc
		compDur := ct.dts[c.last-1] + uint64(ct.dur[c.last-1]) - ct.dts[c.first-1]
		var err error
		acc, err = addUint64(acc, compDur)
		if err != nil {
			return err
		}
	}
	ct.mediaDuration = acc
	return nil
}

// buildTables computes the output sample sequence and builds the new stts,
// ctts, stss, stsz and stsc boxes, the chunk layout and the payload copies.
func (ct *compactTrack) buildTables(stbl *StblBox) error {
	var outSamples []uint32
	for _, c := range ct.components {
		for nr := c.first; nr <= c.last; nr++ {
			outSamples = append(outSamples, nr)
		}
	}

	// stts: run-length encoded durations in output order.
	var sttsCounts, sttsDeltas []uint32
	for _, nr := range outSamples {
		d := ct.dur[nr-1]
		if len(sttsDeltas) > 0 && d == sttsDeltas[len(sttsDeltas)-1] {
			sttsCounts[len(sttsCounts)-1]++
		} else {
			sttsCounts = append(sttsCounts, 1)
			sttsDeltas = append(sttsDeltas, d)
		}
	}
	ct.stts = &SttsBox{
		Version:         stbl.Stts.Version,
		Flags:           stbl.Stts.Flags,
		SampleCount:     sttsCounts,
		SampleTimeDelta: sttsDeltas,
	}

	// ctts: run-length encoded composition offsets in output order, version 1
	// when a negative offset has to be stored.
	if stbl.Ctts != nil {
		var cttsCounts []uint32
		var cttsOffsets []int32
		negative := false
		for _, nr := range outSamples {
			o := ct.cto[nr-1]
			if o < 0 {
				negative = true
			}
			if len(cttsOffsets) > 0 && o == cttsOffsets[len(cttsOffsets)-1] {
				cttsCounts[len(cttsCounts)-1]++
			} else {
				cttsCounts = append(cttsCounts, 1)
				cttsOffsets = append(cttsOffsets, o)
			}
		}
		version := stbl.Ctts.Version
		if negative && version < 1 {
			version = 1
		}
		ct.ctts = &CttsBox{Version: version, Flags: stbl.Ctts.Flags}
		if err := ct.ctts.AddSampleCountsAndOffset(cttsCounts, cttsOffsets); err != nil {
			return err
		}
	}

	// stss: sync sample positions in the output sequence.
	if stbl.Stss != nil {
		var numbers []uint32
		for i, nr := range outSamples {
			if ct.sync[nr-1] {
				numbers = append(numbers, uint32(i+1))
			}
		}
		ct.stss = &StssBox{Version: stbl.Stss.Version, Flags: stbl.Stss.Flags, SampleNumber: numbers}
	}

	// stsz: explicit sizes in output order.
	sizes := make([]uint32, len(outSamples))
	for i, nr := range outSamples {
		sizes[i] = ct.size[nr-1]
	}
	ct.stsz = &StszBox{
		Version:      stbl.Stsz.Version,
		Flags:        stbl.Stsz.Flags,
		SampleNumber: uint32(len(outSamples)),
		SampleSize:   sizes,
	}

	// Chunks: maximal runs of equal sample description inside one component.
	ct.stsc = &StscBox{Version: stbl.Stsc.Version, Flags: stbl.Stsc.Flags}
	ct.copies = nil
	ct.chunks = nil
	ct.payloadSize = 0
	for _, comp := range ct.components {
		var cur *compactChunk
		for nr := comp.first; nr <= comp.last; nr++ {
			sdid := ct.sdid[nr-1]
			sz := uint64(ct.size[nr-1])
			if cur != nil && cur.sdid == sdid {
				cur.nrSamples++
				var err error
				cur.size, err = addUint64(cur.size, sz)
				if err != nil {
					return err
				}
			} else {
				ct.chunks = append(ct.chunks, compactChunk{nrSamples: 1, sdid: sdid, size: sz})
				cur = &ct.chunks[len(ct.chunks)-1]
			}
			// Payload copies: merge ranges that are contiguous in the input.
			off := ct.offset[nr-1]
			if len(ct.copies) > 0 {
				last := &ct.copies[len(ct.copies)-1]
				if last.srcOffset+last.size == off {
					last.size += sz
					continue
				}
			}
			ct.copies = append(ct.copies, payloadCopy{srcOffset: off, size: sz})
		}
	}
	for i := range ct.chunks {
		ct.payloadSize += ct.chunks[i].size // bounded by file size, cannot overflow
		if i == 0 || ct.chunks[i].nrSamples != ct.chunks[i-1].nrSamples || ct.chunks[i].sdid != ct.chunks[i-1].sdid {
			if err := ct.stsc.AddEntry(uint32(i+1), ct.chunks[i].nrSamples, ct.chunks[i].sdid); err != nil {
				return err
			}
		}
	}
	return nil
}

// buildTrak builds the output trak box. All boxes that are not rebuilt are
// shared with the decoded input tree and only read.
func (ct *compactTrack) buildTrak(useCo64 bool) *TrakBox {
	out := &TrakBox{}
	for _, child := range ct.src.Children {
		switch child.Type() {
		case "edts":
			if ct.hasElst {
				edts := &EdtsBox{}
				elst := &ElstBox{
					Version: ct.outElstVersion,
					Flags:   ct.elstFlags,
					Entries: ct.outElstEntries,
				}
				edts.Elst = []*ElstBox{elst}
				edts.AddChild(elst)
				out.AddChild(edts)
			} else {
				out.AddChild(child)
			}
		case "mdia":
			out.AddChild(ct.buildMdia(useCo64))
		default:
			out.AddChild(child)
		}
	}
	return out
}

// buildMdia builds the output mdia box with the new media duration.
func (ct *compactTrack) buildMdia(useCo64 bool) *MdiaBox {
	srcMdia := ct.src.Mdia
	out := &MdiaBox{}
	for _, child := range srcMdia.Children {
		switch child.Type() {
		case "mdhd":
			mdhd := *srcMdia.Mdhd
			mdhd.Duration = ct.mediaDuration
			if ct.mediaDuration > math.MaxUint32 && mdhd.Version < 1 {
				mdhd.Version = 1
			}
			out.AddChild(&mdhd)
		case "minf":
			out.AddChild(ct.buildMinf(useCo64))
		default:
			out.AddChild(child)
		}
	}
	return out
}

// buildMinf builds the output minf box with the rebuilt sample table.
func (ct *compactTrack) buildMinf(useCo64 bool) *MinfBox {
	srcMinf := ct.src.Mdia.Minf
	out := &MinfBox{}
	for _, child := range srcMinf.Children {
		if child.Type() == "stbl" {
			out.AddChild(ct.buildStbl(useCo64))
		} else {
			out.AddChild(child)
		}
	}
	return out
}

// buildStbl assembles the output sample table: the shared sample
// descriptions, the rebuilt tables, and a placeholder chunk offset box that
// fillChunkOffsets completes once the layout is known.
func (ct *compactTrack) buildStbl(useCo64 bool) *StblBox {
	out := NewStblBox()
	out.AddChild(ct.src.Mdia.Minf.Stbl.Stsd)
	out.AddChild(ct.stts)
	if ct.ctts != nil {
		out.AddChild(ct.ctts)
	}
	if ct.stss != nil {
		out.AddChild(ct.stss)
	}
	out.AddChild(ct.stsc)
	out.AddChild(ct.stsz)
	if useCo64 {
		out.AddChild(&Co64Box{ChunkOffset: make([]uint64, len(ct.chunks))})
	} else {
		out.AddChild(&StcoBox{ChunkOffset: make([]uint32, len(ct.chunks))})
	}
	ct.outStbl = out
	return out
}

// fillChunkOffsets writes the laid out chunk offsets into the placeholder
// stco or co64 box of the built track.
func (ct *compactTrack) fillChunkOffsets(useCo64 bool) {
	if useCo64 {
		for i := range ct.chunks {
			ct.outStbl.Co64.ChunkOffset[i] = ct.chunks[i].offset
		}
	} else {
		for i := range ct.chunks {
			ct.outStbl.Stco.ChunkOffset[i] = uint32(ct.chunks[i].offset)
		}
	}
}

// mulDivExact computes value*multiplier/divisor, requiring no overflow and
// no remainder.
func mulDivExact(value, multiplier, divisor uint64) (uint64, error) {
	if divisor == 0 {
		return 0, fmt.Errorf("division by zero")
	}
	if multiplier != 0 && value > math.MaxUint64/multiplier {
		return 0, fmt.Errorf("arithmetic overflow")
	}
	product := value * multiplier
	if product%divisor != 0 {
		return 0, fmt.Errorf("segment duration %d does not convert to whole media ticks", value)
	}
	return product / divisor, nil
}
