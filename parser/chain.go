package parser

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"hash/crc32"
	"io"
)

// Parent locator region GUID:
// B867D6A7-47BE-4682-9172-3624-21EE872C
const parentLocatorGUID = "\xa7\xd6\x67\xb8\xbe\x47\x82\x46\x91\x72\x36\x24\x21\xee\x87\x2c"

const locatorSignature = "locl"

const locatorKeyParentLinkage = "parent_linkage"

// A present sector bitmap block uses the same state value as a fully
// present payload block.
const sectorBitmapBlockPresent = 6

var crc32cTable = crc32.MakeTable(crc32.Castagnoli)

// readFullAt fills p completely or returns an error. A short read is
// never swallowed: a zero-progress underlying read returns
// io.ErrNoProgress instead of looping forever. The number of bytes
// already copied is returned even on error.
func readFullAt(r io.ReaderAt, p []byte, off int64) (int, error) {
	total := 0
	for len(p) > 0 {
		n, err := r.ReadAt(p, off)
		total += n
		if err == io.EOF && n > 0 {
			err = nil
		}
		if err != nil {
			return total, err
		}
		if n == 0 {
			return total, io.ErrNoProgress
		}
		p = p[n:]
		off += int64(n)
	}
	return total, nil
}

// headerChecksumValid verifies the CRC32C stored in a 4096 byte VHDX
// header copy. The checksum field itself is zeroed while hashing.
func headerChecksumValid(reader io.ReaderAt, headerOffset int64) (bool, error) {
	block := make([]byte, 4096)
	_, err := readFullAt(reader, block, headerOffset)
	if err != nil {
		return false, fmt.Errorf("reading VHDX header at %d: %w", headerOffset, err)
	}

	stored := uint32(block[4]) | uint32(block[5])<<8 |
		uint32(block[6])<<16 | uint32(block[7])<<24

	block[4], block[5], block[6], block[7] = 0, 0, 0, 0
	return crc32.Checksum(block, crc32cTable) == stored, nil
}

// selectCurrentHeader picks the currently effective header copy.
// A copy is eligible only when its CRC32C is valid; one corrupt copy
// is tolerated. If both eligible copies carry the same sequence number
// there is no unique current header and the image is rejected.
func (self *VHDXFile) selectCurrentHeader() (*Header, error) {
	type candidate struct {
		header   *Header
		sequence uint64
	}

	var valid []candidate
	for _, h := range []*Header{self.header.Header1(), self.header.Header2()} {
		ok, err := headerChecksumValid(self.reader, h.Offset)
		if err != nil {
			return nil, err
		}
		if !ok || h.Signature() != HeaderSignature {
			continue
		}
		valid = append(valid, candidate{header: h, sequence: h.SequenceNumber()})
	}

	if len(valid) == 0 {
		return nil, errors.New("both VHDX header copies failed CRC32C validation")
	}

	best := valid[0]
	for _, c := range valid[1:] {
		if c.sequence == best.sequence {
			return nil, fmt.Errorf("VHDX header copies share sequence number %d: "+
				"no unique current header", c.sequence)
		}
		if c.sequence > best.sequence {
			best = c
		}
	}

	return best.header, nil
}

// readUTF16Z reads a NUL terminated UTF-16LE string beginning at
// locatorBase+fieldOffset. The terminating NUL unit is aligned on a
// two byte boundary: scanning raw bytes can match the high byte of the
// final character together with the terminator, so words are checked.
func readUTF16Z(reader io.ReaderAt, locatorBase int64, fieldOffset uint32) (string, error) {
	raw := make([]byte, 512)
	_, err := readFullAt(reader, raw, locatorBase+int64(fieldOffset))
	if err != nil && !errors.Is(err, io.EOF) && !errors.Is(err, io.ErrUnexpectedEOF) {
		return "", err
	}

	end := len(raw)
	for i := 0; i+1 < len(raw); i += 2 {
		if raw[i] == 0 && raw[i+1] == 0 {
			end = i
			break
		}
	}
	return UTF16BytesToUTF8(raw[:end], binary.LittleEndian), nil
}

