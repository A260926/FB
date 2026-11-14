/*
	Copyright 2023 Loophole Labs

	Licensed under the Apache License, Version 2.0 (the "License");
	you may not use this file except in compliance with the License.
	You may obtain a copy of the License at

		   http://www.apache.org/licenses/LICENSE-2.0

	Unless required by applicable law or agreed to in writing, software
	distributed under the License is distributed on an "AS IS" BASIS,
	WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
	See the License for the specific language governing permissions and
	limitations under the License.
*/

package wasmfile_test

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"testing"

	"github.com/loopholelabs/wasm-toolkit/pkg/wasm/expression"
	"github.com/loopholelabs/wasm-toolkit/pkg/wasm/types"
	"github.com/loopholelabs/wasm-toolkit/pkg/wasm/wasmfile"
	"github.com/loopholelabs/wasm-toolkit/task_support/taskcheck"
)

// The module builder in this file encodes WebAssembly bytes by hand from the
// binary format. Expected program counters are computed from those bytes by
// the builder itself; they are never obtained from BuildExceptionFlow.

func uv(v uint64) []byte {
	b := make([]byte, 10)
	n := binary.PutUvarint(b, v)
	return b[:n]
}

func vsleb(v int64) []byte {
	out := make([]byte, 0, 5)
	for {
		b := byte(v & 0x7f)
		v >>= 7
		if (v == 0 && b&0x40 == 0) || (v == -1 && b&0x40 != 0) {
			return append(out, b)
		}
		out = append(out, b|0x80)
	}
}

func section(id byte, payload []byte) []byte {
	out := []byte{id}
	out = append(out, uv(uint64(len(payload)))...)
	return append(out, payload...)
}

func vector(items ...[]byte) []byte {
	out := uv(uint64(len(items)))
	for _, item := range items {
		out = append(out, item...)
	}
	return out
}

func wasmName(s string) []byte {
	return append(uv(uint64(len(s))), s...)
}

func funcType(params, results []byte) []byte {
	out := []byte{types.FuncTypePrefix}
	out = append(out, uv(uint64(len(params)))...)
	out = append(out, params...)
	out = append(out, uv(uint64(len(results)))...)
	return append(out, results...)
}

type modImport struct {
	module string
	name   string
	kind   byte
	index  int
}

type modExport struct {
	name  string
	kind  byte
	index int
}

type modFunc struct {
	typeIndex int
	instrs    [][]byte
}

type modSpec struct {
	types   [][]byte
	imports []modImport
	tags    []int
	exports []modExport
	funcs   []modFunc
}

// buildModule returns the encoded image and, for each defined function, the
// absolute PC of every instruction. PCs are relative to the code section
// payload, including the vector count and body size fields, matching
// CodeEntry.CodeSectionPtr and Expression.PC.
func buildModule(spec modSpec) ([]byte, [][]uint64) {
	image := []byte{0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00}

	if len(spec.types) > 0 {
		image = append(image, section(byte(types.SectionType), vector(spec.types...))...)
	}

	if len(spec.imports) > 0 {
		entries := make([][]byte, 0, len(spec.imports))
		for _, imp := range spec.imports {
			e := wasmName(imp.module)
			e = append(e, wasmName(imp.name)...)
			e = append(e, imp.kind)
			if imp.kind == byte(types.ExportTag) {
				e = append(e, 0) // tag attribute
			}
			e = append(e, uv(uint64(imp.index))...)
			entries = append(entries, e)
		}
		image = append(image, section(byte(types.SectionImport), vector(entries...))...)
	}

	if len(spec.funcs) > 0 {
		entries := make([][]byte, 0, len(spec.funcs))
		for _, f := range spec.funcs {
			entries = append(entries, uv(uint64(f.typeIndex)))
		}
		image = append(image, section(byte(types.SectionFunction), vector(entries...))...)
	}

	if len(spec.tags) > 0 {
		entries := make([][]byte, 0, len(spec.tags))
		for _, t := range spec.tags {
			entries = append(entries, append([]byte{0}, uv(uint64(t))...))
		}
		image = append(image, section(byte(types.SectionTag), vector(entries...))...)
	}

	if len(spec.exports) > 0 {
		entries := make([][]byte, 0, len(spec.exports))
		for _, e := range spec.exports {
			entry := wasmName(e.name)
			entry = append(entry, e.kind)
			entry = append(entry, uv(uint64(e.index))...)
			entries = append(entries, entry)
		}
		image = append(image, section(byte(types.SectionExport), vector(entries...))...)
	}

	codePayload := uv(uint64(len(spec.funcs)))
	starts := make([]uint64, len(spec.funcs))
	for i, f := range spec.funcs {
		body := uv(0) // zero local declarations
		for _, instr := range f.instrs {
			body = append(body, instr...)
		}
		size := uv(uint64(len(body)))
		starts[i] = uint64(len(codePayload)) + uint64(len(size)) + 1
		codePayload = append(codePayload, size...)
		codePayload = append(codePayload, body...)
	}
	image = append(image, section(byte(types.SectionCode), codePayload)...)

	pcs := make([][]uint64, len(spec.funcs))
	for i, f := range spec.funcs {
		pcs[i] = make([]uint64, len(f.instrs))
		pc := starts[i]
		for j, instr := range f.instrs {
			pcs[i][j] = pc
			pc += uint64(len(instr))
		}
	}
	return image, pcs
}

func mustDecode(t *testing.T, image []byte) *wasmfile.WasmFile {
	t.Helper()
	wf := wasmfile.NewEmpty()
	if err := wf.DecodeBinary(image); err != nil {
		t.Fatalf("decode module: %v", err)
	}
	return wf
}

