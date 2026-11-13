#include <rz_libdemangle.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(void){
 char *s=libdemangle_handler_msvc("?public_func@TEST_CLASS@@QEAAHXZ",RZ_DEMANGLE_OPT_BASE);
 if(!s||!strstr(s,"TEST_CLASS::public_func(void)"))return 1;printf("NORMAL %s\n",s);free(s);
 s=libdemangle_handler_msvc("?f@C@@WBA@EAAHXZ",RZ_DEMANGLE_OPT_BASE);
 if(!s||!strstr(s,"adjustor{16}"))return 2;printf("STATIC %s\n",s);free(s);
 s=libdemangle_handler_msvc("??_EDerived@@$4PPPPPPPM@A@EAAPEAXI@Z",RZ_DEMANGLE_OPT_BASE);
 printf("VTORDISP %s\n",s?s:"UNSUPPORTED");free(s);
 s=libdemangle_handler_msvc("?f@A@simple@@$R477PPPPPPPM@7AEXXZ",RZ_DEMANGLE_OPT_BASE);
 printf("VTORDISPEX %s\n",s?s:"UNSUPPORTED");free(s);
 puts("BASELINE_OK real_msvc_handler");return 0;
}
