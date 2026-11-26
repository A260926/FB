
#ifndef MLT_EXPORT_H
#define MLT_EXPORT_H

#ifdef MLT_STATIC_DEFINE
#  define MLT_EXPORT
#  define MLT_NO_EXPORT
#else
#  ifndef MLT_EXPORT
#    ifdef task_mlt_EXPORTS
        /* We are building this library */
#      define MLT_EXPORT 
#    else
        /* We are using this library */
#      define MLT_EXPORT 
#    endif
#  endif

#  ifndef MLT_NO_EXPORT
#    define MLT_NO_EXPORT 
#  endif
#endif

#ifndef MLT_DEPRECATED
#  define MLT_DEPRECATED __attribute__ ((__deprecated__))
#endif

#ifndef MLT_DEPRECATED_EXPORT
#  define MLT_DEPRECATED_EXPORT MLT_EXPORT MLT_DEPRECATED
#endif

#ifndef MLT_DEPRECATED_NO_EXPORT
#  define MLT_DEPRECATED_NO_EXPORT MLT_NO_EXPORT MLT_DEPRECATED
#endif

/* NOLINTNEXTLINE(readability-avoid-unconditional-preprocessor-if) */
#if 0 /* DEFINE_NO_DEPRECATED */
#  ifndef MLT_NO_DEPRECATED
#    define MLT_NO_DEPRECATED
#  endif
#endif

#endif /* MLT_EXPORT_H */
