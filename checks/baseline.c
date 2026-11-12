#include "coder.h"
#include "huff2.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static const int q[128]={1,0,0,1,0,1,1,-1,-1,0,1,0,0,-1,1,0,0,-1,0,1,0,-1,1,0,0,-1,1,0,-1,-1,0,1,0,0,0,-1,0,-1,0,-1,1,0,-1,1,0,0,-1,-1,1,0,0,1,1,-1,0,0,0,0,1,1,1,-1,0,0,0,0,0,1,0,1,-1,0,1,1,0,1,1,1,0,1,0,0,-1,-1,0,-1,1,-1,-1,0,1,0,1,1,-1,1,-1,-1,1,-1,1,1,-1,-1,-1,-1,0,1,1,-1,1,1,0,1,0,1,-1,0,0,1,-1,1,-1,0,-1,1,1,1};
static int total(CoderInfo *c) {
    unsigned char bytes[4096]; BitStream bs; InitBitStream(&bs,bytes,sizeof(bytes));
    int n=writebooks(c,&bs); assert(n==(int)bs.currentBit);
    for(int i=0;i<c->datacnt;i++) n+=c->s[i].len;
    return n;
}
int main(int argc,char **argv) {
    int widths[14]={4,4,4,4,4,8,8,8,12,12,12,16,16,16},off[15]={0};
    CoderInfo c={0}; for(int i=0;i<14;i++)off[i+1]=off[i]+widths[i];
    c.block_type=ONLY_SHORT_WINDOW;c.groups.n=2;c.groups.len[0]=1;c.groups.len[1]=7;
    c.sfbn=14;c.bandcnt=28;c.sfb_offset=off;
    for(int i=0;i<14;i++)c.book[i]=HCB_1;
    huffbook(&c,q);int actual=total(&c);c.datacnt=0;
    for(int i=0;i<14;i++){c.book[i]=i<10?HCB_2:HCB_4;huffcode_write(q+off[i],widths[i],c.book[i],&c);}
    int alternative=total(&c);
    printf("FAAC_SECTION_WITNESS current=%d legal_alternative=%d groups=1+7 bands=14\n",actual,alternative);fflush(stdout);
    if(argc>1 && !strcmp(argv[1],"--require-optimal")) {assert(actual<=alternative);return 0;}
    assert(actual==244 && alternative==243);
    puts("FAAC_BASELINE_OK section_cost_gap=1");return 0;
}
