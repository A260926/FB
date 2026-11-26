package mpeg2

import (
	"errors"
	"fmt"

	"github.com/yapingcat/gomedia/go-codec"
)

// CBRConfig configures the library-level constant-bitrate MPEG-TS muxer.
//
// Bitrate is the constant transport rate in bit/s and must be within
// [128000, 1000000000]. StartPCR27MHz is the unwrapped PCR value of the
// first slot in 27 MHz ticks (it is wrapped at 2^33*300 when encoded).
// PCRInterval27MHz is the maximum gap between PCR stamps in 27 MHz ticks;
// it must be <= 1080000 and large enough to contain at least three full
// packet slots at the configured bitrate.
type CBRConfig struct {
	Bitrate          uint64
	StartPCR27MHz    uint64
	PCRInterval27MHz uint64
	Streams          []TS_STREAM_TYPE
}

const (
	cbrClockHz           uint64 = 27000000
	cbrSlotNumerator     uint64 = uint64(TS_PAKCET_SIZE) * 8 * 27000000 // 188*8*27e6
	cbrTableRepriseTicks uint64 = 2700000
	cbrPCRWrapModulus    uint64 = (uint64(1) << 33) * 300
	cbrMinBitrate        uint64 = 128000
	cbrMaxBitrate        uint64 = 1000000000
	cbrMaxPCRInterval    uint64 = 1080000
	cbrProgramNumber     uint16 = 1
	cbrPMTPID            uint16 = 0x0200
	cbrFirstStreamPID    uint16 = 0x0100
	cbrNullPID           uint16 = 0x1FFF
)

type cbrKind int

const (
	cbrKindPAT cbrKind = iota
	cbrKindPMT
	cbrKindHeartbeat
	cbrKindMedia
	cbrKindNull
)

type cbrStream struct {
	pid uint16
	typ TS_STREAM_TYPE
	pes *pes_stream
	cc  uint8 // payload continuity counter, advanced only when a packet is sent
}

type cbrQueuedPacket struct {
	pid    uint16
	pkt    [TS_PAKCET_SIZE]byte
	hasPCR bool
}

// CBRMuxer produces a constant-bitrate MPEG-TS stream. Media is fed with
// Write and turned into pending TS packets; output is driven exclusively by
// AdvanceTo. OnPacket receives every 188-byte transport packet in order.
//
// The byte slice passed to OnPacket is only valid for the duration of the
// call and must be copied if it needs to be retained. Re-entrant calls from
// inside OnPacket are not supported.
type CBRMuxer struct {
	// OnPacket is invoked once per emitted 188-byte TS packet. It must be
	// set before calling AdvanceTo.
	OnPacket func(pkg []byte)

	cfg     CBRConfig
	pids    []uint16
	streams []*cbrStream
	byPID   map[uint16]*cbrStream
	pcrPID  uint16

	queue []cbrQueuedPacket

	// writePES is reused unchanged through this packetizer so that PES
	// framing, PTS/DTS and AUD behaviour stay identical to TSMuxer.
	packetizer *TSMuxer
	pmtTable   *table_pmt

	// Exact slot clock. Slot n starts at floor(n*cbrSlotNumerator/Bitrate).
	// It is maintained as quotient/remainder accumulation so no growing
	// n*numerator product is ever multiplied and no per-packet rounding
	// drift is introduced.
	n     uint64 // index of the next packet/slot
	pos   uint64 // exact start position of slot n (27 MHz ticks)
	whole uint64 // cbrSlotNumerator / Bitrate
	rem   uint64 // cbrSlotNumerator % Bitrate
	frac  uint64 // n*cbrSlotNumerator mod Bitrate

	lastElapsed uint64

	patCC uint8
	pmtCC uint8

	lastPATPos uint64 // actual slot start of the previous PAT
	pmtPending bool

	pcrLast uint64 // unwrapped slot position carrying the most recent PCR
}

