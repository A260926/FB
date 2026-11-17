package task_support;
import mesh.Mesh3D;
import mesh.creator.primitives.CubeCreator;
import mesh.modifier.subdivision.DooSabinModifier;
public final class Baseline {
  public static void main(String[] args) {
    Mesh3D cube=new CubeCreator().create();
    new DooSabinModifier(1).modify(cube);
    if(cube.getVertexCount()!=24 || cube.getFaceCount()!=26) throw new AssertionError("closed cube");
    System.out.println("CLOSED_DOO_SABIN vertices=24 faces=26");
    Mesh3D sheet=new Mesh3D();
    sheet.addVertex(0,0,0); sheet.addVertex(2,0,0); sheet.addVertex(2,2,0); sheet.addVertex(0,2,0);
    sheet.addFace(0,1,2,3);
    try {
      new DooSabinModifier(1).modify(sheet);
      System.out.println("OPEN_SHEET vertices="+sheet.getVertexCount()+" faces="+sheet.getFaceCount());
    } catch (NullPointerException ex) {
      System.out.println("OPEN_SHEET_UNSUPPORTED="+ex.getClass().getSimpleName());
      ex.printStackTrace(System.out);
    }
  }
}
