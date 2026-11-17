public class Arrays {
  // Type path direction (JVMS 4.7.20.2): an annotation on the array itself has an empty type
  // path, while each ARRAY entry navigates one component deeper. Mark is runtime visible,
  // Hidden runtime invisible, so both Code type annotation attributes are exercised.
  public static String[][] matrix(String input) {
    @Mark("element") String @Hidden("array-2d") [] @Mark("array-1d") [] values = new String[1][1];
    values[0][0] = input;
    return values;
  }

  // Single dimension: ARRAY path on the element type, empty path on the array itself.
  public static String[] vector(String input) {
    @Mark("element-one") String @Hidden("array-one") [] values = new String[1];
    values[0] = input;
    return values;
  }
}
