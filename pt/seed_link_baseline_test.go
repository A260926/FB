package pt
import("testing";"math";"math/rand";"fmt")
func TestSeedLinkedBaseline(t *testing.T){
 near:=NewSphere(Vector{0,0,5},1,DiffuseMaterial(White))
 source:=NewSphere(Vector{},1,DiffuseMaterial(White))
 shifted:=NewTransformedShape(source,Translate(Vector{4,0,5}))
 sc:=&Scene{};sc.Add(near);sc.Add(shifted);sc.Compile()
 for _,x:=range []float64{0,4}{h:=sc.Intersect(Ray{Vector{x,0,0},Vector{0,0,1}});if !h.Ok()||math.Abs(h.T-4)>1e-10{t.Fatalf("shared geometry x=%g t=%g",x,h.T)}}
 fmt.Println("SEED_SHARED_GEOMETRY_OK")
 scene:=&Scene{};scene.Add(NewPlane(Vector{},Vector{0,0,1},DiffuseMaterial(White)))
 scene.Add(NewSphere(Vector{0,0,5},1,LightMaterial(White,1)));scene.Compile()
 sampler:=NewDirectSampler();sampler.SoftShadows=false
 color:=sampler.Sample(scene,Ray{Vector{0,0,1},Vector{0,0,-1}},rand.New(rand.NewSource(7)))
 expected:=1.0/24.0
 if math.Abs(color.R-expected)>1e-10||math.Abs(color.G-expected)>1e-10||math.Abs(color.B-expected)>1e-10{t.Fatalf("direct got=%+v expected=%g",color,expected)}
 fmt.Printf("SEED_DIRECT_RGB %.12f %.12f %.12f\n",color.R,color.G,color.B)
 // Query a real second compile without asserting the old capability gap forever.
 sc.Add(NewSphere(Vector{20,0,5},1,DiffuseMaterial(White)));sc.Compile()
 h:=sc.Intersect(Ray{Vector{20,0,0},Vector{0,0,1}})
 fmt.Printf("STARTING_RECOMPILE added_visible=%t\n",h.Ok())
 fmt.Println("BASELINE_LINKING_START_OK")
}
