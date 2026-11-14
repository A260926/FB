// Copyright 2018 Saferwall. All rights reserved.
// Use of this source code is governed by Apache v2 license
// license that can be found in the LICENSE file.

package pe

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"

	"github.com/edsrzf/mmap-go"
)

// ARM64X dynamic value relocation (DVRT) symbol-6 support.
//
// An ARM64X image carries a dynamic value relocation table whose symbol-6
// records describe how to turn the native ARM64 view into the x64-compatible
// hybrid view: a sequence of page blocks containing ZEROFILL, VALUE and DELTA
// fixups. Everything here works on a private, in-memory copy of the input:
// the original bytes are never modified, nothing is written back to disk and
// no image is loaded or executed.
//
// See task_support/arm64x-format.md for the subset of the format handled here.

// ARM64XFixupType identifies the kind of a symbol-6 fixup record.
type ARM64XFixupType uint8

const (
	// ARM64XFixupZerofill (type 0) overwrites the target with zero bytes.
	ARM64XFixupZerofill ARM64XFixupType = 0

	// ARM64XFixupValue (type 1) overwrites the target with an explicit
	// little-endian value carried in the fixup payload.
	ARM64XFixupValue ARM64XFixupType = 1

	// ARM64XFixupDelta (type 2) adds or subtracts a scaled magnitude to the
	// 32-bit value already present at the target.
	ARM64XFixupDelta ARM64XFixupType = 2
)

// ARM64XFixupRecord is a single symbol-6 fixup. Records are kept in file order,
// including repeated fixups against the same target.
type ARM64XFixupRecord struct {
	// SourceOffset is the file offset, in the original input, of the uint16
	// fixup word inside its page block.
	SourceOffset uint32 `json:"source_offset"`

	// TargetRVA is the RVA within the image that the fixup rewrites.
	TargetRVA uint32 `json:"target_rva"`

	// Type is the fixup type (0 ZEROFILL, 1 VALUE, 2 DELTA).
	Type ARM64XFixupType `json:"type"`

	// Width is the number of bytes rewritten at the target.
	Width uint8 `json:"width"`

	// Value holds the little-endian replacement value for VALUE fixups.
	Value uint64 `json:"value,omitempty"`

	// Delta holds the signed, already scaled 32-bit amount applied by DELTA
	// fixups to the previously corrected value at the target.
	Delta int32 `json:"delta,omitempty"`

	// targetOffset is the resolved file offset of the target in the raw
	// layout; it is derived deterministically from TargetRVA.
	targetOffset uint32
}

// Errors returned while building the ARM64X alternate view.
var (
	// ErrARM64XUnsupported is returned when an alternate view is requested
	// for an image that is not PE32+ or whose DVRT is not version 1.
	ErrARM64XUnsupported = errors.New("arm64x alternate view requires a PE32+ image with a DVRT v1 table")

	// ErrARM64XNoTable is returned when the load config does not point at a
	// dynamic value relocation table.
	ErrARM64XNoTable = errors.New("arm64x: no dynamic value relocation table")
)

