package task_support;
import java.util.*;
public final class ContractSuite {
 public interface Body {void run() throws Exception;}
 public static final class Case {
  public final String group,name; public final Body body;
  public Case(String group,String name,Body body){this.group=group;this.name=name;this.body=body;}
 }
 private static int checks, failures;
 public static void check(boolean ok,String message){checks++;if(!ok){failures++;throw new AssertionError(message);}}
 public static void main(String[] args) throws Exception {
  @SuppressWarnings("unchecked")
  List<Case> cases=(List<Case>)Class.forName("BoundsContract").getMethod("cases").invoke(null);
  if(cases==null||cases.isEmpty()||cases.size()>10000)throw new AssertionError("zero/invalid cases");
  String[] groups={"correlated","loops","witness"};int total=0;
  Set<Case> visited=Collections.newSetFromMap(new IdentityHashMap<Case,Boolean>());
  for(String group:groups){int n=0,c=0;
   for(Case x:cases){if(x==null||x.group==null||x.name==null||x.body==null)throw new AssertionError("bad case");
    if(!group.equals(x.group))continue;
    if(!visited.add(x))throw new AssertionError("duplicate registration");
    checks=0;failures=0;x.body.run();
    if(checks==0||failures!=0)throw new AssertionError("no checks or failed checks");
    n++;c+=checks;total++;System.out.println("CASE "+group+" "+x.name+" checks="+checks);
   }
   if(n==0||c==0)throw new AssertionError("missing/empty group "+group);
   System.out.println("GROUP "+group+" cases="+n+" checks="+c+" COMPLETE");
  }
  if(total!=cases.size())throw new AssertionError("unmatched cases");
  System.out.println("TARGET_COMPLETE cases="+total);
 }
}
