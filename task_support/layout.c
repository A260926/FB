#include "baseline.h"
_Static_assert(sizeof(struct shared)==4,"base layout");
_Static_assert(sizeof(struct node)==16,"split layout");
_Static_assert(sizeof(((struct node*)0)->ghost_name)==4,"inherited field");
