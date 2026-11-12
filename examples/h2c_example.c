/*
    Example: hashing domain-separated messages to edwards25519 points.

    Shows both the one-shot entry point and the streaming context for the
    RFC 9380 edwards25519_XMD:SHA-512_ELL2_RO_ suite.  Build together with
    all the library C sources, e.g.

      cc -std=c99 -DED25519_NO_SEED -Isrc (all src .c files) \
         examples/h2c_example.c -o h2c
*/

#include <stdio.h>
#include <string.h>

#include "ed25519_h2c.h"

static void print_bytes(const char *label, const unsigned char *b, size_t n) {
    size_t i;

    printf("%s", label);
    for (i = 0; i < n; ++i) {
        printf("%02x", b[i]);
    }
    printf("\n");
}

int main(void) {
    static const unsigned char dst[] =
        "example-v01-edwards25519_XMD:SHA-512_ELL2_RO_";
    static const unsigned char message[] = "message for the local protocol";
    unsigned char point[ED25519_H2C_HASH_LEN];
    unsigned char streamed[ED25519_H2C_HASH_LEN];
    ed25519_h2c_ctx ctx;
    size_t i;
    int rc;

    /* One-shot encoding of the whole message. */
    rc = ed25519_hash_to_curve(point, message, sizeof(message) - 1,
                               dst, sizeof(dst) - 1);
    if (rc != 0) {
        printf("ed25519_hash_to_curve failed\n");
        return 1;
    }
    print_bytes("one-shot P = ", point, sizeof(point));

    /* Same message fed one byte at a time through the streaming API. */
    if (ed25519_h2c_init(&ctx, dst, sizeof(dst) - 1) != 0) {
        printf("ed25519_h2c_init failed\n");
        return 1;
    }
    for (i = 0; i < sizeof(message) - 1; ++i) {
        if (ed25519_h2c_update(&ctx, message + i, 1) != 0) {
            printf("ed25519_h2c_update failed\n");
            return 1;
        }
    }
    if (ed25519_h2c_final(&ctx, streamed) != 0) {
        printf("ed25519_h2c_final failed\n");
        return 1;
    }
    print_bytes("streamed P = ", streamed, sizeof(streamed));

    if (memcmp(point, streamed, sizeof(point)) != 0) {
        printf("mismatch between one-shot and streaming\n");
        return 1;
    }
    printf("one-shot and streaming encodings agree\n");
    return 0;
}
