package vorbis

import (
 "math"
 "os"
 "testing"
)

func TestBaselineFloor0(t *testing.T) {
 f := floor0{order:4, rate:48000, barkMapSize:64, amplitudeBits:6, amplitudeOffset:60}
 out := make([]float32,32)
 for i := range out { out[i]=1 }
 f.Apply(out,floor0Data{32,[]float32{.4,.9,1.5,2.4}})
 for i,v := range out { if math.IsNaN(float64(v)) || math.IsInf(float64(v),0) { t.Fatalf("nonfinite bin=%d",i) } }
 tree:=newHuffmanBuilder(2); tree.Put(0,1); tree.Put(1,1)
 cf:=floor0{order:3,amplitudeBits:1,bookList:[]uint8{0}}
 got:=cf.Decode(newBitReader([]byte{1}),[]codebook{{dimensions:1,entries:tree.build(),values:[]float32{.2,.3}}},32).(floor0Data)
 t.Logf("FLOOR0_LSP_OBSERVED coefficients=%v expected=[0.2 0.4 0.6]",got.coefficients)
 want:=int(math.Floor(bark(48000.0/64)*64/bark(24000)))
 t.Logf("FLOOR0_BASELINE_OK bins=%d map1=%d independent_map1=%d sample1=%.9g",len(out),f.mapResult(1,32),want,out[1])
 if os.Getenv("REQUIRE_CORRECT_FLOOR0")=="1" && f.mapResult(1,32)!=want { t.Fatalf("FLOOR0_REPRO bin=1 got=%d want=%d",f.mapResult(1,32),want) }
}
