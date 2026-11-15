package main
import (
 "fmt"
 "math"
 "github.com/deadsy/sdfx/sdf"
 "github.com/deadsy/sdfx/render"
)
func main() {
 s,err:=sdf.Sphere3D(1); if err!=nil {panic(err)}
 tris:=render.CollectTriangles(s,render.NewMarchingCubesOctreeSingle(20))
 if len(tris)<100 {panic("empty production mesh")}
 vol:=0.0
 for _,t:=range tris {
  n:=t[1].Sub(t[0]).Cross(t[2].Sub(t[0])); a:=n.Length()
  if math.IsNaN(a)||math.IsInf(a,0)||a<1e-14 {panic("invalid triangle")}
  if n.Dot(t[0].Add(t[1]).Add(t[2]))<=0 {panic("wrong sphere orientation")}
  vol+=t[0].Dot(t[1].Cross(t[2]))/6
 }
 boundary:=render.CountBoundaryEdges(tris)
 if boundary!=0||vol<3||vol>5 {panic("invalid sphere mesh")}
 fmt.Printf("SDFX_CPU_BASELINE triangles=%d boundary=%d volume=%.8f\n",len(tris),boundary,vol)
}
