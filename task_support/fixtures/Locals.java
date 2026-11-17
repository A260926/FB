public class Locals {
  // Method-level type annotations (return type and parameter) coexist with the Code-level
  // LOCAL_VARIABLE entries; both must survive decompilation.
  public static @Mark("return") int lengths(@Mark("parameter") String a, String b) {
    int total = 0;
    {
      @Mark("first") String value = a;
      total += value.length();
    }
    {
      // Same bytecode slot reused by a second, differently annotated lifetime.
      @Mark("second") String value = b;
      total += value.length();
    }
    {
      // RuntimeVisible and RuntimeInvisible annotations on the same declaration.
      @Mark("hidden-one") @Hidden("invisible-one") String value = a;
      total += value.length();
    }
    {
      @Hidden("invisible-two") String value = b;
      total += value.length();
    }
    return total;
  }

  // The annotated variable is assigned on both branches, so its live range may be reported as
  // several offsets entries while still being one Java declaration.
  public static int split(int flag) {
    @Mark("split") String value;
    if (flag == 0) {
      value = "zero";
    }
    else {
      value = "one";
    }
    return value.length();
  }
}
