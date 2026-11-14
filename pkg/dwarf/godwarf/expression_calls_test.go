package godwarf

import (
	"debug/dwarf"
	"fmt"
	"strings"
	"testing"

	"github.com/go-delve/delve/pkg/dwarf/op"
	"github.com/go-delve/delve/task_support/fixture"
	"github.com/go-delve/delve/task_support/taskcheck"
)

func buildCallsEvaluator(t *testing.T) (*ExpressionEvaluator, fixture.CallsMaterial) {
	t.Helper()
	m := fixture.BuildCalls()
	d, err := dwarf.New(m.Abbrev, nil, nil, m.Info, nil, nil, nil, nil)
	if err != nil {
		t.Fatalf("dwarf.New: %v", err)
	}
	ev, err := NewExpressionEvaluator(d, m.Info)
	if err != nil {
		t.Fatalf("NewExpressionEvaluator: %v", err)
	}
	return ev, m
}

func evalProc(ev *ExpressionEvaluator, off uint64) (int64, []op.Piece, error) {
	return ev.Evaluate(dwarf.Offset(off), op.DwarfRegisters{}, 8, nil)
}

func TestExpressionCallStack(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		ev, m := buildCallsEvaluator(t)

		// The caller pushes 7, the called procedure adds 2 on the shared
		// stack, the caller multiplies by 2: (7+2)*2.
		v, pieces, err := evalProc(ev, m.Offsets["mainA"])
		c.Check(err == nil, "mainA: unexpected error: %v", err)
		c.Check(v == 18, "mainA: got %d, want 18", v)
		c.Check(pieces == nil, "mainA: unexpected pieces %v", pieces)

		// Calling the same procedure twice in sequence is legal.
		v, _, err = evalProc(ev, m.Offsets["doubleCall"])
		c.Check(err == nil, "doubleCall: unexpected error: %v", err)
		c.Check(v == 5, "doubleCall: got %d, want 5", v)

		// DW_OP_call2 reads a 2-byte unit-relative operand: 6+2.
		v, _, err = evalProc(ev, m.Offsets["call2Caller"])
		c.Check(err == nil, "call2Caller: unexpected error: %v", err)
		c.Check(v == 8, "call2Caller: got %d, want 8", v)

		// The called procedure ends with DW_OP_reg5; the location is closed
		// by the caller's DW_OP_piece, not by the call return.
		_, pieces, err = evalProc(ev, m.Offsets["mainPiece"])
		c.Check(err == nil, "mainPiece: unexpected error: %v", err)
		c.Check(len(pieces) == 2, "mainPiece: got %d pieces, want 2", len(pieces))
		if len(pieces) == 2 {
			c.Check(pieces[0].Kind == op.RegPiece && pieces[0].Val == 5 && pieces[0].Size == 4,
				"mainPiece: piece[0] is %+v, want 4-byte piece of register 5", pieces[0])
			c.Check(pieces[1].Kind == op.AddrPiece && pieces[1].Val == 9 && pieces[1].Size == 4,
				"mainPiece: piece[1] is %+v, want 4-byte piece at address 9", pieces[1])
		}

		// Same across a call return, with DW_OP_stack_value.
		_, pieces, err = evalProc(ev, m.Offsets["mainStackVal"])
		c.Check(err == nil, "mainStackVal: unexpected error: %v", err)
		c.Check(len(pieces) == 1, "mainStackVal: got %d pieces, want 1", len(pieces))
		if len(pieces) == 1 {
			c.Check(pieces[0].Kind == op.ImmPiece && pieces[0].Val == 5 && pieces[0].Size == 8,
				"mainStackVal: piece[0] is %+v, want 8-byte immediate piece of value 5", pieces[0])
		}

		// A procedure without a location is a legal empty subprogram.
		v, _, err = evalProc(ev, m.Offsets["mainEmpty"])
		c.Check(err == nil, "mainEmpty: unexpected error: %v", err)
		c.Check(v == 5, "mainEmpty: got %d, want 5", v)
		v, pieces, err = evalProc(ev, m.Offsets["emptyProc"])
		c.Check(err == nil, "emptyProc: unexpected error: %v", err)
		c.Check(v == 0 && pieces == nil, "emptyProc: got %d, %v; want 0, nil", v, pieces)
	})
}

