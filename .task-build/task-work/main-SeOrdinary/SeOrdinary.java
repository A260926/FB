/*
 * Decompiled with CFR 0.153-SNAPSHOT (c4145259).
 */
final class SeOrdinary {
    SeOrdinary() {
    }

    static int sum(int n) {
        int n2 = 0;
        for (int i = 0; i < n; ++i) {
            n2 += i;
        }
        return n2;
    }

    static int nested(int n) {
        int n2 = 0;
        for (int i = 0; i < n; ++i) {
            for (int j = 0; j < i; ++j) {
                if (j == 1) continue;
                n2 += j;
            }
        }
        return n2;
    }
}