// NewCBRMuxer validates cfg and builds a CBRMuxer with a fixed topology.
// Between 1 and 8 H264/H265/AAC streams are assigned PIDs 0x100 upward in
// list order. The program number is 1, the PMT PID is 0x200 and the first
// video stream is the PCR PID (the first audio stream when there is no
// video).
func NewCBRMuxer(cfg CBRConfig) (*CBRMuxer, error) {
	if cfg.Bitrate < cbrMinBitrate || cfg.Bitrate > cbrMaxBitrate {
		return nil, fmt.Errorf("cbr: bitrate %d out of allowed range [%d,%d] bit/s",
			cfg.Bitrate, cbrMinBitrate, cbrMaxBitrate)
	}
	if len(cfg.Streams) < 1 || len(cfg.Streams) > 8 {
		return nil, errors.New("cbr: number of streams must be between 1 and 8")
	}
	for _, typ := range cfg.Streams {
		switch typ {
		case TS_STREAM_H264, TS_STREAM_H265, TS_STREAM_AAC:
		default:
			return nil, fmt.Errorf("cbr: unsupported stream type %d", typ)
		}
	}
	if cfg.PCRInterval27MHz > cbrMaxPCRInterval {
		return nil, fmt.Errorf("cbr: PCR interval %d exceeds maximum %d ticks",
			cfg.PCRInterval27MHz, cbrMaxPCRInterval)
	}
	// Three complete slots starting at slot 0 finish at
	// floor(3*cbrSlotNumerator/Bitrate).
	minInterval := (3 * cbrSlotNumerator) / cfg.Bitrate
	if cfg.PCRInterval27MHz < minInterval {
		return nil, fmt.Errorf("cbr: PCR interval %d cannot contain three full packet slots (minimum %d ticks)",
			cfg.PCRInterval27MHz, minInterval)
	}

	m := &CBRMuxer{
		cfg:      cfg,
		byPID:    make(map[uint16]*cbrStream, len(cfg.Streams)),
		whole:    cbrSlotNumerator / cfg.Bitrate,
		rem:      cbrSlotNumerator % cfg.Bitrate,
		pmtTable: NewTablePmt(),
	}
	m.pmtTable.pid = cbrPMTPID
	m.pmtTable.pm = cbrProgramNumber
	m.pmtTable.version_number = 0

	for i, typ := range cfg.Streams {
		pid := cbrFirstStreamPID + uint16(i)
		s := &cbrStream{
			pid: pid,
			typ: typ,
			pes: NewPESStream(pid, typ),
		}
		m.streams = append(m.streams, s)
		m.pids = append(m.pids, pid)
		m.byPID[pid] = s
		m.pmtTable.streams = append(m.pmtTable.streams, s.pes)
	}

	// First video stream wins; otherwise the first stream (audio).
	m.pcrPID = m.streams[0].pid
	for _, s := range m.streams {
		if findPESIDByStreamType(s.typ) == PES_STREAM_VIDEO {
			m.pcrPID = s.pid
			break
		}
	}
	m.pmtTable.pcr_pid = m.pcrPID

	// Route the unchanged PES packetizer into the pending queue instead of
	// the user callback. writePES hands out a writer buffer that is reset on
	// the next packet, so each 188-byte packet must be copied.
	m.packetizer = NewTSMuxer()
	m.packetizer.OnPacket = func(p []byte) {
		qp := cbrQueuedPacket{
			pid: uint16(p[1]&0x1f)<<8 | uint16(p[2]),
		}
		copy(qp.pkt[:], p)
		afc := (p[3] >> 4) & 0x03
		qp.hasPCR = (afc == 0x02 || afc == 0x03) && p[4] > 0 && (p[5]&0x10) != 0
		m.queue = append(m.queue, qp)
	}

	return m, nil
}

// StreamPIDs returns an independent copy of the assigned PIDs in stream
// order (0x100, 0x101, ...).
func (m *CBRMuxer) StreamPIDs() []uint16 {
	out := make([]uint16, len(m.pids))
	copy(out, m.pids)
	return out
}

// PendingPackets returns the number of packetized media TS packets that have
// not yet been emitted by AdvanceTo.
func (m *CBRMuxer) PendingPackets() int {
	return len(m.queue)
}