func TestExpressionCallUnits(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		ev, m := buildCallsEvaluator(t)

		// The same numeric DW_OP_call4 operand selects different procedures
		// in different compilation units: addTwo adds 2 in the first unit,
		// procB adds 30 in the second.
		v, _, err := evalProc(ev, m.Offsets["mainSameA"])
		c.Check(err == nil, "mainSameA: unexpected error: %v", err)
		c.Check(v == 7, "mainSameA: got %d, want 7", v)
		v, _, err = evalProc(ev, m.Offsets["mainSameB"])
		c.Check(err == nil, "mainSameB: unexpected error: %v", err)
		c.Check(v == 35, "mainSameB: got %d, want 35", v)

		// DW_OP_call_ref operands are read with the calling unit's offset
		// width: 4 bytes in the DWARF32 unit.
		v, _, err = evalProc(ev, m.Offsets["refCaller32"])
		c.Check(err == nil, "refCaller32: unexpected error: %v", err)
		c.Check(v == 12, "refCaller32: got %d, want 12", v)

		// The DWARF64 unit calls into the DWARF32 unit with an 8-byte
		// DW_OP_call_ref operand; the relative DW_OP_call4 inside the called
		// procedure uses the called unit's start, not the root caller's.
		v, _, err = evalProc(ev, m.Offsets["mainB"])
		c.Check(err == nil, "mainB: unexpected error: %v", err)
		c.Check(v == 5, "mainB: got %d, want 5", v)
	})
}

func TestExpressionCallErrors(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		ev, m := buildCallsEvaluator(t)

		// Recursion on the active call chain is an error.
		_, _, err := evalProc(ev, m.Offsets["selfRec"])
		c.Check(err != nil && strings.Contains(err.Error(), "recursive call"),
			"selfRec: got %v, want a recursive call error", err)

		// A truncated operand is reported with the DIE and the expression
		// offset of the call opcode.
		_, _, err = evalProc(ev, m.Offsets["truncCall"])
		c.Check(err != nil, "truncCall: expected an error, got none")
		if err != nil {
			c.Check(strings.Contains(err.Error(), fmt.Sprintf("DIE %#x:", m.Offsets["truncCall"])),
				"truncCall: error %q does not name the DIE", err)
			c.Check(strings.Contains(err.Error(), "expression offset 0x1"),
				"truncCall: error %q does not name the expression offset", err)
		}

		// A call target in the middle of a DIE is rejected.
		_, _, err = evalProc(ev, m.Offsets["badTarget"])
		c.Check(err != nil && strings.Contains(err.Error(), "is not the start of a DIE"),
			"badTarget: got %v, want an invalid target error", err)

		// A branch outside the current expression is rejected.
		_, _, err = evalProc(ev, m.Offsets["badBranch"])
		c.Check(err != nil && strings.Contains(err.Error(), "out of bounds"),
			"badBranch: got %v, want an out of bounds branch error", err)

		// The execution limit is an error, not a partial result.
		v, pieces, err := evalProc(ev, m.Offsets["loopForever"])
		c.Check(err != nil && strings.Contains(err.Error(), "execution limit"),
			"loopForever: got %v, want an execution limit error", err)
		c.Check(v == 0 && pieces == nil,
			"loopForever: got partial result %d, %v alongside the error", v, pieces)

		// Location lists are not supported, neither as call targets nor as
		// the root expression.
		_, _, err = evalProc(ev, m.Offsets["mainLoc"])
		c.Check(err != nil && strings.Contains(err.Error(), "location lists are not supported"),
			"mainLoc: got %v, want a location list error", err)
		_, _, err = evalProc(ev, m.Offsets["locProc"])
		c.Check(err != nil && strings.Contains(err.Error(), "location lists are not supported"),
			"locProc: got %v, want a location list error", err)

		// Evaluating at an offset that is not a DIE start fails.
		_, _, err = evalProc(ev, m.Offsets["addTwo"]+1)
		c.Check(err != nil && strings.Contains(err.Error(), "is not the start of a DIE"),
			"addTwo+1: got %v, want an invalid offset error", err)
		_, _, err = evalProc(ev, uint64(len(m.Info))+0x100)
		c.Check(err != nil && strings.Contains(err.Error(), "is not inside any compilation unit"),
			"past end: got %v, want an out of unit error", err)

		// After all these errors a normal expression still evaluates.
		v, pieces, err = evalProc(ev, m.Offsets["mainA"])
		c.Check(err == nil, "mainA after errors: unexpected error: %v", err)
		c.Check(v == 18, "mainA after errors: got %d, want 18", v)
		c.Check(pieces == nil, "mainA after errors: unexpected pieces %v", pieces)
	})
}
