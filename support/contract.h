#ifndef CHECK_CONTRACT_H
#define CHECK_CONTRACT_H
void contract_add(const char *group,void (*fn)(void));
void contract_check(int ok,const char *file,int line);
#define UNIT_CHECK(x) contract_check(!!(x),__FILE__,__LINE__)
void register_contract(void);
#endif
