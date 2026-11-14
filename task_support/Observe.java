package task_support;
import com.googlecode.d2j.*;
import com.googlecode.d2j.reader.*;
import com.googlecode.d2j.visitors.*;
import java.util.*;
public final class Observe {
  public static List<String> events(BaseDexFileReader r){
    List<String> out=new ArrayList<>();
    r.accept(new DexFileVisitor(){
      public DexClassVisitor visit(int access,String name,String parent,String[] interfaces){
        out.add("CLASS "+name+" EXTENDS "+parent);
        return new DexClassVisitor(){
          public void visitSource(String file){out.add("SOURCE "+file);}
          public DexMethodVisitor visitMethod(int flags,Method method){
            out.add("METHOD "+method);
            return new DexMethodVisitor(){public DexCodeVisitor visitCode(){return new DexCodeVisitor(){
              public void visitRegister(int n){out.add("REGISTERS "+n);}
              public void visitConstStmt(Op op,int register,Object value){out.add("CONST "+register+" "+value);}
              public DexDebugVisitor visitDebug(){return new DexDebugVisitor(){public void visitLineNumber(int line,DexLabel label){out.add("LINE "+line);}};}
            };}};
          }
        };
      }
    },0);return out;
  }
  public static void main(String[] args)throws Exception{
    byte[] ordinary=Samples.image(false);
    for(boolean compact:new boolean[]{false,true}) {
      java.nio.file.Path path=java.nio.file.Paths.get("task_support/fixtures/"+(compact?"minimal.cdex":"ordinary.dex"));
      if(!Arrays.equals(java.nio.file.Files.readAllBytes(path),Samples.image(compact)))throw new AssertionError("Fixture drift: "+path);
    }
    List<String> events=events(new DexFileReader(ordinary));
    for(String v:events)System.out.println(v);
    for(String want:Arrays.asList("CLASS LA; EXTENDS Ljava/lang/Object;","SOURCE A.java","REGISTERS 1","CONST 0 7","LINE 42")){
      if(!events.contains(want))throw new AssertionError("missing "+want);
    }
    System.out.println("ORDINARY_DEX_OK");
    if(args.length>0 && args[0].equals("normal"))return;
    try{new DexFileReader(Samples.image(true));throw new AssertionError("Expected missing capability at baseline");}
    catch(DexException e){if(!e.getMessage().contains("Magic unsupported"))throw e;System.out.println("COMPACTDEX_UNSUPPORTED: "+e.getMessage());}
  }
}
