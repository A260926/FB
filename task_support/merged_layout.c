#include <stddef.h>
#include "modules.h"
_Static_assert(sizeof(struct base__shared)==4,"base declaration");
_Static_assert(sizeof(struct left__node)==16,"left layout");
_Static_assert(sizeof(struct right__node)==24,"right layout");
_Static_assert(offsetof(struct left__node,ghost_name)==8,"left member offset");
_Static_assert(_Generic(((struct left__node*)0)->next,struct left__node*:1,default:0),"left self-reference");
_Static_assert(_Generic(((struct right__node*)0)->next,struct right__node*:1,default:0),"right self-reference");
_Static_assert(_Generic(((struct left__node*)0)->ghost_name,struct base__shared:1,default:0),"left inherited structure");
_Static_assert(_Generic(((struct right__node*)0)->ghost_name,int:1,default:0),"right integer");