// applyARM64XAlternateView rewrites the private view buffer of the file so
// that every existing read interface (NtHeader, Export, GetData, RVA helpers)
// reflects the x64-compatible hybrid view. It runs after the native section
// layout has been parsed, and before any data directory is parsed, so the
// corrected headers drive the rest of the normal Parse flow.
func (pe *File) applyARM64XAlternateView() error {

	// Only PE32+ images are supported.
	if !pe.Is64 {
		return fmt.Errorf("%w: PE32 images are not supported",
			ErrARM64XUnsupported)
	}

	oh := pe.NtHeader.OptionalHeader.(ImageOptionalHeader64)
	loadConfigDir := oh.DataDirectory[ImageDirectoryEntryLoadConfig]
	if loadConfigDir.VirtualAddress == 0 {
		// No load config means no symbol-6 table: keep the native view.
		return nil
	}

	loadCfg, err := pe.readLoadConfig64(loadConfigDir.VirtualAddress)
	if err != nil {
		return err
	}

	if loadCfg.DynamicValueRelocTableSection == 0 ||
		loadCfg.DynamicValueRelocTableOffset == 0 {
		// No DVRT location: keep the native view.
		return nil
	}

	tableOffset, err := pe.dvrtTableOffset(
		loadCfg.DynamicValueRelocTableSection,
		loadCfg.DynamicValueRelocTableOffset)
	if err != nil {
		return err
	}

	dvrt, err := pe.parseDVRTAt(tableOffset)
	if err != nil {
		return err
	}

	// Without symbol-6 fixups the alternate view is byte-identical to the
	// native view, so parsing simply continues on the native bytes.
	if len(dvrt.ARM64XFixups) == 0 {
		return nil
	}

	// Build the corrected image on a private copy. The original input slice
	// (and therefore any other File sharing it) is never touched.
	alt := make([]byte, len(pe.rawData))
	copy(alt, pe.rawData)
	if err := applyARM64XFixups(alt, dvrt.ARM64XFixups); err != nil {
		return err
	}

	// Swap to the corrected buffer, then re-parse the NT header so that
	// Machine, the entry point and the data directories describe the
	// alternate view. If anything fails at this stage, roll back to the raw
	// input so a failed Parse never leaves a half-corrected File behind.
	pe.data = mmapBytes(alt)
	if err := pe.ParseNTHeader(); err != nil {
		pe.data = pe.rawData
		return err
	}
	if !pe.Is64 {
		pe.data = pe.rawData
		return fmt.Errorf("%w: alternate optional header is not PE32+",
			ErrARM64XUnsupported)
	}
	pe.arm64xActive = true

	// Section layout does not change, only header and section contents do, so
	// pe.Header keeps the same bounds and is simply re-backed by the view.
	if len(pe.Header) > 0 && len(pe.Header) <= len(pe.data) {
		pe.Header = pe.data[:len(pe.Header)]
	}

	// Expose the records before the data directories are parsed; parsing the
	// load-config directory later replaces this with an equivalent table read
	// from the (unmodified) metadata bytes.
	pe.LoadConfig.DVRT = dvrt
	return nil
}

// readLoadConfig64 unpacks just enough of the PE32+ load config directory to
// locate the dynamic value relocation table. Reads happen through the active
// view, which still holds the native bytes at this stage.
func (pe *File) readLoadConfig64(rva uint32) (ImageLoadConfigDirectory64, error) {
	var loadCfg ImageLoadConfigDirectory64

	fileOffset := pe.GetOffsetFromRva(rva)
	structSize, err := pe.ReadUint32(fileOffset)
	if err != nil {
		return loadCfg, err
	}

	maxSize := uint32(binary.Size(loadCfg))
	if structSize == 0 || structSize > maxSize {
		return loadCfg, fmt.Errorf(
			"arm64x: invalid load config size %d at file offset 0x%x",
			structSize, fileOffset)
	}
	if uint64(fileOffset)+uint64(structSize) > uint64(len(pe.data)) {
		return loadCfg, fmt.Errorf(
			"arm64x: truncated load config at file offset 0x%x", fileOffset)
	}

	raw := make([]byte, maxSize)
	copy(raw, pe.data[fileOffset:fileOffset+structSize])
	err = binary.Read(bytes.NewReader(raw), binary.LittleEndian, &loadCfg)
	return loadCfg, err
}

// dvrtTableOffset resolves the file offset of the DVRT. The table location is
// driven solely by the one-based DynamicValueRelocTableSection number plus the
// DynamicValueRelocTableOffset within that section; section names are never
// guessed.
func (pe *File) dvrtTableOffset(sectionNumber uint16, tableOffset uint32) (uint32, error) {
	if sectionNumber == 0 || tableOffset == 0 {
		return 0, ErrARM64XNoTable
	}
	if int(sectionNumber) > len(pe.Sections) {
		return 0, fmt.Errorf(
			"arm64x: dynamic value reloc table section %d is out of range (sections=%d)",
			sectionNumber, len(pe.Sections))
	}

	header := pe.Sections[sectionNumber-1].Header
	offset := uint64(header.PointerToRawData) + uint64(tableOffset)
	if offset+8 > uint64(len(pe.rawData)) {
		return 0, fmt.Errorf(
			"arm64x: dynamic value reloc table at file offset 0x%x is truncated",
			uint32(offset))
	}
	return uint32(offset), nil
}

