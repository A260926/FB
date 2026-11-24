typedef struct object { int value; } object;
int retain_object(/*@special@*/ object *src, /*@special@*/ object **dst)
  /*@transferswhen 0 src -> *dst@*/
{
  /* A static contract mismatch; no input program is executed. */
  return 0;
}
