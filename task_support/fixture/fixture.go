// Package fixture constructs metadata-only PE images. No bytes are executed.
package fixture
import (
 "bytes"
 "encoding/binary"
 pe "github.com/saferwall/pe"
)
func Image() []byte {
 b:=make([]byte,0xe00)
 p16:=func(o int,v uint16){binary.LittleEndian.PutUint16(b[o:],v)}
 p32:=func(o int,v uint32){binary.LittleEndian.PutUint32(b[o:],v)}
 p64:=func(o int,v uint64){binary.LittleEndian.PutUint64(b[o:],v)}
 put:=func(o int,v interface{}){var x bytes.Buffer;if err:=binary.Write(&x,binary.LittleEndian,v);err!=nil{panic(err)};copy(b[o:],x.Bytes())}
 p16(0,0x5a4d);p32(0x3c,0x80);p32(0x80,0x4550)
 p16(0x84,0xaa64);p16(0x86,3);p16(0x94,240);p16(0x96,0x2022)
 p16(0x98,0x20b);p32(0xa8,0x1000);p64(0xb0,0x180000000)
 p32(0xb8,0x1000);p32(0xbc,0x200);p16(0xc0,6);p16(0xc8,6)
 p32(0xd0,0x4000);p32(0xd4,0x400);p16(0xdc,3);p32(0x104,16)
 p32(0x108,0x2200);p32(0x10c,0x80)
 lc:=pe.ImageLoadConfigDirectory64{DynamicValueRelocTableOffset:0x40,DynamicValueRelocTableSection:3}
 lc.Size=uint32(binary.Size(lc));put(0x600,lc)
 p32(0x108+10*8,0x2000);p32(0x10c+10*8,lc.Size)
 for i,s:=range []struct{name string;va,raw,size uint32}{{".text",0x1000,0x400,0x200},{".data",0x2000,0x600,0x400},{".meta",0x3000,0xa00,0x400}} {
  o:=0x188+i*40;copy(b[o:],s.name);p32(o+8,s.size);p32(o+12,s.va);p32(o+16,s.size);p32(o+20,s.raw);p32(o+36,0x40000040)
 }
 for i:=0;i<2;i++{
  rva:=uint32(0x2200+i*0x100);o:=0x800+i*0x100
  put(o,pe.ImageExportDirectory{Name:rva+0x50,Base:1,NumberOfFunctions:1,NumberOfNames:1,AddressOfFunctions:rva+0x28,AddressOfNames:rva+0x2c,AddressOfNameOrdinals:rva+0x30})
  p32(o+0x28,0x1000+uint32(i)*0x20);p32(o+0x2c,rva+0x60);p16(o+0x30,0)
  copy(b[o+0x50:],"synthetic.dll\x00");copy(b[o+0x60:],[]string{"NativeSymbol\x00","AlternateSymbol\x00"}[i])
 }
 p64(0x7f0,0x1122334455667788);p64(0x7f8,0xffffffffffffffff)
 // One symbol-6 record containing header and data page blocks.
 var blocks bytes.Buffer
 appendBlock:=func(page uint32,words []uint16){binary.Write(&blocks,binary.LittleEndian,page);binary.Write(&blocks,binary.LittleEndian,uint32(8+2*len(words)));binary.Write(&blocks,binary.LittleEndian,words)}
 appendBlock(0,[]uint16{0x5084,0x8664,0x90a8,0x1020,0,0x2108,64,0})
 appendBlock(0x2000,[]uint16{0xd1f0,0x0708,0x0506,0x0304,0x0102,0xc1f8})
 p32(0xa40,1);p32(0xa44,uint32(12+blocks.Len()));p64(0xa48,6);p32(0xa50,uint32(blocks.Len()));copy(b[0xa54:],blocks.Bytes())
 return b
}
