import hgeom.hmesh.elements.*;
public class HgeomBaseline {
  static void check(boolean b) { if(!b) throw new AssertionError(); }
  public static void main(String[] args) {
    int[][] faces={{0,1,2,3}};
    double[][] points={{0,2,5},{3,2,5},{3,4,7},{0,4,7}};
    HMesh3D m=new FaceSource(faces).toHMesh(new Coord3DSource(points)).orElseThrow();
    check(m.vertices().count()==4 && m.edges().count()==8);
    check(m.faces().filter(f->f.status()==HFace.Status.INTERIOR).count()==1);
    HVertex old=m.vertices().findFirst().orElseThrow();
    check(m.vertexZs().get(old)==m.vertexZ(old));
    System.out.println("HGEOM z bulk/scalar consistency PASS");
    HEdge e=m.edges().filter(x->x.face().status()==HFace.Status.INTERIOR).findFirst().orElseThrow();
    double[] p=m.vertexXYZ(e.tail()), q=m.vertexXYZ(e.head());
    HVertex v=m.splitEdge(e,(p[0]+q[0])/2,(p[1]+q[1])/2,(p[2]+q[2])/2);
    check(m.vertices().count()==5 && m.edges().count()==10);
    check(Math.abs(m.vertexZ(v)-(p[2]+q[2])/2)<1e-12);
    m.edges().forEach(x->{check(x.opposite().opposite()==x);check(x.next().previous()==x);});
    System.out.println("HGEOM 3D edge split topology 5 vertices 10 halfedges PASS");
  }
}
