package caf
import (
 "bytes"
 "crypto/sha256"
 "fmt"
 "os"
 "testing"
)
func TestProductionCAF(t *testing.T) {
 b,err:=os.ReadFile("samples/helenkane.caf");if err!=nil{t.Fatal(err)}
 var f File;if err=f.Decode(bytes.NewReader(b));err!=nil{t.Fatal(err)}
 var format *AudioFormat;var data *Data;var table *PacketTable
 for _,c:=range f.Chunks{switch v:=c.Contents.(type){case *AudioFormat:format=v;case *Data:data=v;case *PacketTable:table=v}}
 if format==nil||data==nil||table==nil||len(data.Data)==0||len(table.Entry)==0{t.Fatal("missing real audio chunks")}
 var total uint64;for _,n:=range table.Entry{if n==0{t.Fatal("zero packet")};total+=n}
 if total!=uint64(len(data.Data))||table.Header.NumberPackets!=int64(len(table.Entry)){t.Fatal("packet bytes do not cover audio")}
 var out bytes.Buffer;if err=f.Encode(&out);err!=nil{t.Fatal(err)}
 if !bytes.Equal(b,out.Bytes()){t.Fatal("original sample roundtrip changed bytes")}
 var reread File;if err=reread.Decode(bytes.NewReader(out.Bytes()));err!=nil{t.Fatal(err)}
 var second *Data;for _,c:=range reread.Chunks{if v,ok:=c.Contents.(*Data);ok{second=v}}
 if second==nil||!bytes.Equal(data.Data,second.Data){t.Fatal("actual compressed audio changed")}
 fmt.Printf("CAF_BASELINE codec=%s rate=%.0f channels=%d frames_per_packet=%d\n",format.FormatID[:],format.SampleRate,format.ChannelsPerPacket,format.FramesPerPacket)
 fmt.Printf("CAF_BASELINE packets=%d valid_frames=%d audio_bytes=%d packet_spans=OK\n",table.Header.NumberPackets,table.Header.NumberValidFrames,len(data.Data))
 fmt.Printf("CAF_BASELINE audio_sha256=%x exact_roundtrip=OK\n",sha256.Sum256(data.Data))
}
