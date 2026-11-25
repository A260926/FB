package mp4

import (
	"fmt"
	"io"
	"math"
	"math/big"
	"sort"
)

// DefragmentPreserveGaps is like [Defragment], but recording gaps between
// surviving samples stay blank on the movie timeline instead of extending
// the previous sample's duration.
//
// Without trackIDs every track is kept; with trackIDs only the given track
// IDs are kept (every ID must exist in the moov). Sample collection,
// retransmission/overlap resolution and chunk writing follow the exact same
// path as [Defragment]: later fragments still supersede earlier tails under
// the same sample-boundary, coverage and unresolvable-overlap rules,
// including files with a progressive MP4 prefix.
//
// Once the final surviving samples are known, they are grouped into decode
// islands: consecutive samples whose original DTS and original durations
// touch. The durations added by the ordinary defragmenter to bridge forward
// tfdt jumps are not treated as sample durations, and holes that a later
// retransmission filled disappear like any other resolved overlap. The
// islands of a track are concatenated in source decode order into a new
// media axis starting at zero: original durations, CTS-DTS offsets, sync
// flags, sample descriptions and bytes are unchanged, and every surviving
// sample is physically written once. Edit lists map island coverage back to
// the compacted media and turn everything between islands into empty edits,
// so duplicate edits keep referencing the same media.
//
// Existing edit lists must use rate 1.0 media edits or -1 empty edits;
// media times are absolute source media ticks. Every island/edit cut point
// must convert to an integral number of movie timescale ticks without
// rounding, media edits must not extend past the track source end, and every
// sample's PTS interval inside an island must stay within that island's
// source decode span (normal in-island B-frame reordering is allowed).
// Violations are returned as contextual errors before any output is
// written. Input scope is otherwise the same as [Defragment]: unencrypted
// self-contained fragmented or hybrid MP4 files.
func DefragmentPreserveGaps(f *File, rs io.ReadSeeker, w io.Writer, trackIDs ...uint32) error {
	var keptTrackIDs []uint32
	if len(trackIDs) > 0 {
		keptTrackIDs = trackIDs
	}
	d, err := newDefragmenterMode(f, rs, keptTrackIDs, true)
	if err != nil {
		return err
	}
	return d.writeProgressive(w)
}

// gapSample is one final surviving sample placed on the original absolute
// source decode timeline.
type gapSample struct {
	srcDts  uint64 // absolute source media tick, reconstructed through the collector timeline
	realDur uint32 // declared sample duration, without tfdt gap padding
}

// gapIsland is a run of samples whose original decode intervals touch.
type gapIsland struct {
	startDts   uint64 // source tick of the first sample
	endDts     uint64 // source tick just after the last sample's real duration
	mediaStart uint64 // start of the island on the compacted new media axis
}

// gapTrackPlan is the preserve-gaps result for one kept track, computed
// before any moov box is mutated.
type gapTrackPlan struct {
	track     *defragTrack
	trackID   uint32
	mediaTs   uint32
	samples   []gapSample
	islands   []gapIsland
	mediaDur  uint64 // compacted media duration: sum of real sample durations
	sourceEnd uint64 // endDts of the last island
	hadElst   bool
	entries   []ElstEntry // final edit list entries; nil means "do not write elst"
	movieDur  uint64      // track presentation duration in movie ticks
}

