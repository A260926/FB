// edit_compact.go implements CompactEditedFile, which rewrites a progressive
// MP4 so that only the media referenced by the tracks' edit lists remains in
// the file. The sample tables and the mdat payload are rebuilt; the movie
// and track headers, the edit list structure and the sample descriptions are
// preserved.
//
// The supported input is a non-fragmented, unencrypted, self-contained MP4
// with a single mdat box and tracks of handler type vide or soun. Video
// tracks must have closed GOPs, so that every edit can be decoded starting
// from the sync sample the edit maps to; the bitstream itself is not parsed.

package mp4

import (
	"fmt"
	"io"
	"math"
)

// CompactEditedFile reads the MP4 from in, removes all media that the edit
// lists of its tracks do not reference, and writes the resulting
// self-contained MP4 to out.
//
// A track without an edit list is kept in full. For a track with an edit
// list, every rate-1 media segment selects the media its segment covers,
// empty (media_time -1) segments select nothing, and the union of the
// selected decode-order runs is written to the output exactly once,
// concatenated in source decode order with the first decode time reset to
// zero. Sample durations, composition offsets, sync flags, sample
// description references and payloads are carried over unchanged, and every
// edit segment keeps its duration and order with its media time shifted by
// the same amount as the media run it belongs to.
//
// The movie timescale, the mvhd and tkhd display durations, the track IDs
// and other track properties, and the sample descriptions are preserved.
// All structural and timeline validation is done before anything is written;
// an I/O error is returned as is, and bytes already written are not rolled
// back.
func CompactEditedFile(in io.ReadSeeker, out io.Writer) error {
	dec, err := DecodeFile(in, WithDecodeMode(DecModeLazyMdat))
	if err != nil {
		return err
	}
	plan, err := planCompact(dec)
	if err != nil {
		return err
	}
	return plan.write(in, out)
}

// compactPlan is the fully validated and laid out result of analyzing the
// input file. Writing the output only starts after a plan is complete.
type compactPlan struct {
	ftyp     *FtypBox
	moov     *MoovBox
	mdatSize uint64 // mdat payload size in bytes
	copies   []payloadCopy
}

// payloadCopy is one contiguous byte range copied from input to output mdat.
type payloadCopy struct {
	srcOffset uint64
	size      uint64
}

// write encodes ftyp, moov and the mdat header, then copies the payload
// ranges from in to out.
func (p *compactPlan) write(in io.ReadSeeker, out io.Writer) error {
	if p.ftyp != nil {
		if err := p.ftyp.Encode(out); err != nil {
			return err
		}
	}
	if err := p.moov.Encode(out); err != nil {
		return err
	}
	mdat := &MdatBox{}
	mdat.SetLazyDataSize(p.mdatSize)
	if err := mdat.Encode(out); err != nil {
		return err
	}
	for _, c := range p.copies {
		if _, err := in.Seek(int64(c.srcOffset), io.SeekStart); err != nil {
			return err
		}
		if _, err := io.CopyN(out, in, int64(c.size)); err != nil {
			return err
		}
	}
	return nil
}

// planCompact validates dec and computes the output box tree, the chunk
// layout and the payload copies. Nothing is written.
func planCompact(dec *File) (*compactPlan, error) {
	if dec.IsFragmented() {
		return nil, fmt.Errorf("fragmented files are not supported")
	}
	if dec.Moov == nil || dec.Moov.Mvhd == nil {
		return nil, fmt.Errorf("no moov box with mvhd found")
	}
	mvhd := dec.Moov.Mvhd
	if mvhd.Timescale == 0 {
		return nil, fmt.Errorf("movie timescale is zero")
	}
	if dec.Mdat == nil {
		return nil, fmt.Errorf("no mdat box found")
	}
	nrMdats := 0
	for _, child := range dec.Children {
		if child.Type() == "mdat" {
			nrMdats++
		}
	}
	if nrMdats != 1 {
		return nil, fmt.Errorf("expected exactly one mdat box, found %d", nrMdats)
	}
	mdatPayloadStart := dec.Mdat.StartPos + dec.Mdat.HeaderSize()
	mdatPayloadEnd, err := addUint64(mdatPayloadStart, dec.Mdat.GetLazyDataSize())
	if err != nil {
		return nil, fmt.Errorf("mdat size overflow")
	}

	tracks := make([]*compactTrack, 0, len(dec.Moov.Traks))
	for i, trak := range dec.Moov.Traks {
		ct, err := analyzeCompactTrack(trak, mvhd.Timescale, mdatPayloadStart, mdatPayloadEnd)
		if err != nil {
			return nil, fmt.Errorf("track %d: %w", i+1, err)
		}
		tracks = append(tracks, ct)
	}

	// Lay out the output mdat payload: the kept media of each track in moov
	// track order, one run of chunks per track.
	var mdatPayload uint64
	for _, ct := range tracks {
		var err error
		mdatPayload, err = addUint64(mdatPayload, ct.payloadSize)
		if err != nil {
			return nil, err
		}
	}
	mdat := &MdatBox{}
	mdat.SetLazyDataSize(mdatPayload)
	mdatHeaderSize := mdat.HeaderSize()
	var ftypSize uint64
	if dec.Ftyp != nil {
		ftypSize = dec.Ftyp.Size()
	}

	// Build the output moov with placeholder chunk offsets, then compute the
	// real offsets and fill them in. The stco/co64 choice depends on the
	// offsets, which depend on the moov size, which depends on the choice;
	// switching to co64 only grows the moov and the offsets, so one retry
	// always settles.
	plan := &compactPlan{ftyp: dec.Ftyp, mdatSize: mdatPayload}
	useCo64 := false
	for {
		moov := &MoovBox{}
		trakIdx := 0
		for _, child := range dec.Moov.Children {
			if child.Type() == "trak" {
				moov.AddChild(tracks[trakIdx].buildTrak(useCo64))
				trakIdx++
			} else {
				moov.AddChild(child)
			}
		}
		payloadStart, err := addUint64(ftypSize, moov.Size(), mdatHeaderSize)
		if err != nil {
			return nil, err
		}
		offset := payloadStart
		needsCo64 := false
		plan.copies = plan.copies[:0]
		for _, ct := range tracks {
			for ci := range ct.chunks {
				ct.chunks[ci].offset = offset
				if offset > math.MaxUint32 {
					needsCo64 = true
				}
				offset, err = addUint64(offset, ct.chunks[ci].size)
				if err != nil {
					return nil, err
				}
			}
			plan.copies = append(plan.copies, ct.copies...)
		}
		if offset > math.MaxInt64 {
			return nil, fmt.Errorf("chunk offset overflow")
		}
		if needsCo64 && !useCo64 {
			useCo64 = true
			continue
		}
		for _, ct := range tracks {
			ct.fillChunkOffsets(useCo64)
		}
		plan.moov = moov
		return plan, nil
	}
}

// addUint64 adds unsigned values and reports overflow.
func addUint64(values ...uint64) (uint64, error) {
	var sum uint64
	for _, v := range values {
		next := sum + v
		if next < sum {
			return 0, fmt.Errorf("arithmetic overflow")
		}
		sum = next
	}
	return sum, nil
}
