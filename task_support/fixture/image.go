// Package fixture builds inert Mach-O bytes; it never loads or executes an image.
package fixture

import (
 "bytes"
 "encoding/binary"
 "github.com/blacktop/go-macho/types"
)

// Image contains the source bytes and physical table positions for extensions.
type Image struct { Bytes []byte; Symoff, Stroff, Modoff, Tocoff, Refoff, Indoff uint32 }

// Build emits two same-named modules, two distinct same-named definitions,
// duplicate TOC rows, and reference windows in a different order from modules.
func Build(wide, big, dysymFirst bool) Image {
 var order binary.ByteOrder = binary.LittleEndian
 if big { order = binary.BigEndian }
 out:=Image{Bytes:make([]byte,0x900),Symoff:0x200,Stroff:0x300,Modoff:0x400,Tocoff:0x500,Refoff:0x600,Indoff:0x700}
 var strings bytes.Buffer
 strings.WriteByte(0)
 add:=func(s string) uint32 { p:=uint32(strings.Len());strings.WriteString(s);strings.WriteByte(0);return p }
 local:=add("same");def:=add("_same");need:=add("_need");module:=add("unit.o")
 copy(out.Bytes[out.Stroff:],strings.Bytes())
 sym:=types.SymtabCmd{LoadCmd:types.LC_SYMTAB,Len:24,Symoff:out.Symoff,Nsyms:6,Stroff:out.Stroff,Strsize:uint32(strings.Len())}
 dys:=types.DysymtabCmd{LoadCmd:types.LC_DYSYMTAB,Len:80,Ilocalsym:0,Nlocalsym:2,Iextdefsym:2,Nextdefsym:2,Iundefsym:4,Nundefsym:2,Tocoffset:out.Tocoff,Ntoc:3,Modtaboff:out.Modoff,Nmodtab:2,Extrefsymoff:out.Refoff,Nextrefsyms:3,Indirectsymoff:out.Indoff,Nindirectsyms:3}
 var loads bytes.Buffer
 if dysymFirst { binary.Write(&loads,order,dys);binary.Write(&loads,order,sym) } else { binary.Write(&loads,order,sym);binary.Write(&loads,order,dys) }
 h:=types.FileHeader{Magic:types.Magic32,CPU:types.CPUX86,Type:types.MH_DYLIB,NCommands:2,SizeCommands:uint32(loads.Len())}
 if wide { h.Magic=types.Magic64;h.CPU=types.CPUAmd64 }
 start:=h.Put(out.Bytes,order);copy(out.Bytes[start:],loads.Bytes())
 names:=[]uint32{local,local,def,def,need,need};values:=[]uint64{0x11,0x22,0x100,0x200,0,0}
 var nlists bytes.Buffer
 for i,n:=range names { binary.Write(&nlists,order,n);typ:=byte(2);if i>=2 {typ=3};if i>=4 {typ=1};nlists.WriteByte(typ);nlists.WriteByte(0);binary.Write(&nlists,order,uint16(0));if wide {binary.Write(&nlists,order,values[i])} else {binary.Write(&nlists,order,uint32(values[i]))} }
 copy(out.Bytes[out.Symoff:],nlists.Bytes())
 var mods bytes.Buffer
 for i:=0;i<2;i++ {
  refs,count:=uint32(1),uint32(2);if i==1 {refs=0;count=1}
  head:=[]uint32{module,uint32(2+i),1,refs,count,uint32(i),1,0,0,0,0}
  for _,v:=range head {binary.Write(&mods,order,v)}
  if wide {binary.Write(&mods,order,uint32(0));binary.Write(&mods,order,uint64(0))} else {binary.Write(&mods,order,uint32(0));binary.Write(&mods,order,uint32(0))}
 }
 copy(out.Bytes[out.Modoff:],mods.Bytes())
 for i,pair:=range [][2]uint32{{3,1},{2,0},{2,0}} {order.PutUint32(out.Bytes[int(out.Tocoff)+i*8:],pair[0]);order.PutUint32(out.Bytes[int(out.Tocoff)+i*8+4:],pair[1])}
 for i,pair:=range [][2]uint32{{2,1},{5,0},{4,1}} {raw:=pair[0]|pair[1]<<24;if big {raw=pair[0]<<8|pair[1]};order.PutUint32(out.Bytes[int(out.Refoff)+i*4:],raw)}
 for i,v:=range []uint32{2,types.INDIRECT_SYMBOL_LOCAL,5} {order.PutUint32(out.Bytes[int(out.Indoff)+i*4:],v)}
 return out
}