// buildGapTrackPlan reconstructs the original DTS and real duration of every
// surviving sample, groups them into decode islands and validates their PTS
// containment. It does not mutate the collected boxes.
func buildGapTrackPlan(track *defragTrack) (*gapTrackPlan, error) {
	trak := track.trak
	id := trak.Tkhd.TrackID
	mediaTs := trak.Mdia.Mdhd.Timescale
	if mediaTs == 0 {
		return nil, fmt.Errorf("track %d has zero media timescale", id)
	}
	n := len(track.samples)
	if n == 0 {
		return nil, fmt.Errorf("track %d has no surviving samples", id)
	}
	// extendedDurs remembers the declared duration of samples whose collected
	// duration was padded to bridge a forward tfdt jump.
	padded := make(map[int]uint32, len(track.extendedDurs))
	for _, pd := range track.extendedDurs {
		padded[pd.idx] = pd.dur
	}
	infos := make([]gapSample, n)
	dts := track.origin
	for i, s := range track.samples {
		realDur := s.dur
		if original, ok := padded[i]; ok {
			realDur = original
		}
		infos[i] = gapSample{srcDts: dts, realDur: realDur}
		next, ok := addUint64(dts, uint64(s.dur))
		if !ok {
			return nil, fmt.Errorf("track %d sample %d: source decode time overflows", id, i+1)
		}
		dts = next
	}
	plan := &gapTrackPlan{
		track:   track,
		trackID: id,
		mediaTs: mediaTs,
		samples: infos,
	}
	// Compacted media positions and islands.
	newDts := make([]uint64, n)
	var mediaDur uint64
	var islands []gapIsland
	for i, gs := range infos {
		newDts[i] = mediaDur
		nextMedia, ok := addUint64(mediaDur, uint64(gs.realDur))
		if !ok {
			return nil, fmt.Errorf("track %d: compacted media duration overflows", id)
		}
		mediaDur = nextMedia

		switch {
		case len(islands) == 0:
			islands = append(islands, gapIsland{startDts: gs.srcDts, mediaStart: newDts[i]})
		default:
			prev := infos[i-1]
			prevEnd, ok := addUint64(prev.srcDts, uint64(prev.realDur))
			if !ok {
				return nil, fmt.Errorf("track %d sample %d: previous decode end overflows", id, i)
			}
			switch {
			case gs.srcDts < prevEnd:
				return nil, fmt.Errorf("track %d sample %d: surviving sample starts at %d inside the previous sample ending at %d",
					id, i+1, gs.srcDts, prevEnd)
			case gs.srcDts != prevEnd:
				// A recording gap: close the previous island and start a new one.
				islands[len(islands)-1].endDts = prevEnd
				islands = append(islands, gapIsland{startDts: gs.srcDts, mediaStart: newDts[i]})
			}
		}
	}
	lastEnd, ok := addUint64(infos[n-1].srcDts, uint64(infos[n-1].realDur))
	if !ok {
		return nil, fmt.Errorf("track %d: source end overflows", id)
	}
	islands[len(islands)-1].endDts = lastEnd
	plan.islands = islands
	plan.mediaDur = mediaDur
	plan.sourceEnd = lastEnd
	if trak.Edts != nil && len(trak.Edts.Elst) > 0 {
		plan.hadElst = true
	}
	if err := plan.validatePTSContainment(); err != nil {
		return nil, err
	}
	return plan, nil
}

// validatePTSContainment rejects islands across which composition-time
// reordering reaches a different decode span (and would be hidden by the
// gap edits). Every sample's [PTS, PTS+duration) interval must lie inside
// its island's source decode span. Ordinary in-island B-frame reordering
// keeps all PTS intervals inside that span and is accepted.
func (p *gapTrackPlan) validatePTSContainment() error {
	for ii, isl := range p.islands {
		first := sort.Search(len(p.samples), func(i int) bool { return p.samples[i].srcDts >= isl.startDts })
		last := sort.Search(len(p.samples), func(i int) bool { return p.samples[i].srcDts >= isl.endDts })
		for i := first; i < last; i++ {
			gs := p.samples[i]
			cts := p.track.samples[i].cts
			lo, hi, ok := ptsInterval(gs.srcDts, cts, gs.realDur)
			if !ok {
				return fmt.Errorf("track %d island %d: sample %d with DTS %d and CTS-DTS %d has a presentation interval outside the representable timeline",
					p.trackID, ii+1, i+1, gs.srcDts, cts)
			}
			if lo < isl.startDts || hi > isl.endDts {
				return fmt.Errorf("track %d island %d: sample %d presentation interval [%d,%d) crosses the island decode span [%d,%d); "+
					"reordering across a recording gap cannot be represented without hiding frames",
					p.trackID, ii+1, i+1, lo, hi, isl.startDts, isl.endDts)
			}
		}
	}
	return nil
}

