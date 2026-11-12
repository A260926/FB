/* Fixed registration and assertion driver interface, CC0-1.0. */
#ifndef NC_CONTRACT_H
#define NC_CONTRACT_H
#include <stddef.h>
typedef struct { const char *name; const char *group; int (*run)(void); } nc_case;
const nc_case *nc_contract_cases(size_t *count);
/* Each content assertion must pass through this function. Failure is sticky. */
void nc_check(int truth, const char *message, const char *file, int line);
#define NC_CHECK(expr) nc_check(!!(expr), #expr, __FILE__, __LINE__)
#endif
