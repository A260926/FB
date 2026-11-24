/* Fixed public-API binding only; not the feature's regression suite. */
#define DMALLOC_DISABLE
#include <stddef.h>
#include "dmalloc.h"
#include "allocation-contract.h"
int task_probe_c(void) {
  dmalloc_release_report r;
  void *p=dmalloc_malloc(__FILE__,__LINE__,41,DMALLOC_FUNC_MALLOC,0,0);
  if(!p)return 1;
  if(dmalloc_contract_query(p,DMALLOC_RELEASE_C,41,0,DMALLOC_RELEASE_HAS_SIZE|DMALLOC_RELEASE_HAS_ALIGNMENT,&r)!=1)return 2;
  if(r.code!=DMALLOC_RELEASE_OK || r.mismatch || r.allocation_family!=DMALLOC_RELEASE_C || r.requested_size!=41 || r.requested_alignment!=0)return 3;
  if(dmalloc_contract_release(__FILE__,__LINE__,p,DMALLOC_RELEASE_C,41,0,DMALLOC_RELEASE_HAS_SIZE,&r)!=1)return 4;
  if(dmalloc_contract_query(NULL,DMALLOC_RELEASE_C,0,0,0,&r)!=1 || r.code!=DMALLOC_RELEASE_NULL)return 5;
  return 0;
}