// ptsInterval returns the absolute [PTS, PTS+duration) source interval.
// ok is false when the interval underflows below zero or overflows uint64.
func ptsInterval(dts uint64, cts int32, dur uint32) (lo, hi uint64, ok bool) {
	if cts >= 0 {
		lo, ok = addUint64(dts, uint64(cts))
		if !ok {
			return 0, 0, false
		}
	} else {
		back := uint64(-int64(cts)) // int32 minimum negation is safe in int64
		if dts < back {
			return 0, 0, false
		}
		lo = dts - back
	}
	hi, ok = addUint64(lo, uint64(dur))
	return lo, hi, ok
}

func addUint64(a, b uint64) (uint64, bool) {
	if b > math.MaxUint64-a {
		return 0, false
	}
	return a + b, true
}

// gapMediaEntry builds a rate-1.0 edit pointing into the compacted media.
func gapMediaEntry(segmentDuration, mediaTime uint64) (ElstEntry, error) {
	if mediaTime > math.MaxInt64 {
		return ElstEntry{}, fmt.Errorf("media time %d does not fit an elst media_time", mediaTime)
	}
	return ElstEntry{
		SegmentDuration:  segmentDuration,
		MediaTime:        int64(mediaTime),
		MediaRateInteger: 1,
	}, nil
}

// gapEmptyEntry builds a rate-1.0 empty edit.
func gapEmptyEntry(segmentDuration uint64) ElstEntry {
	return ElstEntry{
		SegmentDuration:  segmentDuration,
		MediaTime:        -1,
		MediaRateInteger: 1,
	}
}

// islandAt returns the island covering source tick b with
// start <= b < end, or nil when b lies in a gap or at an island end.
func (p *gapTrackPlan) islandAt(b uint64) *gapIsland {
	i := sort.Search(len(p.islands), func(i int) bool { return p.islands[i].endDts > b })
	if i < len(p.islands) && p.islands[i].startDts <= b && b < p.islands[i].endDts {
		return &p.islands[i]
	}
	return nil
}

// mapOriginalElst maps the decoded elst entries (absolute source media
// ticks, movie-timescale segment durations) onto the compacted media axis,
// splitting media edits at island boundaries and replacing gap parts with
// empty edits. It returns the new flat entry list and its total movie
// duration.
func (p *gapTrackPlan) mapOriginalElst(entries []ElstEntry, movieTs uint32) ([]ElstEntry, uint64, error) {
	var out []ElstEntry
	var total uint64
	for i, entry := range entries {
		pieces, err := p.mapOneEdit(entry, i, len(entries), movieTs)
		if err != nil {
			return nil, 0, err
		}
		out = append(out, pieces...)
		for _, piece := range pieces {
			next, ok := addUint64(total, piece.SegmentDuration)
			if !ok {
				return nil, 0, fmt.Errorf("track %d: total edit duration overflows the movie timeline", p.trackID)
			}
			total = next
		}
	}
	return out, total, nil
}

// editCut is an exact split point of one edit, expressed both as movie ticks
// from the edit start and as the corresponding integral source media tick.
type editCut struct {
	q uint64
	b uint64
}

