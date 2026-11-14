// Package fixture builds a legal, inert ELF with two debug compilation units.
// It provides material only; it does not resolve or evaluate DWARF calls.
package fixture

import (
 "bytes"
 "encoding/binary"
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
