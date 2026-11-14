package macho

// Offline compact unwind (__TEXT,__unwind_info) decoding for little-endian
// Mach-O64 / x86_64 standalone images. The index is built once from the
// existing NewFile/Section.Data/VM mapping path and then owns every byte it
// needs: Lookup never touches the File, so the File may be closed and the
// underlying bytes released before the index is queried. Nothing here reads a
// process stack or executes personality/LSDA/handler content.

import (
	"encoding/binary"
	"errors"
	"fmt"
	"sort"

	"github.com/blacktop/go-macho/types"
)

// Compact unwind format limits and flag layout (see task_support/FORMAT.md).
const (
	cuVersion1            = uint32(1)
	cuHeaderSize          = 7 * 4
	cuIndexEntrySize      = 3 * 4
	cuPageMaxSize         = 4096
	cuPageKindRegular     = uint32(2)
	cuPageKindCompressed  = uint32(3)
	cuFlagNotFunction     = uint32(0x80000000)
	cuFlagHasLSDA         = uint32(0x40000000)
	cuPersonalityMask     = uint32(0x30000000)
	cuPersonalityShift    = 28
	cuModeMask            = uint32(0x0f000000)
	cuModeShift           = 24
	cuCompressedOffsetMsk = uint32(0x00ffffff)
	cuModeRBPFrame        = uint32(1)
	cuModeStackIMMD       = uint32(2)
	cuModeStackIND        = uint32(3)
	cuModeDWARF           = uint32(4)
)

// ErrCompactUnwindNotFound is returned by Lookup when no unwind range covers
// the requested preferred VM address.
var ErrCompactUnwindNotFound = errors.New("compact unwind: address not covered by any unwind range")

// CompactUnwindIndex is an owned, queryable view of a __TEXT,__unwind_info
// section. All fields are copied out of the file; the index stays valid after
// the source File is closed and its backing bytes released.
type CompactUnwindIndex struct {
	baseAddress   uint64
	sectionStart  uint64 // preferred VM address of the first byte of the section
	levels        []CompactLevel
	entries       []CompactUnwindEntry // range order, every entry including zero encodings
	personalities []CompactPersonality
}

// CompactSource records where a piece of unwind data physically came from.
// Offsets are file offsets unless named as section offsets; the raw bytes are
// copied into the index and remain valid after the File is closed.
type CompactSource struct {
	SectionOffset uint32 // offset relative to the start of __TEXT,__unwind_info
	FileOffset    uint64 // offset relative to the start of the image file
	VMAddress     uint64 // preferred VM address of the first byte
	Raw           []byte // owned copy of the source bytes
}

// CompactEncodingSource identifies which encoding table an entry's encoding
// word came from: the section-global common table, a page-local table, or the
// inline encoding of a regular (kind 2) page.
type CompactEncodingSource struct {
	Kind CompactEncodingKind
	// Index is the table index for Common and Local (local indices are
	// zero-based within the owning page); 0 for Inline.
	Index uint32
	Word  CompactSource // the encoding word itself
}

// CompactEncodingKind says where an encoding word was stored.
type CompactEncodingKind uint8

const (
	CompactEncodingInline CompactEncodingKind = iota
	CompactEncodingCommon
	CompactEncodingLocal
)

func (k CompactEncodingKind) String() string {
	switch k {
	case CompactEncodingInline:
		return "inline"
	case CompactEncodingCommon:
		return "common"
	case CompactEncodingLocal:
		return "local"
	default:
		return fmt.Sprintf("unknown(%d)", uint8(k))
	}
}

// CompactLevel is the first-level index item a range belongs to. The last
// first-level item is a sentinel and never appears as a level.
type CompactLevel struct {
	FunctionOffset         uint32 // image-base relative start of the level's coverage
	LSDAArraySectionOffset uint32 // section offset of this level's LSDA sub-table
	PageSectionOffset      uint32 // section offset of the second-level page
	IndexSectionOffset     uint32 // section offset of this first-level item
}

// CompactUnwindEntry is one half-open unwind range [RangeStart, RangeEnd) in
// preferred VM coordinates. Continuation ranges (IS_NOT_FUNCTION_START) keep
// their own RangeStart while FunctionStart names the logical function's start
// on the same page.
type CompactUnwindEntry struct {
	RangeStart     uint64
	RangeEnd       uint64
	FunctionStart  uint64 // logical function start; equals RangeStart for ordinary entries
	IsContinuation bool

	Level CompactLevel
	Page  CompactPageSource
	Entry CompactSource // the raw on-disk entry bytes

	HasLSDA          bool
	PersonalityIndex uint32 // 1-based personality index; 0 means no personality
	EncodingSource   CompactEncodingSource
	RawEncoding      uint32 // the resolved encoding word, flags and all
	NoUnwind         bool   // encoding word resolves to zero
	UnknownMode      bool   // the x86_64 mode is not one of 1..4

	// Rule is nil for no_unwind, unknown modes, and DWARF fallbacks; those
	// states are carried by NoUnwind, UnknownMode and DWARF instead. Rule
	// construction failures are reported by BuildCompactUnwindIndex, never
	// partially hidden here.
	Rule *CompactUnwindRule

	DWARF *CompactDWARFFallback

	LSDA *CompactLSDARef
}

// CompactPageSource locates the second-level page backing an entry.
type CompactPageSource struct {
	Kind          uint32 // 2 regular, 3 compressed
	SectionOffset uint32 // section offset of the page
	FileOffset    uint64
	VMAddress     uint64
}

// CompactLSDARef is the exact LSDA record matched on a logical FunctionStart.
// The LSDA content is never interpreted.
type CompactLSDARef struct {
	VMAddress           uint64        // preferred VM address of the LSDA
	Record              CompactSource // the (functionOffset, lsdaOffset) table item
	TargetSectionOffset uint32        // lsdaOffset, image-base relative
}

// CompactPersonality is one item of the section personality array. The item is
// a signed image-base delta that points at an indirect pointer slot; neither
// the slot nor its raw pointer are treated as an executable function address.
type CompactPersonality struct {
	// Index is the 1-based index as it appears in an entry's personality field.
	Index int
	// Item locates the signed 32-bit delta inside the personality array.
	Item  CompactSource
	Delta int32
	// Slot is the indirect pointer slot named by the delta.
	SlotVMAddress  uint64
	SlotFileOffset uint64
	Slot           CompactSource // the owned 8 raw slot bytes
	RawPointer     uint64        // unconverted, unauthenticated slot content
}