func (p *gapTrackPlan) mapOneEdit(entry ElstEntry, idx, entryCount int, movieTs uint32) ([]ElstEntry, error) {
	editNr := idx + 1
	if entry.MediaTime == -1 {
		// Empty edits are preserved exactly, including their rate fields.
		return []ElstEntry{entry}, nil
	}
	if entry.MediaTime < 0 {
		return nil, fmt.Errorf("track %d edit %d: unsupported negative media_time %d (only -1 marks an empty edit)",
			p.trackID, editNr, entry.MediaTime)
	}
	if entry.MediaRateInteger != 1 || entry.MediaRateFraction != 0 {
		return nil, fmt.Errorf("track %d edit %d: unsupported media rate %d+%d/65536, only rate 1.0 is supported",
			p.trackID, editNr, entry.MediaRateInteger, entry.MediaRateFraction)
	}
	mt := uint64(entry.MediaTime)
	duration := entry.SegmentDuration
	if duration == 0 && idx == entryCount-1 {
		// A final zero-duration media edit means "play until the track source
		// end"; expand it before segmenting.
		if mt > p.sourceEnd {
			return nil, fmt.Errorf("track %d edit %d: media time %d starts after the track source end %d",
				p.trackID, editNr, mt, p.sourceEnd)
		}
		expanded, exact := exactScaleTicks(p.sourceEnd-mt, p.mediaTs, movieTs)
		if !exact {
			return nil, fmt.Errorf("track %d edit %d: final zero-duration edit to source end %d cannot be converted exactly "+
				"to movie timescale %d from media timescale %d",
				p.trackID, editNr, p.sourceEnd, movieTs, p.mediaTs)
		}
		duration = expanded
	} else if editExtendsPastEnd(mt, duration, p.mediaTs, movieTs, p.sourceEnd) {
		return nil, fmt.Errorf("track %d edit %d: media segment starting at %d with duration %d movie ticks extends past "+
			"the track source end %d (media timescale %d, movie timescale %d)",
			p.trackID, editNr, mt, duration, p.sourceEnd, p.mediaTs, movieTs)
	}

	cuts := []editCut{{q: 0, b: mt}}
	if duration == 0 {
		// A zero-length edit presents nothing; keep a single zero-duration
		// entry with a mapped media time when its point sits on media.
		if isl := p.islandAt(mt); isl != nil {
			newMediaTime, ok := addUint64(isl.mediaStart, mt-isl.startDts)
			if !ok {
				return nil, fmt.Errorf("track %d edit %d: mapped media time overflows", p.trackID, editNr)
			}
			entry, err := gapMediaEntry(0, newMediaTime)
			if err != nil {
				return nil, fmt.Errorf("track %d edit %d: %w", p.trackID, editNr, err)
			}
			return []ElstEntry{entry}, nil
		}
		if mt == p.sourceEnd {
			entry, err := gapMediaEntry(0, p.mediaDur)
			if err != nil {
				return nil, fmt.Errorf("track %d edit %d: %w", p.trackID, editNr, err)
			}
			return []ElstEntry{entry}, nil
		}
		return []ElstEntry{gapEmptyEntry(0)}, nil
	}
	for _, isl := range p.islands {
		for _, b := range [2]uint64{isl.startDts, isl.endDts} {
			if b <= mt {
				continue
			}
			delta := b - mt
			// Strictly inside the edit's movie span iff
			// delta*movieTs < duration*mediaTs.
			if !sourceOffsetLessThanDuration(delta, p.mediaTs, duration, movieTs) {
				continue
			}
			q, exact := exactScaleTicks(delta, p.mediaTs, movieTs)
			if !exact {
				return nil, fmt.Errorf("track %d edit %d: island cut at media tick %d does not convert to an exact "+
					"movie-tick offset from media time %d (media timescale %d, movie timescale %d); refusing to round edit cuts",
					p.trackID, editNr, b, mt, p.mediaTs, movieTs)
			}
			cuts = append(cuts, editCut{q: q, b: b})
		}
	}
	sort.Slice(cuts, func(i, j int) bool { return cuts[i].q < cuts[j].q })
	// Drop duplicate movie positions (defensive; distinct source boundaries
	// cannot map to the same exact positive offset).
	unique := cuts[:0]
	for _, cut := range cuts {
		if n := len(unique); n > 0 && unique[n-1].q == cut.q {
			continue
		}
		unique = append(unique, cut)
	}
	cuts = unique

	pieces := make([]ElstEntry, 0, len(cuts)+1)
	for k, cut := range cuts {
		q2 := duration
		if k+1 < len(cuts) {
			q2 = cuts[k+1].q
		}
		if q2 <= cut.q {
			continue
		}
		segmentDuration := q2 - cut.q
		if isl := p.islandAt(cut.b); isl != nil {
			newMediaTime, ok := addUint64(isl.mediaStart, cut.b-isl.startDts)
			if !ok {
				return nil, fmt.Errorf("track %d edit %d: mapped media time overflows", p.trackID, editNr)
			}
			mediaEntry, err := gapMediaEntry(segmentDuration, newMediaTime)
			if err != nil {
				return nil, fmt.Errorf("track %d edit %d: %w", p.trackID, editNr, err)
			}
			pieces = append(pieces, mediaEntry)
		} else {
			pieces = append(pieces, gapEmptyEntry(segmentDuration))
		}
	}
	return pieces, nil
}

