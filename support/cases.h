#ifndef CHECK_CASES_H
#define CHECK_CASES_H
int unit_check(int condition, const char *expr, const char *file, int line);
#define CHECK_CHECK(x) do { if(!unit_check(!!(x), #x, __FILE__, __LINE__)) return -1; } while(0)
#endif
