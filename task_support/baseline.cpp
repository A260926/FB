#include "PDB.h"
#include "PDB_RawFile.h"
#include "PDB_ModuleSymbolStream.h"
#include "PDB_ModuleLineStream.h"
#include "PDB_NamesStream.h"
#include "fixture.h"
#include <iostream>
int main(){
 auto f=fixture::normal();if(PDB::ValidateFile(f.image.data(),f.image.size())!=PDB::ErrorCode::Success)return 1;
 auto raw=PDB::CreateRawFile(f.image.data());PDB::ModuleSymbolStream symbols(raw,0,f.symbols);
 unsigned sites=0,lines=0,checksums=0;
 symbols.ForEachSymbol([&](auto r){if(r->header.kind==PDB::CodeView::DBI::SymbolRecordKind::S_INLINESITE){++sites;if(r->data.S_INLINESITE.inlinee!=0x1000)sites+=100;}});
 PDB::ModuleLineStream line(raw,0,f.symbols+f.c13,f.symbols);
 line.ForEachSection([&](auto s){using K=PDB::CodeView::DBI::DebugSubsectionKind;
  if(s->header.kind==K::S_INLINEELINES)line.ForEachInlineeSourceLine(s,[&](auto x){++lines;if(x->lineNumber!=41)lines+=100;});
  if(s->header.kind==K::S_FILECHECKSUMS)line.ForEachFileChecksum(s,[&](auto x){++checksums;if(x->filenameOffset!=1)checksums+=100;});
 });
 PDB::NamesStream names(raw,1);if(std::strcmp(names.GetFilename(1),"source.cc"))return 2;
 std::cout<<"RAW_PDB sites="<<sites<<" inlinee_lines="<<lines<<" checksums="<<checksums<<" file="<<names.GetFilename(1)<<"\n";
 return sites==1&&lines==1&&checksums==1?0:3;
}
