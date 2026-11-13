/* Public contract for the requested addition, declarations only. */
#ifndef CHECK_HPKE_API_H
#define CHECK_HPKE_API_H
#include <stddef.h>
typedef struct crypto_hpke_ctx crypto_hpke_ctx;
int crypto_hpke_sender_setup(crypto_hpke_ctx **out,unsigned char enc[32],const unsigned char pkR[32],const unsigned char *skS,const unsigned char *info,size_t info_len,const unsigned char *ikmE,size_t ikmE_len);
int crypto_hpke_receiver_setup(crypto_hpke_ctx **out,const unsigned char enc[32],const unsigned char skR[32],const unsigned char *pkS,const unsigned char *info,size_t info_len);
int crypto_hpke_seal(crypto_hpke_ctx *ctx,unsigned char *c,unsigned long long *clen,const unsigned char *m,unsigned long long mlen,const unsigned char *aad,unsigned long long aadlen);
int crypto_hpke_open(crypto_hpke_ctx *ctx,unsigned char *m,unsigned long long *mlen,const unsigned char *c,unsigned long long clen,const unsigned char *aad,unsigned long long aadlen);
int crypto_hpke_export(const crypto_hpke_ctx *ctx,unsigned char *out,size_t outlen,const unsigned char *exporter_context,size_t contextlen);
void crypto_hpke_free(crypto_hpke_ctx *ctx);
#endif
