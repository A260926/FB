#include "projective.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
extern int load_scene(FILE*);
int main(void){
 const char*dir=getenv("TMPDIR"); if(!dir)dir=".";
 char p[256]; snprintf(p,256,"%s/cls.scn",dir);
 FILE*f=fopen(p,"w+b");
 fprintf(f,
  "M \"a\" 1 0 0 1 1 1 50 0 0 0\nM \"b\" 0 1 0 1 1 1 50 0 0 0\n"
  "p \"g\" 0 0 0 1 \"a\"\np \"g\" 1 0 0 .4 \"b\"\n"
  "i \"one\" \"g\" 1 0 0 -3 0 1 0 0 0 0 1 0 0 0 0 1\n"
  "i \"two\" \"g\" 1 0 0 3 0 1 0 0 0 0 1 0 0 0 0 1\n"
  "i \"warp\" \"g\" 1 0 0 0 0 1 0 0.4 0 0 1 0 0 0 -1 1\n");
 rewind(f); remove(p); load_scene(f); fclose(f); prepare_scene();
 printf("instances=%d unbounded=%d\n",proj_instance_count(),proj_unbounded_count());
 struct instance*w=proj_find_instance("warp"),*o=proj_find_instance("one");
 printf("warp bounded=%d one bounded=%d\n",w->bounded,o->bounded);
 return 0;
}
