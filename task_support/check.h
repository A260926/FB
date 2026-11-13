#ifndef MICROMOD_TASK_CHECK_H
#define MICROMOD_TASK_CHECK_H
void contract_check(int ok, const char *expression, const char *file, int line);
#define CHECK(expression) contract_check(!!(expression), #expression, __FILE__, __LINE__)
#endif
