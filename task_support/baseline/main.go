package main
import (
 "fmt"
 "math"
 "os"
 f "github.com/fogleman/fauxgl"
)
func main(){
 dc:=f.NewContext(16,12); dc.Cull=f.CullNone
 dc.Shader=f.NewSolidColorShader(f.Identity(), f.Color{R:0.2,G:0.6,B:0.9,A:1})
 tri:=f.NewTriangleForPoints(f.Vector{X:-0.73,Y:-0.67,Z:0.2},f.Vector{X:0.81,Y:-0.59,Z:0.2},f.Vector{X:-0.13,Y:0.86,Z:0.2})
 info:=dc.DrawTriangles([]*f.Triangle{tri})
 n:=0;for _,z:=range dc.DepthBuffer {if z<math.MaxFloat64 {if math.Abs(z-0.6)>1e-12 {panic("depth")};n++}}
 if n==0 || info.UpdatedPixels==0 {panic("no production pixels")}
 p:=dc.ColorBuffer.NRGBAAt(8,6);if p.R!=51 || p.G!=153 || p.B!=229 {fmt.Fprintln(os.Stderr,p);panic("color")}
 fmt.Printf("FAUXGL production raster: finite_pixels=%d updated=%d center=%d,%d,%d depth=0.6\n",n,info.UpdatedPixels,p.R,p.G,p.B)
}
