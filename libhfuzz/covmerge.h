#ifndef HF_LIBHFUZZ_COVMERGE_H_
#define HF_LIBHFUZZ_COVMERGE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HF_COVMERGE_MAX_MODULES 16U
#define HF_COVMERGE_MODULE_POSITIONS 256U

size_t   hfCovMergeSize(void);
bool     hfCovMergeInit(void *memory, size_t bytes, uint64_t epoch);
bool     hfCovMergeAttach(void *memory, size_t bytes);
void     hfCovMergeDetach(void);
bool     hfCovMergeBind(char *start, char *end, uint64_t id_hi, uint64_t id_lo);
uint64_t hfCovMergeBegin(void);
bool     hfCovMergeAdvance(uint64_t epoch);
bool     hfCovMergeRead(uint64_t id_hi, uint64_t id_lo, size_t index, uint8_t *out);

#ifdef __cplusplus
}
#endif

#endif /* HF_LIBHFUZZ_COVMERGE_H_ */
