struct resource { int value; };
static struct resource a;
extern void exchange(struct resource *from, struct resource *to)
 __attribute__((context(from,1,0),context(to,0,1)));
static void zero_net(void)
{
 exchange(&a,&a);
}