// CompactUnwindRule is the symbolic x86_64 unwind rule for a RBP_FRAME,
// STACK_IMMD or STACK_IND encoding. Register values are not recovered: only
// the CFA/RIP relationships and the saved-register slots relative to the CFA
// are described.
type CompactUnwindRule struct {
	Mode CompactUnwindMode
	// CFA names the caller's stack pointer. Its expression is symbolic.
	CFA CompactCFA
	// RIP is the saved return address slot; always CFA-8 for these modes.
	RIP CompactSlot
	// SavedRegisters are the callee-saved registers in declared order;
	// frameless permutations are decoded as an ordered list, not bit fields.
	SavedRegisters []CompactSavedRegister
	// StackSize includes the return address and is 8-byte aligned.
	StackSize uint32

	// STACK_IND only: where the frame-size immediate lives.
	Immediate *CompactImmediate
}

// CompactUnwindMode is the x86_64 compact unwind mode of a rule.
type CompactUnwindMode uint8

const (
	CompactModeRBPFrame CompactUnwindMode = iota + 1
	CompactModeStackIMMD
	CompactModeStackIND
)

func (m CompactUnwindMode) String() string {
	switch m {
	case CompactModeRBPFrame:
		return "RBP_FRAME"
	case CompactModeStackIMMD:
		return "STACK_IMMD"
	case CompactModeStackIND:
		return "STACK_IND"
	default:
		return fmt.Sprintf("unknown(%d)", uint8(m))
	}
}

// CompactRegister names a callee-saved x86_64 register in a compact unwind
// save layout.
type CompactRegister string

const (
	CompactRegRBX CompactRegister = "RBX"
	CompactRegR12 CompactRegister = "R12"
	CompactRegR13 CompactRegister = "R13"
	CompactRegR14 CompactRegister = "R14"
	CompactRegR15 CompactRegister = "R15"
	CompactRegRBP CompactRegister = "RBP"
	CompactRegRSP CompactRegister = "RSP"
)

// CompactCFA is the symbolic canonical frame address expression.
type CompactCFA struct {
	Base   CompactRegister // RBP for RBP_FRAME, RSP for the stack modes
	Offset int64           // signed offset from Base (+16 for RBP_FRAME, +StackSize otherwise)
}

// CompactSlot is a stack slot expressed as a signed offset from the CFA.
type CompactSlot struct {
	OffsetFromCFA int64
}

// CompactSavedRegister pairs a saved register with its CFA-relative slot.
type CompactSavedRegister struct {
	Register CompactRegister
	Slot     CompactSlot
}

// CompactImmediate is the STACK_IND frame-size immediate read through the
// File's address mapping. The instruction stream is never scanned or run.
type CompactImmediate struct {
	VMAddress        uint64
	FileOffset       uint64
	Raw              [4]byte
	Value            uint32 // little-endian u32 stored at the immediate location
	Adjustment       uint32 // bits 13..15 of the encoding, times 8
	StackSize        uint32 // Value + Adjustment, checked for overflow
	FunctionRelative uint32 // bits 16..23 of the encoding, relative to FunctionStart
}

// CompactDWARFFallback reports a mode-4 reference into __eh_frame. The CFI is
// not decoded; only the reference location is retained.
type CompactDWARFFallback struct {
	EHFrameSectionOffset uint32 // low 24 bits of the encoding word
	EHFrameFileOffset    uint64
	EHFrameVMAddress     uint64
	Source               CompactSource
}

// compactUnwindError is a malformed-section error that always carries the
// __unwind_info section offset involved and, when one is known, the VM address
// of the entry being processed.
type compactUnwindError struct {
	sectionOffset uint32
	entryVM       uint64
	haveEntryVM   bool
	msg           string
}

func (e *compactUnwindError) Error() string {
	if e.haveEntryVM {
		return fmt.Sprintf("compact unwind: %s (unwind_info section offset %#x, entry VM %#x)", e.msg, e.sectionOffset, e.entryVM)
	}
	return fmt.Sprintf("compact unwind: %s (unwind_info section offset %#x)", e.msg, e.sectionOffset)
}

func cuFail(sectionOffset uint32, format string, args ...any) error {
	return &compactUnwindError{sectionOffset: sectionOffset, msg: fmt.Sprintf(format, args...)}
}

func cuFailEntry(sectionOffset uint32, entryVM uint64, format string, args ...any) error {
	return &compactUnwindError{sectionOffset: sectionOffset, entryVM: entryVM, haveEntryVM: true, msg: fmt.Sprintf(format, args...)}
}

// BuildCompactUnwindIndex parses the image's __TEXT,__unwind_info section into
// an owned, queryable index. A missing section yields an empty index and no
// error. Only little-endian Mach-O64 images for CPU AMD64 are accepted; the
// preferred VM base comes from GetBaseAddress and query addresses are used
// verbatim (no return-address decrement). Any malformed data fails the whole
// build: a half-built index is never returned.
func (f *File) BuildCompactUnwindIndex() (*CompactUnwindIndex, error) {
	if f.Magic != types.Magic64 || f.ByteOrder != binary.LittleEndian || f.CPU != types.CPUAmd64 {
		return nil, fmt.Errorf("compact unwind: only little-endian Mach-O64 AMD64 images are supported (magic %#x, cpu %s)", f.Magic, f.CPU)
	}
	// Only ordinary standalone images are analyzed; a fileset container hands
	// out its member images through GetFileSetFileByName instead.
	if f.Type == types.MH_FILESET {
		return nil, fmt.Errorf("compact unwind: MH_FILESET containers are not standalone images; open the member via GetFileSetFileByName")
	}
	sec := f.Section("__TEXT", "__unwind_info")
	if sec == nil {
		return &CompactUnwindIndex{baseAddress: f.GetBaseAddress()}, nil
	}
	data, err := sec.Data()
	if err != nil {
		return nil, cuFail(0, "reading __TEXT,__unwind_info section data: %v", err)
	}
	base := f.GetBaseAddress()
	b := &cuBuilder{
		f:       f,
		sec:     sec,
		data:    data,
		base:    base,
		secVM:   sec.Addr,
		secFile: uint64(sec.Offset),
	}
	return b.build()
}

