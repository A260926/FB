package main
import (
 "fmt"
 "math"
 "github.com/gopxl/beep/v2"
)
type closer struct{beep.StreamSeeker}
func(*closer)Close()error{return nil}
func clip(v float64)beep.TimelineClip{return beep.TimelineClip{Open:func()(beep.StreamSeekCloser,error){b:=beep.NewBuffer(beep.Format{SampleRate:8000,NumChannels:2,Precision:2});b.Append(beep.Take(8,beep.StreamerFunc(func(s [][2]float64)(int,bool){for i:=range s{s[i]=[2]float64{v,-v}};return len(s),true})));return &closer{b.Streamer(0,8)},nil},Start:0,In:0,Out:8,Duration:8,Gain:1}}
func main(){t,e:=beep.NewTimeline(8000,[]beep.TimelineClip{clip(.25)});must(e);defer t.Close();must(t.Schedule([]beep.TimelineClip{clip(.5)},4,0));yes,e:=t.Cancel();must(e);if !yes||t.Len()!=8{panic("probe cancellation")};var s [2][2]float64;n,ok:=t.Stream(s[:]);if n!=2||!ok||t.Position()!=2||t.Err()!=nil{panic("probe normal stream")};for _,v:=range s{if math.Abs(v[0]-.25)>1e-4||math.Abs(v[1]+.25)>1e-4{panic("probe PCM")}};fmt.Println("TIMELINE_API_NORMAL_OK")}
func must(e error){if e!=nil{panic(e)}}
