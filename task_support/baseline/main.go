package main
import (
 "fmt"
 "math"
 "os"
 "github.com/fogleman/ln/ln"
)
func main() {
 var scene ln.Scene
 for i:=0; i<32; i++ { scene.Add(ln.NewSphere(ln.Vector{X:float64(i)*4},0.5)) }
 scene.Compile()
 ray:=ln.Ray{Origin:ln.Vector{X:12,Z:3}, Direction:ln.Vector{Z:-1}}
 hit:=scene.Intersect(ray)
 if !hit.Ok() || math.Abs(hit.T-2.5)>1e-10 { panic("production nearest mismatch") }
 if scene.Visible(ln.Vector{X:12,Z:3},ln.Vector{X:12,Z:-3}) { panic("occluder missing") }
 p:=scene.Render(ln.Vector{X:12,Y:2,Z:12},ln.Vector{X:12},ln.Vector{Y:1},96,96,45,0.1,100,0.2)
 if len(p)==0 { panic("empty render") }
 if err:=p.WriteToSVG(".task-build/baseline.svg",96,96); err!=nil {panic(err)}
 info,err:=os.Stat(".task-build/baseline.svg"); if err!=nil || info.Size()<100 {panic("svg missing")}
 fmt.Printf("BASELINE nearest=%.6f occluded=true paths=%d SVG_OK\n",hit.T,len(p))
}
