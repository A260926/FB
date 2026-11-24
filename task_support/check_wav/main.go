package main
import("encoding/binary";"fmt";"os";"github.com/gopxl/beep/v2/wav")
func main(){if len(os.Args)!=2{panic("one WAV path required")};raw,e:=os.ReadFile(os.Args[1]);must(e);if len(raw)<44||string(raw[:4])!="RIFF"||string(raw[8:12])!="WAVE"||binary.LittleEndian.Uint16(raw[34:36])!=16{panic("PCM16 WAV required")};f,e:=os.Open(os.Args[1]);must(e);r,format,e:=wav.Decode(f);must(e);defer r.Close();if format.NumChannels!=2{panic("stereo required")};var b [257][2]float64;count:=0;energy:=0.0;for{n,ok:=r.Stream(b[:]);if !ok{break};count+=n;for _,s:=range b[:n]{energy+=s[0]*s[0]+s[1]*s[1]}};must(r.Err());if count<256||energy==0{panic("empty/silent demo")};fmt.Printf("TIMELINE_DEMO_FRESH_OK frames=%d\n",count)}
func must(e error){if e!=nil{panic(e)}}
