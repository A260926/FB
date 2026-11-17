/* PJSIP standalone subset config, selected from the compiler's target ABI. */
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define PJ_IS_LITTLE_ENDIAN 1
#define PJ_IS_BIG_ENDIAN 0
#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define PJ_IS_LITTLE_ENDIAN 0
#define PJ_IS_BIG_ENDIAN 1
#else
#error Unknown byte order
#endif
#define PJ_HAS_LIMITS_H 1
