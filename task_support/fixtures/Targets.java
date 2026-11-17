import java.io.ByteArrayInputStream;

public class Targets {
  // Resource variable (0x41), instanceof expression (0x43) and catch clause (0x42) targets stay
  // present in the Code type annotation reading result. Their source output is not extended, but
  // decompilation must still produce compilable Java.
  public static String run(Object value) throws Exception {
    if (value instanceof @Mark("instance") String text) {
      try (@Mark("resource") ByteArrayInputStream in = new ByteArrayInputStream(new byte[0])) {
        return text + in.available();
      }
    }
    try {
      throw new IllegalStateException();
    }
    catch (@Mark("catch") IllegalStateException e) {
      return e.getMessage();
    }
  }
}
