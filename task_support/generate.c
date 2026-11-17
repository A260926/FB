#include "fixture.h"
#include <stdio.h>
#include <stdlib.h>
static int save(const char *dir, const char *name, const void *bytes, size_t n) {
    char path[1024]; FILE *f; int rc;
    if (!n || snprintf(path,sizeof(path),"%s/%s",dir,name) >= (int)sizeof(path)) return 1;
    f=fopen(path,"wb"); if (!f) return 1;
    rc=fwrite(bytes,1,n,f)!=n; if (fclose(f)) rc=1; return rc;
}
int main(int argc, char **argv) {
    unsigned char p[10000]; size_t n;
    if (argc!=2) return 2;
    n=fixture_elf(p,sizeof(p),64,0,1); if(save(argv[1],"plain64.elf",p,n)) return 1;
    n=fixture_elf(p,sizeof(p),32,1,2); if(save(argv[1],"plain32.elf",p,n)) return 1;
    n=fixture_fat(p,sizeof(p)); if(save(argv[1],"bundle.fat",p,n)) return 1;
    n=fixture_archive(p,sizeof(p),0); if(save(argv[1],"normal.a",p,n)) return 1;
    n=fixture_archive(p,sizeof(p),1); if(save(argv[1],"nested.a",p,n)) return 1;
    return 0;
}
