package parser

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"hash/crc32"
	"io"
	"sort"
	"sync"
	"testing"
	"time"
)

// ---------------------------------------------------------------------
// Sparse in-memory ReaderAt: only touched pages allocate, so multi-GB
// virtual disks cost a few KiB of backing storage.
// ---------------------------------------------------------------------

const testPageSize = 4096

type sparseFile struct {
	mu    sync.Mutex
	pages map[int64][]byte
	size  int64
}

func newSparseFile() *sparseFile {
	return &sparseFile{pages: make(map[int64][]byte)}
}

func (s *sparseFile) WriteAt(p []byte, off int64) {
	s.mu.Lock()
	defer s.mu.Unlock()

	for len(p) > 0 {
		pg := off / testPageSize
		page, ok := s.pages[pg]
		if !ok {
			page = make([]byte, testPageSize)
			s.pages[pg] = page
		}
		n := copy(page[off%testPageSize:], p)
		p = p[n:]
		off += int64(n)
	}
}

func (s *sparseFile) setSize(size int64) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if size > s.size {
		s.size = size
	}
}

func (s *sparseFile) ReadAt(p []byte, off int64) (int, error) {
	s.mu.Lock()
	size := s.size
	s.mu.Unlock()

	if off < 0 {
		return 0, errors.New("negative offset")
	}
	if off >= size {
		return 0, io.EOF
	}

	n := len(p)
	if off+int64(n) > size {
		n = int(size - off)
	}

	for i := 0; i < n; {
		pg := (off + int64(i)) / testPageSize
		end := int((pg+1)*testPageSize - off)
		if end > n {
			end = n
		}
		if page, ok := s.pages[pg]; ok {
			copy(p[i:end], page[(off+int64(i))%testPageSize:])
		} // missing pages read as zero bytes
		i = end
	}

	if n < len(p) {
		return n, io.EOF
	}
	return n, nil
}

// snapshot returns a deterministic fingerprint of every written page,
// used to prove readers never mutate their inputs.
func (s *sparseFile) fingerprint() [][2]string {
	s.mu.Lock()
	defer s.mu.Unlock()

	keys := make([]int64, 0, len(s.pages))
	for pg := range s.pages {
		keys = append(keys, pg)
	}
	sort.Slice(keys, func(i, j int) bool { return keys[i] < keys[j] })

	out := make([][2]string, 0, len(keys))
	for _, pg := range keys {
		out = append(out, [2]string{
			fmt.Sprintf("%d", pg),
			string(append([]byte(nil), s.pages[pg]...)),
		})
	}
	return out
}

// faultReader wraps a ReaderAt and can inject one short read or a
// zero-progress read after construction has finished.
type faultReader struct {
	inner io.ReaderAt

	active bool

	// When set, the first ReadAt touching shortAt returns a shortened
	// result plus an error.
	shortAt    int64
	shortN     int
	shortErr   error
	shortFired bool
	zeroAt     int64
	zeroFired  bool
}

func (f *faultReader) ReadAt(p []byte, off int64) (int, error) {
	if f.active && !f.zeroFired && f.zeroAt != 0 && off == f.zeroAt {
		f.zeroFired = true
		return 0, nil
	}

	if f.active && !f.shortFired && f.shortAt != 0 && off == f.shortAt {
		f.shortFired = true
		n := f.shortN
		if n > len(p) {
			n = len(p)
		}
		got, err := f.inner.ReadAt(p[:n], off)
		if err != nil {
			return got, err
		}
		return got, f.shortErr
	}

	return f.inner.ReadAt(p, off)
}

// ---------------------------------------------------------------------
// VHDX image builder: writes real file headers, header copies, region
// table, metadata, parent locator, BAT and payload/bitmap blocks.
// ---------------------------------------------------------------------

const (
	tbRegionTable1 = 192 * 1024
	tbRegionTable2 = 256 * 1024
	tbBATOffset    = 320 * 1024
	tbMetaOffset   = 384 * 1024
	tbLocOffset    = 448 * 1024
	tbDataOffset   = 1024 * 1024
)

type guid16 [16]byte

type blockPlan struct {
	state   uint64
	fill    byte            // bytes stored in the child payload
	sectors map[uint64]bool // partial block: sector-in-block -> child owned
}

type vhdxImage struct {
	file *sparseFile
}

type vhdxSpec struct {
	blockSize uint32
	lss       uint32
	virtual   uint64
	hasParent bool

	seq1, seq2 uint64
	dataGuid   guid16
	fileGuid   guid16
	logGuid    guid16
	header1Bad bool
	header2Bad bool

	blocks map[int]*blockPlan

	linkage      []byte
	noLocator    bool
	badLocator   bool
	noLinkage    bool
	batLengthCap uint32 // truncate BAT region length when non-zero
	missingBmp   map[uint64]bool
}

func putUint16(b []byte, v uint16) { binary.LittleEndian.PutUint16(b, v) }
func putUint32(b []byte, v uint32) { binary.LittleEndian.PutUint32(b, v) }
func putUint64(b []byte, v uint64) { binary.LittleEndian.PutUint64(b, v) }

func guidBytes(g guid16) []byte { return g[:] }

func writeHeader(f *sparseFile, off int64, seq uint64,
	fileGuid, dataGuid, logGuid guid16, corrupt bool) {

	block := make([]byte, 4096)
	copy(block[0:4], HeaderSignature)
	putUint64(block[8:], seq)
	copy(block[16:32], fileGuid[:])
	copy(block[32:48], dataGuid[:])
	copy(block[48:64], logGuid[:])

	checksum := crc32.Checksum(block, crc32cTable)
	if corrupt {
		checksum ^= 0xdeadbeef
	}
	putUint32(block[4:], checksum)
	f.WriteAt(block, off)
}

