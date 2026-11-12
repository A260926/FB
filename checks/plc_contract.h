#ifndef CHECK_PLC_CONTRACT_H
#define CHECK_PLC_CONTRACT_H
#include <stddef.h>
typedef struct { const char *name; int (*run)(void); } plc_test_case;
/* Tests register at least 12 differently named cases. Exactly 1 means passed. */
extern const plc_test_case plc_cases[];
extern const size_t plc_case_count;
#endif
