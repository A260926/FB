package mpeg2

import (
	"errors"

	"github.com/yapingcat/gomedia/go-codec"
)

// Transport stream timing constants, all expressed in 27 MHz ticks.
const (
	cbrClockRate       uint64 = 27000000
	cbrTablePeriodTick uint64 = 2700000 // PAT/PMT are re-scheduled every 100 ms
	cbrPCRMod          uint64 = (1 << 33) * 300
	cbrNullPID         uint16 = 0x1fff
	// cbrSlotNumerator is 188 bytes * 8 bits * 27 MHz: the numerator of the
	// transmission duration of one slot.
	cbrSlotNumerator uint64 = TS_PAKCET_SIZE * 8 * cbrClockRate
)

// CBRConfig configures a constant-bit-rate MPEG-TS muxer.
type CBRConfig struct {
	Bitrate          uint64           // bits per second, 128000..1000000000
	StartPCR27MHz    uint64           // PCR value carried by the packet emitted at slot 0
	PCRInterval27MHz uint64           // maximum distance between two PCR values, in 27 MHz ticks
	Streams          []TS_STREAM_TYPE // one entry per elementary stream, 1..8 entries
}

// cbrMediaPacket is one fully assembled 188-byte media TS packet waiting for
// its transmission slot.
type cbrMediaPacket struct {
	data   [TS_PAKCET_SIZE]byte
	pid    uint16
	pcrPID bool
	head   bool // first packet of a PES, carries the payload_unit_start_indicator
	idr    bool
}

// cbrStream is the muxer-side view of an elementary stream.
type cbrStream struct {
	pid        uint16
	streamtype TS_STREAM_TYPE
	cc         uint8
}

// CBRMuxer packets an MPEG transport stream at a fixed bitrate. Media written
// through Write is queued as complete TS packets and only leaves the muxer
// when AdvanceTo releases its transmission slot. Every slot is filled: when
// no scheduled content is due, a null packet (PID 0x1fff) is emitted.
type CBRMuxer struct {
	bitrate     uint64
	startPCR    uint64
	pcrInterval uint64
	pcrPID      uint16
	streams     []cbrStream
	streamByPID map[uint16]*cbrStream
	patCC       uint8
	pmtCC       uint8
	pending     []*cbrMediaPacket
	elapsed     uint64
	// sentSlots is the index of the next slot to emit. slotPos/slotRem are
	// floor(sentSlots*K/bitrate) and sentSlots*K mod bitrate, maintained
	// incrementally so neither multiplication nor per-slot rounding can
	// overflow or accumulate drift.
	sentSlots   uint64
	slotPos     uint64
	slotRem     uint64
	lastPCR     uint64 // slot position of the most recent PCR field
	havePCR     bool
	nextTableAt uint64 // slot index at which the next PAT is due
	pmtPending  bool   // a PAT was emitted and its PMT still has to go out
	patPacket   []byte
	pmtPacket   []byte
	OnPacket    func(pkg []byte)
}

// NewCBRMuxer builds a CBR muxer whose PID topology never changes afterwards.
func NewCBRMuxer(config CBRConfig) (*CBRMuxer, error) {
	if config.Bitrate < 128000 || config.Bitrate > 1000000000 {
		return nil, errors.New("cbr muxer: bitrate must be between 128000 and 1000000000 bit/s")
	}
	if len(config.Streams) < 1 || len(config.Streams) > 8 {
		return nil, errors.New("cbr muxer: Streams must contain between 1 and 8 entries")
	}
	mux := &CBRMuxer{
		bitrate:     config.Bitrate,
		startPCR:    config.StartPCR27MHz,
		pcrInterval: config.PCRInterval27MHz,
		streamByPID: make(map[uint16]*cbrStream),
	}
	pcrIndex := -1
	for _, st := range config.Streams {
		switch st {
		case TS_STREAM_H264, TS_STREAM_H265, TS_STREAM_AAC:
		default:
			return nil, errors.New("cbr muxer: only H264, H265 and AAC streams are supported")
		}
	}
	for i, st := range config.Streams {
		if st != TS_STREAM_AAC && pcrIndex == -1 {
			pcrIndex = i
		}
	}
	if pcrIndex == -1 {
		pcrIndex = 0 // audio-only program: the first audio stream owns the PCR
	}
	for i, st := range config.Streams {
		pid := uint16(0x100 + i)
		mux.streams = append(mux.streams, cbrStream{pid: pid, streamtype: st})
		mux.streamByPID[pid] = &mux.streams[i]
	}
	mux.pcrPID = mux.streams[pcrIndex].pid
	// A PCR interval must leave room for at least three packet slots, and the
	// standard ceiling is 100 ms.
	slotTicks := cbrSlotTick(mux.bitrate)
	if config.PCRInterval27MHz > 1080000 {
		return nil, errors.New("cbr muxer: PCR interval must not exceed 1080000 ticks (100 ms)")
	}
	if config.PCRInterval27MHz < 3*slotTicks {
		return nil, errors.New("cbr muxer: PCR interval must hold at least three packet slots")
	}
	mux.patPacket = mux.buildPat()
	mux.pmtPacket = mux.buildPmt()
	return mux, nil
}

