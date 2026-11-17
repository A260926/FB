package task_support;
import java.util.*;
public final class Suite {
  @FunctionalInterface public interface Body { void run() throws Exception; }
  private record Entry(String group, String name, Body body) {}
  private final List<Entry> cases=new ArrayList<>();
  private boolean active;
  private int checks;
  public void add(String group,String name,Body body) {
    if(active || group==null || name==null || name.isBlank() || body==null) throw new IllegalArgumentException("bad case");
    if(cases.stream().anyMatch(e->e.group.equals(group)&&e.name.equals(name))) throw new IllegalArgumentException("duplicate case");
    cases.add(new Entry(group,name,body));
  }
  public void check(boolean condition,String message) {
    if(!active) throw new IllegalStateException("check outside case");
    checks++;
    if(!condition) throw new AssertionError(message);
  }
  public static void main(String[] args) throws Exception {
    Suite s=new Suite();
    Class.forName("regression.OpenDooSabinChecks").getMethod("register",Suite.class).invoke(null,s);
    List<String> groups=List.of("fans","boundary","repeat","compat");
    if(s.cases.isEmpty() || s.cases.stream().anyMatch(e->!groups.contains(e.group))) throw new AssertionError("case groups");
    for(String group:groups) {
      int count=0,total=0;
      for(Entry e:s.cases) if(e.group.equals(group)) {
        s.checks=0;s.active=true;
        try {e.body.run();} finally {s.active=false;}
        if(s.checks==0) throw new AssertionError("zero checks: "+e.name);
        count++;total+=s.checks;
      }
      if(count==0) throw new AssertionError("empty group: "+group);
      System.out.println("GROUP "+group+" cases="+count+" checks="+total+" COMPLETE");
    }
    System.out.println("TARGET_COMPLETE cases="+s.cases.size());
  }
}
