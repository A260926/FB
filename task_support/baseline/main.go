package main
import ("fmt";"math"; f "github.com/fogleman/fauxgl")
func main(){
 dc:=f.NewContext(32,32);dc.Cull=f.CullNone;dc.AlphaBlend=false
 dc.Shader=f.NewSolidColorShader(f.Identity(),f.Color{R:0.2,G:0.6,B:0.8,A:1})
 t:=f.NewTriangleForPoints(f.Vector{X:-1.4,Y:-0.7,Z:0},f.Vector{X:0.8,Y:-0.7,Z:0},f.Vector{X:0,Y:0.8,Z:0})
 info:=dc.DrawTriangles([]*f.Triangle{t});n:=0
 for _,d:=range dc.DepthBuffer {if d<math.MaxFloat64 {n++}}
 if n==0 || info.UpdatedPixels==0 {panic("production clipping/raster empty")}
 fmt.Printf("BASELINE raster_pixels=%d center_depth=%.3f\n",n,dc.DepthBuffer[16*32+16])
 if math.Abs(dc.DepthBuffer[16*32+16]-0.5)>1e-12 {panic("depth")}
}
