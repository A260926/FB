/*
**  Copyright 2002-2003 University of Illinois Board of Trustees
**  Copyright 2002-2003 Mark D. Roth
**  All rights reserved.
**
**  internal.h - internal header file for libtar
**
**  Mark D. Roth <roth@uiuc.edu>
**  Campus Information Technologies and Educational Services
**  University of Illinois at Urbana-Champaign
*/

#ifndef INTERNAL_H
#define INTERNAL_H

#include <config.h>
#include <compat.h>

#include <libtar.h>

#ifdef TLS
#define TLS_THREAD TLS
#else
#define TLS_THREAD
#endif

char* safer_name_suffix(char const*);

/*
** Allocation hooks.  These exist so the regression suite can observe or
** fail each allocation made while reading a GNU extension chain.  They
** only replace the allocator; they do not influence any parse decision.
** Passing NULL restores the default (malloc/free).
*/
typedef void *(*libtar_malloc_func_t)(size_t);
typedef void (*libtar_free_func_t)(void *);

void libtar_set_alloc_hooks(libtar_malloc_func_t malloc_func,
			    libtar_free_func_t free_func);

/* allocation helpers honoring the hooks; used for every buffer owned by
   the GNU extension reader so the hooks observe all of them */
void *tar_gnu_alloc(size_t len);
void tar_gnu_free(void *ptr);

#endif
