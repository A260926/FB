// Package fixture builds a legal, inert ELF with two debug compilation units.
// It provides material only; it does not resolve or evaluate DWARF calls.
package fixture

import (
 "bytes"
 "encoding/binary"
 "strings"
)

type Material struct { Info, Abbrev []byte; RootA, ProcA, RootB, ProcB uint64 }
func put(b *bytes.Buffer, v any) { if err:=binary.Write(b,binary.LittleEndian,v); err!=nil { panic(err) } }
func die(b *bytes.Buffer, name string, expr []byte) uint64 {
 off:=uint64(b.Len()); b.WriteByte(2); b.WriteString(name); b.WriteByte(0)
 if len(expr)>127 { panic("fixture expr too long") }; b.WriteByte(byte(len(expr))); b.Write(expr); return off
}
func Build() Material {
 // CU has name; dwarf_procedure has name and exprloc location.
 a:=[]byte{1,0x11,1,3,8,0,0,2,0x36,0,3,8,2,0x18,0,0,0}
 var b bytes.Buffer
 put(&b,uint32(0)); put(&b,uint16(4)); put(&b,uint32(0)); b.WriteByte(8)
 b.WriteByte(1); b.WriteString("first.c"); b.WriteByte(0)
 r1:=die(&b,"root_a",[]byte{0x37,0x99,0,0,0,0,0x32,0x1e})
 p1:=die(&b,"add_two",[]byte{0x23,2}); b.WriteByte(0)
 firstEnd:=b.Len(); info:=b.Bytes(); binary.LittleEndian.PutUint32(info,uint32(firstEnd-4))
 // DWARF64 CU: call_ref operands use eight bytes, though addresses stay 8-byte.
 put(&b,uint32(0xffffffff)); put(&b,uint64(0)); put(&b,uint16(4)); put(&b,uint64(0)); b.WriteByte(8)
 b.WriteByte(1); b.WriteString("second.c"); b.WriteByte(0)
 r2:=die(&b,"root_b",[]byte{0x33,0x9a,0,0,0,0,0,0,0,0,0x31,0x22})
 p2:=die(&b,"add_three",[]byte{0x23,3}); b.WriteByte(0)
 info=append([]byte(nil),b.Bytes()...)
 binary.LittleEndian.PutUint64(info[firstEnd+4:],uint64(len(info)-firstEnd-12))
 binary.LittleEndian.PutUint32(info[int(r1)+1+len("root_a")+1+1+2:],uint32(p1))
 binary.LittleEndian.PutUint64(info[int(r2)+1+len("root_b")+1+1+2:],p1)
 return Material{info,a,r1,p1,r2,p2}
}

func (m Material) ELF() []byte {
 names:=[]byte("\x00.shstrtab\x00.debug_abbrev\x00.debug_info\x00")
 data:=[][]byte{nil,names,m.Abbrev,m.Info}; nameOff:=[]uint32{0,1,11,25}
 var b bytes.Buffer; b.Write(make([]byte,64)); offs:=make([]uint64,4)
 for i:=1;i<4;i++ { offs[i]=uint64(b.Len()); b.Write(data[i]) }
 for b.Len()%8!=0 {b.WriteByte(0)}; shoff:=uint64(b.Len())
 b.Write(make([]byte,64))
 for i:=1;i<4;i++ {
  put(&b,nameOff[i]); kind:=uint32(1);if i==1 {kind=3};put(&b,kind)
  put(&b,uint64(0));put(&b,uint64(0));put(&b,offs[i]);put(&b,uint64(len(data[i])))
  put(&b,uint32(0));put(&b,uint32(0));put(&b,uint64(1));put(&b,uint64(0))
 }
 out:=b.Bytes();copy(out,[]byte{0x7f,'E','L','F',2,1,1})
 binary.LittleEndian.PutUint16(out[16:],2);binary.LittleEndian.PutUint16(out[18:],62)
 binary.LittleEndian.PutUint32(out[20:],1);binary.LittleEndian.PutUint64(out[40:],shoff)
 binary.LittleEndian.PutUint16(out[52:],64);binary.LittleEndian.PutUint16(out[58:],64)
 binary.LittleEndian.PutUint16(out[60:],4);binary.LittleEndian.PutUint16(out[62:],1)
 return out
}

// CallsMaterial describes the synthetic DWARF built by BuildCalls.
type CallsMaterial struct {
 Info, Abbrev []byte
 CU1, CU2     uint64            // offsets of the two compilation unit headers in Info
 Offsets      map[string]uint64 // absolute offsets of the named DIEs in Info
}