// StreamPIDs returns an independent copy of the stream PID list, in the same
// order as the Streams given to NewCBRMuxer.
func (mux *CBRMuxer) StreamPIDs() []uint16 {
	pids := make([]uint16, len(mux.streams))
	for i := range mux.streams {
		pids[i] = mux.streams[i].pid
	}
	return pids
}

// PendingPackets reports how many assembled media TS packets have not been
// transmitted yet.
func (mux *CBRMuxer) PendingPackets() int {
	return len(mux.pending)
}

// cbrSlotTick is the floored transmission duration of one slot, in 27 MHz
// ticks. It is never zero for bitrates accepted by NewCBRMuxer.
func cbrSlotTick(bitrate uint64) uint64 {
	return cbrSlotNumerator / bitrate
}

// currentPCR wraps the program clock for the current slot position.
func (mux *CBRMuxer) currentPCR() (base uint64, ext uint16) {
	pcr := (mux.startPCR + mux.slotPos) % cbrPCRMod
	return pcr / 300, uint16(pcr % 300)
}

// Write fragments one access unit into queued TS packets. It never invokes
// OnPacket and does not retain data, so the caller may reuse data afterwards.
func (mux *CBRMuxer) Write(pid uint16, data []byte, pts uint64, dts uint64) error {
	stream := mux.streamByPID[pid]
	if stream == nil {
		return errors.New("cbr muxer: unknown pid")
	}
	var withaud bool
	if stream.streamtype == TS_STREAM_H264 || stream.streamtype == TS_STREAM_H265 {
		codec.SplitFrame(data, func(nalu []byte) bool {
			if stream.streamtype == TS_STREAM_H264 {
				naluType := codec.H264NaluTypeWithoutStartCode(nalu)
				if naluType == codec.H264_NAL_AUD {
					withaud = true
					return false
				} else if codec.IsH264VCLNaluType(naluType) {
					return false
				}
				return true
			}
			naluType := codec.H265NaluTypeWithoutStartCode(nalu)
			if naluType == codec.H265_NAL_AUD {
				withaud = true
				return false
			} else if codec.IsH265VCLNaluType(naluType) {
				return false
			}
			return true
		})
	}

	idr := false
	switch stream.streamtype {
	case TS_STREAM_H264:
		idr = codec.IsH264IDRFrame(data)
	case TS_STREAM_H265:
		idr = codec.IsH265IDRFrame(data)
	}

	frame := make([]byte, len(data))
	copy(frame, data)
	first := true
	for {
		pkt := &cbrMediaPacket{pid: pid, pcrPID: pid == mux.pcrPID, head: first, idr: idr && first}
		frame = mux.fillMediaPacket(pkt, stream, frame, pts*90, dts*90, withaud, first)
		mux.pending = append(mux.pending, pkt)
		first = false
		if len(frame) == 0 {
			break
		}
	}
	return nil
}

