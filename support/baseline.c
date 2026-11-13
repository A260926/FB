#include "bdshemu.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
 unsigned char code[]={0xb8,1,0,0,0,0x83,0xc0,2}, stack[8192]={0}, work[8200]={0};
 SHEMU_CONTEXT c; memset(&c,0,sizeof c);
 c.ArchType=SHEMU_ARCH_TYPE_X86; c.Arch.X86.Mode=ND_CODE_64;
 c.Shellcode=code; c.ShellcodeSize=sizeof code; c.ShellcodeBase=0x200000;
 c.Stack=stack; c.StackSize=sizeof stack; c.StackBase=0x100000;
 c.Intbuf=work; c.IntbufSize=sizeof work; c.MaxInstructionsCount=2;
 c.Arch.X86.Registers.RegRip=c.ShellcodeBase;
 c.Arch.X86.Registers.RegRsp=c.StackBase+4096;
 c.Arch.X86.Registers.RegFlags=2;
 assert(ShemuEmulate(&c)==SHEMU_SUCCESS);
 assert(c.Arch.X86.Registers.RegRax==3);
 puts("BASELINE_SHEMU_ALU_OK rax=3");
}
