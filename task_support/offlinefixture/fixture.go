// Package offlinefixture builds transport fixtures, not timestamp conversion logic.
package offlinefixture
import("bytes";"fmt";"io";"net/http";"sync";"time"
 h "github.com/bluenviron/gohlslib/v2"
 "github.com/bluenviron/mediacommon/v2/pkg/formats/fmp4"
 "github.com/bluenviron/mediacommon/v2/pkg/formats/fmp4/seekablebuffer"
 mc "github.com/bluenviron/mediacommon/v2/pkg/formats/mp4/codecs")
type Result struct {PTS []int64; Payload []byte; Times []time.Time}
type memoryTransport struct{objects map[string][]byte}
func(m memoryTransport)RoundTrip(r *http.Request)(*http.Response,error){
 select{case <-r.Context().Done():return nil,r.Context().Err();default:}
 p,ok:=m.objects[r.URL.Path];if !ok{return nil,fmt.Errorf("unlisted object %s",r.URL.Path)}
 return &http.Response{StatusCode:200,Header:make(http.Header),Body:io.NopCloser(bytes.NewReader(p)),Request:r},nil
}
// Run uses fixed bytes and real Client parsing/callbacks; no socket or host files.
func Run(bases []uint64, reset bool, configure func(*h.Client)) (Result,error){
 var ib seekablebuffer.Buffer
 init:=fmp4.Init{Tracks:[]*fmp4.InitTrack{{ID:1,TimeScale:48000,Codec:&mc.Opus{ChannelCount:2}}}}
 if e:=init.Marshal(&ib);e!=nil{return Result{},e}
 objects:=map[string][]byte{"/init.mp4":append([]byte(nil),ib.Bytes()...)}
 pl:="#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-TARGETDURATION:1\n#EXT-X-MEDIA-SEQUENCE:7\n#EXT-X-MAP:URI=\"init.mp4\"\n#EXT-X-DISCONTINUITY-SEQUENCE:5\n"
 t0:=time.Date(2020,1,2,3,4,5,0,time.UTC)
 for i,base:=range bases{
  var pb seekablebuffer.Buffer
  part:=fmp4.Part{SequenceNumber:uint32(i+1),Tracks:[]*fmp4.PartTrack{{ID:1,BaseTime:base,Samples:[]*fmp4.Sample{{Duration:48,Payload:[]byte{byte(0x40+i)}}}}}}
  if e:=part.Marshal(&pb);e!=nil{return Result{},e};name:=fmt.Sprintf("/s%d.mp4",i);objects[name]=append([]byte(nil),pb.Bytes()...)
  if reset&&i>0{pl+="#EXT-X-DISCONTINUITY\n"}
  pl+="#EXT-X-PROGRAM-DATE-TIME:"+t0.Add(time.Duration(i)*time.Millisecond).Format(time.RFC3339Nano)+"\n#EXTINF:0.001,\n"+name[1:]+"\n"
 };pl+="#EXT-X-ENDLIST\n";objects["/main.m3u8"]=[]byte(pl)
 c:=&h.Client{URI:"http://offline.invalid/main.m3u8",HTTPClient:&http.Client{Transport:memoryTransport{objects}}};if configure!=nil{configure(c)}
 var mu sync.Mutex;var result Result
 c.OnTracks=func(tracks []*h.Track)error{if len(tracks)!=1{return fmt.Errorf("tracks=%d",len(tracks))};tr:=tracks[0];c.OnDataOpus(tr,func(pts int64,p [][]byte){mu.Lock();defer mu.Unlock();result.PTS=append(result.PTS,pts);if len(p)!=1||len(p[0])!=1{result.Payload=append(result.Payload,0)}else{result.Payload=append(result.Payload,p[0][0])};at,ok:=c.AbsoluteTime(tr);if !ok{at=time.Time{}};result.Times=append(result.Times,at)});return nil}
 if e:=c.Start();e!=nil{return Result{},e};e:=c.Wait2();c.Close();if e!=h.ErrClientEOS{return Result{},e};mu.Lock();defer mu.Unlock();return result,nil
}
