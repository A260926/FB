void leaf(int *p, int i) { p[i] = 1; }
void entry(void) { int a[2] = {0}; int b[20] = {0}; leaf(a, 1); leaf(b, 10); }
