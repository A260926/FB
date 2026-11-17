#include <stdio.h>
#include "src/headerParserLib.h"
#include "fixture.h"
int main(void) {
    const char *path=".task-build/benign-macho.bin";
    if(fixture_write(path,0))return 1;
    HeaderData *hd=getBasicHeaderParserInfo(path,0,0);
    if(!hd)return 2;
    int ok=hd->headertype==HEADER_TYPE_MACH_O && hd->h_bitness==64 &&
      hd->code_regions_size==1 && hd->code_regions[0].start==1024 &&
      hd->code_regions[0].end==1040;
    if(ok)printf("BASELINE_OK MachO bits=%u code_regions=%zu range=%llu..%llu name=%s\n",
       hd->h_bitness,hd->code_regions_size,(unsigned long long)hd->code_regions[0].start,
       (unsigned long long)hd->code_regions[0].end,hd->code_regions[0].name);
    freeHeaderData(hd);remove(path);return ok?0:3;
}
