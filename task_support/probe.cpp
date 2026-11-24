/* Healthy scalar/array calls bind the actual C++ adapter to the shared API. */
#define DMALLOC_DISABLE
#include <new>
#include <cstdio>
#include "dmalloc.h"
#include "allocation-contract.h"
extern "C" int task_probe_c(void);
int main() {
  if(task_probe_c())return 10;
  dmalloc_release_report r;
  void *p=::operator new(29);
  if(!p || dmalloc_contract_query(p,DMALLOC_RELEASE_SCALAR,29,0,DMALLOC_RELEASE_HAS_SIZE,&r)!=1)return 11;
  if(r.code!=DMALLOC_RELEASE_OK || r.mismatch || r.requested_size!=29 || r.allocation_family!=DMALLOC_RELEASE_SCALAR)return 12;
  ::operator delete(p);
  p=::operator new[](31);
  if(!p || dmalloc_contract_query(p,DMALLOC_RELEASE_ARRAY,31,0,DMALLOC_RELEASE_HAS_SIZE,&r)!=1)return 13;
  if(r.code!=DMALLOC_RELEASE_OK || r.mismatch || r.requested_size!=31 || r.allocation_family!=DMALLOC_RELEASE_ARRAY)return 14;
  ::operator delete[](p);
  std::puts("RELEASE_BINDING_OK");
  return 0;
}
