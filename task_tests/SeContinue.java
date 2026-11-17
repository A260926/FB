// Regression fixture: shared entry plus a continue inside the loop body.
// The back edge produced by continue must keep looping to the while header
// without re-running the shared initialisation.
final class SeContinue {
    static int trace;

    static void scan(int end, int position, int cursor) {
        int saved;
        done: {
            if (position < 0) {
                position = end;
                break done;
            }
            saved = position;
            while (end > position) {
                ++position;
                if (cursor == 0) {
                    continue;
                }
                trace += saved;
            }
        }
        saved = end - position;
        trace += cursor + saved;
    }
}