func writeRegionTable(f *sparseFile, off int64, entries []byte) {
	header := make([]byte, 16)
	copy(header[0:4], RegionSignature)
	putUint32(header[8:], uint32(len(entries)/32))
	f.WriteAt(header, off)
	f.WriteAt(entries, off+16)
}

func utf16z(s string) []byte {
	out := make([]byte, 0, len(s)*2+2)
	for _, r := range s {
		b := make([]byte, 2)
		binary.LittleEndian.PutUint16(b, uint16(r))
		out = append(out, b...)
	}
	return append(out, 0, 0)
}

func buildVHDX(spec *vhdxSpec) *vhdxImage {
	f := newSparseFile()
	f.WriteAt([]byte(VHDXFileSignature), 0)

	writeHeader(f, 65536, spec.seq1, spec.fileGuid, spec.dataGuid,
		spec.logGuid, spec.header1Bad)
	writeHeader(f, 131072, spec.seq2, spec.fileGuid, spec.dataGuid,
		spec.logGuid, spec.header2Bad)

	epc := uint64(1<<23) * uint64(spec.lss) / uint64(spec.blockSize)
	maxBlock := 0
	for b := range spec.blocks {
		if b > maxBlock {
			maxBlock = b
		}
	}

	// ---- BAT layout (payload slots plus one bitmap slot per chunk) ----
	usedChunks := map[uint64]bool{}
	for b, p := range spec.blocks {
		if p.state == PAYLOAD_BLOCK_PARTIALLY_PRESENT {
			usedChunks[uint64(b)/epc] = true
		}
	}

	maxElement := uint64(maxBlock) + uint64(maxBlock)/epc
	for c := range usedChunks {
		idx := (c+1)*(epc+1) - 1
		if idx > maxElement {
			maxElement = idx
		}
	}
	batEntries := maxElement + 1

	// ---- payload and bitmap allocations ----
	dataCursor := int64(tbDataOffset)
	payloadOff := map[int]uint64{}
	for b, p := range spec.blocks {
		if p.state != PAYLOAD_BLOCK_FULLY_PRESENT &&
			p.state != PAYLOAD_BLOCK_PARTIALLY_PRESENT {
			continue
		}
		payloadOff[b] = uint64(dataCursor)
		data := make([]byte, spec.blockSize)
		for i := range data {
			data[i] = p.fill
		}
		if p.state == PAYLOAD_BLOCK_PARTIALLY_PRESENT {
			lss := int(spec.lss)
			for sec := range p.sectors {
				for i := sec * uint64(lss); i < (sec+1)*uint64(lss); i++ {
					data[i] = p.fill
				}
			}
		}
		f.WriteAt(data, dataCursor)
		dataCursor += int64(spec.blockSize)
	}

	bitmapSize := int64(1 << 23 / 8) // one bit per 2^23 chunk sectors
	bitmapOff := map[uint64]uint64{}
	for c := range usedChunks {
		bitmapOff[c] = uint64(dataCursor)
		bmp := make([]byte, bitmapSize)
		for b, p := range spec.blocks {
			if p.state != PAYLOAD_BLOCK_PARTIALLY_PRESENT {
				continue
			}
			if uint64(b)/epc != c {
				continue
			}
			sectorsPerBlock := uint64(spec.blockSize) / uint64(spec.lss)
			for sec, owned := range p.sectors {
				if !owned {
					continue
				}
				sectorInChunk := (uint64(b)%epc)*sectorsPerBlock + sec
				bmp[sectorInChunk/8] |= 1 << (sectorInChunk % 8)
			}
		}
		f.WriteAt(bmp, dataCursor)
		dataCursor += bitmapSize
	}

	// ---- BAT entries ----
	bat := make([]byte, batEntries*8)
	putBAT := func(index uint64, state uint64, offsetMB uint64) {
		putUint64(bat[index*8:], state|(offsetMB<<20))
	}
	for b, p := range spec.blocks {
		element := uint64(b) + uint64(b)/epc
		offMB := uint64(0)
		if o, ok := payloadOff[b]; ok {
			offMB = o / (1024 * 1024)
		}
		putBAT(element, p.state, offMB)
	}
	for c := range usedChunks {
		idx := (c+1)*(epc+1) - 1
		if spec.missingBmp != nil && spec.missingBmp[c] {
			putBAT(idx, PAYLOAD_BLOCK_NOT_PRESENT, 0)
			continue
		}
		putBAT(idx, sectorBitmapBlockPresent, bitmapOff[c]/(1024*1024))
	}
	batLength := uint32(batEntries * 8)
	if spec.batLengthCap != 0 && spec.batLengthCap < batLength {
		batLength = spec.batLengthCap
	}
	f.WriteAt(bat, tbBATOffset)

	// ---- metadata ----
	meta := make([]byte, 32*1024)
	copy(meta[0:8], MetadataSignature)
	putUint16(meta[10:], 5)

	putMetaEntry := func(idx int, guid string, valueOff, length uint32) {
		e := meta[32+idx*32:]
		copy(e[0:16], guid)
		putUint32(e[16:], valueOff)
		putUint32(e[20:], length)
	}

	putMetaEntry(0, MetadataFileParameters, 1024, 8)
	putUint32(meta[1024:], spec.blockSize)
	if spec.hasParent {
		putUint32(meta[1028:], 0x2) // HasParent is bit 1 of the flags dword
	}

	putMetaEntry(1, MetadataVirtualDiskSize, 1032, 8)
	putUint64(meta[1032:], spec.virtual)

	putMetaEntry(2, MetadataLogicalSectorSize, 1040, 4)
	putUint32(meta[1040:], spec.lss)

	putMetaEntry(3, MetadataPhysicalSectorSize, 1044, 4)
	putUint32(meta[1044:], 4096)

	putMetaEntry(4, MetadataVirtualDiskId, 1048, 16)
	copy(meta[1048:1064], spec.dataGuid[:])
	f.WriteAt(meta, tbMetaOffset)

	// ---- parent locator ----
	regionEntries := &bytes.Buffer{}
	appendRegion := func(guid string, off uint64, length uint32, required uint32) {
		e := make([]byte, 32)
		copy(e[0:16], guid)
		putUint64(e[16:], off)
		putUint32(e[24:], length)
		putUint32(e[28:], required)
		regionEntries.Write(e)
	}
	appendRegion(BAT_GUID, tbBATOffset, batLength, 1)
	appendRegion(Metadata_GUID, tbMetaOffset, uint32(len(meta)), 1)

	if spec.hasParent && !spec.noLocator {
		loc := make([]byte, 8192)
		if !spec.badLocator {
			copy(loc[0:4], locatorSignature)
		} else {
			copy(loc[0:4], "xxxx")
		}

		type locEntry struct {
			key   string
			value []byte
		}
		var locs []locEntry
		if !spec.noLinkage {
			locs = append(locs, locEntry{
				key:   locatorKeyParentLinkage,
				value: append([]byte(nil), spec.linkage...),
			})
		}
		// Path strings are metadata only: a deliberately impossible
		// host path proves nothing opens it.
		locs = append(locs, locEntry{
			key:   "relative_path",
			value: utf16z("Z:\\definitely\\not\\opened.vhdx"),
		})

		putUint32(loc[8:], uint32(len(locs)))
		keyOff := uint32(4096)
		valOff := uint32(5120)
		for i, l := range locs {
			e := loc[16+i*24:]
			putUint32(e[0:], keyOff)
			putUint32(e[8:], valOff)
			putUint16(e[16:], uint16(len(l.value)))
			keyOff += uint32(len(utf16z(l.key))) + 16
			valOff += uint32(len(l.value)) + 16
		}
		// Write the locator block first; the key/value strings that
		// follow must not be overwritten by the block's zero tail.
		f.WriteAt(loc, tbLocOffset)
		keyOff = 4096
		valOff = 5120
		for _, l := range locs {
			f.WriteAt(utf16z(l.key), tbLocOffset+int64(keyOff))
			f.WriteAt(l.value, tbLocOffset+int64(valOff))
			keyOff += uint32(len(utf16z(l.key))) + 16
			valOff += uint32(len(l.value)) + 16
		}
		appendRegion(parentLocatorGUID, tbLocOffset, uint32(len(loc)), 1)
	}

	writeRegionTable(f, tbRegionTable1, regionEntries.Bytes())
	writeRegionTable(f, tbRegionTable2, regionEntries.Bytes())

	f.setSize(dataCursor + testPageSize)
	return &vhdxImage{file: f}
}