// parentLinkage extracts the parent_linkage value from the parent
// locator region. Path-like locator entries may exist alongside it but
// are never dereferenced: their strings are metadata only.
func (self *VHDXFile) parentLinkage() ([]byte, error) {
	var locatorOffset int64
	found := false
	for _, r := range self.regions {
		if r.GUID() == parentLocatorGUID {
			locatorOffset = int64(r.FileOffset())
			found = true
			break
		}
	}
	if !found {
		return nil, errors.New("differencing disk is missing the parent locator region")
	}

	locator := self.profile.ParentLocator(self.reader, locatorOffset)
	if locator.Signature() != locatorSignature {
		return nil, fmt.Errorf("parent locator has invalid signature %q",
			locator.Signature())
	}

	count := int(locator.EntryCount())
	if count <= 0 {
		return nil, errors.New("parent locator contains no entries")
	}

	entries := ParseArray_ParentLocatorEntry(self.profile, self.reader,
		locatorOffset+self.profile.Off_ParentLocator_Entries, count)

	for _, e := range entries {
		key, err := readUTF16Z(self.reader, locatorOffset, e.KeyOffset())
		if err != nil {
			return nil, fmt.Errorf("reading parent locator key: %w", err)
		}

		if key != locatorKeyParentLinkage {
			continue
		}

		length := int(e.ValueLength())
		if length == 0 {
			return nil, errors.New("parent_linkage locator entry is empty")
		}

		value := make([]byte, length)
		_, err = readFullAt(self.reader, value,
			locatorOffset+int64(e.ValueOffset()))
		if err != nil {
			return nil, fmt.Errorf("reading parent_linkage value: %w", err)
		}
		return value, nil
	}

	return nil, errors.New("parent locator is missing the parent_linkage entry")
}

// validateChain confirms that child and the supplied parent really
// belong together before any sector is routed between them.
func validateChain(child, parent *VHDXFile) error {
	if child.Metadata.VirtualDiskSize != parent.Metadata.VirtualDiskSize {
		return fmt.Errorf("parent virtual disk size %d does not match child %d",
			parent.Metadata.VirtualDiskSize, child.Metadata.VirtualDiskSize)
	}
	if child.Metadata.LogicalSectorSize != parent.Metadata.LogicalSectorSize {
		return fmt.Errorf("parent logical sector size %d does not match child %d",
			parent.Metadata.LogicalSectorSize, child.Metadata.LogicalSectorSize)
	}

	// Log replay is not supported: a non-zero LogGuid means the image
	// may have pending changes that must be replayed before reading.
	if !child.currentHeader.LogGuid().IsZero() {
		return errors.New("VHDX has a non-zero LogGuid: log replay is not supported")
	}
	if !parent.currentHeader.LogGuid().IsZero() {
		return errors.New("parent VHDX has a non-zero LogGuid: log replay is not supported")
	}

	linkage, err := child.parentLinkage()
	if err != nil {
		return err
	}

	parentIdentity := parent.currentHeader.DataWriteGuid().RawBytes()
	if !bytes.Equal(linkage, parentIdentity) {
		return errors.New("parent_linkage does not match the parent's " +
			"current DataWriteGuid")
	}

	// Reject identity cycles: a disk cannot (transitively) be its own
	// parent.
	childIdentity := child.currentHeader.DataWriteGuid().RawBytes()
	for ancestor := parent; ancestor != nil; ancestor = ancestor.parent {
		if bytes.Equal(childIdentity,
			ancestor.currentHeader.DataWriteGuid().RawBytes()) {
			return errors.New("duplicate DataWriteGuid in parent chain: cycle detected")
		}
	}

	return nil
}

// PayloadEntry returns the BAT state and on-disk offset (in bytes) of
// a payload block. Bitmap slots are accounted for and never returned
// as payload.
func (self *Bat) PayloadEntry(block int) (state uint64, fileOffset uint64, err error) {
	if block < 0 {
		return 0, 0, fmt.Errorf("invalid negative payload block %d", block)
	}

	element := uint64(block) + uint64(block)/self.EntriesPerChunk
	if element >= self.TotalNumberOfEntries {
		return 0, 0, fmt.Errorf("BAT too short: payload block %d (entry %d) "+
			"exceeds %d entries", block, element, self.TotalNumberOfEntries)
	}

	entry := self.profile.BATEntry(self.reader,
		int64(self.StartOffset+element*self.EntrySize))
	state = entry.State()

	switch state {
	case PAYLOAD_BLOCK_FULLY_PRESENT, PAYLOAD_BLOCK_PARTIALLY_PRESENT:
		offsetMB := entry.FileOffsetMB()
		if offsetMB == 0 {
			return state, 0, fmt.Errorf("present payload block %d has a zero file offset", block)
		}
		return state, offsetMB * 1024 * 1024, nil
	}

	return state, 0, nil
}

// BitmapEntry returns the on-disk offset of the sector bitmap block
// for a chunk, preserving the dedicated bitmap slot in the BAT.
func (self *Bat) BitmapEntry(chunk uint64) (uint64, error) {
	index := (chunk+1)*(self.EntriesPerChunk+1) - 1
	if index >= self.TotalNumberOfEntries {
		return 0, fmt.Errorf("BAT too short: bitmap entry %d exceeds %d entries",
			index, self.TotalNumberOfEntries)
	}

	entry := self.profile.BATEntry(self.reader,
		int64(self.StartOffset+index*self.EntrySize))
	if entry.State() != sectorBitmapBlockPresent {
		return 0, fmt.Errorf("sector bitmap for chunk %d is not present (BAT state %d)",
			chunk, entry.State())
	}

	offset := entry.FileOffsetMB() * 1024 * 1024
	if offset == 0 {
		return 0, fmt.Errorf("sector bitmap for chunk %d has a zero file offset", chunk)
	}
	return offset, nil
}

