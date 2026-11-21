package main
import (
 "fmt"
 "math"
 "strings"
 "github.com/fogleman/ln/ln"
)
func main() {
 m:=ln.NewMesh([]*ln.Triangle{ln.NewTriangle(ln.Vector{-1,-1,0},ln.Vector{1,-1,0},ln.Vector{0,1,0})})
 s:=&ln.Scene{}; s.Add(m); s.Compile()
 h:=s.Intersect(ln.Ray{ln.Vector{0,0,2},ln.Vector{0,0,-2}})
 if !h.Ok() || math.Abs(h.T-1)>1e-12 || h.Shape!=m.Triangles[0] {panic("nonunit triangle hit")}
 if s.Visible(ln.Vector{0,0,2},ln.Vector{0,0,-1}) {panic("occlusion")}
 p:=s.Render(ln.Vector{0,0,3},ln.Vector{},ln.Vector{0,1,0},64,64,45,0.1,10,0.05)
 svg:=p.ToSVG(64,64)
 if len(p)==0 || !strings.Contains(svg,"<polyline") {panic("render paths")}
 fmt.Printf("BASELINE mesh_t=%.6f paths=%d svg_bytes=%d OCCLUDED=true\n",h.T,len(p),len(svg))
}