// parseDVRTAt parses a DVRT v1 table starting at the given absolute file
// offset. Symbol 3/4/5 records keep their historical representation while
// symbol-6 records additionally produce one ARM64XFixupRecord per fixup.
func (pe *File) parseDVRTAt(tableOffset uint32) (*DVRT, error) {
	data := pe.data
	if uint64(tableOffset)+8 > uint64(len(data)) {
		return nil, fmt.Errorf(
			"arm64x: truncated DVRT header at file offset 0x%x", tableOffset)
	}

	dvrt := &DVRT{}
	dvrt.Version = binary.LittleEndian.Uint32(data[tableOffset:])
	dvrt.Size = binary.LittleEndian.Uint32(data[tableOffset+4:])

	// Only version 1 of the table is supported.
	if dvrt.Version != 1 {
		return nil, fmt.Errorf("%w: unsupported DVRT version %d at file offset 0x%x",
			ErrARM64XUnsupported, dvrt.Version, tableOffset)
	}

	recordHeaderSize := uint32(12) // Symbol uint64 + BaseRelocSize uint32
	if pe.Is32 {
		recordHeaderSize = 8 // Symbol uint32 + BaseRelocSize uint32
	}

	tableEnd := uint64(tableOffset) + 8 + uint64(dvrt.Size)
	if tableEnd > uint64(len(data)) {
		return nil, fmt.Errorf(
			"arm64x: DVRT payload is truncated at file offset 0x%x (ends 0x%x, file size 0x%x)",
			tableOffset, uint32(tableEnd), len(data))
	}

	cur := uint64(tableOffset) + 8
	for cur < tableEnd {
		if cur+uint64(recordHeaderSize) > tableEnd {
			return nil, fmt.Errorf(
				"arm64x: truncated dynamic relocation record header at file offset 0x%x",
				uint32(cur))
		}

		var (
			symbol        uint64
			baseRelocSize uint32
			relocEntry    RelocEntry
		)
		if pe.Is32 {
			symbol = uint64(binary.LittleEndian.Uint32(data[cur:]))
			baseRelocSize = binary.LittleEndian.Uint32(data[cur+4:])
			relocEntry.ImageDynamicRelocation = ImageDynamicRelocation32{
				Symbol:        uint32(symbol),
				BaseRelocSize: baseRelocSize,
			}
		} else {
			symbol = binary.LittleEndian.Uint64(data[cur:])
			baseRelocSize = binary.LittleEndian.Uint32(data[cur+8:])
			relocEntry.ImageDynamicRelocation = ImageDynamicRelocation64{
				Symbol:        symbol,
				BaseRelocSize: baseRelocSize,
			}
		}

		payloadStart := cur + uint64(recordHeaderSize)
		payloadEnd := payloadStart + uint64(baseRelocSize)
		if payloadEnd < payloadStart || payloadEnd > tableEnd {
			return nil, fmt.Errorf(
				"arm64x: dynamic relocation record at file offset 0x%x has truncated payload",
				uint32(cur))
		}

		payload := data[payloadStart:payloadEnd]
		payloadAbsOffset := uint32(payloadStart)

		if symbol == 6 {
			blocks, fixups, err := pe.parseARM64XBlocks(payload, payloadAbsOffset)
			if err != nil {
				return nil, err
			}
			relocEntry.RelocBlocks = blocks
			dvrt.ARM64XFixups = append(dvrt.ARM64XFixups, fixups...)
		} else {
			blocks, err := parseLegacyDynamicRelocBlocks(
				payload, payloadAbsOffset, uint8(symbol))
			if err != nil {
				return nil, err
			}
			relocEntry.RelocBlocks = blocks
		}

		dvrt.Entries = append(dvrt.Entries, relocEntry)
		cur = payloadEnd
	}

	return dvrt, nil
}

