package main
import("encoding/binary";"fmt";p "github.com/saferwall/pe")
func main(){
 b:=make([]byte,0x400);w16:=func(o int,v uint16){binary.LittleEndian.PutUint16(b[o:],v)};w32:=func(o int,v uint32){binary.LittleEndian.PutUint32(b[o:],v)}
 copy(b,"MZ");w32(0x3c,0x80);copy(b[0x80:],[]byte{'P','E',0,0});w16(0x84,0xaa64);w16(0x86,1);w16(0x94,240);w16(0x96,0x2022)
 o:=0x98;w16(o,0x20b);w32(o+16,0x1000);w32(o+20,0x1000);binary.LittleEndian.PutUint64(b[o+24:],0x140000000);w32(o+32,0x1000);w32(o+36,0x200);w32(o+56,0x2000);w32(o+60,0x200);w16(o+68,3);w32(o+108,16);w32(o+112+3*8,0x1000);w32(o+116+3*8,8)
 s:=o+240;copy(b[s:],".pdata");w32(s+8,0x200);w32(s+12,0x1000);w32(s+16,0x200);w32(s+20,0x200);w32(s+36,0x40000040)
 w32(0x200,0x1100);w32(0x204,1|(16<<2)|(2<<23))
 f,e:=p.NewBytes(b,&p.Options{Fast:true});if e!=nil{panic(e)};defer f.Close();if e=f.Parse();e!=nil{panic(e)};off:=f.GetOffsetFromRva(0x1000);if off!=0x200{panic("RVA mapping")};fmt.Printf("PE_ARM64_HEADER_OK file_offset=%#x sections=%d\n",off,len(f.Sections));if e=f.ParseDataDirectories();e!=nil{panic(e)};fmt.Printf("EXISTING_EXCEPTION_ROWS=%d directory_bytes=8\n",len(f.Exceptions))
}
