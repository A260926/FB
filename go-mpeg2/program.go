package mpeg2

import (
    "bytes"
    "encoding/binary"
    "errors"

    "github.com/yapingcat/gomedia/go-codec"
)

// ProgramStream describes one elementary stream of an explicitly configured
// program. Descriptors must be a concatenation of complete
// tag(1)/length(1)/body(length) descriptor strings; the bytes are copied by
// ConfigureProgram and are preserved verbatim in the PMT.
type ProgramStream struct {
    PID         uint16
    Type        TS_STREAM_TYPE
    Descriptors []byte
}

const (
    // explicitProgramNumber is the single program number used by this muxer.
    explicitProgramNumber = uint16(1)
    // explicitPMTPID is the fixed PMT PID.
    explicitPMTPID = uint16(0x0200)

    minProgramStreamPID = uint16(0x0020)
    maxProgramStreamPID = uint16(0x1FFE)

    // One ProgramStream.Descriptors blob is limited to 255 bytes.
    maxProgramDescriptors = 255
    // ISO 13818-1 limits a PMT section to 1024 bytes, i.e. section_length <= 1021.
    maxPMTSectionLength = 1021
)

// Validation / mode errors returned by ConfigureProgram. Every validation
// failure leaves the current configuration untouched and emits no bytes.
var (
    ErrProgramModeMixed       = errors.New("mpeg2: ConfigureProgram cannot be mixed with AddStream")
    ErrProgramEmpty           = errors.New("mpeg2: program must contain at least one stream")
    ErrProgramStreamType      = errors.New("mpeg2: unsupported program stream type")
    ErrProgramPIDRange        = errors.New("mpeg2: program stream PID out of range 0x20..0x1ffe")
    ErrProgramPIDReserved     = errors.New("mpeg2: program stream PID conflicts with the PMT PID")
    ErrProgramPIDDuplicate    = errors.New("mpeg2: program stream PIDs must be unique")
    ErrProgramPCRPID          = errors.New("mpeg2: PCR PID must be one of the configured stream PIDs")
    ErrProgramDescriptor      = errors.New("mpeg2: malformed program descriptor")
    ErrProgramDescriptorSize  = errors.New("mpeg2: descriptors of one program item exceed 255 bytes")
    ErrProgramSectionTooLarge = errors.New("mpeg2: PMT section_length exceeds 1021")
)

func explicitStreamTypeSupported(cid TS_STREAM_TYPE) bool {
    switch cid {
    case TS_STREAM_AAC, TS_STREAM_AUDIO_MPEG1, TS_STREAM_AUDIO_MPEG2,
        TS_STREAM_H264, TS_STREAM_H265:
        return true
    }
    return false
}

// validateDescriptors walks the complete tag/length/body descriptors and
// rejects truncated strings. A per-item blob longer than 255 bytes is rejected
// as well.
func validateDescriptors(desc []byte) error {
    if len(desc) > maxProgramDescriptors {
        return ErrProgramDescriptorSize
    }
    for off := 0; off < len(desc); {
        if len(desc)-off < 2 {
            return ErrProgramDescriptor
        }
        bodyLen := int(desc[off+1])
        if off+2+bodyLen > len(desc) {
            return ErrProgramDescriptor
        }
        off += 2 + bodyLen
    }
    return nil
}

func sameProgramConfiguration(a []ProgramStream, b []ProgramStream, pcrA, pcrB uint16) bool {
    if pcrA != pcrB || len(a) != len(b) {
        return false
    }
    for i := range a {
        if a[i].PID != b[i].PID || a[i].Type != b[i].Type ||
            !bytes.Equal(a[i].Descriptors, b[i].Descriptors) {
            return false
        }
    }
    return true
}

