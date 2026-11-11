void leaf(int *p, int i) { p[i] = 1; }
void entry(void) { int a[4] = {0}; leaf(a, 4); }
