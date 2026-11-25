package mp4_test

import (
	"bytes"
	"encoding/binary"
	"errors"
	"io"
	"testing"

	mp4 "github.com/abema/go-mp4"
	"github.com/abema/go-mp4/internal/avcstate"
	"github.com/abema/go-mp4/task_support/taskcheck"
)

/*
 * Tiny MP4 builder for the AVC edit tests. Only the boxes exercised by
 * ExportAVCEdits are emitted, but sizes and table semantics are standard.
 */

func boxBytes(typ string, payload []byte) []byte {
	out := make([]byte, 8+len(payload))
	binary.BigEndian.PutUint32(out[0:4], uint32(8+len(payload)))
	copy(out[4:8], typ)
	copy(out[8:], payload)
	return out
}

func fullBox(version byte, flags uint32, body []byte) []byte {
	hdr := []byte{version, byte(flags >> 16), byte(flags >> 8), byte(flags)}
	return append(hdr, body...)
}

type bb struct{ b []byte }

func (b *bb) u16(v uint32) {
	var x [2]byte
	binary.BigEndian.PutUint16(x[:], uint16(v))
	b.b = append(b.b, x[:]...)
}
func (b *bb) u32(v uint32) {
	var x [4]byte
	binary.BigEndian.PutUint32(x[:], v)
	b.b = append(b.b, x[:]...)
}
func (b *bb) bytes(p []byte) { b.b = append(b.b, p...) }

// bitWriter builds RBSP bits.
type bitWriter struct {
	data []byte
	cur  byte
	n    uint
}

func (w *bitWriter) bit(v uint) {
	w.cur |= byte(v&1) << (7 - w.n)
	w.n++
	if w.n == 8 {
		w.data = append(w.data, w.cur)
		w.cur, w.n = 0, 0
	}
}

func (w *bitWriter) ue(v uint32) {
	x := uint64(v) + 1
	bits := 1
	for (uint64(1) << uint(bits)) <= x {
		bits++
	}
	for i := 0; i < bits-1; i++ {
		w.bit(0)
	}
	for i := bits - 1; i >= 0; i-- {
		w.bit(uint((x >> uint(i)) & 1))
	}
}

func (w *bitWriter) finish() []byte {
	if w.n > 0 {
		w.bit(1) // rbsp_stop_one_bit
		for w.n != 0 {
			w.bit(0)
		}
	} else {
		w.data = append(w.data, 0x80)
	}
	return w.data
}

// ebspEscape inserts emulation-prevention bytes.
func ebspEscape(rbsp []byte) []byte {
	var out []byte
	zeros := 0
	for _, c := range rbsp {
		if zeros >= 2 && c <= 3 {
			out = append(out, 0x03)
			zeros = 0
		}
		out = append(out, c)
		if c == 0 {
			zeros++
		} else {
			zeros = 0
		}
	}
	return out
}

func spsNAL(id uint32, marker byte) []byte {
	w := &bitWriter{}
	w.ue(id)
	rbsp := []byte{0x42, 0x00, 0x0A}
	rbsp = append(rbsp, w.finish()...)
	rbsp = append(rbsp, marker)
	return append([]byte{0x67}, ebspEscape(rbsp)...)
}

func ppsNAL(id, spsID uint32, marker byte) []byte {
	w := &bitWriter{}
	w.ue(id)
	w.ue(spsID)
	rbsp := w.finish()
	rbsp = append(rbsp, marker)
	return append([]byte{0x68}, ebspEscape(rbsp)...)
}

func sliceNAL(nalType byte, ppsID uint32, marker byte) []byte {
	w := &bitWriter{}
	w.ue(0) // first_mb_in_slice
	w.ue(2) // slice_type I
	w.ue(ppsID)
	rbsp := w.finish()
	rbsp = append(rbsp, marker, marker)
	return append([]byte{nalType}, ebspEscape(rbsp)...)
}

func audNAL() []byte { return []byte{0x09, 0xF0} }

func avcCBox(lsMinusOne byte, sps, pps [][]byte) []byte {
	var p []byte
	p = append(p, 1, 0x42, 0x00, 0x1E)
	p = append(p, 0xFC|(lsMinusOne&3))
	p = append(p, 0xE0|byte(len(sps)))
	for _, s := range sps {
		var l [2]byte
		binary.BigEndian.PutUint16(l[:], uint16(len(s)))
		p = append(p, l[:]...)
		p = append(p, s...)
	}
	p = append(p, byte(len(pps)))
	for _, x := range pps {
		var l [2]byte
		binary.BigEndian.PutUint16(l[:], uint16(len(x)))
		p = append(p, l[:]...)
		p = append(p, x...)
	}
	return boxBytes("avcC", p)
}

type mxDesc struct {
	typ        string
	lsMinusOne byte
	sps        [][]byte
	pps        [][]byte
}

type mxSample struct {
	nals  [][]byte
	raw   []byte
	delta uint32
	ctts  int32
}

type mxChunk struct {
	samples []int
	desc    uint32 // 1-based
}

type mxElst struct {
	dur       uint32
	mediaTime int32
	rateInt   int16 // defaults to 1 when zero value? use rateSet
	rateFrac  int16
	rateSet   bool
}

type muxCfg struct {
	movieTS  uint32
	mediaTS  uint32
	movieDur uint32
	mediaDur uint32

	descs   []mxDesc
	samples []mxSample
	chunks  []mxChunk

	hasCTTS bool
	stss    []uint32 // nil => omit stss

	elst []mxElst // nil => no elst

	useCO64      bool
	externalDref bool
	extraTop     [][]byte
}

