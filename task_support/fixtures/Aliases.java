public class Aliases {
  // The annotated local is a single-use alias of the parameter; the stack variable passes
  // normally inline it away, but the annotated declaration has to survive.
  public static int length(String input) {
    @Mark("alias") String value = input;
    return value.length();
  }

  // The unannotated alias may be inlined; the annotated one must remain.
  public static int chain(String input) {
    String first = input;
    @Mark("second") String second = first;
    return second.length();
  }
}