// Write packetizes one media access unit (timestamps in milliseconds) into
// pending TS packets. It never invokes OnPacket. The data slice may be
// reused by the caller as soon as Write returns.
func (m *CBRMuxer) Write(pid uint16, data []byte, pts uint64, dts uint64) error {
	st := m.byPID[pid]
	if st == nil {
		return fmt.Errorf("cbr: pid 0x%04x is not part of this muxer", pid)
	}

	var withaud bool
	if st.typ == TS_STREAM_H264 || st.typ == TS_STREAM_H265 {
		codec.SplitFrame(data, func(nalu []byte) bool {
			if st.typ == TS_STREAM_H264 {
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

	var idrFlag bool
	switch st.typ {
	case TS_STREAM_H264:
		idrFlag = codec.IsH264IDRFrame(data)
	case TS_STREAM_H265:
		idrFlag = codec.IsH265IDRFrame(data)
	}

	// Milliseconds to the 90 kHz PTS/DTS time base used by PES.
	m.packetizer.writePES(st.pes, m.pmtTable, data, pts*90, dts*90, idrFlag, withaud)
	return nil
}

// AdvanceTo emits every due TS packet whose slot start position is strictly
// smaller than elapsed27MHz. See the CBR slot formula in the package
// documentation. elapsed27MHz must not move backwards; OnPacket must be set.
// On any returned error the slot clock and all continuity counters are left
// untouched.
func (m *CBRMuxer) AdvanceTo(elapsed27MHz uint64) error {
	if elapsed27MHz < m.lastElapsed {
		return fmt.Errorf("cbr: AdvanceTo moved backwards from %d to %d",
			m.lastElapsed, elapsed27MHz)
	}
	if m.OnPacket == nil {
		return errors.New("cbr: OnPacket callback is not set")
	}

	for m.pos < elapsed27MHz {
		kind := m.chooseKind()
		var out []byte

		switch kind {
		case cbrKindPAT:
			pkt := m.buildPAT(m.patCC)
			out = pkt[:]
			m.OnPacket(out)
			// Commit only after a successful callback return.
			m.patCC = (m.patCC + 1) % 16
			m.pmtPending = true
			m.lastPATPos = m.pos

		case cbrKindPMT:
			pkt := m.buildPMT(m.pmtCC)
			out = pkt[:]
			m.OnPacket(out)
			m.pmtCC = (m.pmtCC + 1) % 16
			m.pmtPending = false

		case cbrKindHeartbeat:
			st := m.byPID[m.pcrPID]
			pcr := cbrWrapPCR(m.cfg.StartPCR27MHz, m.pos)
			pkt := cbrBuildHeartbeat(m.pcrPID, st.cc, pcr)
			out = pkt[:]
			m.OnPacket(out)
			// Adaptation-only PCR packets do not consume the payload CC.
			m.pcrLast = m.pos

		case cbrKindMedia:
			qp := &m.queue[0]
			st := m.byPID[qp.pid]
			qp.pkt[3] = (qp.pkt[3] & 0xf0) | (st.cc & 0x0f)
			if qp.hasPCR {
				pcr := cbrWrapPCR(m.cfg.StartPCR27MHz, m.pos)
				cbrStampPCR(qp.pkt[:], 6, pcr)
			}
			out = qp.pkt[:]
			m.OnPacket(out)
			st.cc = (st.cc + 1) % 16
			if qp.hasPCR {
				m.pcrLast = m.pos
			}
			m.queue = m.queue[1:]

		case cbrKindNull:
			pkt := cbrBuildNull()
			out = pkt[:]
			m.OnPacket(out)
		}

		m.advanceSlot()
	}

	m.lastElapsed = elapsed27MHz
	return nil
}

// chooseKind resolves the packet occupying slot n at position m.pos using
// the fixed priority: PCR heartbeat > due tables > media > null packets.
func (m *CBRMuxer) chooseKind() cbrKind {
	switch m.n {
	case 0:
		return cbrKindPAT
	case 1:
		return cbrKindPMT
	case 2:
		return cbrKindHeartbeat
	}

	if m.pos-m.pcrLast >= m.cfg.PCRInterval27MHz {
		return cbrKindHeartbeat
	}
	if m.pmtPending {
		return cbrKindPMT
	}
	if m.pos >= m.lastPATPos+cbrTableRepriseTicks {
		return cbrKindPAT
	}
	if len(m.queue) > 0 {
		return cbrKindMedia
	}
	return cbrKindNull
}

// advanceSlot moves the exact slot clock from n to n+1 without any
// n*numerator multiplication.
func (m *CBRMuxer) advanceSlot() {
	m.frac += m.rem
	step := m.whole
	if m.frac >= m.cfg.Bitrate {
		m.frac -= m.cfg.Bitrate
		step++
	}
	m.pos += step
	m.n++
}

// cbrWrapPCR returns (start + slotPos) mod 2^33*300 without overflowing on a
// StartPCR27MHz close to the uint64 upper bound.
func cbrWrapPCR(start, slotPos uint64) uint64 {
	return (start%cbrPCRWrapModulus + slotPos%cbrPCRWrapModulus) % cbrPCRWrapModulus
}

// cbrStampPCR writes a 48-bit PCR field (33-bit base, six reserved one-bits,
// 9-bit extension) at the given offset.
func cbrStampPCR(p []byte, offset int, pcr27 uint64) {
	base := pcr27 / 300
	ext := pcr27 % 300
	word := (base << 15) | (uint64(0x3f) << 9) | ext
	p[offset] = byte(word >> 40)
	p[offset+1] = byte(word >> 32)
	p[offset+2] = byte(word >> 24)
	p[offset+3] = byte(word >> 16)
	p[offset+4] = byte(word >> 8)
	p[offset+5] = byte(word)
}

// cbrBuildHeartbeat builds an adaptation-field-only PCR packet. The
// continuity counter is supplied by the caller and is not incremented here.
func cbrBuildHeartbeat(pid uint16, cc uint8, pcr27 uint64) [TS_PAKCET_SIZE]byte {
	var pkt [TS_PAKCET_SIZE]byte
	for i := range pkt {
		pkt[i] = 0xff // adaptation stuffing
	}
	pkt[0] = 0x47
	pkt[1] = byte((pid >> 8) & 0x1f) // TEI=0 PUSI=0 priority=0
	pkt[2] = byte(pid)
	pkt[3] = 0x20 | (cc & 0x0f) // adaptation field only
	pkt[4] = 183                // adaptation_field_length: flags + PCR + 176 stuffing
	pkt[5] = 0x10               // PCR_flag
	cbrStampPCR(pkt[:], 6, pcr27)
	return pkt
}

func cbrBuildNull() [TS_PAKCET_SIZE]byte {
	var pkt [TS_PAKCET_SIZE]byte
	for i := range pkt {
		pkt[i] = 0xff
	}
	pkt[0] = 0x47
	pkt[1] = 0x1f // PID 0x1fff
	pkt[2] = 0xff
	pkt[3] = 0x10 // payload only, continuity counter 0
	return pkt
}

func (m *CBRMuxer) buildPAT(cc uint8) [TS_PAKCET_SIZE]byte {
	pat := NewPat()
	pat.Version_number = 0
	pat.Pmts = append(pat.Pmts, PmtPair{
		Program_number: cbrProgramNumber,
		PID:            cbrPMTPID,
	})

	var hdr TSPacket
	hdr.Payload_unit_start_indicator = 1
	hdr.PID = 0x0000
	hdr.Adaptation_field_control = 0x01
	hdr.Continuity_counter = cc

	bsw := codec.NewBitStreamWriter(TS_PAKCET_SIZE)
	hdr.EncodeHeader(bsw)
	bsw.PutByte(0x00) // pointer_field
	pat.Encode(bsw)
	bsw.FillRemainData(0xff)

	var pkt [TS_PAKCET_SIZE]byte
	copy(pkt[:], bsw.Bits())
	return pkt
}

func (m *CBRMuxer) buildPMT(cc uint8) [TS_PAKCET_SIZE]byte {
	pmt := NewPmt()
	pmt.Program_number = cbrProgramNumber
	pmt.Version_number = 0
	pmt.PCR_PID = m.pcrPID
	for _, s := range m.streams {
		pmt.Streams = append(pmt.Streams, StreamPair{
			StreamType:     uint8(s.typ),
			Elementary_PID: s.pid,
			ES_Info_Length: 0,
		})
	}

	var hdr TSPacket
	hdr.Payload_unit_start_indicator = 1
	hdr.PID = cbrPMTPID
	hdr.Adaptation_field_control = 0x01
	hdr.Continuity_counter = cc

	bsw := codec.NewBitStreamWriter(TS_PAKCET_SIZE)
	hdr.EncodeHeader(bsw)
	bsw.PutByte(0x00) // pointer_field
	pmt.Encode(bsw)
	bsw.FillRemainData(0xff)

	var pkt [TS_PAKCET_SIZE]byte
	copy(pkt[:], bsw.Bits())
	return pkt
}
