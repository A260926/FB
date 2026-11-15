package main
import("fmt";"math";"os";m "github.com/unixpickle/model3d/model3d")
func main(){
 d:=&m.DualContouring{S:m.SolidSurfaceEstimator{Solid:&m.Sphere{Radius:1}},Delta:.18,NoJitter:true,MaxGos:1,BufferSize:256,Repair:true,Clip:true}
 mesh:=d.Mesh();vol:=mesh.Volume()
 if mesh.NumTriangles()==0||mesh.NeedsRepair()||len(mesh.SingularVertices())!=0||math.Abs(vol-4*math.Pi/3)>.15{os.Exit(2)}
 boxes:=m.JoinedSolid{m.NewRect(m.XYZ(0,0,0),m.XYZ(1,1,1)),m.NewRect(m.XYZ(1,0,1),m.XYZ(2,1,2)),m.NewRect(m.XYZ(-1,-1,-1),m.XYZ(0,0,0))}
 d.S.Solid=boxes;d.Delta=.2;d.NoJitter=false;d.Clip=false
 b:=d.Mesh();ok:=!b.NeedsRepair()&&len(b.SingularVertices())==0&&b.Orientable()&&len(b.InconsistentEdges())==0
 if !ok||b.NumTriangles()==0{os.Exit(3)}
 fmt.Printf("BASELINE_DC sphere_triangles=%d volume=%.6f contact_triangles=%d repair_ok=%t\n",mesh.NumTriangles(),vol,b.NumTriangles(),ok)
}
