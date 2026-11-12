package parser
import("bytes"; "encoding/binary"; "testing")
func TestBATBaseline(t *testing.T) {
 const mb=1024*1024
 image:=make([]byte,2*mb)
 binary.LittleEndian.PutUint64(image[0:8],uint64(1<<20)|6)
 copy(image[2*mb-4:],[]byte("EVID"))
 source:=bytes.NewReader(image)
 bat:=&Bat{StartOffset:0,EntrySize:8,TotalNumberOfEntries:2,EntriesPerChunk:4096,profile:NewVHDXProfile(),reader:source}
 disk:=&BatReader{BlockSize:mb,Size:2*mb,Reader:source,bat:bat}
 out:=make([]byte,8)
 n,err:=disk.ReadAt(out,mb-4)
 if err!=nil || n!=8 || !bytes.Equal(out,[]byte{'E','V','I','D',0,0,0,0}){t.Fatalf("n=%d err=%v data=%x",n,err,out)}
 t.Log("BAT_BASELINE_FULL_AND_HOLE_OK")
}