type cuBuilder struct {
	f       *File
	sec     *types.Section
	data    []byte
	base    uint64
	secVM   uint64
	secFile uint64

	common []uint32
	index  *CompactUnwindIndex
}

// u32 reads a little-endian u32 at a section offset, bounds-checked against the
// section itself; no zero filling is ever performed.
func (b *cuBuilder) u32(sectionOffset uint32) (uint32, error) {
	if uint64(sectionOffset) > uint64(len(b.data))-4 {
		return 0, cuFail(sectionOffset, "u32 at section offset %#x needs 4 bytes but section size is %d", sectionOffset, len(b.data))
	}
	return binary.LittleEndian.Uint32(b.data[sectionOffset:]), nil
}

func (b *cuBuilder) u16(sectionOffset uint32) (uint16, error) {
	if uint64(sectionOffset) > uint64(len(b.data))-2 {
		return 0, cuFail(sectionOffset, "u16 at section offset %#x needs 2 bytes but section size is %d", sectionOffset, len(b.data))
	}
	return binary.LittleEndian.Uint16(b.data[sectionOffset:]), nil
}

// source copies len bytes at a section offset into an owned CompactSource.
func (b *cuBuilder) source(sectionOffset uint32, n int) (CompactSource, error) {
	end, ok := addU32(sectionOffset, uint32(n))
	if !ok || uint64(end) > uint64(len(b.data)) {
		return CompactSource{}, cuFail(sectionOffset, "source range [%#x,%#x) extends past section size %d", sectionOffset, end, len(b.data))
	}
	raw := make([]byte, n)
	copy(raw, b.data[sectionOffset:end])
	fileOff, err := b.f.GetOffset(b.secVM + uint64(sectionOffset))
	if err != nil {
		return CompactSource{}, cuFail(sectionOffset, "mapping source to file offset: %v", err)
	}
	return CompactSource{
		SectionOffset: sectionOffset,
		FileOffset:    fileOff,
		VMAddress:     b.secVM + uint64(sectionOffset),
		Raw:           raw,
	}, nil
}

func addU32(a, b uint32) (uint32, bool) {
	if a > ^uint32(0)-b {
		return 0, false
	}
	return a + b, true
}

func addU64(a, b uint64) (uint64, bool) {
	if a > ^uint64(0)-b {
		return 0, false
	}
	return a + b, true
}

// addSignedU32 adds a signed image-base delta without unsigned wraparound.
func addSignedU32(base uint64, delta int32) (uint64, bool) {
	if delta >= 0 {
		return addU64(base, uint64(delta))
	}
	d := uint64(-delta)
	if d > base {
		return 0, false
	}
	return base - d, true
}

func (b *cuBuilder) build() (*CompactUnwindIndex, error) {
	if len(b.data) < cuHeaderSize {
		return nil, cuFail(0, "truncated version-1 header: section is %d bytes, need at least %d", len(b.data), cuHeaderSize)
	}
	version := binary.LittleEndian.Uint32(b.data[0:])
	if version != cuVersion1 {
		return nil, cuFail(0, "unsupported unwind_info version %d (only version 1 is supported)", version)
	}
	commonOff := binary.LittleEndian.Uint32(b.data[4:])
	commonCount := binary.LittleEndian.Uint32(b.data[8:])
	persOff := binary.LittleEndian.Uint32(b.data[12:])
	persCount := binary.LittleEndian.Uint32(b.data[16:])
	indexOff := binary.LittleEndian.Uint32(b.data[20:])
	indexCount := binary.LittleEndian.Uint32(b.data[24:])

	idx := &CompactUnwindIndex{baseAddress: b.base, sectionStart: b.secVM}
	b.index = idx

	if err := b.readCommon(commonOff, commonCount); err != nil {
		return nil, err
	}
	if err := b.readPersonalities(persOff, persCount); err != nil {
		return nil, err
	}
	if err := b.readLevels(indexOff, indexCount); err != nil {
		return nil, err
	}
	return idx, nil
}

func (b *cuBuilder) readCommon(off, count uint32) error {
	if count == 0 {
		if off != 0 {
			return cuFail(4, "common encodings count is zero but offset is %#x", off)
		}
		return nil
	}
	if count > ^uint32(0)/4 {
		return cuFail(8, "common encoding count %#x overflows the array size", count)
	}
	end, ok := addU32(off, count*4)
	if !ok || uint64(end) > uint64(len(b.data)) {
		return cuFail(off, "common encoding array [%#x,%#x) extends past section size %d", off, end, len(b.data))
	}
	b.common = make([]uint32, count)
	for i := range b.common {
		b.common[i] = binary.LittleEndian.Uint32(b.data[off+uint32(i)*4:])
	}
	return nil
}

func (b *cuBuilder) readPersonalities(off, count uint32) error {
	if count == 0 {
		return nil
	}
	if count > ^uint32(0)/4 {
		return cuFail(16, "personality count %#x overflows the array size", count)
	}
	end, ok := addU32(off, count*4)
	if !ok || uint64(end) > uint64(len(b.data)) {
		return cuFail(off, "personality array [%#x,%#x) extends past section size %d", off, end, len(b.data))
	}
	out := make([]CompactPersonality, 0, count)
	for i := uint32(0); i < count; i++ {
		itemOff := off + i*4
		src, err := b.source(itemOff, 4)
		if err != nil {
			return err
		}
		delta := int32(binary.LittleEndian.Uint32(b.data[itemOff:]))
		slotVM, ok := addSignedU32(b.base, delta)
		if !ok {
			return cuFail(itemOff, "personality slot VM overflows")
		}
		if !b.f.hasFileData(slotVM, 8) {
			return cuFail(itemOff, "personality slot at VM %#x is not fully backed by file data", slotVM)
		}
		slotFile, err := b.f.GetOffset(slotVM)
		if err != nil {
			return cuFail(itemOff, "mapping personality slot VM %#x: %v", slotVM, err)
		}
		var raw [8]byte
		n, err := b.f.ReadAtAddr(raw[:], slotVM)
		if err != nil || n != 8 {
			return cuFail(itemOff, "reading 8-byte personality slot at VM %#x: %v", slotVM, err)
		}
		out = append(out, CompactPersonality{
			Index:          int(i + 1),
			Item:           src,
			Delta:          delta,
			SlotVMAddress:  slotVM,
			SlotFileOffset: slotFile,
			Slot: CompactSource{
				FileOffset: slotFile,
				VMAddress:  slotVM,
				Raw:        append([]byte(nil), raw[:]...),
			},
			RawPointer: binary.LittleEndian.Uint64(raw[:]),
		})
	}
	b.index.personalities = out
	return nil
}

