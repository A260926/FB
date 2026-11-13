
/*
 * Private, non-installed test-only hook for the RFC 9180 HPKE
 * implementation.  This header is deliberately absent from the
 * installed public headers; applications have no way to inspect or
 * rewind the AEAD sequence number through crypto_hpke.h.
 */

#ifndef private_hpke_testonly_H
#define private_hpke_testonly_H

#include <stdint.h>

#include "sodium/crypto_hpke.h"

/*
 * Overwrite the internal 96-bit sequence counter of an HPKE context.
 * Only used by test/default/hpke_contract.c to reach the RFC message
 * limit without sealing an impractical number of messages.
 */
void _crypto_hpke_testonly_set_sequence(crypto_hpke_ctx *ctx, uint64_t seq);

#endif
