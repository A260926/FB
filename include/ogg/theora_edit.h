/* Ogg/Theora lossless edit plan.
 *
 * This is a small, self contained helper on top of libogg.  It indexes the
 * Theora logical streams of an Ogg file and lets an application write a new,
 * single logical Theora stream made from one or more frame ranges of the
 * source.  The frame packets themselves are copied byte for byte; only the
 * page structure and the granule positions are rebuilt.
 *
 * The API is deliberately opaque: implementations and their data layout are
 * private to this library.  All functions return 0 on success and a non-zero
 * value on failure.
 */
#ifndef OGG_THEORA_EDIT_H
#define OGG_THEORA_EDIT_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A half-open frame range [begin,end) of a zero based logical stream number. */
typedef struct {
  uint32_t stream;
  uint64_t begin;
  uint64_t end;
} ogg_theora_cut;

typedef struct ogg_theora_plan ogg_theora_plan;

/* Read and validate source completely and build an edit plan.  source is
 * read from its beginning and must remain open for later writes.  On return
 * *out is either a valid plan or NULL. */
int ogg_theora_plan_create(FILE *source, const ogg_theora_cut *cuts,
                           size_t count, ogg_theora_plan **out);

/* Write the plan as one new Theora logical stream.  The output stream state
 * is built from scratch on every call, so the same plan may be written more
 * than once (with different serial numbers).  source and output must not be
 * the same FILE. */
int ogg_theora_plan_write(ogg_theora_plan *plan, FILE *output, int serial);

/* Release a plan.  NULL is accepted. */
void ogg_theora_plan_destroy(ogg_theora_plan *plan);

#ifdef __cplusplus
}
#endif

#endif /* OGG_THEORA_EDIT_H */
