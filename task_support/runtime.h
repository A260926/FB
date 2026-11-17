#ifndef TASK_RUNTIME_H
#define TASK_RUNTIME_H
/* Isolated single-thread host services for the production DWARF core.
   No expression execution or MemoryMap replacement is provided here. */
#include <stdlib.h>
#include "base/base_context_cracking.h"
#include "base/base_core.h"
#include "base/base_profile.h"
#include "base/base_memory.h"
#include "base/base_arena.h"
#include "base/base_math.h"
#include "base/base_strings.h"
#include "base/base_system.h"
#include "base/base_memory_map.h"
#include "x64/x64.h"
#include "dwarf/dwarf.h"
#include "arch/arch.h"
#include "arch/dwarf/arch_dwarf.h"
#include "dwarf/dwarf_expr.h"
internal void abort_self(S32 code) { exit(code); }
internal SystemInfo *get_system_info(void) {
 static SystemInfo info = {.page_size=4096,.large_page_size=4096,.allocation_granularity=4096};
 return &info;
}
internal void *reserve_memory(U64 n) { return calloc(1, (size_t)n); }
internal B32 commit_memory(void *p,U64 n) { (void)n; return p != 0; }
internal void decommit_memory(void *p,U64 n) { (void)p; (void)n; }
internal void release_memory(void *p,U64 n) { (void)n; free(p); }
internal void *reserve_memory_large(U64 n) { return reserve_memory(n); }
internal B32 commit_memory_large(void *p,U64 n) { return commit_memory(p,n); }
internal Temp scratch_begin(Arena **conflicts,U64 count) {
 (void)conflicts; (void)count; Temp t={arena_alloc(),0}; return t;
}
internal void scratch_end(Temp t) { arena_release(t.arena); }
#include "base/base_core.c"
#include "base/base_arena.c"
#include "base/base_math.c"
#include "base/base_strings.c"
#include "base/base_memory_map.c"
#include "x64/generated/x64.meta.c"
#include "dwarf/dwarf.c"
#include "arch/arch.c"
#include "arch/dwarf/arch_dwarf.c"
#include "dwarf/dwarf_expr.c"
#endif