// fillMediaPacket assembles one media TS packet, mirroring the legacy
// TSMuxer PES layout (AUD insertion, PES header, adaptation stuffing) while
// PCR-PID PES heads reserve an adaptation field that is stamped at the real
// output slot.
func (mux *CBRMuxer) fillMediaPacket(pkt *cbrMediaPacket, stream *cbrStream, data []byte,
	pts, dts uint64, withaud, first bool) (remain []byte) {
	bsw := codec.NewBitStreamWriter(TS_PAKCET_SIZE)
	var tshdr TSPacket
	if first {
		tshdr.Payload_unit_start_indicator = 1
	}
	tshdr.PID = stream.pid
	tshdr.Adaptation_field_control = 0x01
	tshdr.Continuity_counter = stream.cc

	headlen := 4
	var adaptation *Adaptation_field
	if first && pkt.idr {
		adaptation = new(Adaptation_field)
		tshdr.Adaptation_field_control |= 0x20
		adaptation.Random_access_indicator = 1
		headlen += 2
	}
	// PCR is stamped at transmission time; reserve the adaptation bytes here
	// so payload layout is identical whether or not a PCR ends up in it.
	if first && pkt.pcrPID {
		if adaptation == nil {
			adaptation = new(Adaptation_field)
			headlen += 2
		}
		tshdr.Adaptation_field_control |= 0x20
		adaptation.PCR_flag = 1
		headlen += 6
	}

	var payload []byte
	var pespkg *PesPacket
	if first {
		oldheadlen := headlen
		headlen += 19
		if !withaud && stream.streamtype == TS_STREAM_H264 {
			headlen += 6
			payload = append(payload, H264_AUD_NALU...)
		} else if !withaud && stream.streamtype == TS_STREAM_H265 {
			headlen += 7
			payload = append(payload, H265_AUD_NALU...)
		}
		pespkg = NewPesPacket()
		pespkg.PTS_DTS_flags = 0x03
		pespkg.PES_header_data_length = 10
		pespkg.Pts = pts
		pespkg.Dts = dts
		pespkg.Stream_id = uint8(findPESIDByStreamType(stream.streamtype))
		if pkt.idr {
			pespkg.Data_alignment_indicator = 1
		}
		if headlen-oldheadlen-6+len(data) > 0xFFFF {
			pespkg.PES_packet_length = 0
		} else {
			pespkg.PES_packet_length = uint16(len(data) + headlen - oldheadlen - 6)
		}
	}

	if len(data)+headlen < TS_PAKCET_SIZE {
		if adaptation == nil {
			adaptation = new(Adaptation_field)
			headlen += 1
			if TS_PAKCET_SIZE-len(data)-headlen >= 1 {
				headlen += 1
			} else {
				adaptation.SingleStuffingByte = true
			}
		}
		adaptation.Stuffing_byte = uint8(TS_PAKCET_SIZE - len(data) - headlen)
		payload = append(payload, data...)
		remain = data[:0]
	} else {
		payload = append(payload, data[0:TS_PAKCET_SIZE-headlen]...)
		remain = data[TS_PAKCET_SIZE-headlen:]
	}

	if adaptation != nil {
		tshdr.Field = adaptation
		tshdr.Adaptation_field_control |= 0x02
	}
	tshdr.EncodeHeader(bsw)
	if pespkg != nil {
		pespkg.Pes_payload = payload
		pespkg.Encode(bsw)
	} else {
		bsw.PutBytes(payload)
	}
	out := bsw.Bits()
	if len(out) != TS_PAKCET_SIZE {
		panic("cbr muxer: media packet is not 188 bytes")
	}
	copy(pkt.data[:], out)
	return remain
}

// buildPat renders the immutable PAT packet (program 1 -> PMT PID 0x200).
func (mux *CBRMuxer) buildPat() []byte {
	var tshdr TSPacket
	tshdr.Payload_unit_start_indicator = 1
	tshdr.PID = 0
	tshdr.Adaptation_field_control = 0x01
	tshdr.Continuity_counter = mux.patCC
	bsw := codec.NewBitStreamWriter(TS_PAKCET_SIZE)
	tshdr.EncodeHeader(bsw)
	bsw.PutByte(0x00) // pointer field
	pat := NewPat()
	pat.Version_number = 0
	pat.Pmts = append(pat.Pmts, PmtPair{Program_number: 1, PID: 0x200})
	pat.Encode(bsw)
	bsw.FillRemainData(0xff)
	return append([]byte(nil), bsw.Bits()...)
}

