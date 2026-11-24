/* Fixed public-API consumer; contains no production implementation.
 * observe keeps both contexts alive and never performs stale-owner releases. */
#include "dwarf.h"
#include "libdwarf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned callbacks[2];
static unsigned tags[2] = {0,1};
static void handler(Dwarf_Error error, Dwarf_Ptr arg) {
    unsigned *tag = (unsigned *)arg;
    if (!error || !tag || *tag > 1) exit(31);
    ++callbacks[*tag];
}
static void require(int yes, const char *what) {
    if (!yes) { fprintf(stderr,"PROBE_FAIL %s\n",what); exit(32); }
}
static Dwarf_Debug open_object(const char *path, unsigned tag) {
    Dwarf_Debug dbg=0; Dwarf_Error err=0;
    int r=dwarf_init_path(path,0,0,DW_GROUPNUMBER_ANY,handler,&tags[tag],&dbg,&err);
    require(r==DW_DLV_OK && dbg && !err,"open legal split object");
    return dbg;
}
static void healthy_read(Dwarf_Debug dbg) {
    Dwarf_Unsigned len=0,typeoff=0,next=0;
    Dwarf_Half version=0,addr=0,length=0,extension=0,kind=0;
    Dwarf_Off abbrev=0; Dwarf_Sig8 sig; Dwarf_Error err=0; Dwarf_Die die=0;
    memset(&sig,0,sizeof(sig));
    int r=dwarf_next_cu_header_d(dbg,1,&len,&version,&abbrev,&addr,
        &length,&extension,&sig,&typeoff,&next,&kind,&err);
    require(r==DW_DLV_OK && version==5 && len>0 && addr==8 && !err,"read DWARF5 CU");
    r=dwarf_siblingof_b(dbg,0,1,&die,&err);
    require(r==DW_DLV_OK && die && !err,"read root DIE");
    dwarf_dealloc_die(die);
    printf("HEALTHY_CU kind=%u version=%u address=%u\n",kind,version,addr);
}
int main(int argc,char **argv) {
    require(argc==2,"mode argument");
    int observe=!strcmp(argv[1],"observe"), contract=!strcmp(argv[1],"contract");
    require(observe||contract||!strcmp(argv[1],"healthy"),"known mode");
    Dwarf_Debug a=open_object(".task-build/fixture/split.dwo",0);
    Dwarf_Debug b=open_object(".task-build/fixture/split.o",1);
    healthy_read(a); healthy_read(b);
    if(observe||contract) {
        Dwarf_Error err=0; Dwarf_Debug out=0;
        require(dwarf_set_tied_dbg(a,b,&err)==DW_DLV_OK && !err,"bind");
        require(dwarf_get_tied_dbg(a,&out,&err)==DW_DLV_OK && out==b && !err,"bound lookup");
        require(dwarf_set_tied_dbg(a,0,&err)==DW_DLV_OK && !err,"detach");
        out=0;
        require(dwarf_get_tied_dbg(a,&out,&err)==DW_DLV_OK && !err,"main lookup");
        int main_is_null=out==0;
        out=0;
        require(dwarf_get_tied_dbg(b,&out,&err)==DW_DLV_OK && !err,"tied lookup");
        int tied_is_null=out==0;
        callbacks[0]=callbacks[1]=0;
        dwarf_error_creation(b,0,"normal user error after detach");
        printf("DETACHED main_null=%d tied_null=%d callback_main=%u callback_tied=%u\n",
            main_is_null,tied_is_null,callbacks[0],callbacks[1]);
        if(contract) {
            require(main_is_null && tied_is_null,"detached lookup must be NULL on both ends");
            require(callbacks[0]==0 && callbacks[1]==1,"detached callback must use its own argument");
            dwarf_error_creation(b,&err,"independent error");
            require(err && strstr(dwarf_errmsg(err),"independent error"),"message preserved");
            dwarf_dealloc_error(b,err); err=0;
            puts("TIED_PUBLIC_CONTRACT_OK");
        }
    }
    /* No access to either object after its finish. The historical observe
       branch has not deallocated errors through a changed allocation route. */
    require(dwarf_finish(b)==DW_DLV_OK,"finish tied");
    require(dwarf_finish(a)==DW_DLV_OK,"finish main");
    puts("HEALTHY_READ_OK");
    return 0;
}