// ConfigureProgram replaces the whole single-program configuration at once.
//
// On the first successful call the muxer enters explicit program mode and must
// not be used with AddStream. Every later successful call that changes stream
// order, PIDs, types, descriptors or the PCR PID bumps the announced PMT
// version by one (mod 32); an identical configuration leaves the version
// untouched. The new configuration is not written by this call: the next valid
// Write emits PAT and the complete new PMT before its PES packets.
func (mux *TSMuxer) ConfigureProgram(streams []ProgramStream, pcrPID uint16) error {
    if mux.add_stream_used {
        return ErrProgramModeMixed
    }
    if mux.pat == nil {
        mux.pat = NewTablePat()
    }
    pmtPID := mux.pmt_pid
    if pmtPID == 0 {
        pmtPID = explicitPMTPID
        mux.pmt_pid = explicitPMTPID
    }

    if len(streams) == 0 {
        return ErrProgramEmpty
    }

    seen := make(map[uint16]struct{}, len(streams))
    esInfoBytes := 0
    for i := range streams {
        s := &streams[i]
        if !explicitStreamTypeSupported(s.Type) {
            return ErrProgramStreamType
        }
        if s.PID < minProgramStreamPID || s.PID > maxProgramStreamPID {
            return ErrProgramPIDRange
        }
        if s.PID == pmtPID {
            return ErrProgramPIDReserved
        }
        if _, dup := seen[s.PID]; dup {
            return ErrProgramPIDDuplicate
        }
        seen[s.PID] = struct{}{}
        if err := validateDescriptors(s.Descriptors); err != nil {
            return err
        }
        esInfoBytes += 5 + len(s.Descriptors)
    }
    if _, ok := seen[pcrPID]; !ok {
        return ErrProgramPCRPID
    }

    // section_length counts everything after the 2 byte length field:
    // 9 fixed PMT bytes + elementary-stream loops + 4 CRC bytes.
    sectionLength := 9 + esInfoBytes + 4
    if sectionLength > maxPMTSectionLength {
        return ErrProgramSectionTooLarge
    }

    changed := !mux.has_program ||
        !sameProgramConfiguration(mux.program_cfg, streams, mux.program_pcr_pid, pcrPID)

    // Validation passed: commit a private copy of the descriptor bytes and
    // reuse persistent pes_stream state so continuity counters survive
    // replacement, removal and re-addition.
    if mux.pes_registry == nil {
        mux.pes_registry = make(map[uint16]*pes_stream)
    }
    cfg := make([]ProgramStream, len(streams))
    pesList := make([]*pes_stream, len(streams))
    for i := range streams {
        s := &streams[i]
        cfg[i] = ProgramStream{
            PID:         s.PID,
            Type:        s.Type,
            Descriptors: append([]byte(nil), s.Descriptors...),
        }
        ps, ok := mux.pes_registry[s.PID]
        if !ok {
            ps = NewPESStream(s.PID, s.Type)
            mux.pes_registry[s.PID] = ps
        } else {
            ps.streamtype = s.Type
        }
        pesList[i] = ps
    }

    if mux.program_pmt == nil {
        mux.program_pmt = NewTablePmt()
        mux.program_pmt.pid = pmtPID
        mux.program_pmt.pm = explicitProgramNumber
    }
    mux.program_pmt.pcr_pid = pcrPID
    mux.program_pmt.streams = pesList

    if changed && mux.has_program {
        mux.program_version = (mux.program_version + 1) % 32
    }
    // Every successful (re)configuration is published before the next valid
    // Write; the version above is what distinguishes real changes.
    mux.program_dirty = true
    mux.program_cfg = cfg
    mux.program_pcr_pid = pcrPID
    mux.has_program = true
    mux.explicit = true
    return nil
}

// buildProgramPMTSection encodes one complete PMT section (including the
// MPEG-2 CRC) with raw descriptor bytes preserved.
func buildProgramPMTSection(version uint8, pcrPID uint16, streams []ProgramStream) []byte {
    esInfoBytes := 0
    for i := range streams {
        esInfoBytes += 5 + len(streams[i].Descriptors)
    }
    sectionLength := 9 + esInfoBytes + 4
    section := make([]byte, 3+sectionLength)

    section[0] = uint8(TS_TID_PMS)
    section[1] = 0xB0 | uint8((sectionLength>>8)&0x0F)
    section[2] = uint8(sectionLength)
    binary.BigEndian.PutUint16(section[3:5], explicitProgramNumber)
    section[5] = 0xC0 | ((version & 0x1F) << 1) | 0x01
    section[6] = 0x00 // section_number
    section[7] = 0x00 // last_section_number
    binary.BigEndian.PutUint16(section[8:10], 0xE000|pcrPID)
    binary.BigEndian.PutUint16(section[10:12], 0xF000) // program_info_length = 0

    off := 12
    for i := range streams {
        s := &streams[i]
        section[off] = uint8(s.Type)
        off++
        binary.BigEndian.PutUint16(section[off:off+2], 0xE000|s.PID)
        off += 2
        binary.BigEndian.PutUint16(section[off:off+2], 0xF000|uint16(len(s.Descriptors)))
        off += 2
        copy(section[off:], s.Descriptors)
        off += len(s.Descriptors)
    }

    crc := codec.CalcCrc32(0xffffffff, section[:off])
    // CalcCrc32 returns the reflected-register value; storing it little
    // endian produces the big-endian MPEG-2 CRC word on the wire.
    binary.LittleEndian.PutUint32(section[off:off+4], crc)
    return section
}

