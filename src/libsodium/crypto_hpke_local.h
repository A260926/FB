#ifndef crypto_hpke_local_h
#define crypto_hpke_local_h 1

#include <stddef.h>

/*
 * Private layout. It is exposed to the contract test only through a
 * source-relative include; it is not installed as a public header and does
 * not provide any sequence-number mutation entry point for applications.
 */
struct crypto_hpke_ctx {
    unsigned char role;
    unsigned char mode;
    unsigned char seq[12]; /* RFC 9180 big-endian 96-bit sequence number */
    unsigned char key[32];
    unsigned char base_nonce[12];
    unsigned char exporter_secret[32];
};

#endif
