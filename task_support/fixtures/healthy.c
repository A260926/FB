typedef struct object { int value; } object;
extern /*@only@*/ object *make_object(void);
extern void drop_object(/*@only@*/ object *obj);
void healthy(void)
{
  object *p = make_object();
  drop_object(p);
}