// buildSynthesizedEdits builds the no-elst presentation anchored at the
// common movie zero zeroDts/zeroTs (the earliest surviving source DTS over
// all kept tracks). It returns nil entries for the trivial identity layout
// (one island starting at the common zero), in which case the caller keeps
// the conventional rounded track duration and writes no elst.
func (p *gapTrackPlan) buildSynthesizedEdits(zeroDts uint64, zeroTs, movieTs uint32) ([]ElstEntry, uint64, bool, error) {
	convert := func(b uint64, what string) (uint64, error) {
		q, status := commonZeroMovieOffset(b, p.mediaTs, zeroDts, zeroTs, movieTs)
		switch status {
		case offsetBeforeZero:
			return 0, fmt.Errorf("track %d: %s at media tick %d is before the common movie zero %d (timescale %d)",
				p.trackID, what, b, zeroDts, zeroTs)
		case offsetInexact:
			return 0, fmt.Errorf("track %d: %s at media tick %d (media timescale %d) does not convert exactly to movie timescale %d "+
				"from the common zero %d/%d; refusing to round edit cuts",
				p.trackID, what, b, p.mediaTs, movieTs, zeroDts, zeroTs)
		case offsetOverflow:
			return 0, fmt.Errorf("track %d: %s movie offset overflows", p.trackID, what)
		}
		return q, nil
	}
	startQ, err := convert(p.islands[0].startDts, "first sample")
	if err != nil {
		return nil, 0, false, err
	}
	if len(p.islands) == 1 && startQ == 0 {
		// Trivial identity timeline: no blank region to express.
		return nil, rescaleDur(p.mediaDur, p.mediaTs, movieTs), true, nil
	}
	var entries []ElstEntry
	if startQ > 0 {
		entries = append(entries, gapEmptyEntry(startQ))
	}
	for i, isl := range p.islands {
		start, err := convert(isl.startDts, fmt.Sprintf("island %d start", i+1))
		if err != nil {
			return nil, 0, false, err
		}
		end, err := convert(isl.endDts, fmt.Sprintf("island %d end", i+1))
		if err != nil {
			return nil, 0, false, err
		}
		mediaEntry, err := gapMediaEntry(end-start, isl.mediaStart)
		if err != nil {
			return nil, 0, false, fmt.Errorf("track %d island %d: %w", p.trackID, i+1, err)
		}
		entries = append(entries, mediaEntry)
		if i+1 < len(p.islands) {
			nextStart, err := convert(p.islands[i+1].startDts, fmt.Sprintf("island %d start", i+2))
			if err != nil {
				return nil, 0, false, err
			}
			entries = append(entries, gapEmptyEntry(nextStart-end))
		}
	}
	endQ, err := convert(p.sourceEnd, "source end")
	if err != nil {
		return nil, 0, false, err
	}
	return entries, endQ, false, nil
}

// movieOffsetStatus classifies an exact common-zero conversion.
type movieOffsetStatus int

const (
	offsetOK movieOffsetStatus = iota
	offsetBeforeZero
	offsetInexact
	offsetOverflow
)

