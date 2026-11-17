package task_support;
public final class Check {
  static int checks;
  public static void check(boolean value,String message) {
    checks++;
    if(!value)throw new AssertionError(message);
  }
  private Check(){}
}