func visualEntry(typ string, drefIndex uint16, children []byte) []byte {
	p := &bb{}
	p.bytes(make([]byte, 6)) // reserved
	p.u16(uint32(drefIndex))
	p.u16(0) // pre_defined
	p.u16(0) // reserved
	p.u32(0)
	p.u32(0)
	p.u32(0) // pre_defined2[3]
	p.u16(320)
	p.u16(180)
	p.u32(0x00480000)
	p.u32(0x00480000)
	p.u32(0)
	p.u16(1) // frame_count
	var cname [32]byte
	p.bytes(cname[:])
	p.u16(0x0018) // depth
	p.u16(0xFFFF) // pre_defined
	p.bytes(children)
	return boxBytes(typ, p.b)
}

func buildMP4(c *muxCfg) []byte {
	if c.movieTS == 0 {
		c.movieTS = 1000
	}
	if c.mediaTS == 0 {
		c.mediaTS = 1000
	}

	// Serialize samples with the length size of their chunk description.
	sampleBytes := make([][]byte, len(c.samples))
	sampleDesc := make([]int, len(c.samples))
	for _, ch := range c.chunks {
		ls := 4
		if int(ch.desc-1) < len(c.descs) {
			ls = int(c.descs[ch.desc-1].lsMinusOne) + 1
		}
		for _, si := range ch.samples {
			sampleDesc[si] = int(ch.desc - 1)
			if c.samples[si].raw != nil {
				sampleBytes[si] = c.samples[si].raw
				continue
			}
			var sb []byte
			for _, nal := range c.samples[si].nals {
				lb := make([]byte, ls)
				n := uint64(len(nal))
				for k := ls - 1; k >= 0; k-- {
					lb[k] = byte(n & 0xff)
					n >>= 8
				}
				sb = append(sb, lb...)
				sb = append(sb, nal...)
			}
			sampleBytes[si] = sb
		}
	}

	var mdatData bytes.Buffer
	chunkRel := make([]uint64, 0, len(c.chunks))
	for _, ch := range c.chunks {
		chunkRel = append(chunkRel, uint64(mdatData.Len()))
		for _, si := range ch.samples {
			mdatData.Write(sampleBytes[si])
		}
	}

	ftyp := boxBytes("ftyp", append([]byte("isom"),
		0x00, 0x00, 0x02, 0x00,
		'i', 's', 'o', 'm',
		'a', 'v', 'c', '1',
		'm', 'p', '4', '2'))

	mdatPayloadStart := int64(len(ftyp)) + 8
	chunkOffsets := make([]uint32, len(c.chunks))
	for i, rel := range chunkRel {
		chunkOffsets[i] = uint32(mdatPayloadStart + int64(rel))
	}

	// mvhd
	mv := &bb{}
	mv.u32(0) // creation
	mv.u32(0) // modification
	mv.u32(c.movieTS)
	mv.u32(c.movieDur)
	mv.u32(0x00010000) // rate
	mv.u16(0x0100)     // volume
	mv.bytes(make([]byte, 10))
	mv.bytes(make([]byte, 36)) // matrix
	mv.bytes(make([]byte, 24)) // pre-defined
	mv.u32(2)                  // next track id
	mvhd := boxBytes("mvhd", fullBox(0, 0, mv.b))

	// tkhd
	tk := &bb{}
	tk.u32(0)
	tk.u32(0)
	tk.u32(1) // track id
	tk.u32(0) // reserved
	tk.u32(0) // duration
	tk.bytes(make([]byte, 8))
	tk.u16(0) // layer
	tk.u16(0) // alt group
	tk.u16(0) // volume
	tk.u16(0) // reserved
	tk.bytes(make([]byte, 36))
	tk.u32(320 << 16)
	tk.u32(180 << 16)
	tkhd := boxBytes("tkhd", fullBox(0, 0x000007, tk.b))

	// mdhd
	md := &bb{}
	md.u32(0)
	md.u32(0)
	md.u32(c.mediaTS)
	md.u32(c.mediaDur)
	md.u16(0x55C4) // language "und"
	md.u16(0)
	mdhd := boxBytes("mdhd", fullBox(0, 0, md.b))

	// hdlr (minimal but valid-looking)
	hd := &bb{}
	hd.u32(0)
	hd.bytes([]byte("vide"))
	hd.bytes(make([]byte, 12))
	hd.bytes([]byte("VideoHandler\x00"))
	hdlr := boxBytes("hdlr", fullBox(0, 0, hd.b))

	// dinf/dref
	var urlEntry []byte
	if c.externalDref {
		body := []byte{0, 0, 0, 0} // version 0 flags 0 => not self contained
		body = append(body, []byte("external.mp4\x00")...)
		urlEntry = boxBytes("url ", body)
	} else {
		urlEntry = boxBytes("url ", []byte{0, 0, 0, 1})
	}
	drefBody := &bb{}
	drefBody.u32(1)
	drefBody.bytes(urlEntry)
	dref := boxBytes("dref", fullBox(0, 0, drefBody.b))
	dinf := boxBytes("dinf", dref)

	// stsd
	var entries []byte
	for _, d := range c.descs {
		var cfg []byte
		if len(d.sps) > 0 || len(d.pps) > 0 {
			cfg = avcCBox(d.lsMinusOne, d.sps, d.pps)
		}
		entries = append(entries, visualEntry(d.typ, 1, cfg)...)
	}
	stsdBody := &bb{}
	stsdBody.u32(uint32(len(c.descs)))
	stsdBody.bytes(entries)
	stsd := boxBytes("stsd", fullBox(0, 0, stsdBody.b))

	// stts: one count=1 run per sample
	sttsBody := &bb{}
	sttsBody.u32(uint32(len(c.samples)))
	for _, s := range c.samples {
		sttsBody.u32(1)
		sttsBody.u32(s.delta)
	}
	stts := boxBytes("stts", fullBox(0, 0, sttsBody.b))

	stblChildren := [][]byte{stsd, stts}

	if c.hasCTTS {
		cttsBody := &bb{}
		cttsBody.u32(uint32(len(c.samples)))
		for _, s := range c.samples {
			cttsBody.u32(1)
			var x [4]byte
			binary.BigEndian.PutUint32(x[:], uint32(s.ctts))
			cttsBody.bytes(x[:])
		}
		ctts := boxBytes("ctts", fullBox(1, 0, cttsBody.b))
		stblChildren = append(stblChildren, ctts)
	}

	// stsc
	type scE struct{ first, spc, desc uint32 }
	var scEntries []scE
	for ci, ch := range c.chunks {
		spc := uint32(len(ch.samples))
		if len(scEntries) == 0 || scEntries[len(scEntries)-1].spc != spc || scEntries[len(scEntries)-1].desc != ch.desc {
			scEntries = append(scEntries, scE{uint32(ci + 1), spc, ch.desc})
		}
	}
	stscBody := &bb{}
	stscBody.u32(uint32(len(scEntries)))
	for _, e := range scEntries {
		stscBody.u32(e.first)
		stscBody.u32(e.spc)
		stscBody.u32(e.desc)
	}
	stsc := boxBytes("stsc", fullBox(0, 0, stscBody.b))

	// stsz
	stszBody := &bb{}
	stszBody.u32(0)
	stszBody.u32(uint32(len(sampleBytes)))
	for _, sb := range sampleBytes {
		stszBody.u32(uint32(len(sb)))
	}
	stsz := boxBytes("stsz", fullBox(0, 0, stszBody.b))

	// stco / co64
	var chunkOffsetBox []byte
	if c.useCO64 {
		coBody := &bb{}
		coBody.u32(uint32(len(chunkOffsets)))
		for _, o := range chunkOffsets {
			var x [8]byte
			binary.BigEndian.PutUint64(x[:], uint64(o))
			coBody.bytes(x[:])
		}
		chunkOffsetBox = boxBytes("co64", fullBox(0, 0, coBody.b))
	} else {
		stcoBody := &bb{}
		stcoBody.u32(uint32(len(chunkOffsets)))
		for _, o := range chunkOffsets {
			stcoBody.u32(o)
		}
		chunkOffsetBox = boxBytes("stco", fullBox(0, 0, stcoBody.b))
	}

	stblChildren = append(stblChildren, stsc, stsz, chunkOffsetBox)

	if c.stss != nil {
		stssBody := &bb{}
		stssBody.u32(uint32(len(c.stss)))
		for _, n := range c.stss {
			stssBody.u32(n)
		}
		stss := boxBytes("stss", fullBox(0, 0, stssBody.b))
		stblChildren = append(stblChildren, stss)
	}

	var stblPayload []byte
	for _, ch := range stblChildren {
		stblPayload = append(stblPayload, ch...)
	}
	stbl := boxBytes("stbl", stblPayload)

	minf := boxBytes("minf", append(dinf, stbl...))
	mdia := boxBytes("mdia", append(append(mdhd, hdlr...), minf...))

	var trakChildren []byte
	trakChildren = append(trakChildren, tkhd...)
	if c.elst != nil {
		elstBody := &bb{}
		elstBody.u32(uint32(len(c.elst)))
		for _, e := range c.elst {
			elstBody.u32(e.dur)
			var mt [4]byte
			binary.BigEndian.PutUint32(mt[:], uint32(e.mediaTime))
			elstBody.bytes(mt[:])
			ri, rf := e.rateInt, e.rateFrac
			if !e.rateSet {
				ri, rf = 1, 0
			}
			elstBody.u16(uint32(uint16(ri)))
			elstBody.u16(uint32(uint16(rf)))
		}
		elst := boxBytes("elst", fullBox(0, 0, elstBody.b))
		edts := boxBytes("edts", elst)
		trakChildren = append(trakChildren, edts...)
	}
	trakChildren = append(trakChildren, mdia...)
	trak := boxBytes("trak", trakChildren)

	moov := boxBytes("moov", append(mvhd, trak...))

	var out bytes.Buffer
	out.Write(ftyp)
	mdatHdr := make([]byte, 8)
	binary.BigEndian.PutUint32(mdatHdr[0:4], uint32(8+mdatData.Len()))
	copy(mdatHdr[4:8], "mdat")
	out.Write(mdatHdr)
	out.Write(mdatData.Bytes())
	out.Write(moov)
	for _, x := range c.extraTop {
		out.Write(x)
	}
	return out.Bytes()
}