// parseARM64XBlocks parses the page blocks of a symbol-6 record payload. It
// returns the page block headers (for the historical DVRT representation) and
// one fixup record per nonzero fixup word, in file order.
func (pe *File) parseARM64XBlocks(payload []byte, payloadAbsOffset uint32) (
	[]RelocBlock, []ARM64XFixupRecord, error) {

	var (
		blocks []RelocBlock
		fixups []ARM64XFixupRecord
	)
	pc := uint32(0)
	for pc < uint32(len(payload)) {
		// Every block starts with a uint32 page RVA and a uint32 block size
		// which includes the 8-byte header.
		if pc+8 > uint32(len(payload)) {
			return nil, nil, fmt.Errorf(
				"arm64x: truncated page block header at file offset 0x%x",
				payloadAbsOffset+pc)
		}
		blockHeaderOffset := payloadAbsOffset + pc
		pageRVA := binary.LittleEndian.Uint32(payload[pc:])
		blockSize := binary.LittleEndian.Uint32(payload[pc+4:])

		if blockSize < 8 || blockSize%4 != 0 {
			return nil, nil, fmt.Errorf(
				"arm64x: invalid page block size %d at file offset 0x%x",
				blockSize, blockHeaderOffset+4)
		}
		// Use uint64 so a hostile block size cannot wrap past the bounds check.
		blockEnd64 := uint64(pc) + uint64(blockSize)
		if blockEnd64 > uint64(len(payload)) {
			return nil, nil, fmt.Errorf(
				"arm64x: page block at file offset 0x%x runs past its record payload",
				blockHeaderOffset)
		}
		blockEnd := uint32(blockEnd64)

		relocBlock := RelocBlock{
			ImgBaseReloc: ImageBaseRelocation{
				VirtualAddress: pageRVA,
				SizeOfBlock:    blockSize,
			},
		}

		// Fixup words occupy the block after its 8-byte header. A zero uint16
		// is end-of-block padding; nothing may follow it but more zeroes.
		wordPC := pc + 8
		seenPadding := false
		for wordPC < blockEnd {
			wordOffset := payloadAbsOffset + wordPC
			word := binary.LittleEndian.Uint16(payload[wordPC:])
			wordPC += 2

			if word == 0 {
				seenPadding = true
				continue
			}
			if seenPadding {
				return nil, nil, fmt.Errorf(
					"arm64x: nonzero fixup after block padding at file offset 0x%x",
					wordOffset)
			}

			pageOffset := uint32(word & 0x0fff)
			fixupType := ARM64XFixupType((word >> 12) & 0x3)
			argument := uint8(word >> 14)
			targetRVA := pageRVA + pageOffset

			rec := ARM64XFixupRecord{
				SourceOffset: wordOffset,
				TargetRVA:    targetRVA,
				Type:         fixupType,
			}

			switch fixupType {
			case ARM64XFixupZerofill, ARM64XFixupValue:
				width, err := arm64xWidth(argument)
				if err != nil {
					return nil, nil, fmt.Errorf(
						"arm64x: %w at file offset 0x%x", err, wordOffset)
				}
				rec.Width = width

				if fixupType == ARM64XFixupValue {
					if uint64(wordPC)+uint64(width) > uint64(blockEnd) {
						return nil, nil, fmt.Errorf(
							"arm64x: truncated VALUE payload at file offset 0x%x",
							wordOffset)
					}
					rec.Value = readLEWidth(payload, wordPC, width)
					wordPC += uint32(width)
				}

			case ARM64XFixupDelta:
				// DELTA always rewrites a 32-bit target and consumes one
				// trailing uint16 magnitude.
				rec.Width = 4
				if wordPC+2 > blockEnd {
					return nil, nil, fmt.Errorf(
						"arm64x: truncated DELTA magnitude at file offset 0x%x",
						wordOffset)
				}
				magnitude := binary.LittleEndian.Uint16(payload[wordPC:])
				wordPC += 2

				scale := int32(4)
				if word&0x8000 != 0 { // bit15: scale 8
					scale = 8
				}
				delta := int32(magnitude) * scale
				if word&0x4000 != 0 { // bit14: subtraction
					delta = -delta
				}
				rec.Delta = delta

			default: // type 3 is unsupported by the format.
				return nil, nil, fmt.Errorf(
					"arm64x: unsupported fixup type 3 at file offset 0x%x",
					wordOffset)
			}

			// The target must map to a complete, unique raw-backed range in
			// the native layout. No virtual zero fill and no bytes borrowed
			// from an adjacent section.
			targetOffset, err := pe.mapARM64XTarget(rec.TargetRVA,
				uint32(rec.Width), rec.SourceOffset)
			if err != nil {
				return nil, nil, err
			}
			rec.targetOffset = targetOffset
			fixups = append(fixups, rec)
		}

		if wordPC != blockEnd {
			return nil, nil, fmt.Errorf(
				"arm64x: malformed page block ending at file offset 0x%x",
				blockHeaderOffset)
		}
		blocks = append(blocks, relocBlock)
		pc = blockEnd
	}

	return blocks, fixups, nil
}

// arm64xWidth maps the 2-bit fixup argument to a byte width. Argument 0 has no
// associated width and is therefore illegal for ZEROFILL/VALUE fixups.
func arm64xWidth(argument uint8) (uint8, error) {
	switch argument {
	case 1:
		return 2, nil
	case 2:
		return 4, nil
	case 3:
		return 8, nil
	default:
		return 0, errors.New("invalid ZEROFILL/VALUE width argument")
	}
}