// commonZeroMovieOffset computes
//
//	q = movieTs * (b/mediaTs - zeroDts/zeroTs)
//	  = movieTs * (b*zeroTs - zeroDts*mediaTs) / (mediaTs*zeroTs)
//
// requiring a nonnegative, integral quotient that fits uint64.
func commonZeroMovieOffset(b uint64, mediaTs uint32, zeroDts uint64, zeroTs, movieTs uint32) (uint64, movieOffsetStatus) {
	bBig := new(big.Int).SetUint64(b)
	zeroBig := new(big.Int).SetUint64(zeroDts)
	num := new(big.Int).Mul(bBig, big.NewInt(int64(zeroTs)))
	num.Sub(num, new(big.Int).Mul(zeroBig, big.NewInt(int64(mediaTs))))
	if num.Sign() < 0 {
		return 0, offsetBeforeZero
	}
	num.Mul(num, big.NewInt(int64(movieTs)))
	den := new(big.Int).SetUint64(uint64(mediaTs) * uint64(zeroTs))
	q, rem := new(big.Int).QuoRem(num, den, new(big.Int))
	if rem.Sign() != 0 {
		return 0, offsetInexact
	}
	if !q.IsUint64() {
		return 0, offsetOverflow
	}
	return q.Uint64(), offsetOK
}

// exactScaleTicks returns delta*from/to when the division is exact and the
// quotient fits uint64.
func exactScaleTicks(delta uint64, from, to uint32) (uint64, bool) {
	num := new(big.Int).Mul(new(big.Int).SetUint64(delta), big.NewInt(int64(to)))
	den := big.NewInt(int64(from))
	q, rem := new(big.Int).QuoRem(num, den, new(big.Int))
	if rem.Sign() != 0 || !q.IsUint64() {
		return 0, false
	}
	return q.Uint64(), true
}

// sourceOffsetLessThanDuration reports whether delta*to < duration*from
// without overflowing.
func sourceOffsetLessThanDuration(delta uint64, from uint32, duration uint64, to uint32) bool {
	left := new(big.Int).Mul(new(big.Int).SetUint64(delta), big.NewInt(int64(to)))
	right := new(big.Int).Mul(new(big.Int).SetUint64(duration), big.NewInt(int64(from)))
	return left.Cmp(right) < 0
}

// editExtendsPastEnd reports whether the media interval
// [mt, mt + duration*mediaTs/movieTs) extends past sourceEnd, i.e.
// mt*movieTs + duration*mediaTs > sourceEnd*movieTs.
func editExtendsPastEnd(mt, duration uint64, mediaTs, movieTs uint32, sourceEnd uint64) bool {
	left := new(big.Int).Add(
		new(big.Int).Mul(new(big.Int).SetUint64(mt), big.NewInt(int64(movieTs))),
		new(big.Int).Mul(new(big.Int).SetUint64(duration), big.NewInt(int64(mediaTs))),
	)
	right := new(big.Int).Mul(new(big.Int).SetUint64(sourceEnd), big.NewInt(int64(movieTs)))
	return left.Cmp(right) > 0
}

// earlierSourceTime reports whether aDts/aTs < bDts/bTs exactly.
func earlierSourceTime(aDts uint64, aTs uint32, bDts uint64, bTs uint32) bool {
	left := new(big.Int).Mul(new(big.Int).SetUint64(aDts), big.NewInt(int64(bTs)))
	right := new(big.Int).Mul(new(big.Int).SetUint64(bDts), big.NewInt(int64(aTs)))
	return left.Cmp(right) < 0
}