// readChainAt serves ReadAt for a differencing disk, choosing the
// source layer for every logical sector rather than once per block or
// per request. All routing state is local so interleaved reads and
// disks sharing a parent cannot interfere with each other.
func readChainAt(v *VHDXFile, buf []byte, off int64) (int, error) {
	if off < 0 {
		return 0, errors.New("negative offset")
	}
	if len(buf) == 0 {
		return 0, nil
	}

	size := int64(v.Metadata.VirtualDiskSize)
	if off >= size {
		return 0, io.EOF
	}

	limit := len(buf)
	if off+int64(limit) > size {
		limit = int(size - off)
	}

	lss := int64(v.Metadata.LogicalSectorSize)
	blockSize := int64(v.Metadata.BlockSize)
	sectorsPerBlock := uint64(blockSize / lss)

	written := 0
	for written < limit {
		sectorOff := uint64(off) + uint64(written)
		sector := sectorOff / uint64(lss)
		inSector := sectorOff % uint64(lss)

		n := uint64(lss) - inSector
		if remaining := uint64(limit - written); n > remaining {
			n = remaining
		}
		dst := buf[written : written+int(n)]

		block := int(sector / sectorsPerBlock)
		state, payloadOffset, err := v.bat_reader.bat.PayloadEntry(block)
		if err != nil {
			return written, err
		}

		blockOffset := int64(sectorOff % uint64(blockSize))

		switch state {
		case PAYLOAD_BLOCK_NOT_PRESENT:
			// Recurse into the parent, which may itself be a
			// differencing disk.
			n2, err := v.parent.ReadAt(dst, int64(sectorOff))
			if err != nil {
				return written + n2, err
			}

		case PAYLOAD_BLOCK_ZERO, PAYLOAD_BLOCK_UNDEFINED, PAYLOAD_BLOCK_UNMAPPED:
			// Explicitly empty: never borrow stale parent content.
			for i := range dst {
				dst[i] = 0
			}

		case PAYLOAD_BLOCK_FULLY_PRESENT:
			got, err := readFullAt(v.reader, dst, int64(payloadOffset)+blockOffset)
			if err != nil {
				return written + got,
					fmt.Errorf("reading payload block %d: %w", block, err)
			}

		case PAYLOAD_BLOCK_PARTIALLY_PRESENT:
			got, err := v.readPartialSector(dst, block,
				sector, sectorOff, blockOffset, payloadOffset)
			if err != nil {
				return written + got, err
			}

		default:
			return written, fmt.Errorf("payload block %d has unknown BAT state %d",
				block, state)
		}

		written += int(n)
	}

	if written < len(buf) {
		return written, io.EOF
	}
	return written, nil
}

// readPartialSector routes a single sector of a PARTIALLY_PRESENT
// block through its chunk's sector bitmap. Bit 0 of each bitmap byte
// is the first sector of the chunk, i.e. bits are little-endian within
// the byte. It returns the number of bytes already copied on error.
func (self *VHDXFile) readPartialSector(dst []byte, block int,
	sector, sectorOff uint64, blockOffset int64, payloadOffset uint64) (int, error) {

	bat := self.bat_reader.bat
	epc := bat.EntriesPerChunk
	chunk := uint64(block) / epc

	bitmapOffset, err := bat.BitmapEntry(chunk)
	if err != nil {
		return 0, err
	}

	lss := uint64(self.Metadata.LogicalSectorSize)
	sectorsPerBlock := uint64(self.Metadata.BlockSize) / lss

	// The bitmap bit identifies the logical sector's position inside
	// the whole chunk (payload blocks plus their sectors).
	sectorInChunk := sector % (epc * sectorsPerBlock)
	bitmapByteOffset := bitmapOffset + sectorInChunk/8
	var bitmapByte [1]byte
	if _, err = readFullAt(self.reader, bitmapByte[:], int64(bitmapByteOffset)); err != nil {
		return 0, fmt.Errorf("reading sector bitmap for chunk %d: %w", chunk, err)
	}

	fromChild := bitmapByte[0]&(1<<(sectorInChunk%8)) != 0
	if fromChild {
		got, err := readFullAt(self.reader, dst,
			int64(payloadOffset)+blockOffset)
		if err != nil {
			return got, fmt.Errorf("reading partial payload block %d: %w", block, err)
		}
		return len(dst), nil
	}

	n, err := self.parent.ReadAt(dst, int64(sectorOff))
	if err != nil {
		if n > 0 {
			return n, err
		}
		return 0, fmt.Errorf("reading parent for partial block %d: %w", block, err)
	}
	return n, nil
}
