package trace

import (
 "fmt"
 "math"
 "math/rand"
 "testing"
 "github.com/peterstace/grayt/colour"
 "github.com/peterstace/grayt/scene"
 "github.com/peterstace/grayt/xmath"
)

func TestBaseline(t *testing.T) {
 proto := scene.Scene{Objects: []scene.Object{{Surface: scene.Surface{Spheres: []scene.Sphere{{Center: xmath.Vect(0,0,-4), Radius:1}}}, Material: scene.Material{Colour:colour.Colour{R:0.2,G:0.4,B:0.8},Emittance:2}}}}
 _, objs := buildScene(proto)
 grid, list := newGrid(4,objs), newListAccelerationStructure(objs)
 tr := newTracer(grid,rand.New(rand.NewSource(19)))
 hits:=0
 var energy float64
 for y:=-2;y<=2;y++ { for x:=-2;x<=2;x++ {
  ray:=xmath.Ray{Start:xmath.Vect(0,0,0),Dir:xmath.Vect(float64(x)*0.1,float64(y)*0.1,-1).Unit()}
  hg,mg,gok:=grid.closestHit(ray); hl,ml,lok:=list.closestHit(ray)
  if gok!=lok || (gok && (math.Abs(hg.distance-hl.distance)>1e-10 || mg!=ml)) {t.Fatal("grid differs from production list")}
  c:=tr.tracePath(ray)
  if gok { hits++; if c!=(colour.Colour{R:0.4,G:0.8,B:1.6}) {t.Fatalf("emission mismatch: %v",c)} } else if c!=(colour.Colour{}) {t.Fatal("non-black miss")}
  energy+=c.R+c.G+c.B
 }}
 if hits==0 || hits==25 {t.Fatal("fixture must exercise both hit and miss")}
 fmt.Printf("BASELINE production rays=25 hits=%d energy=%.6f\n",hits,energy)
}