// ---------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------

func mustOpen(t *testing.T, r io.ReaderAt) *VHDXFile {
	t.Helper()
	v, err := NewVHDXFile(r)
	if err != nil {
		t.Fatalf("NewVHDXFile failed: %v", err)
	}
	return v
}

func readAll(t *testing.T, r io.ReaderAt, off int64, n int) []byte {
	t.Helper()
	buf := make([]byte, n)
	got, err := r.ReadAt(buf, off)
	if err != nil && !errors.Is(err, io.EOF) {
		t.Fatalf("ReadAt off=%d n=%d failed after %d: %v", off, n, got, err)
	}
	return buf[:got]
}

func fillByte(n int, b byte) []byte {
	out := make([]byte, n)
	for i := range out {
		out[i] = b
	}
	return out
}

// ---------------------------------------------------------------------
// Test 1: source routing
// ---------------------------------------------------------------------

func TestVHDXChainRouting(t *testing.T) {
	const (
		lss       = uint32(512)
		blockSize = uint32(4 * 1024 * 1024)
		blocks    = 6
	)
	virtual := uint64(blocks) * uint64(blockSize)

	parentGuid := guid16{0x10, 0x20, 0x30}
	childGuid := guid16{0xaa, 0xbb, 0xcc}
	grandGuid := guid16{0x77, 0x88, 0x99}

	// Parent uses a *different* block size: its block boundaries do not
	// line up with the child's.
	parentImg := buildVHDX(&vhdxSpec{
		blockSize: blockSize / 2,
		lss:       lss,
		virtual:   virtual,
		seq1:      1, seq2: 2,
		dataGuid: parentGuid,
		blocks: map[int]*blockPlan{
			0:  {state: PAYLOAD_BLOCK_FULLY_PRESENT, fill: 0x40},
			1:  {state: PAYLOAD_BLOCK_FULLY_PRESENT, fill: 0x40},
			2:  {state: PAYLOAD_BLOCK_FULLY_PRESENT, fill: 0x41},
			3:  {state: PAYLOAD_BLOCK_FULLY_PRESENT, fill: 0x41},
			4:  {state: PAYLOAD_BLOCK_FULLY_PRESENT, fill: 0x42},
			5:  {state: PAYLOAD_BLOCK_FULLY_PRESENT, fill: 0x43},
			6:  {state: PAYLOAD_BLOCK_FULLY_PRESENT, fill: 0x43},
			7:  {state: PAYLOAD_BLOCK_FULLY_PRESENT, fill: 0x43},
			8:  {state: PAYLOAD_BLOCK_NOT_PRESENT},
			9:  {state: PAYLOAD_BLOCK_FULLY_PRESENT, fill: 0x44},
			10: {state: PAYLOAD_BLOCK_FULLY_PRESENT, fill: 0x45},
			11: {state: PAYLOAD_BLOCK_FULLY_PRESENT, fill: 0x45},
		},
	})
	parent := mustOpen(t, parentImg.file)

	childImg := buildVHDX(&vhdxSpec{
		blockSize: blockSize,
		lss:       lss,
		virtual:   virtual,
		hasParent: true,
		seq1:      5, seq2: 6,
		dataGuid: childGuid,
		linkage:  parentGuid[:],
		blocks: map[int]*blockPlan{
			0: {state: PAYLOAD_BLOCK_FULLY_PRESENT, fill: 0xa0},
			1: {
				state: PAYLOAD_BLOCK_PARTIALLY_PRESENT,
				fill:  0xb0,
				sectors: map[uint64]bool{
					1: true, 3: true, 7: true, // sectors 0,2,8,... -> parent
				},
			},
			2: {state: PAYLOAD_BLOCK_NOT_PRESENT},
			3: {state: PAYLOAD_BLOCK_ZERO},
			4: {state: PAYLOAD_BLOCK_UNMAPPED},
			5: {state: PAYLOAD_BLOCK_PARTIALLY_PRESENT,
				fill: 0xb1, sectors: map[uint64]bool{}}, // every sector -> parent
		},
	})
	child, err := NewVHDXFileWithParent(childImg.file, parent)
	if err != nil {
		t.Fatalf("NewVHDXFileWithParent failed: %v", err)
	}

	bs := int64(blockSize)

	// FULLY_PRESENT never consults the bitmap or parent.
	if got := readAll(t, child, 123, 16); !bytes.Equal(got, fillByte(16, 0xa0)) {
		t.Fatalf("fully present block leaked other data: %x", got)
	}

	// PARTIALLY_PRESENT, per-sector routing inside block 1.
	checkSector := func(sec uint64, want byte) {
		got := readAll(t, child, bs+int64(sec)*int64(lss), int(lss))
		if !bytes.Equal(got, fillByte(int(lss), want)) {
			t.Fatalf("partial block sector %d: want %02x got %x", sec, want, got[:8])
		}
	}
	checkSector(0, 0x41) // parent (parent 2MB block 2)
	checkSector(1, 0xb0) // child
	checkSector(2, 0x41) // parent
	checkSector(3, 0xb0) // child
	checkSector(7, 0xb0) // child, last bit of bitmap byte 0
	checkSector(8, 0x41) // parent, first bit of bitmap byte 1

	// Unaligned read crossing a bitmap *byte* boundary (sector 7 -> 8)
	// and mixing child then parent content.
	cross := readAll(t, child, bs+7*int64(lss), 2*int(lss))
	if !bytes.Equal(cross[:int(lss)], fillByte(int(lss), 0xb0)) ||
		!bytes.Equal(cross[int(lss):], fillByte(int(lss), 0x41)) {
		t.Fatalf("bitmap byte boundary routing wrong: %x...", cross[:16])
	}

	// Unaligned read crossing the payload block boundary between a
	// FULLY_PRESENT block and a PARTIALLY_PRESENT block. The 600 byte
	// tail itself crosses a sector boundary: sector 0 comes from the
	// parent (0x41) while the child-owned sector 1 returns 0xb0.
	cross = readAll(t, child, bs-300, 900)
	if !bytes.Equal(cross[:300], fillByte(300, 0xa0)) ||
		!bytes.Equal(cross[300:300+int(lss)], fillByte(int(lss), 0x41)) ||
		!bytes.Equal(cross[300+int(lss):], fillByte(88, 0xb0)) {
		t.Fatalf("payload block boundary routing wrong: %x %x %x",
			cross[0], cross[300], cross[300+int(lss)])
	}

	// NOT_PRESENT recurses to the parent; the read spans a parent block
	// boundary (parent blocks are 2 MiB, boundary at virtual 10 MiB).
	cross = readAll(t, child, 2*bs+(2*1024*1024)-200, 600)
	if !bytes.Equal(cross[:200], fillByte(200, 0x42)) ||
		!bytes.Equal(cross[200:], fillByte(400, 0x43)) {
		t.Fatalf("parent block boundary routing wrong: %x %x", cross[0], cross[200])
	}

	// ZERO / UNMAPPED must not borrow parent content (parent holds
	// distinct non-zero data at those offsets).
	if got := readAll(t, child, 3*bs+77, 1234); !bytes.Equal(got, make([]byte, 1234)) {
		t.Fatalf("ZERO block returned non-zero data")
	}
	if got := readAll(t, child, 4*bs+77, 1234); !bytes.Equal(got, make([]byte, 1234)) {
		t.Fatalf("UNMAPPED block returned non-zero data")
	}

	// PARTIALLY_PRESENT with no owned sectors routes everything to parent.
	if got := readAll(t, child, 5*bs+512, int(lss)); !bytes.Equal(
		got, fillByte(int(lss), 0x45)) {
		t.Fatalf("all-parent partial block misrouted: %x", got[:8])
	}

	// A grandchild: the parent may itself be a differencing disk.
	midImg := buildVHDX(&vhdxSpec{
		blockSize: blockSize, lss: lss, virtual: virtual,
		hasParent: true, seq1: 1, seq2: 2,
		dataGuid: grandGuid, linkage: parentGuid[:],
		blocks: map[int]*blockPlan{
			0: {state: PAYLOAD_BLOCK_NOT_PRESENT},
			1: {state: PAYLOAD_BLOCK_FULLY_PRESENT, fill: 0x61},
		},
	})
	mid, err := NewVHDXFileWithParent(midImg.file, parent)
	if err != nil {
		t.Fatalf("middle disk open failed: %v", err)
	}
	leafImg := buildVHDX(&vhdxSpec{
		blockSize: blockSize, lss: lss, virtual: virtual,
		hasParent: true, seq1: 1, seq2: 2,
		dataGuid: guid16{0x55, 0x66}, linkage: grandGuid[:],
		blocks: map[int]*blockPlan{
			0: {
				state:   PAYLOAD_BLOCK_PARTIALLY_PRESENT,
				fill:    0x62,
				sectors: map[uint64]bool{0: true}, // sector 0 child, rest -> mid -> gp
			},
			1: {state: PAYLOAD_BLOCK_NOT_PRESENT},
		},
	})
	leaf, err := NewVHDXFileWithParent(leafImg.file, mid)
	if err != nil {
		t.Fatalf("leaf disk open failed: %v", err)
	}
	if got := readAll(t, leaf, 0, int(lss)); !bytes.Equal(got, fillByte(int(lss), 0x62)) {
		t.Fatalf("leaf owned sector wrong")
	}
	if got := readAll(t, leaf, int64(lss), int(lss)); !bytes.Equal(
		got, fillByte(int(lss), 0x40)) {
		t.Fatalf("leaf sector should reach grandparent, got %x", got[:8])
	}
	if got := readAll(t, leaf, bs, 64); !bytes.Equal(got, fillByte(64, 0x61)) {
		t.Fatalf("leaf NOT_PRESENT should reach middle disk")
	}
}

