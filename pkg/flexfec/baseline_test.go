//go:build baseline_gap

package flexfec

import (
 "fmt"
 "testing"
 "github.com/pion/logging"
 "github.com/pion/rtp"
)

// This starting-point witness is deliberately not included in target mode.
func TestRankWitness(t *testing.T) {
 media := make([]rtp.Packet,3)
 for i:=range media { media[i]=rtp.Packet{Header:rtp.Header{Version:2,SSRC:77,PayloadType:96,SequenceNumber:uint16(101+i),Timestamp:90000+uint32(i*900)},Payload:[]byte{0x65,byte(10+i)}} }
 enc:=NewFlexEncoder03(49,88)
 rows:=[][]rtp.Packet{media[:2],media[1:],media}
 dec:=newFECDecoder(88,77,logging.NewDefaultLoggerFactory())
 recovered:=0
 for i,row:=range rows {
  packets:=enc.EncodeFec(row,1)
  if len(packets)!=1 {t.Fatalf("row %d packet count %d",i,len(packets))}
  f,err:=parseFlexFEC03Header(packets[0].Payload)
  if err!=nil {t.Fatal(err)}
  if len(decodeMask(uint64(f.mask0),15,f.seqNumBase))!=len(row) {t.Fatal("coverage mismatch")}
  wire,err:=packets[0].Marshal();if err!=nil {t.Fatal(err)}
  var parsed rtp.Packet;if err=parsed.Unmarshal(wire);err!=nil {t.Fatal(err)}
  recovered+=len(dec.DecodeFec(parsed))
 }
 if recovered!=0 {t.Fatalf("starting-point peeling behavior changed: recovered=%d",recovered)}
 fmt.Printf("BASELINE_FEC rank_witness equations=%d unknowns=%d recovered=%d\n",len(rows),len(media),recovered)
}
