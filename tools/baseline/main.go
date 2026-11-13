package main
import("bytes";"encoding/binary";"fmt";u "github.com/linuxboot/fiano/pkg/uefi";"github.com/linuxboot/fiano/pkg/guid")
func main(){
 data:=bytes.Repeat([]byte{0xff},256)
 put:=func(off int,v interface{}){var b bytes.Buffer;if e:=binary.Write(&b,binary.LittleEndian,v);e!=nil{panic(e)};copy(data[off:],b.Bytes())}
 put(0,u.FirmwareVolumeFixedHeader{FileSystemGUID:*u.FFS2,Length:256,Signature:0x4856465f,Attributes:0x800,HeaderLen:72,Revision:2});put(56,[]u.Block{{Count:1,Size:256},{}})
 var sum uint16;for i:=0;i<72;i+=2{sum+=binary.LittleEndian.Uint16(data[i:])};binary.LittleEndian.PutUint16(data[50:],0-sum)
 put(72,u.FileHeader{GUID:*guid.MustParse("11111111-2222-3333-4444-555555555555"),Checksum:u.IntegrityCheck{File:0xaa},Type:u.FVFileTypeDriver,Size:u.ThreeUint8{32,0,0},State:0xf8})
 var check byte;for i:=72;i<96;i++{if i!=89&&i!=95{check+=data[i]}};data[88]=0-check
 copy(data[96:],[]byte{8,0,0,byte(u.SectionTypeRaw),'D','A','T','A'})
 fv,err:=u.NewFirmwareVolume(data,0x100,true);if err!=nil{panic(err)};if len(fv.Files)!=1||len(fv.Files[0].Sections)!=1{panic("tree mismatch")}
 fmt.Printf("FIRMWARE_TREE_OK fv_offset=%#x files=%d sections=%d payload=%s\n",fv.FVOffset,len(fv.Files),len(fv.Files[0].Sections),fv.Files[0].Sections[0].Buf()[4:]);fmt.Println("LOCAL_OFFSETS_ONLY file_header=24 section_header=4")
}
