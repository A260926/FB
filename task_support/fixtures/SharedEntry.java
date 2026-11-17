public final class SharedEntry {
    static int trace;
    static void scan(int end, int position, int cursor, int step) {
        int saved;
        done: {
            if (position < 0) {
                if (end < 0) {
                    cursor += end - position;
                    position = end;
                    break done;
                } else {
                    cursor += -position * step;
                    position = 0;
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