// mapARM64XTarget resolves an RVA to a unique file offset covering the full
// target width, using only the native section layout. Targets below
// SizeOfHeaders live in the headers (identity mapping); every other target
// must be contained entirely in exactly one raw-backed section range.
func (pe *File) mapARM64XTarget(rva, width uint32, sourceOffset uint32) (uint32, error) {
	var sizeOfHeaders uint32
	if pe.Is64 {
		sizeOfHeaders = pe.NtHeader.OptionalHeader.(ImageOptionalHeader64).SizeOfHeaders
	} else {
		sizeOfHeaders = pe.NtHeader.OptionalHeader.(ImageOptionalHeader32).SizeOfHeaders
	}

	// Header targets use the identity RVA -> file offset mapping. The whole
	// target must stay inside the headers; straddling into section data is an
	// error rather than an excuse to borrow adjacent bytes.
	if rva < sizeOfHeaders {
		end := uint64(rva) + uint64(width)
		if end <= uint64(sizeOfHeaders) && end <= uint64(len(pe.rawData)) {
			return rva, nil
		}
		return 0, fmt.Errorf(
			"arm64x: header target RVA 0x%x width %d is not fully raw-backed (source offset 0x%x)",
			rva, width, sourceOffset)
	}

	// Section targets: the full width must fit inside one raw-backed section.
	var (
		matches int
		offset  uint32
	)
	for _, section := range pe.Sections {
		header := section.Header
		if header.PointerToRawData == 0 || header.SizeOfRawData == 0 {
			continue
		}
		if rva < header.VirtualAddress {
			continue
		}
		relative := uint64(rva) - uint64(header.VirtualAddress)
		if relative+uint64(width) > uint64(header.SizeOfRawData) {
			continue
		}
		candidate := uint64(header.PointerToRawData) + relative
		if candidate+uint64(width) > uint64(len(pe.rawData)) {
			continue
		}
		matches++
		offset = uint32(candidate)
	}

	if matches != 1 {
		return 0, fmt.Errorf(
			"arm64x: target RVA 0x%x width %d has no unique raw file mapping (%d candidates, source offset 0x%x)",
			rva, width, matches, sourceOffset)
	}
	return offset, nil
}

// applyARM64XFixups replays the parsed fixups on the private alternate-view
// buffer, strictly in file order. DELTA uses the 32-bit value previously
// corrected (including by earlier fixups) and wraps modulo 2^32.
func applyARM64XFixups(alt []byte, fixups []ARM64XFixupRecord) error {
	for _, rec := range fixups {
		off := uint64(rec.targetOffset)
		end := off + uint64(rec.Width)
		if end > uint64(len(alt)) {
			return fmt.Errorf(
				"arm64x: fixup target at file offset 0x%x runs past the image (source offset 0x%x)",
				rec.targetOffset, rec.SourceOffset)
		}

		switch rec.Type {
		case ARM64XFixupZerofill:
			for i := off; i < end; i++ {
				alt[i] = 0
			}

		case ARM64XFixupValue:
			writeLEWidth(alt, rec.targetOffset, rec.Width, rec.Value)

		case ARM64XFixupDelta:
			current := binary.LittleEndian.Uint32(alt[off:])
			result := int64(uint64(current)) + int64(rec.Delta)
			binary.LittleEndian.PutUint32(alt[off:], uint32(result))
		}
	}
	return nil
}

// readLEWidth reads a little-endian value of 2, 4 or 8 bytes.
func readLEWidth(data []byte, offset uint32, width uint8) uint64 {
	switch width {
	case 2:
		return uint64(binary.LittleEndian.Uint16(data[offset:]))
	case 4:
		return uint64(binary.LittleEndian.Uint32(data[offset:]))
	default:
		return binary.LittleEndian.Uint64(data[offset:])
	}
}

// writeLEWidth writes a little-endian value of 2, 4 or 8 bytes.
func writeLEWidth(data []byte, offset uint32, width uint8, value uint64) {
	switch width {
	case 2:
		binary.LittleEndian.PutUint16(data[offset:], uint16(value))
	case 4:
		binary.LittleEndian.PutUint32(data[offset:], uint32(value))
	default:
		binary.LittleEndian.PutUint64(data[offset:], value)
	}
}

