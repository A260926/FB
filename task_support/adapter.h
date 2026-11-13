/* Compile original wrappers under local names; libc I/O remains real. */
#include "config.h"
#include <stdint.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/uio.h>
#include "lib-load.h"
#undef NEW
#define NEW(x) zz_test_##x
#include "../src/libzzuf/lib-fd.c"
int g_libzzuf_ready=0, g_debug_level=0, g_debug_fd=-99;
void *_zz_dl_lib;
void libzzuf_init(void) {}
void libzzuf_fini(void) {}
int _zz_hostwatched(int fd) { (void)fd; return 1; }
static void setup(void) {
 open_orig=open; read_orig=read; pread_orig=pread; readv_orig=readv;
 lseek_orig=lseek; close_orig=close; dup_orig=dup; dup2_orig=dup2;
 _zz_fd_init(); zzuf_set_seed(41); zzuf_set_ratio(0.05,0.05);
 g_libzzuf_ready=1;
}
/* Open a brand new, independent watched identity with an explicit seed.
 * Used by tests to build expected output from a sequential reference
 * read rather than from cached file contents. */
static int sequential_reference_open(char const *path, uint32_t s) {
 zzuf_set_seed((int32_t)s);
 return zz_test_open(path, O_RDONLY);
}
static int fixture(char *path) __attribute__((unused));
static int fixture(char *path) {
 int fd=mkstemp(path); if(fd<0)return -1;
 unsigned char bytes[4096];for(int i=0;i<4096;i++)bytes[i]=(unsigned char)(i*13+7);
 if(write(fd,bytes,sizeof bytes)!=sizeof bytes){close(fd);return -1;}
 close(fd);return 0;
}
