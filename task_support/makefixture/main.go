package main
import("context";"encoding/binary";"os";"path/filepath";astits "github.com/asticode/go-astits")
func seg(t byte,p []byte)[]byte{b:=[]byte{15,t,0,1,0,0};binary.BigEndian.PutUint16(b[4:],uint16(len(p)));return append(b,p...)}
func pes(parts ...[]byte)[]byte{b:=[]byte{32,0};for _,p:=range parts{b=append(b,p...)};return append(b,255)}
func main(){dir:=os.Args[1];if e:=os.MkdirAll(dir,0755);e!=nil{panic(e)}
 pcs:=seg(16,[]byte{0,4,1,0,0,1,0,1})
 rcs:=seg(17,[]byte{1,8,0,4,0,4,12,1,0,0,0,1,0,0,0,0})
 clut:=seg(18,[]byte{1,0,0,33,0,128,128,255,1,33,200,128,128,0,2,33,80,128,128,0})
 ods:=seg(19,[]byte{0,1,0,0,5,0,0,18,1,2,0,0})
 eods:=seg(128,nil)
 packets:=[][]byte{pes(pcs,rcs,clut),pes(ods,eods),pes(seg(18,[]byte{1,16,1,33,80,128,128,0}),eods),pes(seg(16,[]byte{0,16}),eods)}
 f,e:=os.Create(filepath.Join(dir,"basic.ts"));if e!=nil{panic(e)};defer f.Close();m:=astits.NewMuxer(context.Background(),f)
 if e=m.AddElementaryStream(astits.PMTElementaryStream{ElementaryPID:256,StreamType:astits.StreamTypePrivateData});e!=nil{panic(e)};m.SetPCRPID(256)
 for i,p:=range packets{if e=os.WriteFile(filepath.Join(dir,[]string{"pes0.bin","pes1.bin","pes2.bin","pes3.bin"}[i]),p,0644);e!=nil{panic(e)};at:=[]int64{90000,90000,270000,360000}[i];_,e=m.WriteData(&astits.MuxerData{PID:256,PES:&astits.PESData{Header:&astits.PESHeader{StreamID:astits.StreamIDPrivateStream1,OptionalHeader:&astits.PESOptionalHeader{MarkerBits:2,PTSDTSIndicator:2,PTS:&astits.ClockReference{Base:at}}},Data:p}});if e!=nil{panic(e)}}
}