// mmapBytes adapts a private byte slice to the mmap.MMap type used by File.
// A plain slice is safe to assign to that named type; Unmap on such a slice
// only reaches Munmap through the real mapping path, which alternate-view
// files avoid via the arm64xActive flag.
func mmapBytes(b []byte) mmap.MMap {
	return b
}

// parseLegacyDynamicRelocBlocks parses the page blocks of a symbol 3/4/5
// dynamic relocation record payload. payload is the record's BaseRelocSize
// bytes and payloadAbsOffset is the file offset of its first byte, used in
// error messages. It preserves the historical representation returned by the
// DVRT parser, including the implicit zero-word padding handling.
func parseLegacyDynamicRelocBlocks(payload []byte, payloadAbsOffset uint32,
	symbol uint8) ([]RelocBlock, error) {

	const baseRelocSize = 8
	var blocks []RelocBlock
	pc := uint32(0)
	for pc < uint32(len(payload)) {
		if pc+baseRelocSize > uint32(len(payload)) {
			return nil, fmt.Errorf(
				"arm64x: truncated legacy reloc block header at file offset 0x%x",
				payloadAbsOffset+pc)
		}
		baseReloc := ImageBaseRelocation{
			VirtualAddress: binary.LittleEndian.Uint32(payload[pc:]),
			SizeOfBlock:    binary.LittleEndian.Uint32(payload[pc+4:]),
		}
		if baseReloc.SizeOfBlock < baseRelocSize {
			return nil, fmt.Errorf(
				"arm64x: invalid legacy reloc block size %d at file offset 0x%x",
				baseReloc.SizeOfBlock, payloadAbsOffset+pc+4)
		}
		blockEnd64 := uint64(pc) + uint64(baseReloc.SizeOfBlock)
		if blockEnd64 > uint64(len(payload)) {
			return nil, fmt.Errorf(
				"arm64x: legacy reloc block at file offset 0x%x runs past its record payload",
				payloadAbsOffset+pc)
		}
		blockEnd := uint32(blockEnd64)

		relocBlock := RelocBlock{ImgBaseReloc: baseReloc}
		entryWords := (baseReloc.SizeOfBlock - baseRelocSize) / 2
		for i := uint32(0); i < entryWords; i++ {
			wordOffset := pc + baseRelocSize + i*2

			switch symbol {
			case 3:
				// Symbol-3 entries are 4 bytes each; entryWords counts uint16
				// slots, so each entry consumes two slots.
				if i+1 >= entryWords || uint64(wordOffset)+4 > uint64(len(payload)) {
					return nil, fmt.Errorf(
						"arm64x: truncated symbol-3 entry at file offset 0x%x",
						payloadAbsOffset+wordOffset)
				}
				dword := binary.LittleEndian.Uint32(payload[wordOffset:])
				relocBlock.TypeOffsets = append(relocBlock.TypeOffsets,
					ImageImportControlTransferDynamicRelocation{
						PageRelativeOffset: uint16(dword) & 0xfff,
						IndirectCall:       (uint16(dword) & 0x1000) >> 12,
						IATIndex:           (dword & 0xFFFFE000) >> 13,
					})
				i++
			case 4:
				word := binary.LittleEndian.Uint16(payload[wordOffset:])
				entry := ImageIndirectControlTransferDynamicRelocation{
					PageRelativeOffset: word & 0xfff,
					IndirectCall:       uint8((word & 0x1000) >> 12),
					RexWPrefix:         uint8((word & 0x2000) >> 13),
					CfgCheck:           uint8((word & 0x4000) >> 14),
					Reserved:           uint8((word & 0x8000) >> 15),
				}
				if (ImageIndirectControlTransferDynamicRelocation{}) != entry {
					relocBlock.TypeOffsets = append(relocBlock.TypeOffsets, entry)
				}
			case 5:
				word := binary.LittleEndian.Uint16(payload[wordOffset:])
				entry := ImageSwitchableBranchDynamicRelocation{
					PageRelativeOffset: word & 0xfff,
					RegisterNumber:     (word & 0xf000) >> 12,
				}
				if (ImageSwitchableBranchDynamicRelocation{}) != entry {
					relocBlock.TypeOffsets = append(relocBlock.TypeOffsets, entry)
				}
			default:
				return nil, fmt.Errorf(
					"arm64x: unsupported dynamic relocation symbol %d at file offset 0x%x",
					symbol, payloadAbsOffset+pc)
			}
		}

		blocks = append(blocks, relocBlock)
		pc = blockEnd
	}
	return blocks, nil
}
