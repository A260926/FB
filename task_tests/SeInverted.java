// Regression fixture: same control-flow problem with the inner conditional's
// polarity reversed and its two arms swapped relative to SeSharedEntry, so the
// detection cannot rely on which arm the break sits in or a fixed jump direction.
final class SeInverted {
    static int trace;

    static void scan(int end, int position, int cursor, int step) {
        int saved;
        done: {
            if (position < 0) {
                if (end >= 0) {
                    cursor += -position * step;
                    position = 0;
                } else {
                    cursor += end - position;
                    position = end;
                    break done;
                }
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
