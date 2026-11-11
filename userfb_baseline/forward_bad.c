void leaf(int *p, int i) { p[i] = 1; }
void relay(int n, int *q) { leaf(q, n + 1); }
void entry(void) { int a[4] = {0}; relay(3, a); }
