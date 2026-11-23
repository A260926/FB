#include "mikmod.h"
#include "mikmod_internals.h"
#include <stdio.h>
#include <stdlib.h>
int main(void){
  int fail=0;
  MikMod_RegisterDriver(&drv_nos);
  /* before init: valid range accepted */
  if(MikMod_SetRetriggerFade(0)!=1){printf("0 should be ok\n");fail=1;}
  if(MikMod_SetRetriggerFade(1)!=1){printf("1 should be ok\n");fail=1;}
  if(MikMod_SetRetriggerFade(4096)!=1){printf("4096 should be ok\n");fail=1;}
  if(MikMod_SetRetriggerFade(4097)!=0){printf("4097 should fail\n");fail=1;}
  if(MikMod_SetRetriggerFade((ULONG)-1)!=0){printf("huge should fail\n");fail=1;}
  /* previous config (4096) retained after rejects */
  if(md_retriggerfade!=4096){printf("retained? got %lu\n",(unsigned long)md_retriggerfade);fail=1;}
  if(MikMod_SetRetriggerFade(8)!=1){printf("8 ok\n");fail=1;}
  if(MikMod_Init("")){printf("init fail\n");return 2;}
  /* after init: rejected, previous kept */
  if(MikMod_SetRetriggerFade(16)!=0){printf("after init should reject\n");fail=1;}
  if(md_retriggerfade!=8){printf("kept 8? got %lu\n",(unsigned long)md_retriggerfade);fail=1;}
  MikMod_Exit();
  /* after exit allowed again */
  if(MikMod_SetRetriggerFade(0)!=1){printf("after exit ok\n");fail=1;}
  printf(fail?"APICHECK_FAIL\n":"APICHECK_OK\n");
  return fail;
}
