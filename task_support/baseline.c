#include "bddisasm.h"
#include "rom_fixture.h"
#include <stdio.h>
#include <string.h>
static int decode(const unsigned char *p,size_t n,unsigned ip,unsigned id) {
    INSTRUX ix;char text[128];NDSTATUS s;
    s=NdDecodeEx(&ix,p,n,ND_CODE_16,ND_DATA_16);
    if(!ND_SUCCESS(s)||ix.Instruction!=id)return 1;
    s=NdToText(&ix,ip,sizeof(text),text);if(!ND_SUCCESS(s))return 2;
    printf("DECODE ip=%04x length=%u %s\n",ip,ix.Length,text);return 0;
}
int main(void) {
    static const unsigned char branch[]={0xeb,0x02};
    static const unsigned char movsp[]={0x89,0xc4};
    INSTRUX ix;ND_ACCESS_MAP m;
    if(decode(branch,sizeof(branch),0xfffe,ND_INS_JMPNR))return 1;
    if(decode(branch,sizeof(branch),0x000e,ND_INS_JMPNR))return 2;
    if(decode(task_rom_main,sizeof(task_rom_main),0,ND_INS_CALLNR))return 3;
    if(decode(task_rom_main+16,sizeof(task_rom_main)-16,16,ND_INS_CALLFD))return 4;
    if(decode(task_rom_main+5,1,5,ND_INS_RETN))return 5;
    if(decode(task_rom_far,sizeof(task_rom_far),16,ND_INS_RETF))return 6;
    memset(&m,0,sizeof(m));
    if(!ND_SUCCESS(NdDecodeEx(&ix,movsp,sizeof(movsp),ND_CODE_16,ND_DATA_16)))return 7;
    if(!ND_SUCCESS(NdGetFullAccessMap(&ix,&m)))return 8;
    if(!(m.GprAccess[NDR_RSP]&ND_ACCESS_ANY_WRITE))return 9;
    printf("SP_WRITE_DETECTED access=%u\n",m.GprAccess[NDR_RSP]);
    puts("BASELINE_OK mode=16 decoded=6 access_map=1 immutable_rom=1");return 0;
}
