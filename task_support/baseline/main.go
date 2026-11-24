package main
import (
 "fmt"
 "math"
 "os"
 "github.com/gopxl/beep/v2"
 "github.com/gopxl/beep/v2/wav"
)
func main(){
 source:=beep.StreamerFunc(func(s [][2]float64)(int,bool){for i:=range s{s[i]=[2]float64{.125,-.25}};return len(s),true})
 b:=beep.NewBuffer(beep.Format{SampleRate:8000,NumChannels:2,Precision:2});b.Append(beep.Take(300,source))
 m:=new(beep.Mixer);m.KeepAlive(false);m.Add(b.Streamer(0,b.Len()),beep.ResampleRatio(1,2,b.Streamer(0,b.Len())))
 f,err:=os.CreateTemp("","beep-baseline-*.wav");must(err);defer os.Remove(f.Name())
 must(wav.Encode(f,m,beep.Format{SampleRate:8000,NumChannels:2,Precision:2}));_,err=f.Seek(0,0);must(err)
 d,format,err:=wav.Decode(f);must(err);defer d.Close();var all [512][2]float64;n,ok:=d.Stream(all[:]);if !ok||n!=300||format.NumChannels!=2{panic("bad WAV length/format")}
 energy:=0.0;for _,v:=range all[:n]{energy+=math.Abs(v[0])+math.Abs(v[1])};if energy<1{panic("silent render")}
 fmt.Printf("BEEP_BASELINE_OK frames=%d energy=%.6f rate=%d\n",n,energy,format.SampleRate)
}
func must(e error){if e!=nil{panic(e)}}