// rebuildMoovPreserveGaps is the preserve-gaps counterpart of
// rebuildMoov: all plans and edit mappings are computed and validated
// first, then the fragment boxes are dropped and the progressive sample
// tables and gap edit lists are installed.
func (d *defragmenter) rebuildMoovPreserveGaps() error {
	movieTs := d.moov.Mvhd.Timescale // nonzero checked by newDefragmenter
	plans := make([]*gapTrackPlan, 0, len(d.tracks))
	for _, track := range d.tracks {
		if !track.keep {
			continue
		}
		plan, err := buildGapTrackPlan(track)
		if err != nil {
			return err
		}
		plans = append(plans, plan)
	}
	if len(plans) == 0 {
		return fmt.Errorf("no tracks left to defragment")
	}
	// Common movie zero: earliest surviving source DTS over all kept tracks.
	zeroDts := plans[0].islands[0].startDts
	zeroTs := plans[0].mediaTs
	for _, plan := range plans[1:] {
		if earlierSourceTime(plan.islands[0].startDts, plan.mediaTs, zeroDts, zeroTs) {
			zeroDts = plan.islands[0].startDts
			zeroTs = plan.mediaTs
		}
	}
	// Resolve every track's edit mapping before mutating the moov box tree.
	// Existing elst boxes get their entries replaced in place; synthesized
	// elst boxes are attached during installation below.
	for _, plan := range plans {
		trak := plan.track.trak
		switch {
		case plan.hadElst:
			for ei, elst := range trak.Edts.Elst {
				mapped, total, err := plan.mapOriginalElst(elst.Entries, movieTs)
				if err != nil {
					return err
				}
				elst.Entries = mapped
				if elst.Version == 0 && elst.needs64Bits() {
					elst.Version = 1
				}
				if ei == 0 {
					plan.movieDur = total
					plan.entries = mapped
				}
			}
		default:
			entries, movieDur, _, err := plan.buildSynthesizedEdits(zeroDts, zeroTs, movieTs)
			if err != nil {
				return err
			}
			plan.entries = entries
			plan.movieDur = movieDur
		}
	}

	// Drop mvex and the traks that are not kept, exactly like the normal rebuild.
	moov := d.moov
	children := make([]Box, 0, len(moov.Children))
	for _, child := range moov.Children {
		switch box := child.(type) {
		case *MvexBox:
			continue
		case *TrakBox:
			if track, ok := d.byID[trakID(box)]; !ok || !track.keep {
				continue
			}
		}
		children = append(children, child)
	}
	moov.Children = children
	moov.Mvex = nil
	moov.Trak = nil
	moov.Traks = nil
	for _, child := range children {
		if trak, ok := child.(*TrakBox); ok {
			if moov.Trak == nil {
				moov.Trak = trak
			}
			moov.Traks = append(moov.Traks, trak)
		}
	}
	if len(moov.Traks) == 0 {
		return fmt.Errorf("no tracks left to defragment")
	}

	var movieDur uint64
	for _, plan := range plans {
		track := plan.track
		// Install the real declared durations so the synthesized stts carries
		// one true duration per surviving sample.
		for i, gs := range plan.samples {
			track.samples[i].dur = gs.realDur
		}
		if err := fillSampleTables(track); err != nil {
			return fmt.Errorf("sample tables of track %d: %w", plan.trackID, err)
		}
		mdhd := track.trak.Mdia.Mdhd
		mdhd.Duration = plan.mediaDur
		if plan.mediaDur > math.MaxUint32 {
			mdhd.Version = 1
		}
		if err := installSynthesizedGapElst(track.trak, plan); err != nil {
			return err
		}
		track.trak.Tkhd.Duration = plan.movieDur
		if plan.movieDur > math.MaxUint32 {
			track.trak.Tkhd.Version = 1
		}
		if plan.movieDur > movieDur {
			movieDur = plan.movieDur
		}
	}
	d.moov.Mvhd.Duration = movieDur
	if movieDur > math.MaxUint32 {
		d.moov.Mvhd.Version = 1
	}
	return nil
}

// installSynthesizedGapElst attaches the synthesized elst of a track that had
// none. Tracks with an original elst had their entries replaced in place
// during plan resolution. A nil entry list is the trivial identity layout
// and needs no elst.
func installSynthesizedGapElst(trak *TrakBox, plan *gapTrackPlan) error {
	if plan.hadElst || len(plan.entries) == 0 {
		return nil
	}
	elst := &ElstBox{Entries: plan.entries}
	if trak.Edts == nil {
		trak.insertEdts(&EdtsBox{})
	}
	trak.Edts.Elst = append(trak.Edts.Elst, elst)
	trak.Edts.AddChild(elst)
	if elst.Version == 0 && elst.needs64Bits() {
		elst.Version = 1
	}
	return nil
}
