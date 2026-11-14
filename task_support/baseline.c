#include <Zydis/Zydis.h>
#include "image.h"
#include <stdio.h>
#define NEED(x) do { if(!(x)) { fprintf(stderr,"baseline check failed: %s\n",#x); return 1; } } while(0)
int main(void) {
 ZydisDecoder d;ZydisDecodedInstruction ins;ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
 ZydisFormatter fmt;char out[256];uint8_t image[TASK_IMAGE_SIZE];size_t n=task_image(image);
 NEED(ZYAN_SUCCESS(ZydisDecoderInit(&d,ZYDIS_MACHINE_MODE_LONG_64,ZYDIS_STACK_WIDTH_64)));
 NEED(ZYAN_SUCCESS(ZydisDecoderDecodeFull(&d,image+TASK_CODE_OFFSET,n,&ins,ops)));
 NEED(ins.mnemonic==ZYDIS_MNEMONIC_MOV && ins.length==3 && ops[0].reg.value==ZYDIS_REGISTER_RAX && ops[1].reg.value==ZYDIS_REGISTER_RCX);
 NEED(ZYAN_SUCCESS(ZydisFormatterInit(&fmt,ZYDIS_FORMATTER_STYLE_INTEL)));
 NEED(ZYAN_SUCCESS(ZydisFormatterFormatInstruction(&fmt,&ins,ops,ins.operand_count_visible,out,sizeof(out),TASK_CODE_ADDRESS,NULL)));
 printf("ordinary length=%u text=%s\n",ins.length,out);
 ZyanStatus status=ZydisDecoderDecodeFull(&d,image+TASK_CODE_OFFSET+3,n-3,&ins,ops);
 NEED(!ZYAN_SUCCESS(status));printf("REX2 unsupported status=0x%08x file_offset=0x103 VA=0x401003\n",(unsigned)status);
 NEED(ZydisRegisterEncode(ZYDIS_REGCLASS_GPR64,16)==ZYDIS_REGISTER_NONE);
 printf("GPR64 id16=NONE; old R15 id=%d\n",ZydisRegisterGetId(ZYDIS_REGISTER_R15));
 const uint8_t aad[]={0xd5,0x0a};
 NEED(ZYAN_SUCCESS(ZydisDecoderInit(&d,ZYDIS_MACHINE_MODE_LEGACY_32,ZYDIS_STACK_WIDTH_32)));
 NEED(ZYAN_SUCCESS(ZydisDecoderDecodeFull(&d,aad,sizeof(aad),&ins,ops)));
 NEED(ins.mnemonic==ZYDIS_MNEMONIC_AAD && ins.length==2);
 printf("legacy32 mnemonic=AAD length=%u\n",ins.length);
 puts("baseline: real decoder and formatter active; REX2/EGPR support absent");return 0;
}
