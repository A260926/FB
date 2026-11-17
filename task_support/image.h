#ifndef TASK_PDB_IMAGE_H
#define TASK_PDB_IMAGE_H
#include <stddef.h>
#include <stdint.h>
struct task_image { unsigned char *data; size_t size; uint32_t reference_offsets[2]; uint32_t procedure_offsets[2]; };
/* modes: 0 = ordinary tables, 1 = no PSI/GSI, 2 = no global record stream either. */
int task_make_image(struct task_image *out, int mode);
void task_free_image(struct task_image *image);
#endif
