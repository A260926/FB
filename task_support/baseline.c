#include <libelf.h>
#include <gelf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static char *read_bytes(const char*p,size_t*n){FILE*f=fopen(p,"rb");long k;char*b;if(!f)exit(2);if(fseek(f,0,SEEK_END)||(k=ftell(f))<0||fseek(f,0,SEEK_SET))exit(2);*n=(size_t)k;b=malloc(*n);if(!b||fread(b,1,*n,f)!=*n)exit(2);fclose(f);return b;}
static void must(int ok,const char*m){if(!ok){fprintf(stderr,"%s: %s\n",m,elf_errmsg(-1));exit(3);}}
int main(int argc,char**argv){
 size_t n,count,i,found=0;char *raw=read_bytes("task_support/samples/regular.a",&n);Elf *ar,*e;Elf_Arsym *symbols;
 must(elf_version(EV_CURRENT)!=EV_NONE,"version");ar=elf_memory(raw,n);must(ar&&elf_kind(ar)==ELF_K_AR,"regular kind");symbols=elf_getarsym(ar,&count);must(symbols&&count==3,"index");
 for(i=0;symbols[i].as_name;i++){Elf_Scn*s=NULL;int hit=0;Elf_Arhdr*h;
  must(elf_rand(ar,(off_t)symbols[i].as_off)==(off_t)symbols[i].as_off,"rand");e=elf_begin(-1,ELF_C_READ,ar);must(e&&elf_kind(e)==ELF_K_ELF,"member");h=elf_getarhdr(e);must(h&&h->ar_size==512,"metadata");
  while((s=elf_nextscn(e,s))){GElf_Shdr sh;must(gelf_getshdr(s,&sh)!=NULL,"shdr");if(sh.sh_type==SHT_SYMTAB){GElf_Sym sym;Elf_Data*d=elf_getdata(s,NULL);char*name;must(d&&gelf_getsym(d,1,&sym),"sym");name=elf_strptr(e,sh.sh_link,sym.st_name);must(name&&!strcmp(name,symbols[i].as_name),"definition identity");hit++;}}
  must(hit==1,"definition count");printf("REGULAR symbol=%s member=%s base=%lld\n",symbols[i].as_name,h->ar_name,(long long)elf_getbase(e));found++;elf_end(e);
 }
 must(found==2,"two modules");elf_end(ar);free(raw);puts("REGULAR definitions=2 OK");
 if(argc>1&&!strcmp(argv[1],"--probe")){raw=read_bytes("task_support/samples/thin.a",&n);ar=elf_memory(raw,n);must(ar!=NULL,"thin descriptor");printf("CURRENT thin_kind=%d expected_AR=%d\n",(int)elf_kind(ar),(int)ELF_K_AR);elf_end(ar);free(raw);}
 return 0;
}