func blockEmpty() []byte      { return []byte{0x02, byte(types.ValNone)} }
func loopEmpty() []byte       { return []byte{0x03, byte(types.ValNone)} }
func endInstr() []byte        { return []byte{0x0b} }
func nopInstr() []byte        { return []byte{0x01} }
func throwTag(tag int) []byte { return append([]byte{0x08}, uv(uint64(tag))...) }
func callFunc(fn int) []byte  { return append([]byte{0x10}, uv(uint64(fn))...) }
func callIndirect(typeIdx, tableIdx int) []byte {
	return append([]byte{0x11}, append(uv(uint64(typeIdx)), uv(uint64(tableIdx))...)...)
}
func throwRef() []byte { return []byte{0x0a} }

const (
	catchTagKind    byte = 0
	catchTagRefKind byte = 1
	catchAllKind    byte = 2
	catchAllRefKind byte = 3
)

func catchTag(kind byte, tag, label int) []byte {
	out := []byte{kind}
	out = append(out, uv(uint64(tag))...)
	return append(out, uv(uint64(label))...)
}

func catchAll(kind byte, label int) []byte {
	return append([]byte{kind}, uv(uint64(label))...)
}

func tryTable(blockType []byte, catches ...[]byte) []byte {
	out := []byte{0x1f}
	out = append(out, blockType...)
	out = append(out, uv(uint64(len(catches)))...)
	for _, c := range catches {
		out = append(out, c...)
	}
	return out
}

func tagID(i int) wasmfile.ExceptionIdentity { return wasmfile.TagException(i) }
func foreignID() wasmfile.ExceptionIdentity  { return wasmfile.ForeignException() }

func findTransfer(flow *wasmfile.ExceptionFlow, fn int, pc uint64, id wasmfile.ExceptionIdentity) *wasmfile.ExceptionTransfer {
	for _, tr := range flow.Transfers {
		if tr.FunctionIndex == fn && tr.SourcePC == pc && tr.Identity == id {
			return tr
		}
	}
	return nil
}

func summary(flow *wasmfile.ExceptionFlow, fn int) *wasmfile.FunctionExceptionSummary {
	for _, s := range flow.Functions {
		if s.FunctionIndex == fn {
			return s
		}
	}
	return nil
}

func sameInts(a, b []int) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