// ---------------------------------------------------------------------
// Test 2: identity, parent locator and dual-header rules
// ---------------------------------------------------------------------

func TestVHDXChainIdentity(t *testing.T) {
	const (
		lss       = uint32(512)
		blockSize = uint32(1024 * 1024)
	)
	virtual := uint64(4 * blockSize)
	parentGuid := guid16{1, 2, 3, 4, 5, 6, 7, 8}
	childGuid := guid16{9, 9, 9, 9}

	baseParent := func(mut func(*vhdxSpec)) *sparseFile {
		spec := &vhdxSpec{
			blockSize: blockSize, lss: lss, virtual: virtual,
			seq1: 2, seq2: 3, dataGuid: parentGuid,
			blocks: map[int]*blockPlan{
				0: {state: PAYLOAD_BLOCK_FULLY_PRESENT, fill: 1},
			},
		}
		if mut != nil {
			mut(spec)
		}
		return buildVHDX(spec).file
	}
	baseChild := func(mut func(*vhdxSpec)) *sparseFile {
		spec := &vhdxSpec{
			blockSize: blockSize, lss: lss, virtual: virtual,
			hasParent: true, seq1: 4, seq2: 5,
			dataGuid: childGuid, linkage: parentGuid[:],
			blocks: map[int]*blockPlan{
				0: {state: PAYLOAD_BLOCK_NOT_PRESENT},
			},
		}
		if mut != nil {
			mut(spec)
		}
		return buildVHDX(spec).file
	}

	// Happy path through the real file header / metadata / BAT path.
	parent := mustOpen(t, baseParent(nil))
	if _, err := NewVHDXFileWithParent(baseChild(nil), parent); err != nil {
		t.Fatalf("valid chain rejected: %v", err)
	}

	// Old entry point: differencing disks are rejected, never zero filled.
	if _, err := NewVHDXFile(baseChild(nil)); err == nil {
		t.Fatal("NewVHDXFile accepted a differencing disk")
	}
	// New entry point requires a differencing disk and a non-nil parent.
	if _, err := NewVHDXFileWithParent(baseParent(nil), parent); err == nil {
		t.Fatal("NewVHDXFileWithParent accepted a non-differencing disk")
	}
	if _, err := NewVHDXFileWithParent(baseChild(nil), nil); err == nil {
		t.Fatal("NewVHDXFileWithParent accepted a nil parent")
	}

	// parent_linkage mismatch.
	wrong := baseChild(func(s *vhdxSpec) { s.linkage = guidBytes(guid16{0xfe, 0xed}) })
	if _, err := NewVHDXFileWithParent(wrong, parent); err == nil {
		t.Fatal("mismatched parent identity accepted")
	}

	// Missing locator region, bad locator signature, missing linkage entry.
	for _, mut := range []func(*vhdxSpec){
		func(s *vhdxSpec) { s.noLocator = true },
		func(s *vhdxSpec) { s.badLocator = true },
		func(s *vhdxSpec) { s.noLinkage = true },
	} {
		if _, err := NewVHDXFileWithParent(baseChild(mut), parent); err == nil {
			t.Fatal("invalid parent locator accepted")
		}
	}

	// One corrupt header copy is tolerated via CRC32C + sequence.
	oneBad := mustOpen(t, baseParent(func(s *vhdxSpec) { s.header1Bad = true }))
	if _, err := NewVHDXFileWithParent(baseChild(nil), oneBad); err != nil {
		t.Fatalf("parent with one bad header copy rejected: %v", err)
	}
	// Header selection by sequence number: the higher-sequenced valid
	// copy defines the current identity.
	selected := baseParent(func(s *vhdxSpec) {
		s.seq1 = 9
		s.dataGuid = guid16{0xc0, 0xff, 0xee} // header 1 wins, identity changes
	})
	selParent := mustOpen(t, selected)
	if _, err := NewVHDXFileWithParent(baseChild(nil), selParent); err == nil {
		t.Fatal("stale linkage matched against the higher-sequence header")
	}
	matchingChild := baseChild(func(s *vhdxSpec) {
		s.linkage = guidBytes(guid16{0xc0, 0xff, 0xee})
	})
	if _, err := NewVHDXFileWithParent(matchingChild, selParent); err != nil {
		t.Fatalf("linkage to the selected current header rejected: %v", err)
	}

	// Both copies corrupt -> reject (at open or at chain assembly).
	bothBad := baseParent(func(s *vhdxSpec) {
		s.header1Bad = true
		s.header2Bad = true
	})
	expectChainRejection(t, baseChild(nil), bothBad)

	// Equal sequence numbers on two valid copies -> no unique header.
	tied := baseParent(func(s *vhdxSpec) { s.seq1 = 7; s.seq2 = 7 })
	expectChainRejection(t, baseChild(nil), tied)

	// Non-zero LogGuid is rejected: log replay is not supported.
	logChild := baseChild(func(s *vhdxSpec) { s.logGuid = guid16{1} })
	if _, err := NewVHDXFileWithParent(logChild, parent); err == nil {
		t.Fatal("child with non-zero LogGuid accepted")
	}
	logParent := mustOpen(t, baseParent(func(s *vhdxSpec) {
		s.logGuid = guid16{2}
	}))
	if _, err := NewVHDXFileWithParent(baseChild(nil), logParent); err == nil {
		t.Fatal("parent with non-zero LogGuid accepted")
	}

	// Geometry mismatches.
	badSize := mustOpen(t, baseParent(func(s *vhdxSpec) {
		s.virtual = virtual + 4096
	}))
	if _, err := NewVHDXFileWithParent(baseChild(nil), badSize); err == nil {
		t.Fatal("virtual size mismatch accepted")
	}
	badLss := mustOpen(t, baseParent(func(s *vhdxSpec) {
		s.lss = 4096
	}))
	if _, err := NewVHDXFileWithParent(baseChild(nil), badLss); err == nil {
		t.Fatal("logical sector size mismatch accepted")
	}

	// Duplicate identity forming a chain cycle.
	gp := mustOpen(t, baseParent(nil))
	midSpec := &vhdxSpec{
		blockSize: blockSize, lss: lss, virtual: virtual,
		hasParent: true, seq1: 1, seq2: 2,
		dataGuid: guid16{0x33}, linkage: parentGuid[:],
		blocks: map[int]*blockPlan{0: {state: PAYLOAD_BLOCK_NOT_PRESENT}},
	}
	mid, err := NewVHDXFileWithParent(buildVHDX(midSpec).file, gp)
	if err != nil {
		t.Fatalf("middle disk failed: %v", err)
	}
	cycling := baseChild(func(s *vhdxSpec) {
		s.dataGuid = parentGuid // collides with the grandparent
		s.linkage = midSpec.dataGuid[:]
	})
	if _, err := NewVHDXFileWithParent(cycling, mid); err == nil {
		t.Fatal("identity cycle accepted")
	}
}

