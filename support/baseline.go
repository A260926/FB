package main
import (
 "fmt"
 "github.com/yapingcat/gomedia/go-mpeg2"
)
func main(){
 m:=mpeg2.NewTSMuxer(); var out [][]byte
 m.OnPacket=func(b []byte){out=append(out,append([]byte(nil),b...))}
 a:=m.AddStream(mpeg2.TS_STREAM_AAC)
 if err:=m.Write(a,[]byte{1,2,3,4},0,0);err!=nil{panic(err)}
 b:=m.AddStream(mpeg2.TS_STREAM_AAC)
 if err:=m.Write(b,[]byte{5,6,7,8},10,10);err!=nil{panic(err)}
 pmts,streams:=0,0
 for _,p:=range out{
  if len(p)!=188 || p[0]!=0x47{panic("bad TS packet")}
  pid:=int(p[1]&31)<<8|int(p[2]); if pid!=0x200{continue}
  pmts++; off:=4; if p[3]&0x20!=0{off+=1+int(p[4])};off+=1+int(p[off])
  sec:=p[off:]; sl:=int(sec[1]&15)<<8|int(sec[2]); end:=3+sl-4
  for i:=12+(int(sec[10]&15)<<8|int(sec[11]));i<end;{streams++;i+=5+(int(sec[i+3]&15)<<8|int(sec[i+4]))}
 }
 if pmts!=1||streams!=1{panic("legacy topology changed")}
 fmt.Printf("baseline: packets=%d pmt=%d advertised=%d added=%d\n",len(out),pmts,streams,b)
}
