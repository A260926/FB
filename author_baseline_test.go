package vorbis
import("encoding/json";"encoding/binary";"fmt";"math";"os";"testing")
func TestAuthorBaseline(t *testing.T){
 var f struct{Headers [][]byte;Left,Right []byte};raw,err:=os.ReadFile("author/residue2.json");if err!=nil {t.Fatal(err)};if err=json.Unmarshal(raw,&f);err!=nil{t.Fatal(err)}
 var warm Decoder;for _,h:=range f.Headers{if err=warm.ReadHeader(h);err!=nil{t.Fatal(err)}};buf:=make([]float32,warm.BufferSize());warm.DecodeInto(f.Left,buf);allocs:=testing.AllocsPerRun(100,func(){if _,e:=warm.DecodeInto(f.Left,buf);e!=nil{panic(e)}});if allocs==0{t.Fatal("current allocation baseline changed")};fmt.Printf("BASELINE allocations=%.0f\n",allocs)
 for side,packet:=range map[string][]byte{"left":f.Left,"right":f.Right}{
  var d Decoder;for _,h:=range f.Headers{if err=d.ReadHeader(h);err!=nil{t.Fatal(err)}}
  out,err:=d.Decode(packet);if err!=nil||len(out)!=0{t.Fatalf("priming: %v %d",err,len(out))}
  out,err=d.Decode(packet);if err!=nil||len(out)!=64{t.Fatalf("audio: %v %d",err,len(out))}
  ref,err:=os.ReadFile("author/"+side+".f32");if err!=nil||len(ref)!=256{t.Fatalf("ref: %v %d",err,len(ref))}
  var energy,want float64;for i,v:=range out{energy+=float64(v*v);r:=math.Float32frombits(binary.LittleEndian.Uint32(ref[4*i:]));want+=float64(r*r);if side=="left"&&math.Abs(float64(v-r))>1e-4{t.Fatalf("normal diff %d %g %g",i,v,r)}}
  if want<0.01{t.Fatal("silent independent reference")};if side=="right"&&energy!=0{t.Fatalf("current symptom changed: %g",energy)}
  if side=="left"&&energy<0.01{t.Fatal("normal silence")};fmt.Printf("BASELINE %s frames=32 actual_energy=%.6f reference_energy=%.6f\n",side,energy,want)
 }
}
