typedef struct object { int value; } object;
int move_object(/*@special@*/ object *src, /*@special@*/ object **dst)
  /*@transferswhen 0 src -> *dst@*/
{
  *dst = src;
  return 0;
}
