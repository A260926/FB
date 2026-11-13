#include "common/treemodel.h"
#include "common/ffs.h"
#include <cassert>
#include <cstdio>
int main() {
 TreeModel m;
 UModelIndex v=m.addItem(0,Types::Volume,Subtypes::Ffs2Volume,"FV","","",UByteArray(72,0),UByteArray(),UByteArray(),Fixed);
 UModelIndex f=m.addItem(72,Types::File,EFI_FV_FILETYPE_DRIVER,"Driver","","",UByteArray(24,0),UByteArray(),UByteArray(),Fixed,v);
 const char depex[]={0x06,0x08};
 UModelIndex s=m.addItem(24,Types::Section,EFI_SECTION_DXE_DEPEX,"Depex","","",UByteArray(4,0),UByteArray(depex,2),UByteArray(),Fixed,f);
 assert(v.isValid() && f.isValid() && s.isValid());
 assert(m.parent(f)==v && m.parent(s)==f && m.findParentOfType(s,Types::Volume)==v);
 assert(m.rowCount(v)==1 && m.rowCount(f)==1);
 assert(m.body(s).size()==2 && (unsigned char)m.body(s)[0]==6);
 assert(m.findParentOfType(s,Types::File)==f);
 assert(m.base(s)==96 && m.fullSize(s)==6);
 puts("BASELINE_UEFI_TREE_OK files=1 depex=1");
}
