package main
import("bytes";"compress/zlib";"fmt";"io";"github.com/at-wat/ebml-go";"github.com/at-wat/ebml-go/mkvcore")
type Encoding struct{ContentEncodingOrder uint64;ContentEncodingScope uint64;ContentEncodingType uint64;ContentCompression struct{ContentCompAlgo uint64;ContentCompSettings []byte}}
type Track struct{TrackNumber uint64;TrackType uint64;CodecID string;CodecPrivate []byte;ContentEncodings struct{ContentEncoding []Encoding}}
type Cluster struct{Timecode uint64;SimpleBlock []ebml.Block}
type Document struct{Segment struct{Tracks struct{TrackEntry []Track};Cluster []Cluster} `ebml:"Segment,size=unknown"`}
func check(e error){if e!=nil{panic(e)}}
func zip(b []byte)[]byte{var o bytes.Buffer;w:=zlib.NewWriter(&o);_,e:=w.Write(b);check(e);check(w.Close());return o.Bytes()}
func main(){
 a,b:=zip([]byte("one")),zip([]byte("two"));tr:=Track{TrackNumber:1,TrackType:2,CodecID:"A_TEST",CodecPrivate:[]byte("cfg")}
 e:=Encoding{ContentEncodingOrder:0,ContentEncodingScope:1};e.ContentCompression.ContentCompAlgo=3;e.ContentCompression.ContentCompSettings=[]byte("HEAD")
 f:=Encoding{ContentEncodingOrder:1,ContentEncodingScope:1};f.ContentCompression.ContentCompAlgo=0
 tr.ContentEncodings.ContentEncoding=[]Encoding{f,e}
 var doc Document;doc.Segment.Tracks.TrackEntry=[]Track{tr};doc.Segment.Cluster=[]Cluster{{Timecode:10,SimpleBlock:[]ebml.Block{{TrackNumber:1,Timecode:2,Keyframe:true,Lacing:ebml.LacingXiph,Data:[][]byte{a,b}}}}}
 var encoded bytes.Buffer;check(ebml.Marshal(&doc,&encoded));rs,err:=mkvcore.NewSimpleBlockReader(bytes.NewReader(encoded.Bytes()));check(err);if len(rs)!=1{panic("track count")};defer rs[0].Close()
 for i,want:=range [][]byte{a,b}{p,key,ts,err:=rs[0].Read();check(err);if !bytes.Equal(p,want)||!key||ts!=12{panic(fmt.Sprint("wrong raw frame ",i))}}
 _,_,_,err=rs[0].Read();if err!=io.EOF{panic(err)}
 fmt.Println("baseline: tracks=1 laced_frames=2 raw_compressed=true timestamp=12")
}
