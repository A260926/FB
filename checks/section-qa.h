#ifndef SECTION_QA_H
#define SECTION_QA_H
/* Registration only. Geometry models and assertions belong in intersections.c.
 * Each callback must perform assertions and return 0 only after completing them.
 * 77 means skipped and is rejected, just like any other nonzero result. */
typedef struct SectionQARegistry SectionQARegistry;
typedef int (*SectionQACase)(void);
void section_qa_add(SectionQARegistry *, const char *group,
                    const char *name, SectionQACase run);
/* Implement this entry point in the new checks/intersections.c. */
void section_qa_register(SectionQARegistry *);
#endif
