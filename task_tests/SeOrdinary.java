// Regression fixture: an ordinary loop with no labelled block and no shared
// entry.  It must keep decompiling to a plain for/while with no change of
// behaviour and no spurious labelled block.
final class SeOrdinary {
    static int sum(int n) {
        int value = 0;
        for (int k = 0; k < n; k++) {
            value += k;
        }
        return value;
    }

    static int nested(int n) {
        int total = 0;
        int k = 0;
        while (k < n) {
            for (int j = 0; j < k; j++) {
                if (j == 1) continue;
                total += j;
            }
            k++;
        }
        return total;
    }
}