func (b *cuBuilder) readLevels(indexOff, indexCount uint32) error {
	if indexCount < 1 {
		return cuFail(20, "first-level index must contain at least the sentinel item")
	}
	if indexCount > ^uint32(0)/cuIndexEntrySize {
		return cuFail(24, "first-level index count %#x overflows the array size", indexCount)
	}
	end, ok := addU32(indexOff, indexCount*cuIndexEntrySize)
	if !ok || uint64(end) > uint64(len(b.data)) {
		return cuFail(indexOff, "first-level index [%#x,%#x) extends past section size %d", indexOff, end, len(b.data))
	}
	type rawLevel struct {
		functionOff uint32
		pageOff     uint32
		lsdaOff     uint32
		itemOff     uint32
	}
	raw := make([]rawLevel, indexCount)
	for i := range raw {
		itemOff := indexOff + uint32(i)*cuIndexEntrySize
		functionOff, err := b.u32(itemOff)
		if err != nil {
			return err
		}
		pageOff, err := b.u32(itemOff + 4)
		if err != nil {
			return err
		}
		lsdaOff, err := b.u32(itemOff + 8)
		if err != nil {
			return err
		}
		raw[i] = rawLevel{functionOff: functionOff, pageOff: pageOff, lsdaOff: lsdaOff, itemOff: itemOff}
	}
	// The final item is the sentinel: zero page offset, only the end function
	// offset and LSDA table tail. Everything else must name a real page.
	sentinel := raw[len(raw)-1]
	if sentinel.pageOff != 0 {
		return cuFail(sentinel.itemOff+4, "last first-level item is not a sentinel (page offset %#x)", sentinel.pageOff)
	}
	for i := 0; i < len(raw)-1; i++ {
		if raw[i].pageOff == 0 {
			return cuFail(raw[i].itemOff+4, "non-sentinel first-level item has zero page offset")
		}
		if raw[i].functionOff >= raw[i+1].functionOff {
			return cuFail(raw[i].itemOff, "first-level function offsets are not strictly increasing (%#x then %#x)", raw[i].functionOff, raw[i+1].functionOff)
		}
	}
	// Empty tables may contain only the sentinel.
	for i := 0; i < len(raw)-1; i++ {
		level := CompactLevel{
			FunctionOffset:         raw[i].functionOff,
			LSDAArraySectionOffset: raw[i].lsdaOff,
			PageSectionOffset:      raw[i].pageOff,
			IndexSectionOffset:     raw[i].itemOff,
		}
		b.index.levels = append(b.index.levels, level)
		if err := b.readPage(level, raw[i+1].functionOff, raw[i+1].lsdaOff); err != nil {
			return err
		}
	}
	return nil
}

// lsdaRecord is a parsed item of a level's LSDA sub-table.
type lsdaRecord struct {
	functionOff uint32
	lsdaOff     uint32
	src         CompactSource
}

func (b *cuBuilder) readLSDA(level CompactLevel, nextLSDAOff uint32) ([]lsdaRecord, error) {
	lo, hi := level.LSDAArraySectionOffset, nextLSDAOff
	if hi < lo {
		return nil, cuFail(level.IndexSectionOffset+8, "LSDA sub-table end %#x precedes start %#x", hi, lo)
	}
	if (hi-lo)%8 != 0 {
		return nil, cuFail(lo, "LSDA sub-table size %d is not a multiple of 8", hi-lo)
	}
	count := (hi - lo) / 8
	if uint64(hi) > uint64(len(b.data)) {
		return nil, cuFail(lo, "LSDA sub-table [%#x,%#x) extends past section size %d", lo, hi, len(b.data))
	}
	out := make([]lsdaRecord, 0, count)
	var last uint32
	for i := uint32(0); i < count; i++ {
		itemOff := lo + i*8
		functionOff := binary.LittleEndian.Uint32(b.data[itemOff:])
		lsdaOff := binary.LittleEndian.Uint32(b.data[itemOff+4:])
		if i > 0 && functionOff <= last {
			return nil, cuFail(itemOff, "LSDA records are not sorted by function offset")
		}
		last = functionOff
		src, err := b.source(itemOff, 8)
		if err != nil {
			return nil, err
		}
		out = append(out, lsdaRecord{functionOff: functionOff, lsdaOff: lsdaOff, src: src})
	}
	return out, nil
}

// pageEntry is the level-independent shape extracted from either page kind.
type pageEntry struct {
	functionOff  uint32 // image-base relative range start (continuation flag stripped)
	continuation bool
	encoding     uint32 // resolved encoding word
	encSrc       CompactEncodingSource
	entrySrc     CompactSource
	entryItemOff uint32 // section offset of the entry word(s)
}

func (b *cuBuilder) readPage(level CompactLevel, endFunctionOff, nextLSDAOff uint32) error {
	lsda, err := b.readLSDA(level, nextLSDAOff)
	if err != nil {
		return err
	}
	pageOff := level.PageSectionOffset
	if uint64(pageOff) > uint64(len(b.data))-4 {
		return cuFail(pageOff, "second-level page header extends past section size %d", len(b.data))
	}
	kind := binary.LittleEndian.Uint32(b.data[pageOff:])
	pageFile, err := b.f.GetOffset(b.secVM + uint64(pageOff))
	if err != nil {
		return cuFail(pageOff, "mapping page to file offset: %v", err)
	}
	pageSrc := CompactPageSource{
		Kind:          kind,
		SectionOffset: pageOff,
		FileOffset:    pageFile,
		VMAddress:     b.secVM + uint64(pageOff),
	}
	switch kind {
	case cuPageKindRegular:
		err = b.readRegularPage(level, pageSrc, endFunctionOff, lsda)
	case cuPageKindCompressed:
		err = b.readCompressedPage(level, pageSrc, endFunctionOff, lsda)
	default:
		err = cuFail(pageOff, "unknown second-level page kind %d (want 2 or 3)", kind)
	}
	return err
}

