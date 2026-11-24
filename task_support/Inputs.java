package task_support;
import cz.muni.stanse.Stanse;
import cz.muni.stanse.cparser.CUnit;
import cz.muni.stanse.codestructures.CFGHandle;
import java.nio.file.*;
import java.nio.charset.StandardCharsets;
import java.util.*;
public final class Inputs {
 public static List<CFGHandle> parse(String name, String text) throws Exception {
  if(!name.matches("[A-Za-z0-9_]+")) throw new IllegalArgumentException("fixture name");
  Path dir=Paths.get(System.getProperty("task.build"));
  Path input=dir.resolve(name+".c"); Files.write(input,text.getBytes(StandardCharsets.UTF_8));
  CUnit unit=new CUnit(new ArrayList<String>(Arrays.asList(input.toString(),dir.resolve(name+"_parsed").toString(),System.getProperty("user.dir"),"")));
  List<CFGHandle> out=new ArrayList<CFGHandle>(Stanse.getUnitManager().getCFGHandles(unit));
  if(out.isEmpty()) throw new AssertionError("CUnit produced zero CFGs");
  return out;
 }
}