/*------------------------------ NAL helpers -------------------------------*/

func splitAnnexB(b []byte) [][]byte {
	code := []byte{0, 0, 0, 1}
	var starts []int
	for i := 0; i+4 <= len(b); i++ {
		if bytes.Equal(b[i:i+4], code) {
			starts = append(starts, i+4)
		}
	}
	var nals [][]byte
	for k, s := range starts {
		end := len(b)
		if k+1 < len(starts) {
			end = starts[k+1] - 4
		}
		nals = append(nals, b[s:end])
	}
	return nals
}

func nalType(nal []byte) int { return int(nal[0] & 0x1f) }

func findNAL(nals [][]byte, typ int) []byte {
	for _, n := range nals {
		if len(n) > 0 && nalType(n) == typ {
			return n
		}
	}
	return nil
}

func avcConfigPrefix(sps, pps [][]byte) []byte {
	var p []byte
	for _, s := range sps {
		p = append(p, 0, 0, 0, 1)
		p = append(p, s...)
	}
	for _, x := range pps {
		p = append(p, 0, 0, 0, 1)
		p = append(p, x...)
	}
	return p
}

/*=========================== TestAVCStateReplay ===========================*/

func TestAVCStateReplay(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Direct avcstate checks: Exp-Golomb ids, emulation prevention,
		// same-id updates and clone isolation.
		sps0A := spsNAL(0, 'A')
		sps7B := spsNAL(7, 'B')
		id, err := avcstate.SPSID(sps0A)
		c.Check(err == nil && id == 0, "SPS id 0 parse: %v %d", err, id)
		id7, err := avcstate.SPSID(sps7B)
		c.Check(err == nil && id7 == 7, "SPS id 7 parse: %v %d", err, id7)

		pps03, sps03 := func() (uint32, uint32) {
			a, b, e := avcstate.PPSIDs(ppsNAL(3, 9, 'x'))
			c.Check(e == nil, "PPS ids: %v", e)
			return a, b
		}()
		c.Check(pps03 == 3 && sps03 == 9, "PPS ids got %d/%d", pps03, sps03)
		pid, err := avcstate.SlicePPSID(sliceNAL(5, 4, 'z'))
		c.Check(err == nil && pid == 4, "slice pps id: %v %d", err, pid)

		// Emulation prevention inside the fixed SPS header: profile=0,
		// constraints=0, level=3 makes RBSP 00 00 03 become EBSP
		// 00 00 03 03 before the ue(0) terminator byte 0x80.
		epSPS := []byte{0x67, 0x00, 0x00, 0x03, 0x03, 0x80}
		eid, err := avcstate.SPSID(epSPS)
		c.Check(err == nil && eid == 0, "EP-protected SPS id: %v %d", err, eid)
		badEscape := []byte{0x67, 0x42, 0x00, 0x0A, 0x01, 0x00, 0x00, 0x03, 0x04}
		_, err = avcstate.SPSID(badEscape)
		c.Check(err != nil, "illegal escape must error")

		tab := avcstate.NewTable()
		_, err = tab.PutSPS(sps0A)
		c.Check(err == nil, "put sps A: %v", err)
		_, err = tab.PutSPS(spsNAL(0, 'B'))
		c.Check(err == nil, "put sps B: %v", err)
		got, ok := tab.SPS(0)
		c.Check(ok && bytes.Equal(got, spsNAL(0, 'B')), "same-id bytes must update")
		snap := tab.Clone()
		_, _ = tab.PutSPS(spsNAL(0, 'C'))
		snapGot, _ := snap.SPS(0)
		c.Check(bytes.Equal(snapGot, spsNAL(0, 'B')), "clone must retain snapshot bytes")

		// End-to-end: three descriptions with different length sizes, an
		// A->B same-id update performed by an unexported sample, and a
		// switch back to the old description which must reset from avcC.
		c0sps := spsNAL(0, 'a')
		c0pps := ppsNAL(0, 0, 'a')
		c1sps := spsNAL(0, '1')
		c1pps := ppsNAL(0, 0, '1')
		c2sps := spsNAL(0, '2')
		c2pps := ppsNAL(0, 0, '2')

		updatedSPS := spsNAL(0, 'U')
		updatedPPS := ppsNAL(0, 0, 'U')

		cfg := &muxCfg{
			movieTS: 1000, mediaTS: 1000, movieDur: 500, mediaDur: 600,
			descs: []mxDesc{
				{"avc1", 3, [][]byte{c0sps}, [][]byte{c0pps}}, // 4 byte lengths
				{"avc3", 1, [][]byte{c1sps}, [][]byte{c1pps}}, // 2 byte lengths
				{"avc1", 0, [][]byte{c2sps}, [][]byte{c2pps}}, // 1 byte lengths
			},
			samples: []mxSample{
				{delta: 100, nals: [][]byte{audNAL(), sliceNAL(5, 0, '1')}},               // s1 IDR d0
				{delta: 100, nals: [][]byte{updatedSPS, updatedPPS, sliceNAL(1, 0, '2')}}, // s2 update, unexported
				{delta: 100, nals: [][]byte{audNAL(), sliceNAL(5, 0, '3')}},               // s3 IDR d0 sees update
				{delta: 100, nals: [][]byte{audNAL(), sliceNAL(5, 0, '4')}},               // s4 IDR d1
				{delta: 100, nals: [][]byte{audNAL(), sliceNAL(5, 0, '5')}},               // s5 IDR d2
				{delta: 100, nals: [][]byte{audNAL(), sliceNAL(5, 0, '6')}},               // s6 IDR d0 reset
			},
			chunks: []mxChunk{
				{samples: []int{0, 1}, desc: 1},
				{samples: []int{2}, desc: 1},
				{samples: []int{3}, desc: 2},
				{samples: []int{4}, desc: 3},
				{samples: []int{5}, desc: 1},
			},
			stss: []uint32{1, 3, 4, 5, 6},
			elst: []mxElst{
				{dur: 100, mediaTime: 0},
				{dur: 100, mediaTime: 200},
				{dur: 100, mediaTime: 300},
				{dur: 100, mediaTime: 400},
				{dur: 100, mediaTime: 500},
			},
		}
		mp4Bytes := buildMP4(cfg)

		r := bytes.NewReader(mp4Bytes)
		_, _ = r.Seek(9, io.SeekStart)
		var out bytes.Buffer
		rep, err := mp4.ExportAVCEdits(r, &out, 1)
		c.Check(err == nil, "ExportAVCEdits state replay: %v", err)
		pos, _ := r.Seek(0, io.SeekCurrent)
		c.Check(pos == 9, "reader position restored, got %d", pos)
		if err != nil || rep == nil {
			return
		}
		c.Check(len(rep.Edits) == 5, "edits count %d", len(rep.Edits))
		c.Check(len(rep.Samples) == 5, "sample count %d", len(rep.Samples))

		wantPrefix := [][]byte{
			avcConfigPrefix([][]byte{c0sps}, [][]byte{c0pps}),
			avcConfigPrefix([][]byte{updatedSPS}, [][]byte{updatedPPS}),
			avcConfigPrefix([][]byte{c1sps}, [][]byte{c1pps}),
			avcConfigPrefix([][]byte{c2sps}, [][]byte{c2pps}),
			avcConfigPrefix([][]byte{c0sps}, [][]byte{c0pps}),
		}
		for i, smp := range rep.Samples {
			pkt := out.Bytes()[smp.ByteOffset : smp.ByteOffset+smp.ByteSize]
			c.Check(bytes.HasPrefix(pkt, []byte{0, 0, 0, 1}), "edit %d packet start code", i)
			c.Check(bytes.HasPrefix(pkt, wantPrefix[i]), "edit %d parameter prefix mismatch", i)

			nals := splitAnnexB(pkt)
			spsN := findNAL(nals, 7)
			ppsN := findNAL(nals, 8)
			idrN := findNAL(nals, 5)
			c.Check(spsN != nil && ppsN != nil && idrN != nil, "edit %d has sps/pps/idr", i)
			if idrN == nil || ppsN == nil || spsN == nil {
				continue
			}
			ppsID, err := avcstate.SlicePPSID(idrN)
			c.Check(err == nil, "edit %d idr pps parse: %v", i, err)
			foundPPS := findNAL(nals, 8)
			pid, sid, err := avcstate.PPSIDs(foundPPS)
			c.Check(err == nil && pid == ppsID, "edit %d pps id match %d vs %d: %v", i, pid, ppsID, err)
			wantSID, err := avcstate.SPSID(spsN)
			c.Check(err == nil && sid == wantSID, "edit %d sps ref %d vs %d", i, sid, wantSID)
			// Exact original bytes of the referenced parameter sets.
			c.Check(bytes.Equal(foundPPS, wantPPSBytes(i, c0pps, updatedPPS, c1pps, c2pps)), "edit %d exact PPS bytes", i)
		}
	})
}