// writeProgramPMT splits one PMT section across TS packets. The first packet
// has PUSI=1 and a zero pointer field; continuation packets have PUSI=0.
// 0xFF stuffing is appended only after the end of the section in the last
// packet. The PMT continuity counter advances per emitted packet.
func (mux *TSMuxer) writeProgramPMT(section []byte) {
    pmt := mux.program_pmt
    pid := pmt.pid
    off := 0
    first := true
    for first || off < len(section) {
        pkt := make([]byte, TS_PAKCET_SIZE)
        pkt[0] = 0x47
        if first {
            pkt[1] = 0x40 | byte((pid>>8)&0x1F) // PUSI
            pkt[4] = 0x00                      // pointer_field
        } else {
            pkt[1] = byte((pid >> 8) & 0x1F)
        }
        pkt[2] = byte(pid)
        pkt[3] = 0x10 | (pmt.cc & 0x0F) // adaptation control: payload only
        pmt.cc = (pmt.cc + 1) % 16

        pos := 4
        if first {
            pos = 5
        }
        chunk := len(section) - off
        if capacity := TS_PAKCET_SIZE - pos; chunk > capacity {
            chunk = capacity
        }
        copy(pkt[pos:], section[off:off+chunk])
        off += chunk
        pos += chunk
        for i := pos; i < TS_PAKCET_SIZE; i++ {
            pkt[i] = 0xFF
        }
        first = false
        if mux.OnPacket != nil {
            mux.OnPacket(pkt)
        }
    }
}

// writeProgram is the explicit-mode counterpart of TSMuxer.Write.
func (mux *TSMuxer) writeProgram(pid uint16, data []byte, pts uint64, dts uint64) error {
    var whichstream *pes_stream
    for _, stream := range mux.program_pmt.streams {
        if stream.pid == pid {
            whichstream = stream
            break
        }
    }
    if whichstream == nil {
        // Do not touch program_dirty: the pending announcement survives an
        // invalid PID write and is consumed by the next valid Write.
        return errors.New("not Found pid stream")
    }
    whichpmt := mux.program_pmt
    // Unlike the legacy path, pcr_pid is never reselected here: it is owned
    // solely by ConfigureProgram.

    var withaud bool = false
    if whichstream.streamtype == TS_STREAM_H264 || whichstream.streamtype == TS_STREAM_H265 {
        codec.SplitFrame(data, func(nalu []byte) bool {
            if whichstream.streamtype == TS_STREAM_H264 {
                nalu_type := codec.H264NaluTypeWithoutStartCode(nalu)
                if nalu_type == codec.H264_NAL_AUD {
                    withaud = true
                    return false
                } else if codec.IsH264VCLNaluType(nalu_type) {
                    return false
                }
                return true
            } else {
                nalu_type := codec.H265NaluTypeWithoutStartCode(nalu)
                if nalu_type == codec.H265_NAL_AUD {
                    withaud = true
                    return false
                } else if codec.IsH265VCLNaluType(nalu_type) {
                    return false
                }
                return true
            }
        })
    }

    if mux.program_dirty || mux.pat_period == 0 || mux.pat_period+400 < dts {
        mux.program_dirty = false
        mux.pat_period = dts
        if mux.pat_period == 0 {
            mux.pat_period = 1 //avoid write pat twice
        }

        tmppat := NewPat()
        tmppat.Version_number = mux.pat.version_number
        tmppat.Pmts = append(tmppat.Pmts, PmtPair{
            Program_number: explicitProgramNumber,
            PID:            mux.program_pmt.pid,
        })
        mux.writePat(tmppat)

        section := buildProgramPMTSection(mux.program_version, mux.program_pcr_pid, mux.program_cfg)
        mux.writeProgramPMT(section)
    }

    flag := false
    switch whichstream.streamtype {
    case TS_STREAM_H264:
        flag = codec.IsH264IDRFrame(data)
    case TS_STREAM_H265:
        flag = codec.IsH265IDRFrame(data)
    }

    mux.writePES(whichstream, whichpmt, data, pts*90, dts*90, flag, withaud)
    return nil
}