// checkPageArray verifies an array declared by a page stays within the page's
// own 4096-byte window and within the section. Pages never borrow bytes from a
// neighboring page.
func (b *cuBuilder) checkPageArray(pageOff, arrayPageOff uint32, bytes int) error {
	start, ok := addU32(pageOff, arrayPageOff)
	if !ok {
		return cuFail(pageOff, "page array offset %#x overflows", arrayPageOff)
	}
	if arrayPageOff+uint32(bytes) > cuPageMaxSize {
		return cuFail(start, "page array extends past the 4096-byte page window")
	}
	end, ok := addU32(start, uint32(bytes))
	if !ok || uint64(end) > uint64(len(b.data)) {
		return cuFail(start, "page array [%#x,%#x) extends past section size %d", start, end, len(b.data))
	}
	return nil
}

func (b *cuBuilder) readRegularPage(level CompactLevel, page CompactPageSource, endFunctionOff uint32, lsda []lsdaRecord) error {
	pageOff := page.SectionOffset
	if err := b.checkPageArray(pageOff, 0, 8); err != nil {
		return err
	}
	entryPageOff, err := b.u16(pageOff + 4)
	if err != nil {
		return err
	}
	entryCount, err := b.u16(pageOff + 6)
	if err != nil {
		return err
	}
	if entryCount == 0 {
		return cuFail(pageOff+6, "second-level page declares zero entries")
	}
	arrayBytes := int(entryCount) * 8
	if err := b.checkPageArray(pageOff, uint32(entryPageOff), arrayBytes); err != nil {
		return err
	}
	entries := make([]pageEntry, 0, entryCount)
	var lastOff uint32
	for i := uint16(0); i < entryCount; i++ {
		itemPageOff := uint32(entryPageOff) + uint32(i)*8
		itemOff := pageOff + itemPageOff
		functionOff, err := b.u32(itemOff)
		if err != nil {
			return err
		}
		inlineEncoding, err := b.u32(itemOff + 4)
		if err != nil {
			return err
		}
		wordSrc, err := b.source(itemOff+4, 4)
		if err != nil {
			return err
		}
		entrySrc, err := b.source(itemOff, 8)
		if err != nil {
			return err
		}
		continuation := functionOff&cuFlagNotFunction != 0
		rawOff := functionOff &^ cuFlagNotFunction
		if i == 0 {
			if continuation {
				return cuFail(itemOff, "first page entry is an IS_NOT_FUNCTION_START continuation with no parent")
			}
			if rawOff != level.FunctionOffset {
				return cuFail(itemOff, "first page entry offset %#x does not match first-level start %#x", rawOff, level.FunctionOffset)
			}
		} else {
			if rawOff <= lastOff {
				return cuFail(itemOff, "page entry offsets are not strictly increasing (%#x after %#x)", rawOff, lastOff)
			}
		}
		lastOff = rawOff
		// Regular pages carry the full encoding word inline.
		entries = append(entries, pageEntry{
			functionOff:  rawOff,
			continuation: continuation,
			encoding:     inlineEncoding,
			encSrc:       CompactEncodingSource{Kind: CompactEncodingInline, Word: wordSrc},
			entrySrc:     entrySrc,
			entryItemOff: itemOff,
		})
	}
	return b.materialize(level, page, entries, endFunctionOff, lsda)
}

func (b *cuBuilder) readCompressedPage(level CompactLevel, page CompactPageSource, endFunctionOff uint32, lsda []lsdaRecord) error {
	pageOff := page.SectionOffset
	if err := b.checkPageArray(pageOff, 0, 12); err != nil {
		return err
	}
	entryPageOff, err := b.u16(pageOff + 4)
	if err != nil {
		return err
	}
	entryCount, err := b.u16(pageOff + 6)
	if err != nil {
		return err
	}
	encPageOff, err := b.u16(pageOff + 8)
	if err != nil {
		return err
	}
	encCount, err := b.u16(pageOff + 10)
	if err != nil {
		return err
	}
	if entryCount == 0 {
		return cuFail(pageOff+6, "second-level page declares zero entries")
	}
	locals := make([]uint32, encCount)
	if encCount > 0 {
		if err := b.checkPageArray(pageOff, uint32(encPageOff), int(encCount)*4); err != nil {
			return err
		}
		for i := range locals {
			off := pageOff + uint32(encPageOff) + uint32(i)*4
			locals[i], err = b.u32(off)
			if err != nil {
				return err
			}
		}
	}
	if err := b.checkPageArray(pageOff, uint32(entryPageOff), int(entryCount)*4); err != nil {
		return err
	}
	entries := make([]pageEntry, 0, entryCount)
	var lastOff uint32
	for i := uint16(0); i < entryCount; i++ {
		itemOff := pageOff + uint32(entryPageOff) + uint32(i)*4
		word, err := b.u32(itemOff)
		if err != nil {
			return err
		}
		entrySrc, err := b.source(itemOff, 4)
		if err != nil {
			return err
		}
		continuation := word&cuFlagNotFunction != 0
		delta := word & cuCompressedOffsetMsk
		// The encoding index shares the high byte with IS_NOT_FUNCTION_START;
		// strip that flag before indexing either encoding table.
		encIndex := (word >> 24) & 0x7f
		functionOff, ok := addU32(level.FunctionOffset, delta)
		if !ok {
			return cuFail(itemOff, "compressed function offset %#x + delta %#x overflows", level.FunctionOffset, delta)
		}
		if i == 0 {
			if continuation {
				return cuFail(itemOff, "first page entry is an IS_NOT_FUNCTION_START continuation with no parent")
			}
			if delta != 0 {
				return cuFail(itemOff, "first compressed entry must start at the first-level function offset (delta %#x)", delta)
			}
		} else {
			if functionOff <= lastOff {
				return cuFail(itemOff, "compressed page entry offsets are not strictly increasing (%#x after %#x)", functionOff, lastOff)
			}
		}
		lastOff = functionOff

		// Encoding indices select the global common table first; only indices
		// past the common count reach into the page-local table.
		var encoding uint32
		var encSrc CompactEncodingSource
		if encIndex < uint32(len(b.common)) {
			encoding = b.common[encIndex]
			wordSrc, srcErr := b.source(itemOff, 4)
			if srcErr != nil {
				return srcErr
			}
			encSrc = CompactEncodingSource{Kind: CompactEncodingCommon, Index: encIndex, Word: wordSrc}
		} else {
			localIndex := encIndex - uint32(len(b.common))
			if localIndex >= uint32(len(locals)) {
				return cuFail(itemOff, "encoding index %d out of range (common %d, local %d)", encIndex, len(b.common), len(locals))
			}
			encoding = locals[localIndex]
			encWordOff := pageOff + uint32(encPageOff) + localIndex*4
			wordSrc, srcErr := b.source(encWordOff, 4)
			if srcErr != nil {
				return srcErr
			}
			encSrc = CompactEncodingSource{Kind: CompactEncodingLocal, Index: localIndex, Word: wordSrc}
		}
		entries = append(entries, pageEntry{
			functionOff:  functionOff,
			continuation: continuation,
			encoding:     encoding,
			encSrc:       encSrc,
			entrySrc:     entrySrc,
			entryItemOff: itemOff,
		})
	}
	return b.materialize(level, page, entries, endFunctionOff, lsda)
}

