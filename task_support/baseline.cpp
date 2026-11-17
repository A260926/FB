#include <cstdio>
#include <cstring>
#include "dis_mc68000.h"
#include "array_memory.h"
int main() {
    libasm::mc68000::DisMc68000 dis;
    libasm::Disassembler &decoder=dis;
    if(!decoder.setCpu("68020"))return 1;
    const uint8_t words[]={0x20,0x30,0x1b,0x22,0x00,0x12,0x00,0x34};
    const libasm::ArrayMemory memory(0x1000,words,sizeof words,libasm::ENDIAN_BIG);
    auto cursor=memory.iterator();
    libasm::Insn insn(cursor.address());
    char operands[128]={0};
    const auto error=dis.decode(cursor,insn,operands,sizeof operands);
    if(error!=libasm::OK || insn.length()!=8 || std::strcmp(insn.name(),"MOVE.L") ||
       !std::strstr(operands,"A0,D1.L*2") || !std::strstr(operands,"D0")) {
        std::printf("OBSERVED error=%d name=%s operands=%s length=%u\n",error,insn.name(),operands,insn.length());return 2;
    }
    std::printf("BASELINE_OK %s %s length=%u next=0x%X\n",insn.name(),operands,insn.length(),cursor.address());
    return 0;
}
