package main
import (
 "fmt"
 "math"
 "sync"
 "github.com/deadsy/sdfx/render/dc"
 "github.com/deadsy/sdfx/sdf"
 "github.com/deadsy/sdfx/render"
 v3 "github.com/deadsy/sdfx/vec/v3"
)
func main() {
 s,e:=sdf.Sphere3D(1);if e!=nil {panic(e)}
 tris:=render.CollectTriangles(s,render.NewMarchingCubesUniform(18))
 if len(tris)<100 {panic("empty mesh")}
 for _,t:=range tris {for _,p:=range t {if math.Abs(p.Length()-1)>.03 {panic("surface drift")}}}
 b,e:=sdf.Box3D(v3.Vec{X:2,Y:3,Z:4},0);if e!=nil {panic(e)}
 if b.Evaluate(v3.Vec{})>=0 {panic("box sign")}
 fmt.Printf("BASELINE SDF mesh PASS triangles=%d\n",len(tris))
 var wg sync.WaitGroup;var raw []*sdf.Triangle3
 ch:=sdf.WriteTriangles(&wg,&raw)
 dc.NewDualContouringDefault(12).Render(s,ch);close(ch);wg.Wait()
 if len(raw)<100 {panic("empty dual contour")}
 for _,t:=range raw {for _,p:=range t {if math.IsNaN(p.X)||math.IsInf(p.X,0) {panic("nonfinite dual contour")}}}
 fmt.Printf("BASELINE legacy DC PASS triangles=%d\n",len(raw))
}