// materialize turns parsed page entries into owned ranges, resolving
// continuations, LSDA matches, personalities and symbolic rules. The page's
// final range is closed by the next first-level function offset, never by the
// physical position of another page.
func (b *cuBuilder) materialize(level CompactLevel, page CompactPageSource, entries []pageEntry, endFunctionOff uint32, lsda []lsdaRecord) error {
	if len(entries) == 0 {
		return cuFail(page.SectionOffset, "internal: empty page reached materialize")
	}
	// lsdaIndex maps a logical function offset to its exact LSDA record.
	lsdaIndex := make(map[uint32]lsdaRecord, len(lsda))
	for _, rec := range lsda {
		lsdaIndex[rec.functionOff] = rec
	}
	var logicalStart uint32
	out := b.index.entries
	for i, pe := range entries {
		if !pe.continuation {
			logicalStart = pe.functionOff
		}
		rangeEndOff := endFunctionOff
		if i+1 < len(entries) {
			rangeEndOff = entries[i+1].functionOff
		}
		if pe.functionOff >= rangeEndOff {
			return cuFail(pe.entryItemOff, "entry offset %#x is not before its range end %#x", pe.functionOff, rangeEndOff)
		}
		entryVM, ok := addU64(b.base, uint64(pe.functionOff))
		if !ok {
			return cuFail(pe.entryItemOff, "entry VM overflows")
		}
		functionVM, ok := addU64(b.base, uint64(logicalStart))
		if !ok {
			return cuFail(pe.entryItemOff, "function start VM overflows")
		}
		endVM, ok := addU64(b.base, uint64(rangeEndOff))
		if !ok {
			return cuFail(pe.entryItemOff, "range end VM overflows")
		}
		ce := CompactUnwindEntry{
			RangeStart:       entryVM,
			RangeEnd:         endVM,
			FunctionStart:    functionVM,
			IsContinuation:   pe.continuation,
			Level:            level,
			Page:             page,
			Entry:            pe.entrySrc,
			RawEncoding:      pe.encoding,
			EncodingSource:   pe.encSrc,
			HasLSDA:          pe.encoding&cuFlagHasLSDA != 0,
			PersonalityIndex: (pe.encoding & cuPersonalityMask) >> cuPersonalityShift,
		}
		if ce.PersonalityIndex > uint32(len(b.index.personalities)) {
			return cuFailEntry(pe.entryItemOff, entryVM, "personality index %d out of range (array has %d entries)", ce.PersonalityIndex, len(b.index.personalities))
		}
		if ce.HasLSDA {
			rec, found := lsdaIndex[logicalStart]
			if !found {
				return cuFailEntry(pe.entryItemOff, entryVM, "HAS_LSDA set but no exact LSDA record for logical function start %#x", logicalStart)
			}
			lsdaVM, ok := addU64(b.base, uint64(rec.lsdaOff))
			if !ok {
				return cuFail(rec.src.SectionOffset, "LSDA VM overflows")
			}
			ce.LSDA = &CompactLSDARef{
				VMAddress:           lsdaVM,
				Record:              rec.src,
				TargetSectionOffset: rec.lsdaOff,
			}
		}
		if pe.encoding == 0 {
			ce.NoUnwind = true
		} else {
			mode := (pe.encoding & cuModeMask) >> cuModeShift
			switch mode {
			case cuModeRBPFrame, cuModeStackIMMD, cuModeStackIND:
				rule, err := b.buildRule(mode, pe.encoding, functionVM, pe.entryItemOff, entryVM)
				if err != nil {
					return err
				}
				ce.Rule = rule
			case cuModeDWARF:
				fb, err := b.buildDWARF(pe.encoding, pe.entryItemOff, entryVM)
				if err != nil {
					return err
				}
				ce.DWARF = fb
			default:
				ce.UnknownMode = true
			}
		}
		out = append(out, ce)
	}
	b.index.entries = out
	return nil
}

// framelessRegisters is the public compact-unwind permutation universe for
// STACK_IMMD/STACK_IND, in permutation-code order.
var framelessRegisters = []CompactRegister{
	CompactRegRBX, CompactRegR12, CompactRegR13, CompactRegR14, CompactRegR15, CompactRegRBP,
}

// rbpFrameRegisters maps RBP_FRAME slot codes 1..5; slot code 0 means empty.
var rbpFrameRegisters = map[uint32]CompactRegister{
	1: CompactRegRBX,
	2: CompactRegR12,
	3: CompactRegR13,
	4: CompactRegR14,
	5: CompactRegR15,
}

