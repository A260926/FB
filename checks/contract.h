#ifndef PORTAL_CONTRACT_H
#define PORTAL_CONTRACT_H
#include <stdio.h>
typedef struct PortalCheck { unsigned checks, failures; } PortalCheck;
typedef struct PortalCase { const char *group; const char *name; int (*run)(PortalCheck *); } PortalCase;
#define PORTAL_CHECK(c, expr) do { (c)->checks++; if (!(expr)) { (c)->failures++; fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#expr); } } while(0)
const PortalCase *portal_cases(unsigned *count);
#endif
