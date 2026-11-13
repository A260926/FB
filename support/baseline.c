#include <sodium.h>
#include <stdio.h>
#include <string.h>
int main(void) {
 unsigned char a[32]={7},b[32]={9},ap[32],bp[32],ab[32],ba[32],zero[32]={0};
 unsigned char prk[32],key[32],nonce[12]={0},c[32],m[32]; unsigned long long cn,mn;
 if(sodium_init()<0) return 1;
 if(crypto_scalarmult_curve25519_base(ap,a)||crypto_scalarmult_curve25519_base(bp,b)) return 2;
 if(crypto_scalarmult_curve25519(ab,a,bp)||crypto_scalarmult_curve25519(ba,b,ap)||memcmp(ab,ba,32)) return 3;
 if(crypto_scalarmult_curve25519(ba,a,zero)==0) return 4;
 if(crypto_kdf_hkdf_sha256_extract(prk,NULL,0,ab,32)||crypto_kdf_hkdf_sha256_expand(key,32,"context",7,prk)) return 5;
 if(crypto_aead_chacha20poly1305_ietf_encrypt(c,&cn,(const unsigned char*)"hello",5,(const unsigned char*)"ad",2,NULL,nonce,key)) return 6;
 if(crypto_aead_chacha20poly1305_ietf_decrypt(m,&mn,NULL,c,cn,(const unsigned char*)"ad",2,nonce,key)||mn!=5||memcmp(m,"hello",5)) return 7;
 c[0]^=1;
 if(crypto_aead_chacha20poly1305_ietf_decrypt(m,&mn,NULL,c,cn,(const unsigned char*)"ad",2,nonce,key)==0) return 8;
 puts("BASELINE_X25519_HKDF_AEAD_OK"); return 0;
}