// BuildCalls builds debug_info and debug_abbrev contents exercising
// DW_OP_call2, DW_OP_call4 and DW_OP_call_ref. The first compilation unit is
// DWARF32, the second DWARF64, so DW_OP_call_ref operands have different
// widths; both units contain a procedure at the same unit-relative offset.
func BuildCalls() CallsMaterial {
 // compile unit with a name; procedure with name and exprloc location;
 // procedure with name only; procedure with a location list.
 abbrev:=[]byte{
  1,0x11,1,3,8,0,0,
  2,0x36,0,3,8,2,0x18,0,0,
  3,0x36,0,3,8,0,0,
  4,0x36,0,3,8,2,0x17,0,0,
  0,
 }
 b:=&callsBuilder{offs:map[string]uint64{}}

 // first compilation unit: DWARF32
 cu1:=uint64(b.info.Len())
 b.cu=cu1
 b.info.Write([]byte{0,0,0,0,4,0,0,0,0,0,8})
 b.compileUnit("a.c")
 b.die("mainA",func(w *exprWriter){ // lit7; call4 addTwo; lit2; mul
  w.op(0x37); w.callRel(0x99,4,"addTwo"); w.op(0x32,0x1e)
 })
 b.die("addTwo",func(w *exprWriter){ w.op(0x23,2) }) // plus_uconst 2
 b.die("pieceReg",func(w *exprWriter){ w.op(0x55) }) // reg5
 b.die("mainPiece",func(w *exprWriter){ // call4 pieceReg; piece 4; lit9; piece 4
  w.callRel(0x99,4,"pieceReg"); w.op(0x93,4,0x39,0x93,4)
 })
 b.die("stackVal",func(w *exprWriter){ w.op(0x35,0x9f) }) // lit5; stack_value
 b.die("mainStackVal",func(w *exprWriter){ // call4 stackVal; piece 8
  w.callRel(0x99,4,"stackVal"); w.op(0x93,8)
 })
 b.die("doubleCall",func(w *exprWriter){ // lit1; call4 addTwo; call4 addTwo
  w.op(0x31); w.callRel(0x99,4,"addTwo"); w.callRel(0x99,4,"addTwo")
 })
 b.die("refCaller32",func(w *exprWriter){ // lit10; call_ref addTwo (4-byte operand)
  w.op(0x3a); w.callAbs(0x9a,4,"addTwo")
 })
 b.die("call2Caller",func(w *exprWriter){ // lit6; call2 addTwo
  w.op(0x36); w.callRel(0x98,2,"addTwo")
 })
 b.dieEmpty("emptyProc")
 b.die("mainEmpty",func(w *exprWriter){ // lit4; call4 emptyProc; lit1; plus
  w.op(0x34); w.callRel(0x99,4,"emptyProc"); w.op(0x31,0x22)
 })
 b.dieLocList("locProc")
 b.die("mainLoc",func(w *exprWriter){ w.callRel(0x99,4,"locProc") })
 b.die("selfRec",func(w *exprWriter){ w.callRel(0x98,2,"selfRec") }) // call2 itself
 b.die("truncCall",func(w *exprWriter){ w.op(0x96,0x99,0x05) }) // nop; call4 with truncated operand
 b.die("badTarget",func(w *exprWriter){ w.callRelAdd(0x99,4,"addTwo",1) }) // middle of addTwo
 b.die("badBranch",func(w *exprWriter){ w.op(0x2f,0x64,0x00) }) // skip +100, out of bounds
 b.die("loopForever",func(w *exprWriter){ w.op(0x2f,0xfd,0xff) }) // skip -3, infinite loop
 b.die("mainSameA",func(w *exprWriter){ // lit5; call4 <same relative offset as mainSameB>
  w.op(0x35); w.callRel(0x99,4,"addTwo")
 })
 b.die("chainA",func(w *exprWriter){ w.callRel(0x99,4,"addTwo") })
 b.nullDIE()
 cu1End:=uint64(b.info.Len())
 binary.LittleEndian.PutUint32(b.info.Bytes()[cu1:],uint32(cu1End-cu1-4))

 // second compilation unit: DWARF64
 cu2:=uint64(b.info.Len())
 b.cu=cu2
 b.info.Write([]byte{0xff,0xff,0xff,0xff})
 b.info.Write(make([]byte,8)) // unit length, patched below
 b.info.Write([]byte{4,0})    // DWARF version 4
 b.info.Write(make([]byte,8)) // debug_abbrev offset
 b.info.WriteByte(8)          // address size
 b.compileUnit("b.c")
 // procB sits at the same unit-relative offset as addTwo in the first unit
 b.pad(cu2+(b.offs["addTwo"]-cu1))
 b.die("procB",func(w *exprWriter){ w.op(0x23,30) }) // plus_uconst 30
 b.die("mainSameB",func(w *exprWriter){ // lit5; call4 <same relative offset as mainSameA>
  w.op(0x35); w.callRel(0x99,4,"procB")
 })
 b.die("mainB",func(w *exprWriter){ // lit3; call_ref chainA (8-byte operand)
  w.op(0x33); w.callAbs(0x9a,8,"chainA")
 })
 b.nullDIE()
 binary.LittleEndian.PutUint64(b.info.Bytes()[cu2+4:],uint64(b.info.Len())-cu2-12)

 for _,p:=range b.patches {
  v:=b.offs[p.ref]-p.rel+p.add
  switch p.sz {
  case 2: binary.LittleEndian.PutUint16(b.info.Bytes()[p.at:],uint16(v))
  case 4: binary.LittleEndian.PutUint32(b.info.Bytes()[p.at:],uint32(v))
  case 8: binary.LittleEndian.PutUint64(b.info.Bytes()[p.at:],v)
  default: panic("bad patch size")
  }
 }
 return CallsMaterial{Info:b.info.Bytes(),Abbrev:abbrev,CU1:cu1,CU2:cu2,Offsets:b.offs}
}

