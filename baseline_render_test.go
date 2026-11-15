package fauxgl
import("testing"; "math"; "fmt")
func TestBaselineRender(t *testing.T){
 dc:=NewContext(37,29);dc.Cull=CullNone;dc.AlphaBlend=false
 s:=NewPhongShader(Identity(),Vector{0,0,1},Vector{0,0,3});s.ObjectColor=Color{0.8,0.3,0.1,1};s.SpecularColor=Color{0,0,0,1};dc.Shader=s
 tri:=NewTriangleForPoints(Vector{-0.7,-0.6,0.2},Vector{0.75,-0.35,0.2},Vector{-0.1,0.8,0.2})
 mesh:=NewTriangleMesh([]*Triangle{tri});original:=*tri
 info:=dc.DrawMesh(mesh);written:=0
 for _,d:=range dc.DepthBuffer{if d!=math.MaxFloat64{if math.IsNaN(d)||math.IsInf(d,0){t.Fatal("invalid depth")};written++}}
 if written==0||info.UpdatedPixels==0||*tri!=original{t.Fatal("render or source invariant failed")}
 c:=dc.ColorBuffer.NRGBAAt(18,14);if c.A!=255||c.R==0{t.Fatalf("missing Phong pixel: %v",c)}
 // The original shared mesh can be reused by an independent Context.
 other:=NewContext(37,29);other.Cull=CullNone;other.AlphaBlend=false;other.Shader=s;again:=other.DrawMesh(mesh)
 if again.UpdatedPixels!=info.UpdatedPixels{t.Fatal("reused mesh changed")}
 fmt.Printf("CPU_RASTER pixels=%d phong=%d,%d,%d shared_mesh=unchanged\n",written,c.R,c.G,c.B)
}