func wantPPSBytes(edit int, c0, upd, c1, c2 []byte) []byte {
	switch edit {
	case 0:
		return c0
	case 1:
		return upd
	case 2:
		return c1
	case 3:
		return c2
	default:
		return c0
	}
}

/*=========================== TestAVCEditTiming ============================*/

func TestAVCEditTiming(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		sps := spsNAL(0, 't')
		pps := ppsNAL(0, 0, 't')
		samples := []mxSample{
			{delta: 100, ctts: 0, nals: [][]byte{audNAL(), sliceNAL(5, 0, '0')}},    // s1 I0 pts0
			{delta: 100, ctts: 200, nals: [][]byte{audNAL(), sliceNAL(1, 0, '1')}},  // s2 P  pts300
			{delta: 100, ctts: -100, nals: [][]byte{audNAL(), sliceNAL(1, 0, '2')}}, // s3 B  pts100
			{delta: 100, ctts: -100, nals: [][]byte{audNAL(), sliceNAL(1, 0, '3')}}, // s4 B  pts200
			{delta: 100, ctts: 0, nals: [][]byte{audNAL(), sliceNAL(5, 0, '4')}},    // s5 I  pts400
			{delta: 100, ctts: 0, nals: [][]byte{audNAL(), sliceNAL(1, 0, '5')}},    // s6 P  pts500
		}
		cfg := &muxCfg{
			movieTS: 1000, mediaTS: 1000, movieDur: 800, mediaDur: 600,
			descs:   []mxDesc{{"avc1", 3, [][]byte{sps}, [][]byte{pps}}},
			samples: samples,
			chunks: []mxChunk{
				{samples: []int{0, 1}, desc: 1},
				{samples: []int{2, 3}, desc: 1},
				{samples: []int{4, 5}, desc: 1},
			},
			hasCTTS: true,
			stss:    []uint32{1, 5},
			elst: []mxElst{
				{dur: 200, mediaTime: -1}, // empty
				{dur: 200, mediaTime: 100},
				{dur: 200, mediaTime: 400},
				{dur: 200, mediaTime: 100}, // reverse + repeat of edit 1
			},
		}
		b := buildMP4(cfg)
		r := bytes.NewReader(b)
		_, _ = r.Seek(3, io.SeekStart)
		var out bytes.Buffer
		rep, err := mp4.ExportAVCEdits(r, &out, 1)
		c.Check(err == nil, "ExportAVCEdits timing: %v", err)
		pos, _ := r.Seek(0, io.SeekCurrent)
		c.Check(pos == 3, "restore pos got %d", pos)
		if err != nil || rep == nil {
			return
		}

		c.Check(rep.MovieTimescale == 1000 && rep.MediaTimescale == 1000, "timescales")
		wantEdits := []mp4.AVCEditReportEdit{
			{MovieStart: 0, MovieDuration: 200, Empty: true},
			{MovieStart: 200, MovieDuration: 200, Empty: false},
			{MovieStart: 400, MovieDuration: 200, Empty: false},
			{MovieStart: 600, MovieDuration: 200, Empty: false},
		}
		c.Check(len(rep.Edits) == len(wantEdits), "edit count %d", len(rep.Edits))
		if len(rep.Edits) == len(wantEdits) {
			for i, e := range wantEdits {
				c.Check(rep.Edits[i] == e, "edit %d = %+v want %+v", i, rep.Edits[i], e)
			}
		}
		// 4 + 2 + 4 samples across the three media edits.
		c.Check(len(rep.Samples) == 10, "sample report count %d", len(rep.Samples))

		type wantS struct {
			edit     uint32
			src      uint32
			visible  bool
			dts      uint64
			pts      int64
			vStart   int64
			vEnd     int64
			firstPkt bool
		}
		wants := []wantS{
			{1, 1, false, 0, 0, 0, 0, true},
			{1, 2, false, 100, 300, 0, 0, false},
			{1, 3, true, 200, 100, 200000, 300000, false},
			{1, 4, true, 300, 200, 300000, 400000, false},
			{2, 5, true, 400, 400, 400000, 500000, true},
			{2, 6, true, 500, 500, 500000, 600000, false},
			{3, 1, false, 0, 0, 0, 0, true},
			{3, 2, false, 100, 300, 0, 0, false},
			{3, 3, true, 200, 100, 600000, 700000, false},
			{3, 4, true, 300, 200, 700000, 800000, false},
		}
		prefix := avcConfigPrefix([][]byte{sps}, [][]byte{pps})
		offsets := map[uint64]bool{}
		for i, w := range wants {
			if i >= len(rep.Samples) {
				break
			}
			s := rep.Samples[i]
			c.Check(s.EditIndex == w.edit, "s%d edit %d want %d", i, s.EditIndex, w.edit)
			c.Check(s.SourceSample == w.src, "s%d src %d want %d", i, s.SourceSample, w.src)
			c.Check(s.Visible == w.visible, "s%d visible %v", i, s.Visible)
			c.Check(s.DTS == w.dts, "s%d dts %d", i, s.DTS)
			c.Check(s.PTS == w.pts, "s%d pts %d", i, s.PTS)
			if w.visible {
				c.Check(s.VisibleDen == 1000, "s%d den %d", i, s.VisibleDen)
				c.Check(s.VisibleStartNum == w.vStart, "s%d vstart %d want %d", i, s.VisibleStartNum, w.vStart)
				c.Check(s.VisibleEndNum == w.vEnd, "s%d vend %d want %d", i, s.VisibleEndNum, w.vEnd)
				c.Check(s.VisibleEndNum > s.VisibleStartNum, "s%d positive visible span", i)
			} else {
				c.Check(s.VisibleDen == 0 && s.VisibleStartNum == 0 && s.VisibleEndNum == 0, "s%d invisible zeros", i)
			}
			// Address range is readable and exactly sized.
			c.Check(s.ByteOffset <= uint64(out.Len()), "s%d offset in range", i)
			c.Check(s.ByteSize > 0 && s.ByteOffset+s.ByteSize <= uint64(out.Len()), "s%d size in range", i)
			pkt := out.Bytes()[s.ByteOffset : s.ByteOffset+s.ByteSize]
			c.Check(bytes.HasPrefix(pkt, []byte{0, 0, 0, 1}), "s%d start code", i)
			c.Check(!offsets[s.ByteOffset], "s%d unique output offset %d", i, s.ByteOffset)
			offsets[s.ByteOffset] = true

			// Exact packet bytes versus independently built expectation.
			src := cfg.samples[w.src-1]
			var wantPkt []byte
			if w.firstPkt {
				wantPkt = append(wantPkt, prefix...)
			}
			for _, n := range src.nals {
				wantPkt = append(wantPkt, 0, 0, 0, 1)
				wantPkt = append(wantPkt, n...)
			}
			c.Check(bytes.Equal(pkt, wantPkt), "s%d exact packet bytes (got %d want %d)", i, len(pkt), len(wantPkt))

			// Every edit segment independently starts from an IDR after the
			// parameter prefix.
			if w.firstPkt {
				nals := splitAnnexB(pkt)
				c.Check(findNAL(nals, 7) != nil && findNAL(nals, 8) != nil, "s%d prefix sets", i)
				c.Check(findNAL(nals, 5) != nil, "s%d starts from IDR", i)
			}
		}

		// No elst: implicit media zero..mdhd.duration, no stss, co64 offsets.
		{
			cfg2 := baseRejectCfg()
			cfg2.elst = nil
			cfg2.stss = nil
			cfg2.useCO64 = true
			cfg2.movieDur = 200
			cfg2.mediaDur = 200
			b2 := buildMP4(cfg2)
			var out2 bytes.Buffer
			rep2, err := mp4.ExportAVCEdits(bytes.NewReader(b2), &out2, 1)
			c.Check(err == nil, "no-elst export: %v", err)
			if err == nil && rep2 != nil {
				c.Check(len(rep2.Edits) == 1 && !rep2.Edits[0].Empty, "no-elst one edit")
				c.Check(rep2.Edits[0].MovieStart == 0, "no-elst movie start")
				c.Check(len(rep2.Samples) == 2, "no-elst samples %d", len(rep2.Samples))
				if len(rep2.Samples) == 2 {
					c.Check(rep2.Samples[0].Visible && rep2.Samples[1].Visible, "no-elst all visible")
					c.Check(rep2.Samples[0].VisibleStartNum == 0 && rep2.Samples[0].VisibleEndNum == 100000,
						"no-elst first nums %d..%d", rep2.Samples[0].VisibleStartNum, rep2.Samples[0].VisibleEndNum)
				}
			}
		}

		// Distinct timescales with a fractional rational edit end.
		{
			spsX := spsNAL(0, 'x')
			ppsX := ppsNAL(0, 0, 'x')
			cfg3 := &muxCfg{
				movieTS: 1000, mediaTS: 1001, movieDur: 100, mediaDur: 400,
				descs: []mxDesc{{"avc1", 3, [][]byte{spsX}, [][]byte{ppsX}}},
				samples: []mxSample{
					{delta: 200, nals: [][]byte{sliceNAL(5, 0, 'a')}},
					{delta: 200, nals: [][]byte{sliceNAL(1, 0, 'b')}},
				},
				chunks: []mxChunk{{samples: []int{0, 1}, desc: 1}},
				stss:   []uint32{1},
				elst:   []mxElst{{dur: 100, mediaTime: 0}}, // end = 100*1001/1000 = 100.1
			}
			b3 := buildMP4(cfg3)
			var out3 bytes.Buffer
			rep3, err := mp4.ExportAVCEdits(bytes.NewReader(b3), &out3, 1)
			c.Check(err == nil, "fractional export: %v", err)
			if err == nil && rep3 != nil {
				c.Check(len(rep3.Samples) == 1, "fractional only first sample visible/in range, got %d", len(rep3.Samples))
				if len(rep3.Samples) == 1 {
					s := rep3.Samples[0]
					c.Check(s.Visible && s.VisibleDen == 1001, "fractional den")
					c.Check(s.VisibleStartNum == 0, "fractional start %d", s.VisibleStartNum)
					// Movie end 100 ticks scaled by mediaTS: 100*1001.
					c.Check(s.VisibleEndNum == 100100, "fractional end %d", s.VisibleEndNum)
				}
			}
		}
	})
}

