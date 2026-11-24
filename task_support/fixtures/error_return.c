struct resource { int value; };
static struct resource a, b;
extern void keep(struct resource *p) __attribute__((context(p,0,1)));
extern void give(struct resource *p) __attribute__((context(p,1,0)));
static int error_path(int fail)
{
 keep(&a);
 if (fail) { give(&b); return -1; }
 give(&a);
 return 0;
}
