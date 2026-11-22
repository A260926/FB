package main

import (
 "bytes"
 "crypto/sha256"
 "fmt"
 "io"
 "os"
 "github.com/Eyevinn/mp4ff/mp4"
)
func must(e error) { if e != nil { panic(e) } }
func main() {
 path := "mp4/testdata/bbb_prog_10s.mp4"
 if len(os.Args)>1 {path=os.Args[1]}
 input,e:=os.Open(path);must(e);defer input.Close()
 f,e:=mp4.DecodeFile(input);must(e)
 if f.Moov==nil || len(f.Moov.Traks)!=2 {panic("expected dual track movie")}
 for _,t:=range f.Moov.Traks {
  n:=t.GetNrSamples();if n<6 {panic("missing production samples")}
  sm,e:=t.GetSampleData(2,6);must(e)
  ranges,e:=t.GetRangesForSampleInterval(2,6);must(e)
  var payload []byte
  for _,r:=range ranges {_,e=input.Seek(int64(r.Offset),io.SeekStart);must(e);b:=make([]byte,r.Size);_,e=io.ReadFull(input,b);must(e);payload=append(payload,b...)}
  size:=0;for _,s:=range sm {size+=int(s.Size)}
  if size!=len(payload)||size==0 {panic("real sample payload mismatch")}
  cb,e:=mp4.CloneBox(t);must(e);ct:=cb.(*mp4.TrakBox)
  if ct.GetNrSamples()!=n || ct==t {panic("clone mismatch")}
  var b bytes.Buffer;must(ct.Encode(&b)); box,e:=mp4.DecodeBox(0,bytes.NewReader(b.Bytes()));must(e)
  if box.(*mp4.TrakBox).GetNrSamples()!=n {panic("sample table roundtrip mismatch")}
  edits:=0;if t.Edts!=nil {for _,el:=range t.Edts.Elst {edits+=len(el.Entries)}}
  fmt.Printf("RAW_TRACK id=%d samples=%d edits=%d read_samples=5 bytes=%d sha256=%x\n",t.Tkhd.TrackID,n,edits,len(payload),sha256.Sum256(payload))
 }
 fmt.Println("BASELINE_REAL_SAMPLE_READ_AND_TABLE_ROUNDTRIP_OK")
}
