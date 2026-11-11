package pt
import("fmt";"math";"math/rand";"testing")
func TestAbsorptionBaseline(t *testing.T){
 m:=ClearMaterial(1,0)
 inner:=NewSphere(Vector{},1,m)
 for _,radius:=range []float64{1,2}{
  s:=&Scene{Color:White}
  s.Add(NewTransformedShape(inner,Scale(Vector{radius,1,1})))
  s.Compile()
  ray:=Ray{Vector{-4,0,0},Vector{1,0,0}}
  h:=s.Intersect(ray);if !h.Ok(){t.Fatal("no entry")}
  hi:=h.Info(ray); exit:=s.Intersect(Ray{hi.Position.Add(Vector{1e-4,0,0}),ray.Direction})
  if !exit.Ok()||!exit.Info(ray).Inside {t.Fatal("no exit")}
  sample:=NewSampler(1,8);sample.DirectLighting=false
  c:=sample.Sample(s,ray,rand.New(rand.NewSource(19)))
  if math.Abs(c.R-1)>1e-9||math.Abs(c.G-1)>1e-9||math.Abs(c.B-1)>1e-9 {t.Fatal(c)}
  fmt.Printf("CLEAR_PATH radius=%.0f entry=%.4f rgb=%.6f,%.6f,%.6f\n",radius,h.T,c.R,c.G,c.B)
 }
 fmt.Println("ABSORPTION_BASELINE_OK")
}
