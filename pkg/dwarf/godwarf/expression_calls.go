package godwarf

import (
	"debug/dwarf"
	"encoding/binary"
	"errors"
	"fmt"
	"sort"

	"github.com/go-delve/delve/pkg/dwarf/op"
)

// ExpressionEvaluator evaluates DWARF location expressions read directly
// from debug_info, following DW_OP_call2, DW_OP_call4 and DW_OP_call_ref to
// the DW_TAG_dwarf_procedure entries they reference. It supports
// little-endian DWARF4 compilation units with 8-byte addresses; DWARF32 and
// DWARF64 units can be mixed in the same debug_info section.
type ExpressionEvaluator struct {
	dw  *dwarf.Data
	cus []exprCU
}

// exprCU describes one compilation unit of debug_info.
type exprCU struct {
	base       uint64 // offset of the unit header in debug_info
	end        uint64 // offset just past the end of the unit
	offsetSize int    // 4 for DWARF32, 8 for DWARF64
	dies       map[dwarf.Offset]struct{}
}

// NewExpressionEvaluator returns an ExpressionEvaluator reading compilation
// unit headers from debugInfo (the contents of the .debug_info section) and
// DIEs from dw.
func NewExpressionEvaluator(dw *dwarf.Data, debugInfo []byte) (*ExpressionEvaluator, error) {
	if dw == nil {
		return nil, errors.New("no DWARF data")
	}
	if len(debugInfo) == 0 {
		return nil, errors.New("empty .debug_info section")
	}
	cus, err := parseExprCUs(debugInfo)
	if err != nil {
		return nil, err
	}
	ev := &ExpressionEvaluator{dw: dw, cus: cus}
	if err := ev.indexDIEs(); err != nil {
		return nil, err
	}
	return ev, nil
}

// parseExprCUs parses the headers of all compilation units in debugInfo.
func parseExprCUs(debugInfo []byte) ([]exprCU, error) {
	var cus []exprCU
	for off := 0; off < len(debugInfo); {
		base := off
		lenField := 4
		if off+4 > len(debugInfo) {
			return nil, fmt.Errorf("truncated compilation unit header at offset %#x", base)
		}
		length := uint64(binary.LittleEndian.Uint32(debugInfo[off:]))
		off += 4
		offsetSize := 4
		if length == 0xffffffff {
			// DWARF64
			offsetSize = 8
			lenField = 12
			if off+8 > len(debugInfo) {
				return nil, fmt.Errorf("truncated compilation unit header at offset %#x", base)
			}
			length = binary.LittleEndian.Uint64(debugInfo[off:])
			off += 8
		}
		if off+2 > len(debugInfo) {
			return nil, fmt.Errorf("truncated compilation unit header at offset %#x", base)
		}
		version := binary.LittleEndian.Uint16(debugInfo[off:])
		off += 2
		if version < 2 || version > 4 {
			return nil, fmt.Errorf("unsupported DWARF version %d in compilation unit at offset %#x", version, base)
		}
		if off+offsetSize+1 > len(debugInfo) {
			return nil, fmt.Errorf("truncated compilation unit header at offset %#x", base)
		}
		off += offsetSize // debug_abbrev offset
		addrSize := debugInfo[off]
		off++
		if addrSize != 8 {
			return nil, fmt.Errorf("unsupported address size %d in compilation unit at offset %#x", addrSize, base)
		}
		end := uint64(base) + uint64(lenField) + length
		if end > uint64(len(debugInfo)) || end < uint64(off) {
			return nil, fmt.Errorf("compilation unit at offset %#x extends past the end of .debug_info", base)
		}
		cus = append(cus, exprCU{
			base:       uint64(base),
			end:        end,
			offsetSize: offsetSize,
			dies:       make(map[dwarf.Offset]struct{}),
		})
		off = int(end)
	}
	return cus, nil
}

// indexDIEs records the offset of every DIE in debug_info so that call
// targets can be validated against real DIE starts.
func (ev *ExpressionEvaluator) indexDIEs() error {
	rdr := ev.dw.Reader()
	for {
		e, err := rdr.Next()
		if err != nil {
			return err
		}
		if e == nil {
			return nil
		}
		if e.Tag == 0 {
			continue
		}
		cu := ev.cuOf(e.Offset)
		if cu == nil {
			return fmt.Errorf("DIE at offset %#x is not inside any compilation unit", uint64(e.Offset))
		}
		cu.dies[e.Offset] = struct{}{}
	}
}

// cuOf returns the compilation unit containing offset off, or nil.
func (ev *ExpressionEvaluator) cuOf(off dwarf.Offset) *exprCU {
	i := sort.Search(len(ev.cus), func(i int) bool { return ev.cus[i].end > uint64(off) })
	if i < len(ev.cus) && ev.cus[i].base <= uint64(off) && uint64(off) < ev.cus[i].end {
		return &ev.cus[i]
	}
	return nil
}

// Evaluate evaluates the location expression of the DIE at offset off (an
// absolute offset into debug_info). A DIE without a location attribute is a
// legal empty subprogram and evaluates to nothing; location lists are not
// supported and produce an error.
func (ev *ExpressionEvaluator) Evaluate(off dwarf.Offset, regs op.DwarfRegisters, ptrSize int, readMemory op.ReadMemoryFunc) (int64, []op.Piece, error) {
	cu := ev.cuOf(off)
	if cu == nil {
		return 0, nil, fmt.Errorf("DIE %#x is not inside any compilation unit", uint64(off))
	}
	if _, ok := cu.dies[off]; !ok {
		return 0, nil, fmt.Errorf("offset %#x is not the start of a DIE", uint64(off))
	}
	expr, hasLoc, err := ev.location(off)
	if err != nil {
		return 0, nil, err
	}
	if !hasLoc {
		return 0, nil, nil
	}
	return op.ExecuteStackProgramCalls(regs, expr, ptrSize, readMemory, &op.CallContext{
		DIEOffset:  uint64(off),
		CUBase:     cu.base,
		OffsetSize: cu.offsetSize,
		Resolve:    ev.resolve,
	})
}

// resolve implements op.CallContext.Resolve: it returns the location
// expression of the procedure DIE at offset target, the base of the
// compilation unit containing it and that unit's offset size.
func (ev *ExpressionEvaluator) resolve(target uint64) ([]byte, uint64, int, error) {
	off := dwarf.Offset(target)
	cu := ev.cuOf(off)
	if cu == nil {
		return nil, 0, 0, fmt.Errorf("call target %#x is not inside any compilation unit", target)
	}
	if _, ok := cu.dies[off]; !ok {
		return nil, 0, 0, fmt.Errorf("call target %#x is not the start of a DIE", target)
	}
	expr, _, err := ev.location(off)
	if err != nil {
		return nil, 0, 0, err
	}
	return expr, cu.base, cu.offsetSize, nil
}

// location returns the location expression of the DIE at offset off. hasLoc
// is false for a DIE without a location attribute (a legal empty
// subprogram); a location list produces an error.
func (ev *ExpressionEvaluator) location(off dwarf.Offset) (expr []byte, hasLoc bool, err error) {
	rdr := ev.dw.Reader()
	rdr.Seek(off)
	e, err := rdr.Next()
	if err != nil {
		return nil, false, err
	}
	if e == nil || e.Offset != off {
		return nil, false, fmt.Errorf("no DIE at offset %#x", uint64(off))
	}
	switch v := e.Val(dwarf.AttrLocation).(type) {
	case nil:
		return nil, false, nil
	case []byte:
		return v, true, nil
	default:
		return nil, false, fmt.Errorf("DIE %#x: location lists are not supported", uint64(off))
	}
}