// expectChainRejection asserts that a parent image which cannot offer
// a unique valid header is refused either at open or at chain assembly.
func expectChainRejection(t *testing.T, childFile, parentFile *sparseFile) {
	t.Helper()
	parent, err := NewVHDXFile(parentFile)
	if err != nil {
		return // opening the parent already refused it
	}
	if _, err := NewVHDXFileWithParent(childFile, parent); err == nil {
		t.Fatal("invalid parent chain was accepted")
	}
}

// ---------------------------------------------------------------------
// Test 3: boundaries, EOF semantics and error accounting
// ---------------------------------------------------------------------

func TestVHDXChainBoundaries(t *testing.T) {
	const (
		lss       = uint32(512)
		blockSize = uint32(4 * 1024 * 1024)
	)
	parentGuid := guid16{0x61, 0x62}
	childGuid := guid16{0x71, 0x72}

	// Image spanning a chunk boundary (chunk = 2^23 sectors = 4 GiB).
	const epc = uint64(1024) // 4 GiB / 4 MiB
	virtual := uint64(1026) * uint64(blockSize)

	parentSpec := &vhdxSpec{
		blockSize: blockSize, lss: lss, virtual: virtual,
		seq1: 1, seq2: 2, dataGuid: parentGuid,
		blocks: map[int]*blockPlan{
			1023: {state: PAYLOAD_BLOCK_FULLY_PRESENT, fill: 0x70},
			1024: {state: PAYLOAD_BLOCK_FULLY_PRESENT, fill: 0x71},
		},
	}
	parent := mustOpen(t, buildVHDX(parentSpec).file)

	childSpec := &vhdxSpec{
		blockSize: blockSize, lss: lss, virtual: virtual,
		hasParent: true, seq1: 3, seq2: 4,
		dataGuid: childGuid, linkage: parentGuid[:],
		blocks: map[int]*blockPlan{
			1023: {state: PAYLOAD_BLOCK_FULLY_PRESENT, fill: 0xc0},
			1024: {
				state: PAYLOAD_BLOCK_PARTIALLY_PRESENT, fill: 0xd0,
				sectors: map[uint64]bool{
					0: true, 8: true, // sectors 1, 9, ... come from parent
				},
			},
		},
	}
	childImg := buildVHDX(childSpec)
	child, err := NewVHDXFileWithParent(childImg.file, parent)
	if err != nil {
		t.Fatalf("chain across chunk boundary rejected: %v", err)
	}

	// Bitmap slot is preserved: chunk 1 bitmap is BAT index 2049,
	// payload block 1024 lands at index 2050.
	if off, err := child.bat_reader.bat.BitmapEntry(1); err != nil || off == 0 {
		t.Fatalf("chunk 1 bitmap not located: off=%d err=%v", off, err)
	}
	state, _, err := child.bat_reader.bat.PayloadEntry(1024)
	if err != nil || state != PAYLOAD_BLOCK_PARTIALLY_PRESENT {
		t.Fatalf("payload entry misread across bitmap slot: state=%d err=%v",
			state, err)
	}

	chunkEdge := int64(4 * 1024 * 1024 * 1024)

	// Unaligned read crossing the chunk boundary and the first bitmap
	// byte's sector routing inside the new chunk.
	buf := make([]byte, 256+2*int(lss))
	n, err := child.ReadAt(buf, chunkEdge-256)
	if err != nil {
		t.Fatalf("cross-chunk read failed: %v", err)
	}
	if n != len(buf) {
		t.Fatalf("cross-chunk short read: %d", n)
	}
	if !bytes.Equal(buf[:256], fillByte(256, 0xc0)) {
		t.Fatal("tail of chunk 0 block misrouted")
	}
	if !bytes.Equal(buf[256:256+int(lss)], fillByte(int(lss), 0xd0)) {
		t.Fatal("chunk 1 owned sector misrouted")
	}
	if !bytes.Equal(buf[256+int(lss):], fillByte(int(lss), 0x71)) {
		t.Fatal("chunk 1 parent-backed sector misrouted")
	}

	// Bitmap byte boundary inside the final (short) chunk.
	buf = make([]byte, 2*int(lss))
	n, err = child.ReadAt(buf, chunkEdge+8*int64(lss))
	if err != nil {
		t.Fatalf("final chunk bitmap read failed: %v", err)
	}
	if !bytes.Equal(buf[:int(lss)], fillByte(int(lss), 0xd0)) ||
		!bytes.Equal(buf[int(lss):], fillByte(int(lss), 0x71)) {
		t.Fatal("final chunk bitmap byte boundary wrong")
	}

	// ReaderAt contract.
	if n, err := child.ReadAt([]byte{}, 1234); n != 0 || err != nil {
		t.Fatalf("empty buffer must be legal: n=%d err=%v", n, err)
	}
	if n, err := child.ReadAt(make([]byte, 8), -1); n != 0 || err == nil {
		t.Fatalf("negative offset must error: n=%d err=%v", n, err)
	}
	if n, err := child.ReadAt(make([]byte, 8), int64(virtual)); n != 0 ||
		!errors.Is(err, io.EOF) {
		t.Fatalf("read at virtual end: n=%d err=%v", n, err)
	}
	tail := make([]byte, 4096)
	n, err = child.ReadAt(tail, int64(virtual)-100)
	if !errors.Is(err, io.EOF) || n != 100 {
		t.Fatalf("EOF prefix semantics wrong: n=%d err=%v", n, err)
	}

	// BAT too short: entries beyond the declared region length error
	// instead of being treated as holes.
	shortImg := buildVHDX(&vhdxSpec{
		blockSize: blockSize, lss: lss, virtual: virtual,
		hasParent: true, seq1: 1, seq2: 2,
		dataGuid: childGuid, linkage: parentGuid[:],
		batLengthCap: 16,
		blocks:       map[int]*blockPlan{0: {state: PAYLOAD_BLOCK_NOT_PRESENT}},
	})
	shortChild, err := NewVHDXFileWithParent(shortImg.file, parent)
	if err != nil {
		t.Fatalf("short BAT image open failed: %v", err)
	}
	if _, err := shortChild.ReadAt(make([]byte, lss), chunkEdge); err == nil {
		t.Fatal("truncated BAT read did not error")
	}

	// Missing bitmap for a partial block must error, not fall back.
	noBmpImg := buildVHDX(&vhdxSpec{
		blockSize: blockSize, lss: lss, virtual: virtual,
		hasParent: true, seq1: 1, seq2: 2,
		dataGuid: childGuid, linkage: parentGuid[:],
		missingBmp: map[uint64]bool{1: true},
		blocks: map[int]*blockPlan{
			1024: {state: PAYLOAD_BLOCK_PARTIALLY_PRESENT, fill: 1,
				sectors: map[uint64]bool{0: true}},
		},
	})
	noBmpChild, err := NewVHDXFileWithParent(noBmpImg.file, parent)
	if err != nil {
		t.Fatalf("missing-bitmap image open failed: %v", err)
	}
	if _, err := noBmpChild.ReadAt(make([]byte, lss), chunkEdge); err == nil {
		t.Fatal("missing sector bitmap was silently ignored")
	}

	// Reserved/unknown BAT state errors rather than returning zeros.
	unknownImg := buildVHDX(&vhdxSpec{
		blockSize: blockSize, lss: lss, virtual: virtual,
		hasParent: true, seq1: 1, seq2: 2,
		dataGuid: childGuid, linkage: parentGuid[:],
		blocks: map[int]*blockPlan{0: {state: 5}},
	})
	unknownChild, err := NewVHDXFileWithParent(unknownImg.file, parent)
	if err != nil {
		t.Fatalf("unknown-state image open failed: %v", err)
	}
	if _, err := unknownChild.ReadAt(make([]byte, 8), 0); err == nil {
		t.Fatal("unknown BAT state did not error")
	}

	// Underlying short read on a present child block: the exact copied
	// count must be surfaced with the error, with no zero-progress loop.
	// The fault fires at the real on-disk payload offset, not the
	// virtual offset.
	faultImg := buildVHDX(&vhdxSpec{
		blockSize: blockSize, lss: lss, virtual: virtual,
		hasParent: true, seq1: 1, seq2: 2,
		dataGuid: childGuid, linkage: parentGuid[:],
		blocks: map[int]*blockPlan{
			1023: {state: PAYLOAD_BLOCK_FULLY_PRESENT, fill: 0xc0},
		},
	})
	payloadFileOff := int64(tbDataOffset) + int64(blockSize) - 100
	fault := &faultReader{
		inner:    faultImg.file,
		shortAt:  payloadFileOff,
		shortN:   40,
		shortErr: io.ErrUnexpectedEOF,
	}
	faultChild, err := NewVHDXFileWithParent(fault, parent)
	if err != nil {
		t.Fatalf("fault-wrapped image open failed: %v", err)
	}
	fault.active = true
	n, err = faultChild.ReadAt(make([]byte, 2*lss), chunkEdge-100)
	if err == nil {
		t.Fatal("injected short read not surfaced")
	}
	if n != 40 {
		t.Fatalf("short read accounting wrong: n=%d", n)
	}

	// Zero-progress read on a present block terminates with an error
	// instead of looping forever.
	zeroImg := buildVHDX(&vhdxSpec{
		blockSize: blockSize, lss: lss, virtual: virtual,
		hasParent: true, seq1: 1, seq2: 2,
		dataGuid: childGuid, linkage: parentGuid[:],
		blocks: map[int]*blockPlan{
			1023: {state: PAYLOAD_BLOCK_FULLY_PRESENT, fill: 0xc0},
		},
	})
	zero := &faultReader{inner: zeroImg.file, zeroAt: tbDataOffset}
	zeroChild, err := NewVHDXFileWithParent(zero, parent)
	if err != nil {
		t.Fatalf("zero-progress image open failed: %v", err)
	}
	zero.active = true
	done := make(chan struct{})
	go func() {
		_, _ = zeroChild.ReadAt(make([]byte, lss), 0)
		close(done)
	}()
	select {
	case <-done:
	case <-time.After(5 * time.Second):
		t.Fatal("zero-progress read looped forever")
	}
}