// decodeFramelessPermutation decodes the 10-bit permutation code used by the
// frameless encodings into the ordered register list. The encoder writes
// result = result*(6-i) + index over a shrinking six-register universe
// (bases 6, 5, 4, 3, 2, 1), so the code is a mixed-radix Lehmer number and
// cannot be read as raw register numbers.
func decodeFramelessPermutation(code uint32, count uint32) ([]CompactRegister, error) {
	if count > uint32(len(framelessRegisters)) {
		return nil, fmt.Errorf("frameless register count %d exceeds %d", count, len(framelessRegisters))
	}
	// Peel digits off in reverse: the last pick used the smallest radix.
	picks := make([]uint32, count)
	c := code
	for i := int(count) - 1; i >= 0; i-- {
		radix := uint32(6 - i)
		picks[i] = c % radix
		c /= radix
	}
	if c != 0 {
		return nil, fmt.Errorf("frameless permutation code %#x has trailing bits for count %d", code, count)
	}
	remaining := append([]CompactRegister(nil), framelessRegisters...)
	out := make([]CompactRegister, 0, count)
	for _, pick := range picks {
		if pick >= uint32(len(remaining)) {
			return nil, fmt.Errorf("frameless permutation code %#x selects missing register", code)
		}
		out = append(out, remaining[pick])
		remaining = append(remaining[:pick], remaining[pick+1:]...)
	}
	return out, nil
}

func (b *cuBuilder) buildRule(mode, encoding uint32, functionVM uint64, itemOff uint32, entryVM uint64) (*CompactUnwindRule, error) {
	switch mode {
	case cuModeRBPFrame:
		return b.buildRBPRule(encoding, itemOff, entryVM)
	case cuModeStackIMMD:
		return b.buildIMMDRule(encoding, itemOff, entryVM)
	case cuModeStackIND:
		return b.buildINDRule(encoding, functionVM, itemOff, entryVM)
	default:
		return nil, cuFailEntry(itemOff, entryVM, "internal: mode %d is not a symbolic rule", mode)
	}
}

func (b *cuBuilder) buildRBPRule(encoding uint32, itemOff uint32, entryVM uint64) (*CompactUnwindRule, error) {
	offset := (encoding >> 16) & 0xff
	rule := &CompactUnwindRule{
		Mode:      CompactModeRBPFrame,
		CFA:       CompactCFA{Base: CompactRegRBP, Offset: 16},
		RIP:       CompactSlot{OffsetFromCFA: -8},
		StackSize: 0, // RBP-frame encodings carry no total frame size
	}
	seen := make(map[CompactRegister]bool, 5)
	// Five 3-bit slots, low 15 bits; slot 0 is empty but still occupies a
	// position. Register i lives at RBP-8*(offset+i), growing upward.
	for i := uint32(0); i < 5; i++ {
		code := (encoding >> (i * 3)) & 0x7
		if code == 0 {
			continue
		}
		reg, ok := rbpFrameRegisters[code]
		if !ok {
			return nil, cuFailEntry(itemOff, entryVM, "RBP_FRAME slot %d uses reserved register code %d", i, code)
		}
		if seen[reg] {
			return nil, cuFailEntry(itemOff, entryVM, "RBP_FRAME saves %s more than once", reg)
		}
		seen[reg] = true
		words, ok := addU32(offset, i)
		if !ok {
			return nil, cuFailEntry(itemOff, entryVM, "RBP_FRAME slot offset overflows")
		}
		if words == 0 {
			return nil, cuFailEntry(itemOff, entryVM, "RBP_FRAME slot for %s overlaps the fixed saved-RBP slot at CFA-16", reg)
		}
		// RBP-8*words, and RBP = CFA-16, so the slot is CFA-16-8*words.
		rule.SavedRegisters = append(rule.SavedRegisters, CompactSavedRegister{
			Register: reg,
			Slot:     CompactSlot{OffsetFromCFA: -16 - int64(words)*8},
		})
	}
	return rule, nil
}

func (b *cuBuilder) buildIMMDRule(encoding uint32, itemOff uint32, entryVM uint64) (*CompactUnwindRule, error) {
	count := (encoding >> 10) & 0x7
	perm := encoding & 0x3ff
	regs, err := decodeFramelessPermutation(perm, count)
	if err != nil {
		return nil, cuFailEntry(itemOff, entryVM, "invalid STACK_IMMD register layout: %v", err)
	}
	stackSize := ((encoding >> 16) & 0xff) * 8
	if stackSize < 8 || stackSize%8 != 0 {
		return nil, cuFailEntry(itemOff, entryVM, "STACK_IMMD stack size %d is smaller than the return address or misaligned", stackSize)
	}
	return b.framelessRule(CompactModeStackIMMD, regs, stackSize, nil, itemOff, entryVM)
}

func (b *cuBuilder) buildINDRule(encoding uint32, functionVM uint64, itemOff uint32, entryVM uint64) (*CompactUnwindRule, error) {
	count := (encoding >> 10) & 0x7
	perm := encoding & 0x3ff
	regs, err := decodeFramelessPermutation(perm, count)
	if err != nil {
		return nil, cuFailEntry(itemOff, entryVM, "invalid STACK_IND register layout: %v", err)
	}
	functionRel := (encoding >> 16) & 0xff
	adjust := ((encoding >> 13) & 0x7) * 8
	immVM, ok := addU64(functionVM, uint64(functionRel))
	if !ok {
		return nil, cuFailEntry(itemOff, entryVM, "STACK_IND immediate VM overflows")
	}
	// The immediate is read through the File's existing address mapping and
	// must be fully file-backed: no zero filling, no borrowing from another
	// section, no instruction scanning.
	if !b.f.hasFileData(immVM, 4) {
		return nil, cuFailEntry(itemOff, entryVM, "STACK_IND immediate at VM %#x is not fully backed by file data", immVM)
	}
	immFile, err := b.f.GetOffset(immVM)
	if err != nil {
		return nil, cuFailEntry(itemOff, entryVM, "mapping STACK_IND immediate VM %#x: %v", immVM, err)
	}
	var raw [4]byte
	n, err := b.f.ReadAtAddr(raw[:], immVM)
	if err != nil || n != 4 {
		return nil, cuFailEntry(itemOff, entryVM, "reading STACK_IND immediate at VM %#x: %v", immVM, err)
	}
	immValue := binary.LittleEndian.Uint32(raw[:])
	stackSize, ok := addU32(immValue, adjust)
	if !ok {
		return nil, cuFailEntry(itemOff, entryVM, "STACK_IND stack size %#x + adjustment %#x overflows", immValue, adjust)
	}
	if stackSize < 8 || stackSize%8 != 0 {
		return nil, cuFailEntry(itemOff, entryVM, "STACK_IND stack size %d is smaller than the return address or misaligned", stackSize)
	}
	imm := &CompactImmediate{
		VMAddress:        immVM,
		FileOffset:       immFile,
		Raw:              raw,
		Value:            immValue,
		Adjustment:       adjust,
		StackSize:        stackSize,
		FunctionRelative: functionRel,
	}
	return b.framelessRule(CompactModeStackIND, regs, stackSize, imm, itemOff, entryVM)
}

