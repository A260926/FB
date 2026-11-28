package main
import("fmt";"math"; f "github.com/fogleman/fauxgl")
func main(){
 m:=f.AtlasMesh{Positions:[]f.Vector{{X:-0.8,Y:-0.8},{X:0.8,Y:-0.8},{X:0.8,Y:0.8},{X:-0.8,Y:0.8}},Faces:[]f.AtlasFace{{Position:[3]int{0,1,2},UV:[3]f.Vector{{},{X:1},{X:1,Y:1}}},{Position:[3]int{0,2,3},UV:[3]f.Vector{{},{X:1,Y:1},{Y:1}}}}}
 rgb:=make([]f.Color,32*32);for i:=range rgb {rgb[i]=f.Color{R:0.4,G:0.4,B:0.4,A:1}}
 s,e:=f.CaptureAtlasSource(m,f.AtlasView{Matrix:f.Identity(),Width:32,Height:32,RGB:rgb});if e!=nil {panic(e)}
 r,e:=f.ReconstructAtlas(m,[]*f.AtlasSource{s},f.AtlasOptions{Width:4,Height:4,SeamWeight:1,Tolerance:1e-7,MaxIterations:200});if e!=nil{panic(e)}
 n:=0;for i,v:=range r.Covered{if v{n++;if math.Abs(r.RGB[i].R-0.4/(1.0001))>1e-5 {panic("normal reconstruction")}}}
 if n==0 || r.RelativeResidual>1e-7{panic("empty/invalid reconstruction")};fmt.Println("API_PROBE normal_atlas")
}
