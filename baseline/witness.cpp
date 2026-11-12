#include "fixture.hpp"
#include <libdwarf.h>
#include <dwarf.h>
#include <cassert>
#include <iostream>
#include <cstring>
int main(int argc,char**argv) {
 assert(argc>=2);bool expect=argc>2;int bad=0;
 for(bool neighbor:{false,true}) {
  std::string path=std::string(argv[1])+(neighbor?"/neighbor.o":"/macro.o");fixture(path.c_str(),neighbor);
  Dwarf_Debug dbg=0;Dwarf_Error err=0;assert(dwarf_init_path(path.c_str(),0,0,0,0,0,&dbg,&err)==DW_DLV_OK);
  Dwarf_Unsigned len,off,next;Dwarf_Half ver,addr,ls,ext,ut;Dwarf_Off ab;Dwarf_Sig8 sig;
  assert(dwarf_next_cu_header_d(dbg,1,&len,&ver,&ab,&addr,&ls,&ext,&sig,&off,&next,&ut,&err)==DW_DLV_OK);
  Dwarf_Die die=0;assert(dwarf_siblingof_b(dbg,0,1,&die,&err)==DW_DLV_OK);
  const char* text=0;int status=0;
  if(neighbor){Dwarf_Attribute attr=0;assert(dwarf_attr(die,DW_AT_producer,&attr,&err)==DW_DLV_OK);char* value=0;status=dwarf_formstring(attr,&value,&err);text=value;dwarf_dealloc_attribute(attr);}
  else {Dwarf_Macro_Context mc=0;Dwarf_Unsigned mo,count,bytes,line,index,str_off;Dwarf_Half forms;
   assert(dwarf_get_macro_context(die,&off,&mc,&mo,&count,&bytes,&err)==DW_DLV_OK);
   status=dwarf_get_macro_defundef(mc,0,&line,&index,&str_off,&forms,&text,&err);dwarf_dealloc_macro_context(mc);}
  std::cout<<(neighbor?"NEIGHBOR_STRX_OBSERVED":"MACRO_STRX_OBSERVED")<<" status="<<status<<" text="<<(text?text:"<none>")<<"\n";
  if(expect && (neighbor?status!=DW_DLV_ERROR:status!=DW_DLV_OK||!text||std::strcmp(text,"SECOND 2")))bad++;
  if(err)dwarf_dealloc_error(dbg,err);dwarf_dealloc_die(die);dwarf_finish(dbg);
 }
 if(bad){std::cerr<<"STRX_EXPECTED_BEHAVIOR_MISMATCH count="<<bad<<"\n";return 1;}
 std::cout<<"LIBDWARF_BASELINE_OK\n";
}
