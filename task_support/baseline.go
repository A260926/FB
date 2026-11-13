package main
import("crypto/sha256";"fmt";"os"; m "github.com/yapingcat/gomedia/go-mpeg2")
func main(){x:=m.NewTSMuxer();v:=x.AddStream(m.TS_STREAM_H264);a:=x.AddStream(m.TS_STREAM_AAC);var packets [][]byte
x.OnPacket=func(p []byte){if len(p)!=188||p[0]!=0x47{panic("bad TS")};packets=append(packets,append([]byte(nil),p...))}
video:=make([]byte,700);copy(video,[]byte{0,0,0,1,0x65,0x88,0x84,0x21});for i:=8;i<len(video);i++{video[i]=0x55}
if x.Write(v,video,1000,960)!=nil||x.Write(a,[]byte{0xff,0xf1,0x4c,0x80,1,0x7f,0xfc,0x11},1000,1000)!=nil||x.Write(v,video,2080,2040)!=nil{os.Exit(2)}
var all []byte;pc:=0;for _,p:=range packets{all=append(all,p...);if p[3]&0x20!=0&&p[4]>=7&&p[5]&0x10!=0{pc++}}
fmt.Printf("legacy packets=%d PCR=%d media_DTS_gap_ms=1080 sha256=%x\n",len(packets),pc,sha256.Sum256(all));if len(packets)<6||pc!=2{os.Exit(3)}}