// TestExceptionDecode covers binary decoding of the final exception handling
// encoding: interleaved tag/function imports, tag section entries, tag exports,
// original PCs, the full catch order, and all supported block type encodings.
func TestExceptionDecode(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		image, pcs := buildModule(modSpec{
			types: [][]byte{
				funcType(nil, nil),
				funcType([]byte{byte(types.ValI32)}, nil),
			},
			imports: []modImport{
				{module: "host", name: "a", kind: byte(types.ExportFunc), index: 0},
				{module: "env", name: "T0", kind: byte(types.ExportTag), index: 1},
				{module: "host", name: "b", kind: byte(types.ExportFunc), index: 0},
				{module: "env", name: "T1", kind: byte(types.ExportTag), index: 1},
				{module: "host", name: "c", kind: byte(types.ExportFunc), index: 0},
			},
			tags:    []int{0},
			exports: []modExport{{name: "T1out", kind: byte(types.ExportTag), index: 1}},
			funcs: []modFunc{{
				typeIndex: 0,
				instrs:    [][]byte{throwTag(2), endInstr()},
			}},
		})

		wf := mustDecode(t, image)

		c.Check(wf.FunctionImportCount() == 3, "function import count is %d, want 3 (tag imports must not prefix functions)", wf.FunctionImportCount())
		c.Check(len(wf.Import) == 3, "function imports tracked %d, want 3", len(wf.Import))
		c.Check(len(wf.TagImport) == 2, "tag imports tracked %d, want 2", len(wf.TagImport))
		c.Check(len(wf.Tag) == 1, "defined tags tracked %d, want 1", len(wf.Tag))
		c.Check(len(wf.TagImport)+len(wf.Tag) == 3, "declared tag index space is %d, want 3", len(wf.TagImport)+len(wf.Tag))
		if len(wf.TagImport) == 2 {
			c.Check(wf.TagImport[0].Module == "env" && wf.TagImport[0].Name == "T0" && wf.TagImport[0].Index == 1, "first tag import is %+v, want env:T0 type 1", wf.TagImport[0])
			c.Check(wf.TagImport[1].Name == "T1" && wf.TagImport[1].Index == 1, "second tag import is %+v, want env:T1 type 1", wf.TagImport[1])
		}
		if len(wf.Tag) == 1 {
			c.Check(wf.Tag[0].Attribute == 0 && wf.Tag[0].TypeIndex == 0, "defined tag is %+v, want attribute 0 type 0", wf.Tag[0])
		}
		if len(wf.Export) == 1 {
			c.Check(wf.Export[0].Type == types.ExportTag && wf.Export[0].Index == 1 && wf.Export[0].Name == "T1out", "tag export decoded as %+v", wf.Export[0])
		} else {
			c.Check(false, "expected one export, got %d", len(wf.Export))
		}

		c.Check(len(wf.Code) == 1, "decoded %d defined functions, want 1", len(wf.Code))
		// The function-terminating end terminates decoding, as in the existing
		// reader, so only the throw is recorded; its PC and PCNext still
		// preserve the instruction span.
		if len(wf.Code) == 1 && len(wf.Code[0].Expression) == 1 {
			th := wf.Code[0].Expression[0]
			c.Check(th.PC == pcs[0][0] && th.PCNext == pcs[0][0]+2, "throw PC span %d..%d, want %d..%d", th.PC, th.PCNext, pcs[0][0], pcs[0][0]+2)
			c.Check(th.Opcode == expression.InstrToOpcode["throw"] && th.TagIndex == 2, "throw decoded opcode %d tag %d, want throw tag 2", th.Opcode, th.TagIndex)
			c.Check(wf.Code[0].CodeSectionPtr <= th.PC && th.PCNext <= wf.Code[0].CodeSectionPtr+wf.Code[0].CodeSectionLen, "throw span %d..%d outside code entry %d..%d", th.PC, th.PCNext, wf.Code[0].CodeSectionPtr, wf.Code[0].CodeSectionPtr+wf.Code[0].CodeSectionLen)
		} else if len(wf.Code) == 1 {
			c.Check(false, "decoded %d instructions, want 1 (throw; the final end terminates decoding)", len(wf.Code[0].Expression))
		}
		c.Check(wf.FindFunction(pcs[0][0]) == 3, "FindFunction at throw PC = %d, want 3 (only function imports form the prefix)", wf.FindFunction(pcs[0][0]))

		// Instructions, catch order and block types.
		btImage, btPCs := buildModule(modSpec{
			types: [][]byte{funcType(nil, nil)},
			tags:  []int{0, 0},
			funcs: []modFunc{
				{
					typeIndex: 0,
					instrs: [][]byte{
						blockEmpty(),
						[]byte{0x03, byte(types.ValI64)},
						append([]byte{0x04}, vsleb(200)...),
						throwRef(),
						endInstr(),
						endInstr(),
						endInstr(),
						endInstr(),
					},
				},
				{
					typeIndex: 0,
					instrs: [][]byte{
						tryTable([]byte{byte(types.ValNone)},
							catchTag(catchTagKind, 0, 0),
							catchTag(catchTagRefKind, 1, 0),
							catchAll(catchAllKind, 0),
							catchAll(catchAllRefKind, 0),
						),
						nopInstr(),
						endInstr(),
						endInstr(),
					},
				},
			},
		})
		wfBT := mustDecode(t, btImage)
		if len(wfBT.Code) == 2 && len(wfBT.Code[0].Expression) == 7 {
			e := wfBT.Code[0].Expression
			c.Check(e[0].Result == types.ValNone && !e[0].BlockTypeIsIndex, "empty block type decoded as result %#x index=%v", e[0].Result, e[0].BlockTypeIsIndex)
			c.Check(e[0].PC == btPCs[0][0] && e[0].PCNext == btPCs[0][1], "block PC span %d..%d, want %d..%d", e[0].PC, e[0].PCNext, btPCs[0][0], btPCs[0][1])
			c.Check(e[1].Result == types.ValI64 && !e[1].BlockTypeIsIndex, "single result block type decoded as result %#x", e[1].Result)
			c.Check(e[2].BlockTypeIsIndex && e[2].BlockTypeIndex == 200, "multibyte LEB33 block type decoded index=%v value=%d, want 200", e[2].BlockTypeIsIndex, e[2].BlockTypeIndex)
			c.Check(e[2].PCNext == btPCs[0][3], "if with LEB33 type spans to %d, want %d", e[2].PCNext, btPCs[0][3])
			c.Check(e[3].Opcode == expression.InstrToOpcode["throw_ref"] && e[3].PCNext == e[3].PC+1, "throw_ref PC span %d..%d, want one byte", e[3].PC, e[3].PCNext)
		} else {
			c.Check(false, "block type function decoded %d expressions, want 7", len(wfBT.Code[0].Expression))
		}
		if len(wfBT.Code) == 2 && len(wfBT.Code[1].Expression) >= 1 {
			tt := wfBT.Code[1].Expression[0]
			c.Check(tt.Opcode == expression.InstrToOpcode["try_table"], "try_table opcode is %d", tt.Opcode)
			c.Check(tt.PC == btPCs[1][0] && tt.PCNext == btPCs[1][1], "try_table PC span %d..%d, want %d..%d", tt.PC, tt.PCNext, btPCs[1][0], btPCs[1][1])
			c.Check(tt.Result == types.ValNone && !tt.BlockTypeIsIndex, "try_table empty block type decoded result %#x", tt.Result)
			c.Check(len(tt.Catches) == 4, "catch vector has %d entries, want 4", len(tt.Catches))
			if len(tt.Catches) == 4 {
				c.Check(tt.Catches[0] == (expression.Catch{Kind: expression.CatchTag, TagIndex: 0, LabelIndex: 0}), "catch 0 is %+v", tt.Catches[0])
				c.Check(tt.Catches[1] == (expression.Catch{Kind: expression.CatchTagRef, TagIndex: 1, LabelIndex: 0}), "catch 1 is %+v", tt.Catches[1])
				c.Check(tt.Catches[2] == (expression.Catch{Kind: expression.CatchAll, TagIndex: -1, LabelIndex: 0}), "catch 2 is %+v", tt.Catches[2])
				c.Check(tt.Catches[3] == (expression.Catch{Kind: expression.CatchAllRef, TagIndex: -1, LabelIndex: 0}), "catch 3 is %+v", tt.Catches[3])
			}
		}

		// Ordinary modules keep their previous decode behavior and PCs.
		ordinary, ordinaryPCs := buildModule(modSpec{
			types: [][]byte{funcType(nil, nil)},
			funcs: []modFunc{{typeIndex: 0, instrs: [][]byte{nopInstr(), endInstr()}}},
		})
		wfOrd := mustDecode(t, ordinary)
		if len(wfOrd.Code) == 1 && len(wfOrd.Code[0].Expression) == 1 {
			e := wfOrd.Code[0].Expression[0]
			c.Check(e.Opcode == expression.InstrToOpcode["nop"] && e.PC == ordinaryPCs[0][0] && e.PCNext == e.PC+1, "ordinary nop PC span %d..%d, want %d", e.PC, e.PCNext, ordinaryPCs[0][0])
			c.Check(wfOrd.FindFunction(e.PC) == 0, "ordinary FindFunction = %d, want 0", wfOrd.FindFunction(e.PC))
		} else {
			c.Check(false, "ordinary module decoded %d functions / %d instructions", len(wfOrd.Code), len(wfOrd.Code[0].Expression))
		}
		c.Check(wfOrd.FunctionImportCount() == 0, "ordinary module function import count = %d", wfOrd.FunctionImportCount())

		// Legacy try/catch encodings are not supported and the error names the
		// function and the offset.
		legacy, _ := buildModule(modSpec{
			types: [][]byte{funcType(nil, nil)},
			funcs: []modFunc{{typeIndex: 0, instrs: [][]byte{{0x06, byte(types.ValNone)}, endInstr(), endInstr()}}},
		})
		legacyWF := wasmfile.NewEmpty()
		err := legacyWF.DecodeBinary(legacy)
		c.Check(err != nil, "legacy try encoding decoded without error")
		if err != nil {
			c.Check(bytes.Contains([]byte(err.Error()), []byte("function 0")), "legacy try error %q does not name the function", err)
			c.Check(bytes.Contains([]byte(err.Error()), []byte("pc ")), "legacy try error %q does not carry an offset", err)
		}

		// Building the flow must not mutate the decoded module or image.
		before := make([]struct {
			pc, pcNext, opcode, tag, label int
		}, 0)
		for _, ce := range wfBT.Code {
			for _, e := range ce.Expression {
				before = append(before, struct {
					pc, pcNext, opcode, tag, label int
				}{int(e.PC), int(e.PCNext), int(e.Opcode), e.TagIndex, e.LabelIndex})
			}
		}
		imageCopy := append([]byte(nil), btImage...)
		if _, err := wfBT.BuildExceptionFlow(); err != nil {
			t.Fatalf("build exception flow: %v", err)
		}
		c.Check(bytes.Equal(imageCopy, btImage), "module image changed during analysis")
		idx := 0
		for _, ce := range wfBT.Code {
			for _, e := range ce.Expression {
				got := before[idx]
				c.Check(int(e.PC) == got.pc && int(e.PCNext) == got.pcNext && int(e.Opcode) == got.opcode && e.TagIndex == got.tag && e.LabelIndex == got.label,
					"expression changed during analysis at %d", idx)
				idx++
			}
		}
	})
}

