import java.lang.annotation.*;
@Target(ElementType.TYPE_USE)
@Retention(RetentionPolicy.RUNTIME)
@interface Mark { String value(); }
public class Locals {
  public static @Mark("return") int lengths(@Mark("parameter") String a, String b) {
    int total = 0;
    { @Mark("first") String value = a; total += value.length(); }
    { @Mark("second") String value = b; total += value.length(); }
    return total;
  }
}
