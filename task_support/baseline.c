#include "fixture.h"
#include <libelf/libelf.h>
#include <libelf/gelf.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#define OK(x) do {if(!(x)){fprintf(stderr,"baseline:%d %s: %s\n",__LINE__,#x,elf_errmsg(-1));return 1;}} while(0)
static int inspect(Elf *e, unsigned machine, unsigned value) {
    GElf_Ehdr h; GElf_Shdr sh; GElf_Sym sym; Elf_Scn *s; Elf_Data *d; const char *name;
    OK(e && elf_kind(e)==ELF_K_ELF); OK(gelf_getehdr(e,&h)); OK(h.e_machine==machine);
    s=elf_getscn(e,3); OK(s && gelf_getshdr(s,&sh)); OK(sh.sh_type==SHT_SYMTAB && sh.sh_link==2);
    d=elf_getdata(s,NULL); OK(d && gelf_getsym(d,1,&sym));
    name=elf_strptr(e,sh.sh_link,sym.st_name); OK(name && !strcmp(name,"same"));
    OK(sym.st_value==value && sym.st_shndx==1); printf("SYMBOL machine=%u name=%s value=%u\n",machine,name,value);
    return 0;
}
int main(void) {
    unsigned char bytes[768], before[768]; Elf *e,*ar,*child; int fd; size_t n; char *raw;
    OK(elf_version(EV_CURRENT)==EV_CURRENT);
    fd=open(".task-build/samples/plain64.elf",O_RDONLY); OK(fd>=0);
    e=elf_begin(fd,ELF_C_READ,NULL); OK(inspect(e,62,1)==0); OK(elf_end(e)==0); OK(close(fd)==0);
    OK(fixture_elf(bytes,sizeof(bytes),32,1,2)==768); memcpy(before,bytes,sizeof(bytes));
    e=elf_memory((char*)bytes,sizeof(bytes)); OK(inspect(e,20,2)==0);
    raw=elf_rawfile(e,&n); OK(raw && n==768 && !memcmp(raw,before,768)); OK(elf_end(e)==0);
    fd=open(".task-build/samples/normal.a",O_RDONLY); OK(fd>=0); ar=elf_begin(fd,ELF_C_READ,NULL); OK(ar && elf_kind(ar)==ELF_K_AR);
    child=elf_begin(fd,ELF_C_READ,ar); OK(inspect(child,62,1)==0); OK(elf_getbase(child)==68); OK(elf_next(child)==ELF_C_READ); OK(elf_end(child)==0);
    child=elf_begin(fd,ELF_C_READ,ar); OK(inspect(child,20,2)==0); OK(elf_next(child)==ELF_C_NULL); OK(elf_end(child)==0);
    OK(elf_end(ar)==0); OK(close(fd)==0); puts("ARCHIVE_OK members=2");
    fd=open(".task-build/samples/bundle.fat",O_RDONLY); OK(fd>=0); e=elf_begin(fd,ELF_C_READ,NULL); OK(e);
    printf("FATELF_KIND=%d\n",(int)elf_kind(e)); OK(elf_end(e)==0); OK(close(fd)==0);
    puts("BASELINE_OK"); return 0;
}
