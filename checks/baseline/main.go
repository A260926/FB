package main
import (
 "fmt"
 "math"
 m "github.com/unixpickle/model3d/model3d"
)
func main() {
 mesh:=m.NewMeshRect(m.XYZ(-1,-1,-1),m.XYZ(1,1,1))
 sdf:=m.MeshToSDF(mesh)
 a,b:=sdf.SDF(m.Coord3D{}),sdf.SDF(m.XYZ(2,0,0))
 if math.Abs(a-1)>1e-12||math.Abs(b+1)>1e-12 {panic("box distance mismatch")}
 outer:=m.NewMeshRect(m.XYZ(-2,-2,-2),m.XYZ(2,2,2))
 outer.AddMesh(mesh)
 inside:=m.ColliderContains(m.MeshToCollider(outer),m.Coord3D{},0)
 if inside {panic("baseline parity changed")}
 fmt.Printf("MODEL3D_BASELINE_OK box_sdf=1,-1 nested_parity=%t\n",inside)
}