// ---------------------------------------------------------------------
// Test 4: shared-parent isolation and input immutability
// ---------------------------------------------------------------------

func TestVHDXChainIsolation(t *testing.T) {
	const (
		lss       = uint32(512)
		blockSize = uint32(2 * 1024 * 1024)
	)
	virtual := uint64(3 * blockSize)
	parentGuid := guid16{0x81, 0x82}

	parentImg := buildVHDX(&vhdxSpec{
		blockSize: blockSize, lss: lss, virtual: virtual,
		seq1: 1, seq2: 2, dataGuid: parentGuid,
		blocks: map[int]*blockPlan{
			0: {state: PAYLOAD_BLOCK_FULLY_PRESENT, fill: 0x90},
			1: {state: PAYLOAD_BLOCK_FULLY_PRESENT, fill: 0x91},
			2: {state: PAYLOAD_BLOCK_FULLY_PRESENT, fill: 0x92},
		},
	})
	beforeParent := parentImg.file.fingerprint()
	parent := mustOpen(t, parentImg.file)

	childAImg := buildVHDX(&vhdxSpec{
		blockSize: blockSize, lss: lss, virtual: virtual,
		hasParent: true, seq1: 1, seq2: 2,
		dataGuid: guid16{0xa1}, linkage: parentGuid[:],
		blocks: map[int]*blockPlan{
			0: {state: PAYLOAD_BLOCK_FULLY_PRESENT, fill: 0xa0},
			1: {state: PAYLOAD_BLOCK_NOT_PRESENT},
			2: {state: PAYLOAD_BLOCK_ZERO},
		},
	})
	beforeA := childAImg.file.fingerprint()
	childA, err := NewVHDXFileWithParent(childAImg.file, parent)
	if err != nil {
		t.Fatalf("child A open failed: %v", err)
	}

	childBImg := buildVHDX(&vhdxSpec{
		blockSize: blockSize, lss: lss, virtual: virtual,
		hasParent: true, seq1: 1, seq2: 2,
		dataGuid: guid16{0xb1}, linkage: parentGuid[:],
		blocks: map[int]*blockPlan{
			0: {state: PAYLOAD_BLOCK_PARTIALLY_PRESENT, fill: 0xb0,
				sectors: map[uint64]bool{0: true}},
			1: {state: PAYLOAD_BLOCK_FULLY_PRESENT, fill: 0xb1},
			2: {state: PAYLOAD_BLOCK_NOT_PRESENT},
		},
	})
	beforeB := childBImg.file.fingerprint()
	childB, err := NewVHDXFileWithParent(childBImg.file, parent)
	if err != nil {
		t.Fatalf("child B open failed: %v", err)
	}

	// Interleaved reads on two children sharing one parent: routing
	// state must not leak between them.
	var wg sync.WaitGroup
	rounds := 50
	for r := 0; r < rounds; r++ {
		wg.Add(3)
		go func() {
			defer wg.Done()
			if got := readAll(t, childA, 0, 64); !bytes.Equal(got, fillByte(64, 0xa0)) {
				t.Errorf("child A block 0 corrupted: %x", got[:8])
			}
		}()
		go func() {
			defer wg.Done()
			got := readAll(t, childB, 0, int(lss))
			if !bytes.Equal(got, fillByte(int(lss), 0xb0)) {
				t.Errorf("child B owned sector corrupted: %x", got[:8])
			}
			got = readAll(t, childB, int64(lss), int(lss))
			if !bytes.Equal(got, fillByte(int(lss), 0x90)) {
				t.Errorf("child B parent sector corrupted: %x", got[:8])
			}
		}()
		go func() {
			defer wg.Done()
			if got := readAll(t, parent, int64(blockSize), 64); !bytes.Equal(
				got, fillByte(64, 0x91)) {
				t.Errorf("shared parent read corrupted: %x", got[:8])
			}
		}()
	}
	wg.Wait()

	// Cross-check stable expectations sequentially as well.
	if got := readAll(t, childA, int64(blockSize), 64); !bytes.Equal(
		got, fillByte(64, 0x91)) {
		t.Fatal("child A NOT_PRESENT did not reach shared parent")
	}
	if got := readAll(t, childB, int64(blockSize), 64); !bytes.Equal(
		got, fillByte(64, 0xb1)) {
		t.Fatal("child B FULLY_PRESENT block leaked parent data")
	}
	if got := readAll(t, childA, 2*int64(blockSize), 64); !bytes.Equal(
		got, make([]byte, 64)) {
		t.Fatal("child A ZERO block leaked parent data")
	}
	if got := readAll(t, childB, 2*int64(blockSize), 64); !bytes.Equal(
		got, fillByte(64, 0x92)) {
		t.Fatal("child B NOT_PRESENT did not reach shared parent block 2")
	}

	// Input images must be byte-identical after all the reads.
	if fmt.Sprint(beforeParent) != fmt.Sprint(parentImg.file.fingerprint()) {
		t.Fatal("parent image was mutated")
	}
	if fmt.Sprint(beforeA) != fmt.Sprint(childAImg.file.fingerprint()) {
		t.Fatal("child A image was mutated")
	}
	if fmt.Sprint(beforeB) != fmt.Sprint(childBImg.file.fingerprint()) {
		t.Fatal("child B image was mutated")
	}
}