/*=========================== TestAVCEditReject ===========================*/

type errReadSeeker struct {
	data      []byte
	pos       int64
	allowed   int64
	readBytes int64
}

var errBoom = errors.New("boom read failure")

func (e *errReadSeeker) Read(p []byte) (int, error) {
	if e.readBytes >= e.allowed {
		return 0, errBoom
	}
	max := int64(len(p))
	if rem := e.allowed - e.readBytes; rem < max {
		max = rem
	}
	n := copy(p[:max], e.data[e.pos:])
	e.pos += int64(n)
	e.readBytes += int64(n)
	if e.readBytes >= e.allowed {
		return n, errBoom
	}
	if n < len(p) {
		return n, io.EOF
	}
	return n, nil
}
func (e *errReadSeeker) Seek(off int64, whence int) (int64, error) {
	switch whence {
	case io.SeekStart:
		e.pos = off
	case io.SeekCurrent:
		e.pos += off
	case io.SeekEnd:
		e.pos = int64(len(e.data)) + off
	}
	return e.pos, nil
}

type byteByByteReader struct {
	r *bytes.Reader
}

func (b *byteByByteReader) Read(p []byte) (int, error) {
	if len(p) == 0 {
		return 0, nil
	}
	return b.r.Read(p[:1])
}
func (b *byteByByteReader) Seek(off int64, whence int) (int64, error) { return b.r.Seek(off, whence) }