// TestExceptionHandlers checks catch search semantics: nested tables, encoded
// catch order, catch_all, identical signatures on different tags, loop label
// continuations, and invalid label diagnostics.
func TestExceptionHandlers(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		image, pcs := buildModule(modSpec{
			types: [][]byte{funcType(nil, nil)},
			tags:  []int{0, 0, 0}, // tag0, tag1, tag2 share one signature
			funcs: []modFunc{
				// f0: first non-matching clause is skipped; second clause hits.
				{typeIndex: 0, instrs: [][]byte{
					blockEmpty(),
					tryTable([]byte{byte(types.ValNone)},
						catchTag(catchTagKind, 1, 1),
						catchTag(catchTagKind, 0, 0)),
					throwTag(0),
					endInstr(), endInstr(), endInstr(),
				}},
				// f1: duplicate alternatives; the first matching clause wins.
				{typeIndex: 0, instrs: [][]byte{
					blockEmpty(),
					tryTable([]byte{byte(types.ValNone)},
						catchTag(catchTagKind, 0, 0),
						catchTag(catchTagKind, 0, 1)),
					throwTag(0),
					endInstr(), endInstr(), endInstr(),
				}},
				// f2: catch_all first consumes a tag that a later clause names.
				{typeIndex: 0, instrs: [][]byte{
					blockEmpty(),
					tryTable([]byte{byte(types.ValNone)},
						catchAll(catchAllKind, 0),
						catchTag(catchTagKind, 0, 1)),
					throwTag(0),
					endInstr(), endInstr(), endInstr(),
				}},
				// f3: same signature, wrong tag: not caught.
				{typeIndex: 0, instrs: [][]byte{
					blockEmpty(),
					tryTable([]byte{byte(types.ValNone)}, catchTag(catchTagKind, 0, 0)),
					throwTag(1),
					endInstr(), endInstr(), endInstr(),
				}},
				// f4: same signature, right tag: caught.
				{typeIndex: 0, instrs: [][]byte{
					blockEmpty(),
					tryTable([]byte{byte(types.ValNone)}, catchTag(catchTagKind, 0, 0)),
					throwTag(0),
					endInstr(), endInstr(), endInstr(),
				}},
				// f5: a catch to a loop label continues at the loop body entry.
				{typeIndex: 0, instrs: [][]byte{
					loopEmpty(),
					tryTable([]byte{byte(types.ValNone)}, catchTag(catchTagKind, 2, 0)),
					throwTag(2),
					endInstr(), endInstr(), endInstr(),
				}},
				// f6: call_indirect may raise any tag or a foreign exception;
				// only tag0 is caught here.
				{typeIndex: 0, instrs: [][]byte{
					blockEmpty(),
					tryTable([]byte{byte(types.ValNone)}, catchTag(catchTagKind, 0, 0)),
					callIndirect(0, 0),
					endInstr(), endInstr(), endInstr(),
				}},
				// f7: catch_all_ref catches every identity.
				{typeIndex: 0, instrs: [][]byte{
					blockEmpty(),
					tryTable([]byte{byte(types.ValNone)}, catchAll(catchAllRefKind, 0)),
					callIndirect(0, 0),
					endInstr(), endInstr(), endInstr(),
				}},
				// f8: catch_ref matches a tag and continues after the block.
				{typeIndex: 0, instrs: [][]byte{
					blockEmpty(),
					tryTable([]byte{byte(types.ValNone)}, catchTag(catchTagRefKind, 1, 0)),
					throwTag(1),
					endInstr(), endInstr(), endInstr(),
				}},
				// f9: nested tables. The inner table only names tag1, so a
				// tag0 throw reaches the outer table, whose catch order starts
				// with tag0 and continues at its own block end.
				{typeIndex: 0, instrs: [][]byte{
					blockEmpty(),
					tryTable([]byte{byte(types.ValNone)}, catchTag(catchTagKind, 0, 0)),
					blockEmpty(),
					tryTable([]byte{byte(types.ValNone)}, catchTag(catchTagKind, 1, 0)),
					throwTag(0),
					endInstr(), endInstr(),
					endInstr(), endInstr(), endInstr(),
				}},
				// f10: nested tables where the inner table catches tag1 that
				// the outer table would also catch; the inner one wins.
				{typeIndex: 0, instrs: [][]byte{
					blockEmpty(),
					tryTable([]byte{byte(types.ValNone)}, catchTag(catchTagKind, 1, 0)),
					blockEmpty(),
					tryTable([]byte{byte(types.ValNone)}, catchTag(catchTagKind, 1, 0)),
					throwTag(1),
					endInstr(), endInstr(),
					endInstr(), endInstr(), endInstr(),
				}},
			},
		})
		wf := mustDecode(t, image)

		// Before analysis: catch clauses preserved in order with tag labels.
		tt0 := wf.Code[0].Expression[1]
		c.Check(len(tt0.Catches) == 2 && tt0.Catches[0].TagIndex == 1 && tt0.Catches[1].TagIndex == 0, "f0 catch order not preserved: %+v", tt0.Catches)
		c.Check(tt0.Catches[0].LabelIndex == 1 && tt0.Catches[1].LabelIndex == 0, "f0 catch labels not preserved: %+v", tt0.Catches)

		flow, err := wf.BuildExceptionFlow()
		if err != nil {
			t.Fatalf("build exception flow: %v", err)
		}

		// f0: throw at instruction 2 matches clause #1, to the block end.
		t0 := findTransfer(flow, 0, pcs[0][2], tagID(0))
		if t0 == nil {
			c.Check(false, "f0 throw tag0 transfer missing")
		} else {
			c.Check(t0.Handler != nil, "f0 throw tag0 should be caught")
			if t0.Handler != nil {
				c.Check(t0.Handler.CatchIndex == 1, "f0 handler catch index = %d, want 1", t0.Handler.CatchIndex)
				c.Check(t0.Handler.TryTablePC == pcs[0][1], "f0 handler try_table pc = %d, want %d", t0.Handler.TryTablePC, pcs[0][1])
				c.Check(len(t0.Handler.CatchOrder) == 2 && t0.Handler.CatchOrder[0].TagIndex == 1, "f0 handler does not carry the full catch order: %+v", t0.Handler.CatchOrder)
			}
			c.Check(!t0.Target.Escapes && t0.Target.PC == pcs[0][4]+1, "f0 target %+v, want continuation %d", t0.Target, pcs[0][4]+1)
		}
		if s := summary(flow, 0); s != nil {
			c.Check(len(s.EscapingTags) == 0 && !s.Foreign, "f0 summary %+v, want no escapes", s)
		}

		// f1: first duplicate wins; later duplicate never sees the identity.
		t1 := findTransfer(flow, 1, pcs[1][2], tagID(0))
		if t1 == nil || t1.Handler == nil {
			c.Check(false, "f1 throw tag0 transfer missing or uncaught: %+v", t1)
		} else {
			c.Check(t1.Handler.CatchIndex == 0, "f1 handler catch index = %d, want first duplicate (0)", t1.Handler.CatchIndex)
			c.Check(!t1.Target.Escapes && t1.Target.PC == pcs[1][4]+1, "f1 target %+v, want continuation %d", t1.Target, pcs[1][4]+1)
		}

		// f2: catch_all at index 0 shadows the later tag clause.
		t2 := findTransfer(flow, 2, pcs[2][2], tagID(0))
		if t2 == nil || t2.Handler == nil {
			c.Check(false, "f2 throw tag0 transfer missing or uncaught: %+v", t2)
		} else {
			c.Check(t2.Handler.CatchIndex == 0 && t2.Handler.Kind == expression.CatchAll, "f2 handler %+v, want catch_all #0", t2.Handler)
		}

		// f3/f4: identical signatures do not merge identities.
		t3 := findTransfer(flow, 3, pcs[3][2], tagID(1))
		c.Check(t3 != nil, "f3 throw tag1 transfer missing")
		if t3 != nil {
			c.Check(t3.Handler == nil && t3.Target.Escapes, "f3 throw tag1 wrong-tag catch hit: handler=%+v target=%+v", t3.Handler, t3.Target)
		}
		if s := summary(flow, 3); s != nil {
			c.Check(sameInts(s.EscapingTags, []int{1}), "f3 summary tags %v, want [1]", s.EscapingTags)
		}
		t4 := findTransfer(flow, 4, pcs[4][2], tagID(0))
		if t4 == nil || t4.Handler == nil {
			c.Check(false, "f4 throw tag0 should match the identical-signature tag0 clause: %+v", t4)
		}
		if s := summary(flow, 4); s != nil {
			c.Check(len(s.EscapingTags) == 0 && !s.Foreign, "f4 summary %+v, want no escapes", s)
		}

		// f5: loop label continuation is the loop body entry, not a label
		// number interpreted as an instruction index.
		t5 := findTransfer(flow, 5, pcs[5][2], tagID(2))
		if t5 == nil || t5.Handler == nil {
			c.Check(false, "f5 throw tag2 transfer missing or uncaught: %+v", t5)
		} else {
			c.Check(!t5.Target.Escapes && !t5.Target.FunctionExit, "f5 target %+v, want loop continuation", t5.Target)
			c.Check(t5.Target.PC == pcs[5][1], "f5 loop continuation = %d, want loop body entry %d", t5.Target.PC, pcs[5][1])
		}

		// f6: indirect call possibilities; only tag0 is caught.
		for _, id := range []wasmfile.ExceptionIdentity{tagID(1), tagID(2), foreignID()} {
			tr := findTransfer(flow, 6, pcs[6][2], id)
			c.Check(tr != nil, "f6 call_indirect transfer for %s missing", id)
			if tr != nil {
				c.Check(tr.Handler == nil && tr.Target.Escapes, "f6 %s should escape, got handler=%+v target=%+v", id, tr.Handler, tr.Target)
			}
		}
		if tr := findTransfer(flow, 6, pcs[6][2], tagID(0)); tr == nil || tr.Handler == nil {
			c.Check(false, "f6 call_indirect tag0 should be caught: %+v", tr)
		}
		if s := summary(flow, 6); s != nil {
			c.Check(sameInts(s.EscapingTags, []int{1, 2}), "f6 summary tags %v, want [1 2]", s.EscapingTags)
			c.Check(s.Foreign, "f6 summary should include the foreign possibility")
		}

		// f7: catch_all_ref covers every declared tag and the foreign case.
		for _, id := range []wasmfile.ExceptionIdentity{tagID(0), tagID(1), tagID(2), foreignID()} {
			tr := findTransfer(flow, 7, pcs[7][2], id)
			if tr == nil || tr.Handler == nil {
				c.Check(false, "f7 catch_all_ref transfer for %s missing or uncaught: %+v", id, tr)
				continue
			}
			c.Check(tr.Handler.Kind == expression.CatchAllRef && tr.Handler.CatchIndex == 0, "f7 %s handler %+v, want catch_all_ref #0", id, tr.Handler)
			c.Check(!tr.Target.Escapes, "f7 %s unexpectedly escapes", id)
		}
		if s := summary(flow, 7); s != nil {
			c.Check(len(s.EscapingTags) == 0 && !s.Foreign, "f7 summary %+v, want no escapes", s)
		}

		// f8: catch_ref matches the tag identity.
		t8 := findTransfer(flow, 8, pcs[8][2], tagID(1))
		if t8 == nil || t8.Handler == nil {
			c.Check(false, "f8 catch_ref transfer missing or uncaught: %+v", t8)
		} else {
			c.Check(t8.Handler.Kind == expression.CatchTagRef && t8.Handler.CatchIndex == 0, "f8 handler %+v, want catch_ref #0", t8.Handler)
		}

		// f9: the outer table catches the tag0 throw; the inner try_table's
		// own block end is the continuation of the outer block label, so the
		// transfer continues at the outer block's end-next, not at the inner
		// one.
		t9 := findTransfer(flow, 9, pcs[9][4], tagID(0))
		if t9 == nil || t9.Handler == nil {
			c.Check(false, "f9 nested table transfer missing or uncaught: %+v", t9)
		} else {
			c.Check(t9.Handler.TryTablePC == pcs[9][1] && t9.Handler.CatchIndex == 0, "f9 handler %+v, want outer try_table at %d", t9.Handler, pcs[9][1])
			c.Check(t9.Target.Escapes == false && t9.Target.FunctionExit == false, "f9 target %+v, want a block continuation", t9.Target)
			c.Check(t9.Target.PC == pcs[9][8]+1, "f9 continuation = %d, want outer block end-next %d", t9.Target.PC, pcs[9][8]+1)
		}
		c.Check(findTransfer(flow, 9, pcs[9][4], tagID(1)) == nil, "f9 throw of tag0 must not produce a tag1 identity")

		// f10: both tables name tag1; the innermost table catches first.
		t10 := findTransfer(flow, 10, pcs[10][4], tagID(1))
		if t10 == nil || t10.Handler == nil {
			c.Check(false, "f10 nested table transfer missing or uncaught: %+v", t10)
		} else {
			c.Check(t10.Handler.TryTablePC == pcs[10][3], "f10 handler table pc = %d, want inner table %d", t10.Handler.TryTablePC, pcs[10][3])
			c.Check(t10.Target.PC == pcs[10][6]+1, "f10 continuation = %d, want inner block end-next %d", t10.Target.PC, pcs[10][6]+1)
		}

		// Invalid catch labels and throw tags are reported with function and
		// offset.
		badLabel, badPCs := buildModule(modSpec{
			types: [][]byte{funcType(nil, nil)},
			tags:  []int{0},
			funcs: []modFunc{{typeIndex: 0, instrs: [][]byte{
				tryTable([]byte{byte(types.ValNone)}, catchTag(catchTagKind, 0, 9)),
				nopInstr(), endInstr(), endInstr(),
			}}},
		})
		badWF := mustDecode(t, badLabel)
		_, err = badWF.BuildExceptionFlow()
		c.Check(err != nil, "invalid catch label was accepted")
		if err != nil {
			c.Check(bytes.Contains([]byte(err.Error()), []byte("function 0")), "invalid catch label error %q lacks the function", err)
			c.Check(bytes.Contains([]byte(err.Error()), []byte(fmt.Sprintf("%d", badPCs[0][0]))), "invalid catch label error %q lacks the offset %d", err, badPCs[0][0])
		}

		badThrow, badThrowPCs := buildModule(modSpec{
			types: [][]byte{funcType(nil, nil)},
			tags:  []int{0},
			funcs: []modFunc{{typeIndex: 0, instrs: [][]byte{throwTag(5), endInstr()}}},
		})
		badThrowWF := mustDecode(t, badThrow)
		_, err = badThrowWF.BuildExceptionFlow()
		c.Check(err != nil, "invalid throw tag was accepted")
		if err != nil {
			c.Check(bytes.Contains([]byte(err.Error()), []byte("function 0")), "invalid throw tag error %q lacks the function", err)
			c.Check(bytes.Contains([]byte(err.Error()), []byte(fmt.Sprintf("%d", badThrowPCs[0][0]))), "invalid throw tag error %q lacks the offset %d", err, badThrowPCs[0][0])
		}
	})
}

