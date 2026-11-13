#ifndef REFRESH_PLATFORM_H
#define REFRESH_PLATFORM_H
#if defined(__APPLE__)
#include <machine/endian.h>
#ifndef BYTE_ORDER
#define BYTE_ORDER __BYTE_ORDER__
#define BIG_ENDIAN __ORDER_BIG_ENDIAN__
#endif
#endif
#if defined(__aarch64__)
#include <arm_neon.h>
#define vtbl2q_u8 vqtbl2_u8
#endif
#endif
