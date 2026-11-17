import java.nio.file.*;
import org.openjdk.asmtools.common.inputs.ByteInput;
import org.openjdk.asmtools.common.outputs.TextOutput;
import org.openjdk.asmtools.common.outputs.log.StringLog;
import org.openjdk.asmtools.jdis.Main;
public final class Baseline {
  static byte[] bytes;
  static Main make(TextOutput out, String... args) {
    return new Main(out,new StringLog(),new ByteInput(bytes),args);
  }
  static String finish(Main m, TextOutput out) {
    int n=out.getOutputs().size();
    if(m.disasm()!=0 || out.getOutputs().size()!=n+1) throw new AssertionError("disassembly failed");
    return out.getOutputs().get(n).getBody();
  }
  public static void main(String[] args) throws Exception {
    bytes=Files.readAllBytes(Path.of(".task-build/fixture/LocalFixture.class"));
    TextOutput normal=new TextOutput(); String reference=finish(make(normal),normal);
    if(!reference.contains("inspect") || !reference.contains("invokeinterface")) throw new AssertionError("no production decoding");
    System.out.println("BASELINE_OK class=LocalFixture instruction=invokeinterface");
    if(args.length>0 && args[0].equals("normal")) return;
    TextOutput aOut=new TextOutput(),bOut=new TextOutput();
    Main a=make(aOut); Main b=make(bOut,"-g","-table","-lvt","-lnt:numbers");
    String interleaved=finish(a,aOut); String detailed=finish(b,bOut);
    Files.writeString(Path.of(".task-build/reference.jasm"),reference);
    Files.writeString(Path.of(".task-build/interleaved.jasm"),interleaved);
    Files.writeString(Path.of(".task-build/detailed.jasm"),detailed);
    System.out.println("INSTANCE_OPTIONS_LEAK="+!reference.equals(interleaved));
    System.out.println("reference_chars="+reference.length()+" interleaved_chars="+interleaved.length());
  }
}
