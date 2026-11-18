/**
 * A benign fixture with an exception table, several local variable scopes and line
 * numbers, used to check that those events are produced once per invocation.
 */
public class ExceptionFixture {

    public static int guarded(String value) {
        int result = -1;
        try {
            result = Integer.parseInt(value);
        } catch (NumberFormatException e) {
            result = 0;
        } finally {
            result += 1;
        }
        return result;
    }
}
