#include "ubpf.h"
#include "ebpf.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint64_t identity(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e)
{ (void)b;(void)c;(void)d;(void)e; return a; }
static void raw_test(const char *path)
{
    FILE *f=fopen(path,"r"); assert(f);
    unsigned char bytes[256]; size_t used=0; char line[512], error_want[512]="";
    uint64_t want=0; int section=0, have_result=0;
    while(fgets(line,sizeof(line),f)) {
        if(!strncmp(line,"-- raw",6)) { section=1; continue; }
        if(!strncmp(line,"-- result",9)) { section=2; continue; }
        if(!strncmp(line,"-- error",8)) { section=3; continue; }
        if(!strncmp(line,"--",2)) { section=0; continue; }
        if(section==1) {
            char *end; unsigned long long word=strtoull(line,&end,0);
            assert(end!=line && used+8<=sizeof(bytes));
            for(unsigned k=0;k<8;k++) bytes[used++]=(unsigned char)(word>>(k*8));
        } else if(section==2) { want=strtoull(line,NULL,0); have_result=1; }
        else if(section==3) {
            const char *prefix="Failed to load code: "; assert(!strncmp(line,prefix,strlen(prefix)));
            snprintf(error_want,sizeof(error_want),"%s",line+strlen(prefix));
            error_want[strcspn(error_want,"\r\n")]=0;
        }
    }
    assert(!ferror(f)); fclose(f); assert(used && (have_result || error_want[0]));
    struct ubpf_vm *v=ubpf_create(); assert(v); char *error=NULL;
    int rv=ubpf_load(v,bytes,(uint32_t)used,&error);
    if(error_want[0]) assert(rv<0 && error && !strcmp(error,error_want));
    else { uint64_t got=0; assert(rv==0 && ubpf_exec(v,NULL,0,&got)==0 && got==want); }
    free(error); ubpf_destroy(v); printf("PASS upstream_raw %s\n",path);
}
static void normal_legacy(void)
{
    struct ebpf_inst p[]={
        {.opcode=EBPF_OP_STDW,.dst=10,.offset=-8,.imm=42},
        {.opcode=EBPF_OP_LDXDW,.dst=1,.src=10,.offset=-8},
        {.opcode=EBPF_OP_CALL,.src=0,.imm=1},
        {.opcode=EBPF_OP_EXIT}};
    struct ubpf_vm *v=ubpf_create(); assert(v); char *e=NULL; uint64_t got=0;
    assert(ubpf_register(v,1,"identity",identity)==0);
    assert(ubpf_load(v,p,sizeof(p),&e)==0);
    assert(ubpf_exec(v,NULL,0,&got)==0 && got==42);
    unsigned char buffer[16384]; size_t size=sizeof(buffer);
    assert(ubpf_translate(v,buffer,&size,&e)==0 && size>0);
    ubpf_unload_code(v);
    assert(ubpf_set_unwind_function_index(v,1)==0);
    p[0].imm=0;
    struct ebpf_inst unwind[]={p[0],p[1],p[2],
        {.opcode=EBPF_OP_MOV64_IMM,.dst=0,.imm=99},p[3]};
    assert(ubpf_load(v,unwind,sizeof(unwind),&e)==0);
    assert(ubpf_exec(v,NULL,0,&got)==0 && got==0);
    ubpf_destroy(v); free(e);
    puts("PASS legacy stack=42 helper=42 unwind=0 translate=nonempty");
}
int main(void)
{
    const char *files[]={"tests/lddw.data","tests/err-incomplete-lddw.data",
        "tests/err-incomplete-lddw2.data","tests/err-endian-size.data","tests/err-unknown-opcode.data"};
    for(size_t i=0;i<sizeof(files)/sizeof(files[0]);i++) raw_test(files[i]);
    normal_legacy(); puts("UBPF_LEGACY_OK upstream_cases=5"); return 0;
}