// TestExceptionRecursion checks least-fixed-point summaries over recursive and
// mutually recursive functions, local capture inside a cycle, and the static
// plugin boundary policy audit.
func TestExceptionRecursion(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		image, pcs := buildModule(modSpec{
			types: [][]byte{funcType(nil, nil)},
			tags:  []int{0, 0, 0}, // tag0, tag1, tag2
			funcs: []modFunc{
				// f0 calls f1; f1 throws tag0.
				{typeIndex: 0, instrs: [][]byte{callFunc(1), endInstr()}},
				{typeIndex: 0, instrs: [][]byte{throwTag(0), endInstr()}},
				// f2 <-> f3 mutual recursion; f3 throws tag1 only.
				{typeIndex: 0, instrs: [][]byte{callFunc(3), endInstr()}},
				{typeIndex: 0, instrs: [][]byte{callFunc(2), throwTag(1), endInstr()}},
				// f4/f5 mutual recursion with a local catch in f4.
				{typeIndex: 0, instrs: [][]byte{
					blockEmpty(),
					tryTable([]byte{byte(types.ValNone)}, catchTag(catchTagKind, 0, 0)),
					callFunc(5),
					endInstr(), endInstr(), endInstr(),
				}},
				{typeIndex: 0, instrs: [][]byte{callFunc(4), throwTag(0), endInstr()}},
				// f6 catches tag0 from a recursive self call.
				{typeIndex: 0, instrs: [][]byte{
					blockEmpty(),
					tryTable([]byte{byte(types.ValNone)}, catchTag(catchTagKind, 0, 0)),
					callFunc(6),
					throwTag(2),
					endInstr(), endInstr(), endInstr(),
				}},
			},
		})
		wf := mustDecode(t, image)
		flow, err := wf.BuildExceptionFlow()
		if err != nil {
			t.Fatalf("build exception flow: %v", err)
		}

		// Direct call summaries do not widen to every declared tag.
		if s := summary(flow, 0); s != nil {
			c.Check(sameInts(s.EscapingTags, []int{0}) && !s.Foreign, "f0 summary %+v, want tags [0] only", s)
		}
		if s := summary(flow, 1); s != nil {
			c.Check(sameInts(s.EscapingTags, []int{0}) && !s.Foreign, "f1 summary %+v, want tags [0] only", s)
		}
		t0 := findTransfer(flow, 0, pcs[0][0], tagID(0))
		if t0 == nil {
			c.Check(false, "f0 call to f1 has no tag0 transfer")
		} else {
			c.Check(t0.Callee == 1 && t0.Site == wasmfile.ExceptionSiteCall, "f0 transfer callee=%d site=%v, want defined function 1", t0.Callee, t0.Site)
			c.Check(t0.Handler == nil && t0.Target.Escapes, "f0 tag0 should escape: %+v", t0)
		}
		c.Check(findTransfer(flow, 0, pcs[0][0], tagID(1)) == nil, "f0 call to f1 must not manufacture tag1")
		c.Check(findTransfer(flow, 0, pcs[0][0], tagID(2)) == nil, "f0 call to f1 must not manufacture tag2")
		c.Check(findTransfer(flow, 0, pcs[0][0], foreignID()) == nil, "f0 direct defined call must not manufacture a foreign exception")

		// Mutual recursion reaches the least fixed point: tag1 only.
		for _, fn := range []int{2, 3} {
			if s := summary(flow, fn); s != nil {
				c.Check(sameInts(s.EscapingTags, []int{1}) && !s.Foreign, "mutual recursion summary f%d = %+v, want tags [1] only", fn, s)
			}
		}

		// Local capture inside a cycle: f5's throw escapes f5, but f4 catches
		// the callee's exception locally, so f4 escapes nothing.
		if s := summary(flow, 4); s != nil {
			c.Check(len(s.EscapingTags) == 0 && !s.Foreign, "f4 summary %+v, want local capture to leave no escapes", s)
		}
		if s := summary(flow, 5); s != nil {
			c.Check(sameInts(s.EscapingTags, []int{0}), "f5 summary tags %v, want [0]", s.EscapingTags)
		}
		t4 := findTransfer(flow, 4, pcs[4][2], tagID(0))
		if t4 == nil || t4.Handler == nil {
			c.Check(false, "f4 call inside try_table did not catch tag0: %+v", t4)
		} else {
			c.Check(t4.Handler.TryTablePC == pcs[4][1] && t4.Handler.CatchIndex == 0, "f4 handler %+v, want try_table at %d catch 0", t4.Handler, pcs[4][1])
			c.Check(!t4.Target.Escapes && t4.Target.PC == pcs[4][4]+1, "f4 target %+v, want continuation %d", t4.Target, pcs[4][4]+1)
		}

		// Self recursion: the function's own tag0 catch is part of its
		// summary fixed point, so the recursive call carries only the tag2
		// that actually escapes. The cycle must not widen to tag0 or all tags.
		if s := summary(flow, 6); s != nil {
			c.Check(sameInts(s.EscapingTags, []int{2}) && !s.Foreign, "f6 summary %+v, want tags [2] only", s)
		}
		if tr := findTransfer(flow, 6, pcs[6][2], tagID(2)); tr == nil || !tr.Target.Escapes {
			c.Check(false, "f6 self-call tag2 should escape: %+v", tr)
		}
		c.Check(findTransfer(flow, 6, pcs[6][2], tagID(0)) == nil, "f6 self-call must not carry tag0 that f6 itself catches")
		if tr := findTransfer(flow, 6, pcs[6][3], tagID(2)); tr == nil || !tr.Target.Escapes {
			c.Check(false, "f6 direct throw tag2 should escape: %+v", tr)
		}

		// Static plugin boundary policy: one host import, three boundaries.
		policyImage, policyPCs := buildModule(modSpec{
			types: [][]byte{funcType(nil, nil)},
			imports: []modImport{
				{module: "env", name: "host", kind: byte(types.ExportFunc), index: 0},
			},
			tags: []int{0, 0},
			funcs: []modFunc{
				// function 1: uncaught host call.
				{typeIndex: 0, instrs: [][]byte{callFunc(0), endInstr()}},
				// function 2: tag0 and catch_all catch everything.
				{typeIndex: 0, instrs: [][]byte{
					blockEmpty(),
					tryTable([]byte{byte(types.ValNone)},
						catchTag(catchTagKind, 0, 0),
						catchAll(catchAllKind, 0)),
					callFunc(0),
					endInstr(), endInstr(), endInstr(),
				}},
				// function 3: only tag0 is caught.
				{typeIndex: 0, instrs: [][]byte{
					blockEmpty(),
					tryTable([]byte{byte(types.ValNone)}, catchTag(catchTagKind, 0, 0)),
					callFunc(0),
					endInstr(), endInstr(), endInstr(),
				}},
			},
		})
		policyWF := mustDecode(t, policyImage)
		c.Check(policyWF.FunctionImportCount() == 1, "policy module function prefix = %d, want 1", policyWF.FunctionImportCount())

		audit, err := policyWF.AuditPluginBoundary(&wasmfile.PluginBoundaryPolicy{
			IsolatedImports: []string{"env:host"},
			Boundaries:      []int{1, 2, 3},
		})
		if err != nil {
			t.Fatalf("audit plugin boundary: %v", err)
		}
		c.Check(audit.Status == wasmfile.BoundaryRisk, "overall audit status = %v, want risk", audit.Status)
		c.Check(len(audit.Boundaries) == 3, "audited %d boundaries, want 3", len(audit.Boundaries))
		if len(audit.Boundaries) == 3 {
			b1, b2, b3 := audit.Boundaries[0], audit.Boundaries[1], audit.Boundaries[2]
			c.Check(b1.FunctionIndex == 1 && b1.Status == wasmfile.BoundaryRisk, "boundary 1 %+v, want risk", b1)
			c.Check(len(b1.Findings) == 3, "boundary 1 findings = %d, want tag0, tag1 and foreign", len(b1.Findings))
			if len(b1.Findings) == 3 {
				seen := map[wasmfile.ExceptionIdentity]bool{}
				for _, f := range b1.Findings {
					seen[f.Identity] = true
					c.Check(f.SourcePC == policyPCs[0][0], "boundary 1 finding source pc = %d, want %d", f.SourcePC, policyPCs[0][0])
					c.Check(f.ImportModule == "env" && f.ImportName == "host" && f.ImportFunctionIndex == 0, "boundary 1 finding import %+v", f)
					c.Check(f.Handler == nil && f.Target.Escapes && f.Status == wasmfile.BoundaryRisk, "boundary 1 finding %+v should be an escaping risk", f)
				}
				c.Check(seen[tagID(0)] && seen[tagID(1)] && seen[foreignID()], "boundary 1 identities %v, want tag0, tag1, foreign", seen)
			}

			c.Check(b2.FunctionIndex == 2 && b2.Status == wasmfile.BoundaryCovered, "boundary 2 %+v, want covered", b2)
			c.Check(len(b2.Findings) == 3, "boundary 2 findings = %d, want 3", len(b2.Findings))
			for _, f := range b2.Findings {
				c.Check(f.Handler != nil && !f.Target.Escapes && f.Status == wasmfile.BoundaryCovered, "boundary 2 finding %+v should be covered", f)
				if f.Identity == tagID(0) {
					c.Check(f.Handler != nil && f.Handler.CatchIndex == 0 && f.Handler.Kind == expression.CatchTag, "boundary 2 tag0 handler %+v, want catch tag #0", f.Handler)
				} else {
					c.Check(f.Handler != nil && f.Handler.CatchIndex == 1 && f.Handler.Kind == expression.CatchAll, "boundary 2 %s handler %+v, want catch_all #1", f.Identity, f.Handler)
				}
			}

			c.Check(b3.FunctionIndex == 3 && b3.Status == wasmfile.BoundaryRisk, "boundary 3 %+v, want risk", b3)
			// tag0 and tag1 share a signature but not an identity: the
			// same-signature positive/negative pair must show up here, with
			// tag0 covered and tag1 plus the foreign possibility at risk.
			covered, risk := 0, 0
			for _, f := range b3.Findings {
				c.Check(f.SourcePC == policyPCs[2][2], "boundary 3 finding source pc = %d, want call at %d", f.SourcePC, policyPCs[2][2])
				if f.Identity == tagID(0) {
					c.Check(f.Handler != nil && f.Handler.CatchIndex == 0 && f.Handler.Kind == expression.CatchTag, "boundary 3 tag0 handler %+v, want catch tag #0", f.Handler)
					c.Check(f.Handler.TryTablePC == policyPCs[2][1], "boundary 3 tag0 handler try_table pc = %d, want %d", f.Handler.TryTablePC, policyPCs[2][1])
					c.Check(!f.Target.Escapes && f.Status == wasmfile.BoundaryCovered, "boundary 3 tag0 should be covered: %+v", f)
					covered++
				} else {
					c.Check(f.Handler == nil && f.Target.Escapes && f.Status == wasmfile.BoundaryRisk, "boundary 3 %s should escape: %+v", f.Identity, f)
					risk++
				}
			}
			c.Check(covered == 1, "boundary 3 covered findings = %d, want 1 (tag0)", covered)
			c.Check(risk == 2, "boundary 3 risk findings = %d, want 2 (tag1 and foreign)", risk)
		}

		// An encoding that cannot be analyzed reports insufficient coverage,
		// never safety.
		unsupported := wasmfile.NewEmpty()
		unsupported.Import = []*wasmfile.ImportEntry{{Module: "env", Name: "host", Type: types.ExportFunc, Index: 0}}
		unsupported.Code = []*wasmfile.CodeEntry{{Expression: []*expression.Expression{
			{
				PC:      10,
				PCNext:  15,
				Opcode:  expression.InstrToOpcode["try_table"],
				Result:  types.ValNone,
				Catches: []expression.Catch{{Kind: expression.CatchTag, TagIndex: 0, LabelIndex: 9}},
			},
		}}}
		insufficient, err := unsupported.AuditPluginBoundary(&wasmfile.PluginBoundaryPolicy{
			IsolatedImports: []string{"host"},
			Boundaries:      []int{1},
		})
		if err != nil {
			t.Fatalf("audit unsupported module: %v", err)
		}
		c.Check(insufficient.Status == wasmfile.BoundaryInsufficient, "unsupported audit status = %v, want insufficient", insufficient.Status)
		c.Check(len(insufficient.InsufficientReasons) > 0, "unsupported audit has no reason")
		c.Check(insufficient.Status != wasmfile.BoundaryCovered, "unsupported audit must not default to safe")
	})
}
