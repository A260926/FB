
#ifndef UVGRTP_EXPORT_H
#define UVGRTP_EXPORT_H

#ifdef UVGRTP_STATIC_DEFINE
#  define UVGRTP_EXPORT
#  define UVGRTP_NO_EXPORT
#else
#  ifndef UVGRTP_EXPORT
#    ifdef uvgrtp_EXPORTS
        /* We are building this library */
#      define UVGRTP_EXPORT 
#    else
        /* We are using this library */
#      define UVGRTP_EXPORT 
#    endif
#  endif

#  ifndef UVGRTP_NO_EXPORT
#    define UVGRTP_NO_EXPORT 
#  endif
#endif

#ifndef UVGRTP_DEPRECATED
#  define UVGRTP_DEPRECATED __attribute__ ((__deprecated__))
#endif

#ifndef UVGRTP_DEPRECATED_EXPORT
#  define UVGRTP_DEPRECATED_EXPORT UVGRTP_EXPORT UVGRTP_DEPRECATED
#endif

#ifndef UVGRTP_DEPRECATED_NO_EXPORT
#  define UVGRTP_DEPRECATED_NO_EXPORT UVGRTP_NO_EXPORT UVGRTP_DEPRECATED
#endif

/* NOLINTNEXTLINE(readability-avoid-unconditional-preprocessor-if) */
#if 0 /* DEFINE_NO_DEPRECATED */
#  ifndef UVGRTP_NO_DEPRECATED
#    define UVGRTP_NO_DEPRECATED
#  endif
#endif

#endif /* UVGRTP_EXPORT_H */
