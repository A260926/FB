import java.util.ArrayList;
import java.util.List;

/**
 * A benign fixture that keeps generic local variables alive long enough for javac -g to
 * emit both a LocalVariableTable and a LocalVariableTypeTable (signature) attribute.
 */
public class GenericFixture {

    public static <T> List<T> collect(List<? extends T> source) {
        List<T> result = new ArrayList<>(source);
        for (T value : source) {
            result.add(value);
        }
        return result;
    }
}