// buildPmt renders the immutable PMT packet, including every configured stream.
func (mux *CBRMuxer) buildPmt() []byte {
	var tshdr TSPacket
	tshdr.Payload_unit_start_indicator = 1
	tshdr.PID = 0x200
	tshdr.Adaptation_field_control = 0x01
	tshdr.Continuity_counter = mux.pmtCC
	bsw := codec.NewBitStreamWriter(TS_PAKCET_SIZE)
	tshdr.EncodeHeader(bsw)
	bsw.PutByte(0x00) // pointer field
	pmt := NewPmt()
	pmt.Program_number = 1
	pmt.Version_number = 0
	pmt.PCR_PID = mux.pcrPID
	for i := range mux.streams {
		pmt.Streams = append(pmt.Streams, StreamPair{
			StreamType:     uint8(mux.streams[i].streamtype),
			Elementary_PID: mux.streams[i].pid,
			ES_Info_Length: 0,
		})
	}
	pmt.Encode(bsw)
	bsw.FillRemainData(0xff)
	return append([]byte(nil), bsw.Bits()...)
}

// AdvanceTo releases every slot whose transmission position is strictly
// smaller than elapsed27MHz. The emitted byte stream depends only on the
// sequence of slot indices, so one big advance and several smaller advances
// produce identical output.
func (mux *CBRMuxer) AdvanceTo(elapsed27MHz uint64) error {
	if mux.OnPacket == nil {
		return errors.New("cbr muxer: OnPacket callback is not set")
	}
	if elapsed27MHz < mux.elapsed {
		return errors.New("cbr muxer: elapsed time cannot go backwards")
	}
	for mux.slotPos < elapsed27MHz {
		mux.emitSlot(mux.sentSlots)
		// Advance the exact floor(n*K/bitrate) state by one slot.
		mux.slotRem += cbrSlotNumerator
		mux.slotPos += mux.slotRem / mux.bitrate
		mux.slotRem %= mux.bitrate
		mux.sentSlots++
	}
	mux.elapsed = elapsed27MHz
	return nil
}

// emitSlot fills exactly one 188-byte slot following the priority order:
// PCR heartbeat > due PAT/PMT tables > queued media > null packet.
func (mux *CBRMuxer) emitSlot(slot uint64) {
	// The stream always opens with PAT, PMT and then the first PCR heartbeat.
	if slot == 0 {
		mux.emitPat(slot)
		return
	}
	if slot == 1 {
		mux.emitPmt()
		return
	}

	pcrDue := !mux.havePCR || mux.slotPos-mux.lastPCR >= mux.pcrInterval
	if pcrDue {
		// Before any PCR exists the opening heartbeat is mandatory, even if
		// media is already queued. Afterwards a PCR-PID PES head emitted in
		// this slot takes the PCR in its own adaptation field.
		if mux.havePCR && len(mux.pending) > 0 && mux.pending[0].head && mux.pending[0].pcrPID {
			mux.emitMedia()
			return
		}
		mux.emitHeartbeat()
		return
	}

	if mux.pmtPending {
		mux.emitPmt()
		return
	}
	// A due table can be pushed back by a PCR heartbeat winning the slot; use
	// >= so a delayed PAT still goes out on the next available slot.
	if mux.nextTableAt != 0 && slot >= mux.nextTableAt {
		mux.emitPat(slot)
		return
	}

	if len(mux.pending) > 0 {
		mux.emitMedia()
		return
	}
	mux.emitNull()
}

// emitPat sends a PAT packet, bumps the PAT continuity counter and schedules
// the next PAT/PMT group exactly 2,700,000 transport ticks after this PAT's
// own slot start.
func (mux *CBRMuxer) emitPat(slot uint64) {
	pkt := mux.patPacket
	pkt[3] = (pkt[3] & 0xf0) | (mux.patCC & 0x0f)
	mux.patCC = (mux.patCC + 1) % 16
	mux.OnPacket(pkt)
	mux.pmtPending = true
	mux.scheduleNextTable(slot)
}

// emitPmt sends the PMT that belongs to the most recently emitted PAT.
func (mux *CBRMuxer) emitPmt() {
	pkt := mux.pmtPacket
	pkt[3] = (pkt[3] & 0xf0) | (mux.pmtCC & 0x0f)
	mux.pmtCC = (mux.pmtCC + 1) % 16
	mux.OnPacket(pkt)
	mux.pmtPending = false
}

