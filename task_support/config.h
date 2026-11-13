#ifndef CHECK_LIBTAR_CONFIG_H
#define CHECK_LIBTAR_CONFIG_H
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif
#define STDC_HEADERS 1
#define HAVE_UNISTD_H 1
#define HAVE_LIBGEN_H 1
#define HAVE_FNMATCH_H 1
#define HAVE_FNMATCH 1
#define NEED_FNMATCH 1
#define HAVE_DIRENT_H 1
#define NEED_MAKEDEV 1
#define HAVE_STRDUP 1
#define PACKAGE_VERSION "pinned"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libgen.h>
#ifdef __linux__
#include <sys/sysmacros.h>
#endif
#include <stddef.h>
#ifndef __APPLE__
size_t strlcpy(char *, const char *, size_t);
size_t strlcat(char *, const char *, size_t);
#endif
#endif
