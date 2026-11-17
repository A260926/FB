package task;
import java.util.*;
/** Registration, assertion counting, and normal-return checks for plain Java tests. */
public final class TaskCheck {
 public interface Body { void run(Check c) throws Exception; }
 public record Case(String group,String name,Body body) {}
 public static final class Check {
  private int count;
  public void check(boolean condition,String message) { count++;if(!condition)throw new AssertionError(message); }
 }
 public static void main(String[] args) throws Exception {
  String[] required={"aliases","flow","compatibility"};
  Object value=Class.forName("task.InitializationContract").getMethod("cases").invoke(null);
  Case[] cases=(Case[])value;
  if(cases==null||cases.length==0)throw new AssertionError("zero cases");
  Map<String,Integer> counts=new HashMap<>(),checks=new HashMap<>();Set<String> seen=new HashSet<>();
  for(Case c:cases){
   if(c==null||c.name()==null||!seen.add(c.group()+"/"+c.name())||!Arrays.asList(required).contains(c.group()))throw new AssertionError("registration");
   Check check=new Check();c.body().run(check);
   if(check.count==0)throw new AssertionError("zero assertions: "+c.name());
   counts.merge(c.group(),1,Integer::sum);checks.merge(c.group(),check.count,Integer::sum);
  }
  for(String group:required){if(counts.getOrDefault(group,0)==0)throw new AssertionError("missing group "+group);
   System.out.println("GROUP "+group+" cases="+counts.get(group)+" checks="+checks.get(group)+" COMPLETE");}
  System.out.println("TARGET_COMPLETE cases="+cases.length);
 }
}
