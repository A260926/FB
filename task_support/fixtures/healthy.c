struct resource { int value; };
static struct resource a;
extern void keep(struct resource *p) __attribute__((context(p,0,1)));
extern void give(struct resource *p) __attribute__((context(p,1,0)));
extern void exchange(struct resource *from, struct resource *to)
 __attribute__((context(from,1,0),context(to,0,1)));
static void well_balanced(int fail)
{
 keep(&a);
 if (fail) { give(&a); return; }
 exchange(&a,&a);
 give(&a);
}
