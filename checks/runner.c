#include "suite.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct unit_case { const char* group; const char* name; unit_fn fn; };
static struct unit_case cases[256];
static size_t count, assertions;
static int skipped;
void unit_register(const char* g, const char* n, unit_fn fn) {
  if(count == 256) abort();
  cases[count++] = (struct unit_case){g,n,fn};
}
void unit_check(int ok,const char* file,int line) {
  assertions++;
  if(!ok) { fprintf(stderr,"CHECK_FAILED %s:%d\n",file,line); exit(1); }
}
void unit_skip(const char* why) { skipped=1; fprintf(stderr,"SKIPPED %s\n",why); }
// A fixed driver includes exactly the new file named in the user request.
#include "../test/segment_summary_test.c"
int main(void) {
  const char* groups[]={"filter","gating","composition"};
  size_t completed=0;
  for(size_t g=0;g<3;g++) {
    size_t n=0;
    for(size_t i=0;i<count;i++) if(!strcmp(cases[i].group,groups[g])) {
      assertions=0;skipped=0;cases[i].fn();
      if(!assertions || skipped) { fputs("EMPTY_OR_SKIPPED_CASE\n",stderr);return 1; }
      printf("CASE %s %s CHECKS=%zu PASS\n",groups[g],cases[i].name,assertions);
      ++n;++completed;
    }
    if(!n) { fprintf(stderr,"MISSING_GROUP %s\n",groups[g]);return 1; }
    printf("GROUP %s CASES=%zu PASS\n",groups[g],n);
  }
  if(completed!=count) { fputs("UNKNOWN_GROUP\n",stderr);return 1; }
  printf("SEGMENT_TARGET_COMPLETE CASES=%zu\n",completed);return 0;
}
