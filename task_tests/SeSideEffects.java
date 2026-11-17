// Regression fixture: the shared initialisation has side effects (a method call, a
// field write and several assignments) and the while loop may run zero times.
// On the loop path the initialisation must still run exactly once; on the bypass path
// it must not run at all; the loop back edge must not run it again.
final class SeSideEffects {
    static int trace;
    static int side;

    static int observe() {
        return ++side;
    }

    static int scan(int end, int position, int cursor) {
        int first;
        int second;
        done: {
            if (position < 0) {
                cursor += observe();
                position = end;
                break done;
            }
            first = observe();
            side = first + cursor;
            second = side + position;
            first = position;
            while (end > position) {
                trace += first + second;
                ++position;
            }
        }
        second = end - position;
        trace += cursor + second;
        return second;
    }
}
