package task_support;
import java.util.*;
/** Independent groups must record executed assertions; a main that returns early is rejected. */
public final class Check {
  private static final Map<String,Integer> counts=new LinkedHashMap<>();
  private static String current;
  public static void group(String name){current=name;counts.putIfAbsent(name,0);}
  public static void check(boolean ok,String message){if(current==null)throw new AssertionError("No group");if(!ok)throw new AssertionError(message);counts.put(current,counts.get(current)+1);}
  public static void run(Class<?> test)throws Exception{
    test.getMethod("main",String[].class).invoke(null,(Object)new String[0]);
    for(String group:Arrays.asList("code","debug","integration")){
      if(counts.getOrDefault(group,0)==0)throw new AssertionError("No executed checks: "+group);
      System.out.println("GROUP_OK "+group+" checks="+counts.get(group));
    }
  }
  public static void main(String[] args)throws Exception{run(Class.forName(args[0]));System.out.println("COMPACTDEX_REGRESSION_OK");}
}
