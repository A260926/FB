/* Legal, synthetic x86-64 ELF containing fixed instruction bytes. Never execute. */
#ifndef TASK_IMAGE_H
#define TASK_IMAGE_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#define TASK_IMAGE_SIZE 512
#define TASK_CODE_OFFSET 0x100
#define TASK_CODE_ADDRESS UINT64_C(0x401000)
static const uint8_t task_code[] = {
 0x48,0x89,0xc8,                   /* ordinary MOV */
 0xd5,0x58,0x89,0xd1,              /* REX2, extended ModRM registers */
 0xd5,0x38,0x8b,0x44,0xa3,0x10,    /* REX2, extended SIB */
 0xd5,0xc8,0x44,0xc1               /* REX2, map 1 */
};
static void task_put16(uint8_t *b,size_t o,uint16_t v) { b[o]=(uint8_t)v;b[o+1]=(uint8_t)(v>>8); }
static void task_put32(uint8_t *b,size_t o,uint32_t v) { size_t i;for(i=0;i<4;i++)b[o+i]=(uint8_t)(v>>(8*i)); }
static void task_put64(uint8_t *b,size_t o,uint64_t v) { size_t i;for(i=0;i<8;i++)b[o+i]=(uint8_t)(v>>(8*i)); }
static size_t task_image(uint8_t b[TASK_IMAGE_SIZE]) {
 memset(b,0,TASK_IMAGE_SIZE);memcpy(b,"\177ELF\002\001\001",7);
 task_put16(b,16,2);task_put16(b,18,62);task_put32(b,20,1);
 task_put64(b,24,TASK_CODE_ADDRESS);task_put64(b,32,64);
 task_put16(b,52,64);task_put16(b,54,56);task_put16(b,56,1);
 task_put32(b,64,1);task_put32(b,68,5);task_put64(b,72,TASK_CODE_OFFSET);
 task_put64(b,80,TASK_CODE_ADDRESS);task_put64(b,88,TASK_CODE_ADDRESS);
 task_put64(b,96,sizeof(task_code));task_put64(b,104,sizeof(task_code));task_put64(b,112,1);
 memcpy(b+TASK_CODE_OFFSET,task_code,sizeof(task_code));return sizeof(task_code);
}
#endif
