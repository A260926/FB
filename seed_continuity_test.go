package astits_test

import (
 "bytes"
 "context"
 "testing"
 astits "github.com/asticode/go-astits"
)

// Hand-built TS/PES: the fixture does not use the library muxer.
func seedTSPacket(cc byte, start bool, payload []byte) []byte {
 p:=bytes.Repeat([]byte{0xff},188)
 p[0]=0x47; p[1]=0x01; if start {p[1]|=0x40}; p[2]=0x00
 p[3]=0x10|(cc&15); off:=4
 if len(payload)<184 {p[3]|=0x20; n:=183-len(payload); p[4]=byte(n); if n>0 {p[5]=0}; off=5+n}
 copy(p[off:],payload); return p
}
func seedTS(duplicate bool) ([]byte,[]byte) {
 data:=make([]byte,400); for i:=range data {data[i]=byte(i*7+11)}
 pes:=[]byte{0,0,1,0xe0,1,0x93,0x80,0,0} // 403 bytes after length
 pes=append(pes,data...)
 var out []byte
 for i,off:=0,0; off<len(pes); i,off=i+1,off+184 {
  end:=off+184; if end>len(pes){end=len(pes)}
  p:=seedTSPacket(byte(14+i),off==0,pes[off:end]);out=append(out,p...)
  if duplicate && i==1 {out=append(out,p...)}
 }
 return out,data
}
func seedPES(t *testing.T, raw []byte) [][]byte {
 t.Helper(); d:=astits.NewDemuxer(context.Background(),bytes.NewReader(raw),astits.DemuxerOptPacketSize(188))
 var got [][]byte
 for {v,err:=d.NextData();if err==astits.ErrNoMorePackets {break};if err!=nil{t.Fatal(err)};if v.PES!=nil{got=append(got,append([]byte(nil),v.PES.Data...))}}
 return got
}
func TestSeedContinuityClean(t *testing.T){raw,want:=seedTS(false);got:=seedPES(t,raw);if len(got)!=1 || !bytes.Equal(got[0],want){t.Fatalf("clean PES: count=%d, want one 400-byte payload",len(got))};t.Log("clean: one 400-byte PES, CC 14/15/0")}
func TestSeedContinuityDuplicate(t *testing.T){raw,want:=seedTS(true);got:=seedPES(t,raw);if len(got)!=1 || !bytes.Equal(got[0],want){t.Fatalf("middle TS packet repeated: count=%d, want one unchanged 400-byte PES",len(got))}}