type callsPatch struct {
 at  uint64 // absolute offset of the operand in info
 sz  int    // operand size in bytes
 ref string // referenced DIE name
 rel uint64 // subtracted from the target offset (CU base for relative calls)
 add uint64 // added to the operand value
}

type callsBuilder struct {
 info    bytes.Buffer
 cu      uint64 // base of the compilation unit being written
 offs    map[string]uint64
 patches []callsPatch
}

func (b *callsBuilder) emit(bytes ...byte) uint64 {
 off:=uint64(b.info.Len()); b.info.Write(bytes); return off
}

// compileUnit writes the compile_unit DIE (abbrev code 1).
func (b *callsBuilder) compileUnit(name string) {
 b.emit(1); b.info.WriteString(name); b.info.WriteByte(0)
}

// die writes a procedure DIE (abbrev code 2) whose location expression is
// built by expr.
func (b *callsBuilder) die(name string, expr func(w *exprWriter)) uint64 {
 off:=b.emit(2); b.offs[name]=off
 b.info.WriteString(name); b.info.WriteByte(0)
 lenPos:=uint64(b.info.Len()); b.info.WriteByte(0) // exprloc length, patched below
 start:=uint64(b.info.Len())
 expr(&exprWriter{b:b})
 n:=uint64(b.info.Len())-start
 if n>127 { panic("fixture expr too long") }
 b.info.Bytes()[lenPos]=byte(n)
 return off
}

// dieEmpty writes a procedure DIE without a location (abbrev code 3), a
// legal empty subprogram.
func (b *callsBuilder) dieEmpty(name string) uint64 {
 off:=b.emit(3); b.offs[name]=off
 b.info.WriteString(name); b.info.WriteByte(0)
 return off
}

// dieLocList writes a procedure DIE whose location is a loclistptr (abbrev
// code 4).
func (b *callsBuilder) dieLocList(name string) uint64 {
 off:=b.emit(4); b.offs[name]=off
 b.info.WriteString(name); b.info.WriteByte(0)
 b.info.Write([]byte{0,0,0,0})
 return off
}

// pad writes name-only filler DIEs until the absolute offset target.
func (b *callsBuilder) pad(target uint64) {
 for uint64(b.info.Len())<target {
  need:=target-uint64(b.info.Len())
  if need<2 { panic("cannot pad 1 byte") }
  b.emit(3); b.info.WriteString(strings.Repeat("p",int(need)-2)); b.info.WriteByte(0)
 }
}

// nullDIE terminates the children of the current compilation unit.
func (b *callsBuilder) nullDIE() { b.emit(0) }

type exprWriter struct{ b *callsBuilder }

func (w *exprWriter) op(bytes ...byte) { w.b.info.Write(bytes) }

// callRel writes a call opcode whose operand is relative to the current
// compilation unit (DW_OP_call2, DW_OP_call4).
func (w *exprWriter) callRel(opcode byte, sz int, ref string) {
 w.call(opcode,sz,ref,w.b.cu,0)
}

// callAbs writes a DW_OP_call_ref opcode, whose operand is an absolute
// offset into debug_info.
func (w *exprWriter) callAbs(opcode byte, sz int, ref string) {
 w.call(opcode,sz,ref,0,0)
}

// callRelAdd is like callRel but adds add to the operand, to build invalid
// targets.
func (w *exprWriter) callRelAdd(opcode byte, sz int, ref string, add uint64) {
 w.call(opcode,sz,ref,w.b.cu,add)
}

func (w *exprWriter) call(opcode byte, sz int, ref string, rel, add uint64) {
 w.b.info.WriteByte(opcode)
 at:=uint64(w.b.info.Len())
 w.b.info.Write(make([]byte,sz))
 w.b.patches=append(w.b.patches,callsPatch{at:at,sz:sz,ref:ref,rel:rel,add:add})
}
