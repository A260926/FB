void leaf(int *p) { p[4] = 1; }
void entry(void) { int a[4] = {0}; leaf(a); }