func (b *cuBuilder) framelessRule(mode CompactUnwindMode, regs []CompactRegister, stackSize uint32, imm *CompactImmediate, itemOff uint32, entryVM uint64) (*CompactUnwindRule, error) {
	// n registers occupy n consecutive 8-byte slots starting at CFA-8-8*n;
	// they must not collide with the return slot at CFA-8.
	need := 8 + uint32(len(regs))*8
	if stackSize < need {
		return nil, cuFailEntry(itemOff, entryVM, "%s stack size %d cannot hold return address plus %d saved registers (need %d)", mode, stackSize, len(regs), need)
	}
	rule := &CompactUnwindRule{
		Mode:      mode,
		CFA:       CompactCFA{Base: CompactRegRSP, Offset: int64(stackSize)},
		RIP:       CompactSlot{OffsetFromCFA: -8},
		StackSize: stackSize,
		Immediate: imm,
	}
	seen := make(map[CompactRegister]bool, len(regs))
	n := uint32(len(regs))
	for i, reg := range regs {
		if seen[reg] {
			return nil, cuFailEntry(itemOff, entryVM, "%s saves %s more than once", mode, reg)
		}
		seen[reg] = true
		rule.SavedRegisters = append(rule.SavedRegisters, CompactSavedRegister{
			Register: reg,
			Slot:     CompactSlot{OffsetFromCFA: -int64(8 + (n-uint32(i))*8)},
		})
	}
	return rule, nil
}

func (b *cuBuilder) buildDWARF(encoding uint32, itemOff uint32, entryVM uint64) (*CompactDWARFFallback, error) {
	eh := b.f.Section("__TEXT", "__eh_frame")
	if eh == nil {
		return nil, cuFailEntry(itemOff, entryVM, "DWARF fallback references __eh_frame but the section is absent")
	}
	fdeOff := encoding & cuCompressedOffsetMsk
	if uint64(fdeOff) > eh.Size || eh.Size-uint64(fdeOff) < 4 {
		return nil, cuFailEntry(itemOff, entryVM, "DWARF FDE offset %#x is not readable in __eh_frame (size %#x)", fdeOff, eh.Size)
	}
	fdeVM, ok := addU64(eh.Addr, uint64(fdeOff))
	if !ok {
		return nil, cuFailEntry(itemOff, entryVM, "DWARF FDE VM overflows")
	}
	fdeFile, err := b.f.GetOffset(fdeVM)
	if err != nil {
		return nil, cuFailEntry(itemOff, entryVM, "mapping DWARF FDE VM %#x: %v", fdeVM, err)
	}
	src, err := b.source(itemOff, 4)
	if err != nil {
		return nil, err
	}
	return &CompactDWARFFallback{
		EHFrameSectionOffset: fdeOff,
		EHFrameFileOffset:    fdeFile,
		EHFrameVMAddress:     fdeVM,
		Source:               src,
	}, nil
}

// Lookup returns the unwind range covering addr, a preferred VM address used
// verbatim without return-address adjustment. The search is index-directed
// (binary search over first-level starts then the level's owned ranges); it
// never re-reads the file or scans every entry. ErrCompactUnwindNotFound is
// returned when no range covers the address.
func (idx *CompactUnwindIndex) Lookup(addr uint64) (*CompactUnwindEntry, error) {
	if idx == nil || len(idx.entries) == 0 || addr < idx.baseAddress {
		return nil, ErrCompactUnwindNotFound
	}
	// First-level selection by the level's image-base-relative start.
	rel := addr - idx.baseAddress
	if rel > uint64(^uint32(0)) {
		return nil, ErrCompactUnwindNotFound
	}
	levelPos := sort.Search(len(idx.levels), func(i int) bool {
		return idx.levels[i].FunctionOffset > uint32(rel)
	}) - 1
	if levelPos < 0 {
		return nil, ErrCompactUnwindNotFound
	}
	level := idx.levels[levelPos]
	// Entries are stored in range order; find this level's slice boundaries
	// through the level marks carried on each entry.
	lo := sort.Search(len(idx.entries), func(i int) bool {
		return idx.entries[i].Level.IndexSectionOffset >= level.IndexSectionOffset
	})
	hi := sort.Search(len(idx.entries), func(i int) bool {
		return idx.entries[i].Level.IndexSectionOffset > level.IndexSectionOffset
	})
	page := idx.entries[lo:hi]
	pos := sort.Search(len(page), func(i int) bool {
		return page[i].RangeStart > addr
	}) - 1
	if pos < 0 {
		return nil, ErrCompactUnwindNotFound
	}
	ent := &page[pos]
	if addr >= ent.RangeEnd {
		return nil, ErrCompactUnwindNotFound
	}
	return ent, nil
}

// Entries returns the index's owned unwind entries in range order. The slice
// is a copy; callers may retain it after the index itself is dropped.
func (idx *CompactUnwindIndex) Entries() []CompactUnwindEntry {
	out := make([]CompactUnwindEntry, len(idx.entries))
	copy(out, idx.entries)
	return out
}

// Personalities returns the owned personality array entries.
func (idx *CompactUnwindIndex) Personalities() []CompactPersonality {
	out := make([]CompactPersonality, len(idx.personalities))
	copy(out, idx.personalities)
	return out
}

// BaseAddress reports the preferred image base used to build the index.
func (idx *CompactUnwindIndex) BaseAddress() uint64 { return idx.baseAddress }

// SectionStart reports the preferred VM address of the first byte of the
// backing __TEXT,__unwind_info section.
func (idx *CompactUnwindIndex) SectionStart() uint64 { return idx.sectionStart }

// Levels reports the first-level index items (the terminal sentinel excluded).
func (idx *CompactUnwindIndex) Levels() []CompactLevel {
	out := make([]CompactLevel, len(idx.levels))
	copy(out, idx.levels)
	return out
}
