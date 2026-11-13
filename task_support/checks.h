#ifndef WV_PUSH_CHECKS_H
#define WV_PUSH_CHECKS_H
/* Tests implement this function, without main(), and call task_check. */
void task_check(int condition, const char *message);
void push_cases(const char *group);
#endif
