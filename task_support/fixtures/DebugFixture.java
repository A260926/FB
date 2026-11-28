import java.util.List;

/** Exercises generic local variable types, line numbers, and exception ranges. */
public class DebugFixture {
    public static <T extends CharSequence> int scan(List<T> items) {
        int total = 0;
        try {
            for (int i = 0; i < items.size(); i++) {
                T item = items.get(i);
                if (item != null) {
                    total += item.length();
                }
            }
        } catch (RuntimeException e) {
            total = -1;
        }
        return total;
    }
}
