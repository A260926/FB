package task_support;
import java.lang.reflect.*;
public final class Driver {
  public static void main(String[] args) throws Exception {
    Class<?> test=Class.forName("org.jcodec.movtool.MultiSourceFlattenTest");
    String[] groups={"route","onTop","processor","rejectCompat"};
    for(String group:groups) {
      Check.checks=0;
      Method method=test.getMethod(group);
      if(method.getReturnType()!=void.class||!Modifier.isStatic(method.getModifiers()))throw new AssertionError("expected static void "+group);
      method.invoke(null);
      if(Check.checks<1)throw new AssertionError("no checks "+group);
      System.out.println("GROUP "+group+" checks="+Check.checks+" COMPLETE");
    }
    System.out.println("TARGET_COMPLETE groups=4");
  }
}