// scheduleNextTable computes the smallest slot index whose start position is
// at least cbrTablePeriodTick after the given PAT slot, using exact modular
// arithmetic so no large multiplication is ever performed.
func (mux *CBRMuxer) scheduleNextTable(patSlot uint64) {
	// pos(d)-pos(patSlot) = floor((rem + (d-patSlot)*K)/bitrate), where rem is
	// patSlot*K mod bitrate and equals the current slotRem while emitting patSlot.
	num := cbrTablePeriodTick*mux.bitrate - mux.slotRem
	d := num / cbrSlotNumerator
	if num%cbrSlotNumerator != 0 {
		d++
	}
	mux.nextTableAt = patSlot + d
}

// emitHeartbeat sends an adaptation-only packet on the PCR PID. It does not
// consume that PID's payload continuity counter.
func (mux *CBRMuxer) emitHeartbeat() {
	stream := mux.streamByPID[mux.pcrPID]
	cc := uint8(0)
	if stream != nil {
		cc = stream.cc
	}
	pkt := make([]byte, TS_PAKCET_SIZE)
	pkt[0] = 0x47
	pkt[1] = byte(mux.pcrPID >> 8)
	pkt[2] = byte(mux.pcrPID)
	pkt[3] = 0x20 | (cc & 0x0f) // adaptation field only
	// 184 bytes follow the 4-byte header: length byte, flags byte, 6 PCR
	// bytes and 0xff stuffing.
	pkt[4] = TS_PAKCET_SIZE - 4 - 1
	pkt[5] = 0x10 // PCR_flag set, every other indicator zero
	base, ext := mux.currentPCR()
	mux.writePCRBytes(pkt[6:12], base, ext)
	for i := 12; i < TS_PAKCET_SIZE; i++ {
		pkt[i] = 0xff
	}
	mux.OnPacket(pkt)
	mux.lastPCR = mux.slotPos
	mux.havePCR = true
}

// writePCRBytes encodes base (33 bits) and extension (9 bits) with all six
// reserved bits set to 1, as required by the transport stream specification.
func (mux *CBRMuxer) writePCRBytes(b []byte, base uint64, ext uint16) {
	b[0] = byte(base >> 25)
	b[1] = byte(base >> 17)
	b[2] = byte(base >> 9)
	b[3] = byte(base >> 1)
	b[4] = byte(base<<7) | 0x7e | byte(ext>>8)
	b[5] = byte(ext)
}

// emitMedia transmits the head of the media queue. The payload continuity
// counter is assigned at this point so it advances exactly once per actual
// output packet; heartbeats on the same PID never touch it. A PCR-PID PES
// head always carries a PCR stamped at its real transmission slot and
// refreshes the PCR clock.
func (mux *CBRMuxer) emitMedia() {
	pkt := mux.pending[0]
	mux.pending = mux.pending[1:]
	out := make([]byte, TS_PAKCET_SIZE)
	copy(out, pkt.data[:])
	stream := mux.streamByPID[pkt.pid]
	out[3] = (out[3] & 0xf0) | (stream.cc & 0x0f)
	stream.cc = (stream.cc + 1) % 16
	if pkt.head && pkt.pcrPID {
		base, ext := mux.currentPCR()
		mux.stampPCR(out, base, ext)
		mux.lastPCR = mux.slotPos
		mux.havePCR = true
	}
	mux.OnPacket(out)
}

// stampPCR writes the PCR into a packet that already reserved an adaptation
// field with PCR_flag set (the first packet of a PCR-PID PES).
func (mux *CBRMuxer) stampPCR(pkt []byte, base uint64, ext uint16) {
	if pkt[3]&0x20 == 0 || pkt[4] == 0 {
		return // no adaptation field to stamp
	}
	pkt[5] |= 0x10 // PCR_flag
	mux.writePCRBytes(pkt[6:12], base, ext)
}

// emitNull sends a null packet on PID 0x1fff.
func (mux *CBRMuxer) emitNull() {
	pkt := make([]byte, TS_PAKCET_SIZE)
	pkt[0] = 0x47
	pkt[1] = 0x1f
	pkt[2] = 0xff
	for i := 3; i < TS_PAKCET_SIZE; i++ {
		pkt[i] = 0xff
	}
	mux.OnPacket(pkt)
}
