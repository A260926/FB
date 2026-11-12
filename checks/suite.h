#ifndef CHECK_SUITE_H
#define CHECK_SUITE_H
void unit_expect(int, const char *, const char *, int);
#define UNIT_CHECK(x) unit_expect(!!(x), #x, __FILE__, __LINE__)
#endif