type zeroWriter struct{}

func (zeroWriter) Write(p []byte) (int, error) { return 0, nil }

type chunkWriter struct{ buf *bytes.Buffer }

func (w chunkWriter) Write(p []byte) (int, error) {
	n := 7
	if n > len(p) {
		n = len(p)
	}
	return w.buf.Write(p[:n])
}

type boomWriter struct{ left int }

var errWriteBoom = errors.New("boom write")

func (w *boomWriter) Write(p []byte) (int, error) {
	if w.left <= 0 {
		return 0, errWriteBoom
	}
	n := len(p)
	if n > w.left {
		n = w.left
	}
	w.left -= n
	if w.left <= 0 {
		return n, errWriteBoom
	}
	return n, nil
}

func baseRejectCfg() *muxCfg {
	sps := spsNAL(0, 'r')
	pps := ppsNAL(0, 0, 'r')
	return &muxCfg{
		movieTS: 1000, mediaTS: 1000, movieDur: 100, mediaDur: 200,
		descs: []mxDesc{{"avc1", 3, [][]byte{sps}, [][]byte{pps}}},
		samples: []mxSample{
			{delta: 100, nals: [][]byte{sliceNAL(5, 0, 'a')}},
			{delta: 100, nals: [][]byte{sliceNAL(1, 0, 'b')}},
		},
		chunks: []mxChunk{{samples: []int{0, 1}, desc: 1}},
		stss:   []uint32{1},
		elst:   []mxElst{{dur: 100, mediaTime: 0}},
	}
}

