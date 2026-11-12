#ifndef CONTRACT_H
#define CONTRACT_H
#include <stddef.h>
struct contract_case {const char *group; const char *name; int (*run)(void);};
const struct contract_case *contract_cases(size_t *count);
#endif
