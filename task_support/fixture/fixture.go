// Package fixture constructs an inert Mach-O with one ordinary compact record.
package fixture
import (
 "bytes"
 "encoding/binary"
 "fmt"
 "github.com/blacktop/go-macho/types"
)
const Base uint64 = 0x100000000
func Normal() ([]byte,error) {
 const unwindOffset=0x800
 const unwindSize=0x2000
 const fileSize=unwindOffset+unwindSize
 var segname,textname,unwindname [16]byte
 copy(segname[:],"__TEXT");copy(textname[:],"__text");copy(unwindname[:],"__unwind_info")
 commandSize:=uint32(binary.Size(types.Segment64{})+2*binary.Size(types.Section64{}))
 header:=types.FileHeader{Magic:types.Magic64,CPU:types.CPUAmd64,Type:types.MH_EXECUTE,NCommands:1,SizeCommands:commandSize}
 buf:=bytes.NewBuffer(nil);hdr:=make([]byte,types.FileHeaderSize64);header.Put(hdr,binary.LittleEndian);buf.Write(hdr)
 values:=[]any{
 types.Segment64{LoadCmd:types.LC_SEGMENT_64,Len:commandSize,Name:segname,Addr:Base,Memsz:fileSize,Filesz:fileSize,Maxprot:7,Prot:5,Nsect:2},
 types.Section64{Name:textname,Seg:segname,Addr:Base+0x400,Size:0x40,Offset:0x400,Flags:types.SectionFlag(0x80000000)},
 types.Section64{Name:unwindname,Seg:segname,Addr:Base+unwindOffset,Size:unwindSize,Offset:unwindOffset},
 }
 for _,v:=range values {if err:=binary.Write(buf,binary.LittleEndian,v);err!=nil{return nil,err}}
 if buf.Len()>0x400{return nil,fmt.Errorf("headers too long")}
 raw:=make([]byte,fileSize);copy(raw,buf.Bytes());raw[0x400]=0xc3
 u:=raw[unwindOffset:];put:=func(p int,v uint32){binary.LittleEndian.PutUint32(u[p:],v)}
 put(0,1);put(4,28);put(8,1);put(12,32);put(16,0);put(20,32);put(24,2)
 put(28,0x02040000)
 put(32,0x400);put(36,0x1000);put(40,56)
 put(44,0x440);put(48,0);put(52,56)
 put(0x1000,2);binary.LittleEndian.PutUint16(u[0x1004:],8);binary.LittleEndian.PutUint16(u[0x1006:],1)
 put(0x1008,0x400);put(0x100c,0x02040000)
 return raw,nil
}