func TestAVCEditReject(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		runReject := func(name string, mutate func(*muxCfg), callTrack uint32, input []byte) {
			var b []byte
			if input != nil {
				b = input
			} else {
				cfg := baseRejectCfg()
				if mutate != nil {
					mutate(cfg)
				}
				b = buildMP4(cfg)
			}
			rdr := bytes.NewReader(b)
			_, _ = rdr.Seek(23, io.SeekStart)
			var out bytes.Buffer
			rep, err := mp4.ExportAVCEdits(rdr, &out, callTrack)
			c.Check(err != nil, "%s: expected error", name)
			c.Check(rep == nil, "%s: nil report on rejection", name)
			c.Check(out.Len() == 0, "%s: zero output before rejection, got %d bytes", name, out.Len())
			pos, _ := rdr.Seek(0, io.SeekCurrent)
			c.Check(pos == 23, "%s: reader restored to 23, got %d", name, pos)
		}

		runReject("bad pps reference", func(cfg *muxCfg) {
			cfg.samples[0].nals = [][]byte{sliceNAL(5, 9, 'x')}
		}, 1, nil)

		runReject("bad sps reference", func(cfg *muxCfg) {
			badPPS := ppsNAL(2, 77, 'x')
			cfg.samples[0].nals = [][]byte{badPPS, sliceNAL(5, 2, 'x')}
		}, 1, nil)

		runReject("non-unit rate", func(cfg *muxCfg) {
			cfg.elst[0].rateSet = true
			cfg.elst[0].rateInt = 2
		}, 1, nil)

		runReject("external dref", func(cfg *muxCfg) {
			cfg.externalDref = true
		}, 1, nil)

		runReject("truncated nal length", func(cfg *muxCfg) {
			cfg.samples[0].raw = []byte{0, 0, 0, 99, 0x65, 0x80}
		}, 1, nil)

		runReject("illegal escape", func(cfg *muxCfg) {
			cfg.samples[0].nals = [][]byte{{0x67, 0x42, 0x00, 0x0A, 0x01, 0x00, 0x00, 0x03, 0x04}}
		}, 1, nil)

		runReject("partition nal", func(cfg *muxCfg) {
			cfg.samples[0].nals = [][]byte{{0x02, 0x80, 0x00}}
		}, 1, nil)

		runReject("extension vcl", func(cfg *muxCfg) {
			cfg.samples[0].nals = [][]byte{{0x14, 0x80, 0x00}}
		}, 1, nil)

		runReject("stss misses idr", func(cfg *muxCfg) {
			cfg.stss = []uint32{2}
		}, 1, nil)

		runReject("no idr", func(cfg *muxCfg) {
			cfg.samples[0].nals = [][]byte{sliceNAL(1, 0, 'a')}
			cfg.stss = nil
		}, 1, nil)

		runReject("pts overlap", func(cfg *muxCfg) {
			cfg.hasCTTS = true
			cfg.samples[0].ctts = 0
			cfg.samples[1].ctts = -100 // pts 100 and 0? force equal:
			cfg.samples[1].ctts = 0    // both pts 0 and 100? use overlap below
			cfg.samples[0].delta = 100
			cfg.samples[1].delta = 100
			// Make sample 2 PTS start before sample 1 interval end while
			// sharing decode ordering.
			cfg.samples[1].ctts = -50
		}, 1, nil)

		runReject("coverage gap", func(cfg *muxCfg) {
			cfg.elst[0].mediaTime = 500
			cfg.elst[0].dur = 100
		}, 1, nil)

		runReject("unknown track", nil, 999, nil)
		runReject("not an mp4", nil, 1, []byte("not an mp4 file at all just bytes"))

		// Read faults must surface as errors and still restore position.
		good := buildMP4(baseRejectCfg())
		for _, allowed := range []int64{1, 3, 8, 20, 40, 80} {
			r := &errReadSeeker{data: good, allowed: allowed}
			_, _ = r.Seek(31, io.SeekStart)
			var out bytes.Buffer
			_, err := mp4.ExportAVCEdits(r, &out, 1)
			c.Check(err != nil, "read fault allowed=%d must error", allowed)
			pos, _ := r.Seek(0, io.SeekCurrent)
			c.Check(pos == 31, "read fault restore pos, got %d", pos)
		}

		// Byte-at-a-time short reads still succeed via io.ReadFull.
		br := &byteByByteReader{r: bytes.NewReader(good)}
		_, _ = br.Seek(0, io.SeekStart)
		var slowOut bytes.Buffer
		rep, err := mp4.ExportAVCEdits(br, &slowOut, 1)
		c.Check(err == nil && rep != nil, "byte-by-byte reader: %v", err)
		c.Check(slowOut.Len() > 0, "byte-by-byte produced output")

		// Writer that rejects immediately: validation has completed, but the
		// write error must be returned.
		{
			r := bytes.NewReader(good)
			_, err := mp4.ExportAVCEdits(r, zeroWriter{}, 1)
			c.Check(err != nil, "zero writer must return short-write error")
		}
		{
			r := bytes.NewReader(good)
			_, err := mp4.ExportAVCEdits(r, &boomWriter{left: 13}, 1)
			c.Check(err != nil, "boom writer must error")
		}
		// Writer accepting tiny chunks with nil returns still completes.
		{
			r := bytes.NewReader(good)
			var buf bytes.Buffer
			_, err := mp4.ExportAVCEdits(r, chunkWriter{buf: &buf}, 1)
			c.Check(err == nil, "chunk writer: %v", err)
			c.Check(buf.Len() > 0, "chunk writer output")
		}
	})
}
