// Regression fixture: a labelled block whose break bypasses shared initialisation
// before a while loop.  The local written before the loop is reused after it.
// Compiled with plain javac; never executed by the regression driver.
final class SeSharedEntry {
    static int trace;

    static void scan(int end, int position, int cursor, int step) {
        int saved;
        done: {
            if (position < 0) {
                if (end < 0) {
                    cursor += end - position;
                    position = end;
                    break done;
                }
                cursor += -position * step;
                position = 0;
            }
            saved = position;
            while (end > position) {
                trace += cursor + saved;
                ++position;
                saved += 3;
                cursor += step;
            }
        }
        saved = end - position;
        trace += cursor + saved;
    }
}
