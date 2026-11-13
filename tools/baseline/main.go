package main
import("bytes";"encoding/binary";"fmt"; m "github.com/blacktop/go-macho";"github.com/blacktop/go-macho/types")
func main(){
 var name,sect [16]byte;copy(name[:],"__TEXT");copy(sect[:],"__text")
 payload:=[]byte{0x7f,1,1,1,1,8,2,5,1,0,6,1,4}
 var loads bytes.Buffer
 for _,v:=range []interface{}{types.Segment64{LoadCmd:types.LC_SEGMENT_64,Len:152,Name:name,Addr:0x100000,Memsz:0x1000,Filesz:0x500,Nsect:1},types.Section64{Name:sect,Seg:name,Addr:0x100400,Size:16,Offset:0x400,Align:2,Flags:0x80000400},types.SegmentSplitInfoCmd{LoadCmd:types.LC_SEGMENT_SPLIT_INFO,Len:16,Offset:0x480,Size:uint32(len(payload))}}{if err:=binary.Write(&loads,binary.LittleEndian,v);err!=nil{panic(err)}}
 data:=make([]byte,0x500);h:=types.FileHeader{Magic:types.Magic64,CPU:types.CPUArm64,Type:types.MH_DYLIB,NCommands:2,SizeCommands:uint32(loads.Len())};n:=h.Put(data,binary.LittleEndian);copy(data[n:],loads.Bytes());copy(data[0x480:],payload)
 binary.LittleEndian.PutUint32(data[0x400:],0x90000000);binary.LittleEndian.PutUint32(data[0x404:],0x91102001);binary.LittleEndian.PutUint32(data[0x408:],0xd503201f);binary.LittleEndian.PutUint32(data[0x40c:],0xd65f03c0)
 f,err:=m.NewFile(bytes.NewReader(data));if err!=nil{panic(err)};defer f.Close();count:=0
 err=f.ForEachV2SplitSegReference(func(fs,fo,ts,to uint64,k types.SplitInfoKind){count++;fmt.Printf("REF %d:%#x -> %d:%#x kind=%d\n",fs,fo,ts,to,k)})
 if err!=nil||count!=2{panic(fmt.Sprint(err,count))};fmt.Printf("SPLIT_REFERENCE_BASELINE_OK refs=%d sections=%d\n",count,len(f.Sections))
}
