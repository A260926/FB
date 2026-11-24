/*
** resultState.h
**
** Status-conditioned ownership transfer contracts (@transferswhen@).
**
** The implementation in resultState.c is intentionally self-contained: the
** Splint scanner recognizes the annotation spelling and calls these entry
** points, while the association/flow code in this module performs the
** finite-source analysis and library round trip.
*/

#ifndef RESULTSTATE_H
#define RESULTSTATE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Return true when a stylized comment starts with transferswhen. */
int resultState_isContractComment(const char *text);

/* Record an original C input file for the supplemental analysis. */
void resultState_notePath(const char *path);

/* True when at least one recorded input contains this annotation. */
int resultState_sourcesHaveContracts(void);

/* Record -dump/-load arguments (either may be NULL). */
void resultState_configure(const char *dumpPath, const char *loadPath);

/* Load result-state records from a Splint library dump name. */
void resultState_loadLibrary(const char *libraryName);

/* Number of currently known declaration relations. */
int resultState_contractCount(void);

/* Parse declarations and analyze all recorded input files. */
int resultState_run(void);

/* Append result-state records to the named Splint library dump. */
void resultState_saveLibrary(const char *libraryName);

/* Number of diagnostics reported by this module. */
int resultState_diagnosticCount(void);

#ifdef __cplusplus
}
#endif

#endif
