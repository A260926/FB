/*
 * Decompiled with CFR 0.153-SNAPSHOT (c4145259).
 */
final class SeSideEffects {
    static int trace;
    static int side;

    SeSideEffects() {
    }

    static int observe() {
        return ++side;
    }

    static int scan(int n, int n2, int n3) {
        int n4;
        if (n2 < 0) {
            n3 += SeSideEffects.observe();
            n2 = n;
        } else {
            int n5 = SeSideEffects.observe();
            side = n5 + n3;
            n4 = side + n2;
            n5 = n2;
            while (n > n2) {
                trace += n5 + n4;
                ++n2;
            }
        }
        n4 = n - n2;
        trace += n3 + n4;
        return n4;
    }
}
