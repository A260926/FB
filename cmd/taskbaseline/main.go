package main
import (
 "fmt"
 "math"
 "os"
 s "github.com/fogleman/simplify"
)
func main() {
 const n=17
 v:=func(x,y int)s.Vector {xx:=float64(x)/8-1; yy:=float64(y)/8-1; return s.Vector{xx,yy,0.15*math.Sin(xx*2)*math.Cos(yy*3)}}
 ts:=make([]*s.Triangle,0,512)
 for y:=0;y<n-1;y++ {for x:=0;x<n-1;x++ {a,b,c,d:=v(x,y),v(x+1,y),v(x+1,y+1),v(x,y+1);ts=append(ts,s.NewTriangle(a,b,c),s.NewTriangle(a,c,d))}}
 input:=s.NewMesh(ts); output:=input.Simplify(0.5)
 if len(input.Triangles)!=512 || len(output.Triangles)==0 || len(output.Triangles)>300 {fmt.Fprintln(os.Stderr,"production reduction failed");os.Exit(1)}
 for _,t:=range output.Triangles {for _,p:=range []s.Vector{t.V1,t.V2,t.V3}{if math.IsNaN(p.X)||math.IsInf(p.X,0)||math.IsNaN(p.Y)||math.IsInf(p.Y,0)||math.IsNaN(p.Z)||math.IsInf(p.Z,0){os.Exit(2)}}}
 fmt.Printf("QEM_BASELINE input=%d output=%d finite=true\n",len(input.Triangles),len(output.Triangles))
}
