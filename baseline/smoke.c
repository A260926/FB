#include "ed25519.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
 unsigned char seed[32]={7}, pk[32], sk[64], sig[64];
 unsigned char msg[]={0,1,2,0,255};
 ed25519_create_keypair(pk,sk,seed);
 ed25519_sign(sig,msg,sizeof(msg),pk,sk);
 assert(ed25519_verify(sig,msg,sizeof(msg),pk)==1);
 msg[1]^=1; assert(ed25519_verify(sig,msg,sizeof(msg),pk)==0);
 memset(sk,0,sizeof(sk));
 puts("ED25519_BASELINE_OK valid=1 changed-message=0");
 return 0;
}
