/* split_dwarf_test.c -- Offline split DWARF attachment tests.

   These tests never execute an image and never collect samples from a
   live process.  Every debug section is synthesized in C below.  A PC
   sample comes from a fixed local list and is symbolized through the
   production backtrace_pcinfo callback path.

   Each case builds a skeleton main object (PC ranges, real DWARF 5 line
   program and .debug_addr) and a separate DW_UT_split_compile bundle
   (function tree, abbrev, strings, str_offsets contribution and its own
   DWARF 5 file table), then exercises either the section-bundle entry
   point backtrace_dwarf_attach_split or the ELF64 file entry point
   backtrace_elf_attach_split.  */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "backtrace.h"
#include "internal.h"
#include "task_support/check.h"
#include "task_support/basic_fixture.h"
#include "task_support/elf_fixture.h"

/* A synthetic main object: one DW_UT_skeleton unit, its line program
   and, optionally, a .debug_addr contribution.  */

struct main_fx
{
  struct fxbuf abbrev;
  struct fxbuf info;
  struct fxbuf line;
  struct fxbuf addr;
  struct dwarf_sections sections;
  int with_addr;
};

/* A synthetic DW_UT_split_compile bundle.  */

struct dwo_fx
{
  struct fxbuf abbrev;
  struct fxbuf info;
  struct fxbuf line;
  struct fxbuf str;
  struct fxbuf stroff;
  struct dwarf_sections sections;
};

/* Parameters of one synthesized split compilation.  Two bundles can
   therefore share every abbreviation code, local DIE offset and string
   index while carrying different names and a different dwo_id.  */

struct dwo_spec
{
  uint64_t dwo_id;
  int dwarf64;
  int explicit_str_base;
  int use_addrx;
  uint64_t base_pc;
  const char *unit_name;
  const char *func_name;
  const char *inline_name;
  const char *call_file;
  unsigned int call_line;
};

static void
put16 (struct fxbuf *b, unsigned v)
{
  fx8 (b, v & 0xff);
  fx8 (b, (v >> 8) & 0xff);
}

/* Fix up a DWARF 64 initial length at AT, where AT points at the
   0xffffffff marker followed by an 8-byte length field.  */

static void
fxlen64 (struct fxbuf *b, size_t at)
{
  uint64_t v;
  unsigned i;

  v = (uint64_t) (b->n - at - 12);
  for (i = 0; i < 8; ++i)
    b->b[at + 4 + i] = (unsigned char) (v >> (8 * i));
}

/* DWARF 5 line table identical in spirit to the one in basic_fixture.h:
   one directory, file index 0, a single row at BASE_PC of sample.c line
   7, ended half-open at BASE_PC + 0x20.  The table may start at a
   non-zero section offset when several tables are concatenated.  */

static void
make_line_table_at (struct fxbuf *l, uint64_t base_pc)
{
  static const unsigned char lens[] =
    {0, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 1};
  size_t start;
  size_t hl;
  size_t hs;
  unsigned h;
  unsigned j;

  start = l->n;
  fxn (l, 0, 4);		/* unit_length placeholder.  */
  put16 (l, 5);			/* DWARF version 5.  */
  fx8 (l, 8);			/* address_size.  */
  fx8 (l, 0);			/* segment_selector_size.  */
  hl = l->n;
  fxn (l, 0, 4);		/* header_length placeholder.  */
  hs = l->n;
  fx8 (l, 1);			/* minimum_instruction_length.  */
  fx8 (l, 1);			/* maximum_operations_per_instruction.  */
  fx8 (l, 1);			/* default_is_stmt.  */
  fx8 (l, 251);			/* line_base.  */
  fx8 (l, 14);			/* line_range.  */
  fx8 (l, 13);			/* opcode_base (12 length bytes follow).  */
  for (j = 0; j < 12; ++j)
    fx8 (l, lens[j]);

  /* Directories: zero formats, zero entries.  */
  fx8 (l, 0);
  fx8 (l, 0);

  /* File names: one format (DW_LNCT_path, DW_FORM_string), then one
     entry, matching the read order format count/formats/entry
     count/entries.  */
  fx8 (l, 1);
  fx8 (l, 1);
  fx8 (l, 8);
  fx8 (l, 1);
  fxs (l, "sample.c");

  h = (unsigned) (l->n - hs);
  for (j = 0; j < 4; ++j)
    l->b[hl + j] = (unsigned char) (h >> (8 * j));

  /* Line program.  */
  fx8 (l, 0);
  fx8 (l, 9);
  fx8 (l, 2);			/* DW_LNE_set_address.  */
  fxn (l, base_pc, 8);
  fx8 (l, 4);
  fx8 (l, 0);			/* DW_LNS_set_file 0.  */
  fx8 (l, 3);
  fx8 (l, 6);			/* DW_LNS_advance_line +6 -> line 7.  */
  fx8 (l, 1);			/* DW_LNS_copy.  */
  fx8 (l, 2);
  fx8 (l, 0x20);		/* DW_LNS_advance_pc.  */
  fx8 (l, 0);
  fx8 (l, 1);
  fx8 (l, 1);			/* DW_LNE_end_sequence.  */
  fxlen (l, start);
}

static void
make_line_table (struct fxbuf *l, uint64_t base_pc)
{
  make_line_table_at (l, base_pc);
}

static void
make_main_skeleton (struct main_fx *m, uint64_t dwo_id, uint64_t base_pc,
		    int with_addr)
{
  memset (m, 0, sizeof *m);
  m->with_addr = with_addr;

  /* Abbrev 1: DW_TAG_skeleton_unit, no children.  */
  fx8 (&m->abbrev, 1);
  fx8 (&m->abbrev, 0x4a);	/* DW_TAG_skeleton_unit.  */
  fx8 (&m->abbrev, 0);		/* no children.  */
  fx8 (&m->abbrev, 0x03); fx8 (&m->abbrev, 0x08); /* name, string.  */
  fx8 (&m->abbrev, 0x11); fx8 (&m->abbrev, 0x01); /* low_pc, addr.  */
  fx8 (&m->abbrev, 0x12); fx8 (&m->abbrev, 0x06); /* high_pc, data4.  */
  fx8 (&m->abbrev, 0x17); fx8 (&m->abbrev, 0x17); /* stmt_list, sec_offset.  */
  if (with_addr)
    {
      fx8 (&m->abbrev, 0x73); fx8 (&m->abbrev, 0x17); /* addr_base.  */
    }
  fx8 (&m->abbrev, 0);
  fx8 (&m->abbrev, 0);
  fx8 (&m->abbrev, 0);

  /* .debug_info header.  */
  fxn (&m->info, 0, 4);		/* unit_length placeholder.  */
  put16 (&m->info, 5);		/* version.  */
  fx8 (&m->info, 4);		/* DW_UT_skeleton.  */
  fx8 (&m->info, 8);		/* address_size.  */
  fxn (&m->info, 0, 4);		/* abbrev offset.  */
  fxn (&m->info, dwo_id, 8);

  /* Root DIE.  */
  fx8 (&m->info, 1);		/* abbrev code.  */
  fxs (&m->info, "sample.c");
  fxn (&m->info, base_pc, 8);	/* low_pc.  */
  fxn (&m->info, 0x20, 4);	/* high_pc constant length.  */
  fxn (&m->info, 0, 4);		/* stmt_list.  */
  if (with_addr)
    fxn (&m->info, 0, 4);	/* addr_base = start of .debug_addr.  */
  fx8 (&m->info, 0);
  fxlen (&m->info, 0);

  make_line_table (&m->line, base_pc);

  if (with_addr)
    {
      fxn (&m->addr, base_pc, 8);		/* addrx 0.  */
      fxn (&m->addr, base_pc + 0x8, 8);	/* addrx 1.  */
    }

  m->sections.data[DEBUG_INFO] = m->info.b;
  m->sections.size[DEBUG_INFO] = m->info.n;
  m->sections.data[DEBUG_ABBREV] = m->abbrev.b;
  m->sections.size[DEBUG_ABBREV] = m->abbrev.n;
  m->sections.data[DEBUG_LINE] = m->line.b;
  m->sections.size[DEBUG_LINE] = m->line.n;
  if (with_addr)
    {
      m->sections.data[DEBUG_ADDR] = m->addr.b;
      m->sections.size[DEBUG_ADDR] = m->addr.n;
    }
}

/* One main object containing two skeleton units that carry the SAME
   dwo_id.  A split bundle can therefore not be paired unambiguously.
   The two units keep distinct PC ranges and their own line tables.  */

static void
make_ambiguous_main (struct main_fx *m, uint64_t dwo_id,
		     uint64_t pc0, uint64_t pc1)
{
  size_t line_off0;
  size_t line_off1;
  size_t len0;
  size_t len1;

  memset (m, 0, sizeof *m);
  m->with_addr = 1;

  /* Shared abbreviation table (same shape as make_main_skeleton with
     addr_base).  */
  fx8 (&m->abbrev, 1);
  fx8 (&m->abbrev, 0x4a);	/* DW_TAG_skeleton_unit.  */
  fx8 (&m->abbrev, 0);
  fx8 (&m->abbrev, 0x03); fx8 (&m->abbrev, 0x08);
  fx8 (&m->abbrev, 0x11); fx8 (&m->abbrev, 0x01);
  fx8 (&m->abbrev, 0x12); fx8 (&m->abbrev, 0x06);
  fx8 (&m->abbrev, 0x17); fx8 (&m->abbrev, 0x17);
  fx8 (&m->abbrev, 0x73); fx8 (&m->abbrev, 0x17);
  fx8 (&m->abbrev, 0);
  fx8 (&m->abbrev, 0);
  fx8 (&m->abbrev, 0);

  /* First line table.  */
  line_off0 = m->line.n;
  make_line_table_at (&m->line, pc0);
  line_off1 = m->line.n;
  make_line_table_at (&m->line, pc1);

  /* First skeleton unit.  */
  len0 = m->info.n;
  fxn (&m->info, 0, 4);
  put16 (&m->info, 5);
  fx8 (&m->info, 4);		/* DW_UT_skeleton.  */
  fx8 (&m->info, 8);
  fxn (&m->info, 0, 4);		/* abbrev offset.  */
  fxn (&m->info, dwo_id, 8);
  fx8 (&m->info, 1);
  fxs (&m->info, "sample.c");
  fxn (&m->info, pc0, 8);
  fxn (&m->info, 0x20, 4);
  fxn (&m->info, line_off0, 4);
  fxn (&m->info, 0, 4);		/* addr_base.  */
  fx8 (&m->info, 0);
  fxlen (&m->info, len0);

  /* Second skeleton unit with the same dwo_id.  */
  len1 = m->info.n;
  fxn (&m->info, 0, 4);
  put16 (&m->info, 5);
  fx8 (&m->info, 4);
  fx8 (&m->info, 8);
  fxn (&m->info, 0, 4);
  fxn (&m->info, dwo_id, 8);
  fx8 (&m->info, 1);
  fxs (&m->info, "sample.c");
  fxn (&m->info, pc1, 8);
  fxn (&m->info, 0x20, 4);
  fxn (&m->info, line_off1, 4);
  fxn (&m->info, 0, 4);
  fx8 (&m->info, 0);
  fxlen (&m->info, len1);

  fxn (&m->addr, pc0, 8);
  fxn (&m->addr, pc0 + 0x8, 8);

  m->sections.data[DEBUG_INFO] = m->info.b;
  m->sections.size[DEBUG_INFO] = m->info.n;
  m->sections.data[DEBUG_ABBREV] = m->abbrev.b;
  m->sections.size[DEBUG_ABBREV] = m->abbrev.n;
  m->sections.data[DEBUG_LINE] = m->line.b;
  m->sections.size[DEBUG_LINE] = m->line.n;
  m->sections.data[DEBUG_ADDR] = m->addr.b;
  m->sections.size[DEBUG_ADDR] = m->addr.n;
}

/* Build the .debug_str.dwo string table and remember the offsets of
   the four indexed strings.  Index 0 is never the empty string here:
   equal indices in two bundles must resolve to different contents.  */

static void
build_dwo_strings (struct dwo_fx *d, const struct dwo_spec *s,
		   size_t off[4])
{
  fx8 (&d->str, 0);		/* conventional empty string at 0.  */
  off[0] = d->str.n;
  fxs (&d->str, s->func_name);
  off[1] = d->str.n;
  fxs (&d->str, s->inline_name);
  off[2] = d->str.n;
  fxs (&d->str, s->unit_name);
  off[3] = d->str.n;
  fxs (&d->str, s->call_file);
}

/* Build .debug_str_offsets.dwo.  With EXPLICIT_STR_BASE the compile unit
   will point straight past the header; otherwise the reader must derive
   that same default base from the DWARF 32/64 header itself.  */

static size_t
build_dwo_str_offsets (struct dwo_fx *d, const struct dwo_spec *s,
		       const size_t off[4])
{
  size_t entry_size;
  size_t base;
  unsigned i;

  entry_size = s->dwarf64 ? 8 : 4;
  if (s->dwarf64)
    {
      fxn (&d->stroff, 0xffffffff, 4);
      fxn (&d->stroff, 0, 8);	/* 64-bit length placeholder.  */
    }
  else
    fxn (&d->stroff, 0, 4);	/* 32-bit length placeholder.  */

  put16 (&d->stroff, 5);	/* version.  */
  fx8 (&d->stroff, 0);
  fx8 (&d->stroff, 0);		/* padding.  */

  base = d->stroff.n;
  for (i = 0; i < 4; ++i)
    fxn (&d->stroff, off[i], (unsigned) entry_size);

  if (s->dwarf64)
    fxlen64 (&d->stroff, 0);
  else
    fxlen (&d->stroff, 0);

  return base;
}

/* Abbreviations for the split compile unit.  */

static void
build_dwo_abbrev (struct dwo_fx *d, const struct dwo_spec *s)
{
  /* Code 1: DW_TAG_compile_unit with children.  */
  fx8 (&d->abbrev, 1);
  fx8 (&d->abbrev, 0x11);	/* DW_TAG_compile_unit.  */
  fx8 (&d->abbrev, 1);		/* has children.  */
  fx8 (&d->abbrev, 0x03); fx8 (&d->abbrev, 0x25); /* name, strx1.  */
  if (s->explicit_str_base)
    {
      fx8 (&d->abbrev, 0x72); fx8 (&d->abbrev, 0x17); /* str_offsets_base.  */
    }
  fx8 (&d->abbrev, 0);
  fx8 (&d->abbrev, 0);

  /* Code 2: abstract subprogram (name + declaration line, no PC).  */
  fx8 (&d->abbrev, 2);
  fx8 (&d->abbrev, 0x2e);	/* DW_TAG_subprogram.  */
  fx8 (&d->abbrev, 0);		/* no children.  */
  fx8 (&d->abbrev, 0x03); fx8 (&d->abbrev, 0x25); /* name, strx1.  */
  fx8 (&d->abbrev, 0x3b); fx8 (&d->abbrev, 0x0b); /* decl_line, data1.  */
  fx8 (&d->abbrev, 0);
  fx8 (&d->abbrev, 0);

  /* Code 3: concrete subprogram with an inlined child.  */
  fx8 (&d->abbrev, 3);
  fx8 (&d->abbrev, 0x2e);
  fx8 (&d->abbrev, 1);		/* has children.  */
  fx8 (&d->abbrev, 0x31); fx8 (&d->abbrev, 0x13); /* abstract_origin, ref4.  */
  fx8 (&d->abbrev, 0x11);
  fx8 (&d->abbrev, s->use_addrx ? 0x29 : 0x01); /* low_pc.  */
  fx8 (&d->abbrev, 0x12); fx8 (&d->abbrev, 0x06); /* high_pc, data4.  */
  fx8 (&d->abbrev, 0);
  fx8 (&d->abbrev, 0);

  /* Code 4: inlined subroutine.  */
  fx8 (&d->abbrev, 4);
  fx8 (&d->abbrev, 0x1d);	/* DW_TAG_inlined_subroutine.  */
  fx8 (&d->abbrev, 0);
  fx8 (&d->abbrev, 0x31); fx8 (&d->abbrev, 0x13); /* abstract_origin, ref4.  */
  fx8 (&d->abbrev, 0x58); fx8 (&d->abbrev, 0x0b); /* call_file, data1.  */
  fx8 (&d->abbrev, 0x59); fx8 (&d->abbrev, 0x0b); /* call_line, data1.  */
  fx8 (&d->abbrev, 0x11);
  fx8 (&d->abbrev, s->use_addrx ? 0x29 : 0x01); /* low_pc.  */
  fx8 (&d->abbrev, 0x12); fx8 (&d->abbrev, 0x06); /* high_pc, data4.  */
  fx8 (&d->abbrev, 0);
  fx8 (&d->abbrev, 0);

  fx8 (&d->abbrev, 0);
}

/* DWARF 5 line header carrying only the split file table.  No program
   is ever run from a DWO.  */

static void
build_dwo_line_header (struct dwo_fx *d, const struct dwo_spec *s)
{
  size_t unit_len_at;
  size_t hl;
  size_t hs;
  unsigned h;
  unsigned j;

  unit_len_at = d->line.n;
  fxn (&d->line, 0, 4);		/* unit_length.  */
  put16 (&d->line, 5);
  fx8 (&d->line, 8);		/* address_size.  */
  fx8 (&d->line, 0);		/* segment_selector_size.  */
  hl = d->line.n;
  fxn (&d->line, 0, 4);		/* header_length.  */
  hs = d->line.n;
  fx8 (&d->line, 1);
  fx8 (&d->line, 1);
  fx8 (&d->line, 1);
  fx8 (&d->line, 251);
  fx8 (&d->line, 14);
  fx8 (&d->line, 13);
  {
    static const unsigned char lens[] =
      {0, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 1};
    for (j = 0; j < 12; ++j)
      fx8 (&d->line, lens[j]);
  }
  /* Directories: none.  */
  fx8 (&d->line, 0);
  fx8 (&d->line, 0);
  /* File names: one format, then one entry (index 0).  */
  fx8 (&d->line, 1);		/* format count.  */
  fx8 (&d->line, 1);		/* DW_LNCT_path.  */
  fx8 (&d->line, 8);		/* DW_FORM_string.  */
  fx8 (&d->line, 1);		/* entry count.  */
  fxs (&d->line, s->call_file);

  h = (unsigned) (d->line.n - hs);
  for (j = 0; j < 4; ++j)
    d->line.b[hl + j] = (unsigned char) (h >> (8 * j));
  fxlen (&d->line, unit_len_at);
}

static void
make_dwo (struct dwo_fx *d, const struct dwo_spec *s)
{
  size_t stroff[4];
  size_t str_base;
  size_t len_at;
  size_t off_alpha;
  size_t off_beta;

  memset (d, 0, sizeof *d);

  build_dwo_strings (d, s, stroff);
  str_base = build_dwo_str_offsets (d, s, stroff);
  build_dwo_abbrev (d, s);
  build_dwo_line_header (d, s);

  /* .debug_info.dwo.  ref4 stays a 4-byte unit-relative offset in both
     DWARF 32 and DWARF 64.  */
  len_at = d->info.n;
  if (s->dwarf64)
    {
      fxn (&d->info, 0xffffffff, 4);
      fxn (&d->info, 0, 8);
    }
  else
    fxn (&d->info, 0, 4);
  put16 (&d->info, 5);		/* version.  */
  fx8 (&d->info, 5);		/* DW_UT_split_compile.  */
  fx8 (&d->info, 8);		/* address_size.  */
  if (s->dwarf64)
    fxn (&d->info, 0, 8);	/* abbrev offset.  */
  else
    fxn (&d->info, 0, 4);
  fxn (&d->info, s->dwo_id, 8);

  /* Root compile unit.  */
  fx8 (&d->info, 1);		/* abbrev 1.  */
  fx8 (&d->info, 2);		/* strx index 2 -> unit_name.  */
  if (s->explicit_str_base)
    {
      if (s->dwarf64)
	fxn (&d->info, str_base, 8);
      else
	fxn (&d->info, str_base, 4);
    }

  /* Abstract subprogram alpha (no PC).  */
  off_alpha = d->info.n - len_at;
  fx8 (&d->info, 2);
  fx8 (&d->info, 0);		/* strx index 0 -> func_name.  */
  fx8 (&d->info, 3);		/* decl_line.  */

  /* Abstract subprogram beta (no PC), the inlined callee.  */
  off_beta = d->info.n - len_at;
  fx8 (&d->info, 2);
  fx8 (&d->info, 1);		/* strx index 1 -> inline_name.  */
  fx8 (&d->info, 5);		/* decl_line.  */

  /* Concrete out-of-line subprogram.  ref4 is unit-relative, including
     the unit header, matching the production reader's semantics.  */
  fx8 (&d->info, 3);
  fxn (&d->info, off_alpha, 4);	/* ref4 abstract origin.  */
  if (s->use_addrx)
    fx8 (&d->info, 0);		/* addrx 0.  */
  else
    fxn (&d->info, s->base_pc, 8); /* DW_FORM_addr.  */
  fxn (&d->info, 0x20, 4);	/* high_pc constant length.  */

  /* Inlined beta inside the concrete subprogram.  */
  fx8 (&d->info, 4);
  fxn (&d->info, off_beta, 4);
  fx8 (&d->info, 0);		/* call_file 0 (DWO file table).  */
  fx8 (&d->info, (unsigned) s->call_line);
  if (s->use_addrx)
    fx8 (&d->info, 1);		/* addrx 1 -> base + 8.  */
  else
    fxn (&d->info, s->base_pc + 0x8, 8);
  fxn (&d->info, 0x10, 4);	/* half-open inline length.  */

  fx8 (&d->info, 0);		/* end concrete children.  */
  fx8 (&d->info, 0);		/* end root children.  */

  if (s->dwarf64)
    fxlen64 (&d->info, len_at);
  else
    fxlen (&d->info, len_at);

  d->sections.data[DEBUG_INFO] = d->info.b;
  d->sections.size[DEBUG_INFO] = d->info.n;
  d->sections.data[DEBUG_ABBREV] = d->abbrev.b;
  d->sections.size[DEBUG_ABBREV] = d->abbrev.n;
  d->sections.data[DEBUG_LINE] = d->line.b;
  d->sections.size[DEBUG_LINE] = d->line.n;
  d->sections.data[DEBUG_STR] = d->str.b;
  d->sections.size[DEBUG_STR] = d->str.n;
  d->sections.data[DEBUG_STR_OFFSETS] = d->stroff.b;
  d->sections.size[DEBUG_STR_OFFSETS] = d->stroff.n;
}

/* Error callback bookkeeping.  */

struct error_tracker
{
  unsigned int count;
  char last[160];
};

static void
track_error (void *data, const char *msg, int errnum)
{
  struct error_tracker *t = (struct error_tracker *) data;

  (void) errnum;
  ++t->count;
  snprintf (t->last, sizeof t->last, "%s", msg);
}

/* Evidence captured from backtrace_pcinfo for one fixed PC sample.  */

struct evidence
{
  struct
  {
    uintptr_t pc;
    const char *file;
    int line;
    const char *function;
  } frames[8];
  unsigned int n;
  int stop_after_first;
};

/* Combined callback data: the full callback records frames in E, while
   error reporting still lands in ERR.  */

struct query_context
{
  struct evidence *e;
  struct error_tracker *err;
};

static int
evidence_callback (void *data, uintptr_t pc, const char *file, int line,
		   const char *function)
{
  struct query_context *ctx = (struct query_context *) data;
  struct evidence *e = ctx->e;

  if (e->n >= sizeof e->frames / sizeof e->frames[0])
    return 1;
  e->frames[e->n].pc = pc;
  e->frames[e->n].file = file;
  e->frames[e->n].line = line;
  e->frames[e->n].function = function;
  ++e->n;
  return e->stop_after_first ? 1 : 0;
}

/* Error callback paired with evidence_callback: the full callback and
   the error callback share one query_context but write to different
   members, so the tracker is reached through the context rather than
   by reinterpreting the data pointer.  */

static void
query_error (void *data, const char *msg, int errnum)
{
  struct query_context *ctx = (struct query_context *) data;

  track_error (ctx->err, msg, errnum);
}

/* Print the offline crash-evidence symbolization report and verify its
   provenance in-process.  MODULE tags the synthetic build so frames from
   two different builds can never be silently merged.  */

static void
report_evidence (const char *module, const struct evidence *e)
{
  unsigned int i;

  printf ("REPORT module=%s frames=%u\n", module, e->n);
  for (i = 0; i < e->n; ++i)
    {
      if (e->frames[i].function == NULL)
	printf ("REPORT module=%s pc=%lx file=%s line=%d "
		"function=FUNCTION_EVIDENCE_MISSING\n",
		module, (unsigned long) e->frames[i].pc,
		e->frames[i].file ? e->frames[i].file : "-",
		e->frames[i].line);
      else
	printf ("REPORT module=%s pc=%lx file=%s line=%d function=%s\n",
		module, (unsigned long) e->frames[i].pc,
		e->frames[i].file ? e->frames[i].file : "-",
		e->frames[i].line, e->frames[i].function);
    }
}

/* Add a synthetic main object to a stack-constructed state, mirroring
   the baseline harness.  */

static int
add_main (struct backtrace_state *st, const struct main_fx *m,
	  struct error_tracker *err, struct dwarf_data **entry)
{
  struct libbacktrace_base_address base;
  fileline fn;

  memset (&base, 0, sizeof base);
  return backtrace_dwarf_add (st, base, &m->sections, 0, NULL, track_error,
			      err, &fn, entry)
	 && (st->fileline_fn = fn) != NULL;
}

static void
query (struct backtrace_state *st, uintptr_t pc, struct evidence *e,
       struct error_tracker *err)
{
  struct query_context ctx;

  ctx.e = e;
  ctx.err = err;
  memset (e, 0, sizeof *e);
  backtrace_pcinfo (st, pc, evidence_callback, query_error, &ctx);
}

/* Create a unique ELF64 DWO path inside TMPDIR.  */

static void
dwo_path (char *buf, size_t len, const char *tag)
{
  const char *dir;

  dir = getenv ("TMPDIR");
  if (dir == NULL || dir[0] == '\0')
    dir = "/tmp";
  snprintf (buf, len, "%s/bt-split-%s-%ld.dwo", dir, tag, (long) getpid ());
}

/* Verify the fully attached function + inline chain for one build.  */

static int
check_full_chain (struct backtrace_state *st, const struct dwo_spec *s,
		  struct error_tracker *err)
{
  struct evidence e;

  /* PC in the concrete function but outside the inlined range.  */
  query (st, s->base_pc + 1, &e, err);
  TASK_CHECK (e.n == 1);
  TASK_CHECK (e.frames[0].function != NULL);
  TASK_CHECK (strcmp (e.frames[0].function, s->func_name) == 0);
  TASK_CHECK (e.frames[0].file != NULL
	      && strstr (e.frames[0].file, "sample.c") != NULL);
  TASK_CHECK (e.frames[0].line == 7);

  /* PC inside the inlined range: inner beta first, then outer alpha.  */
  query (st, s->base_pc + 0xa, &e, err);
  TASK_CHECK (e.n == 2);
  TASK_CHECK (strcmp (e.frames[0].function, s->inline_name) == 0);
  TASK_CHECK (e.frames[0].file != NULL
	      && strstr (e.frames[0].file, "sample.c") != NULL);
  TASK_CHECK (e.frames[0].line == 7);
  TASK_CHECK (strcmp (e.frames[1].function, s->func_name) == 0);
  TASK_CHECK (e.frames[1].file != NULL
	      && strcmp (e.frames[1].file, s->call_file) == 0);
  TASK_CHECK (e.frames[1].line == (int) s->call_line);

  report_evidence ("attached", &e);
  return 0;
}

/* pairing: identity rejection must leave line-only evidence, then the
   correct dwo_id pairs and the full chain appears.  Section API.  */

static int
case_pairing_sections (void)
{
  static const uint64_t id_a = UINT64_C (0x1122334455667788);
  static const uint64_t id_b = UINT64_C (0x8877665544332211);
  struct backtrace_state st;
  struct error_tracker err;
  struct main_fx m;
  struct dwo_fx right;
  struct dwo_fx wrong;
  struct dwo_spec sr;
  struct dwo_spec sw;
  struct dwarf_data *entry;
  struct evidence e;

  memset (&st, 0, sizeof st);
  memset (&err, 0, sizeof err);
  make_main_skeleton (&m, id_a, 0x1000, 1);

  memset (&sr, 0, sizeof sr);
  sr.dwo_id = id_a;
  sr.base_pc = 0x1000;
  sr.unit_name = "sample.c";
  sr.func_name = "alpha";
  sr.inline_name = "beta";
  sr.call_file = "calls.c";
  sr.call_line = 9;
  make_dwo (&right, &sr);

  sw = sr;
  sw.dwo_id = id_b;
  sw.func_name = "alpha_FROM_OTHER_BUILD";
  make_dwo (&wrong, &sw);

  TASK_CHECK (add_main (&st, &m, &err, &entry));

  /* Before attachment the skeleton still provides line evidence, with
     function evidence explicitly missing.  */
  query (&st, 0x1001, &e, &err);
  TASK_CHECK (e.n == 1);
  TASK_CHECK (e.frames[0].function == NULL);
  TASK_CHECK (e.frames[0].line == 7);
  report_evidence ("unbound", &e);

  /* Wrong identity: reject, report, and never borrow another build's
     function name.  */
  TASK_CHECK (!backtrace_dwarf_attach_split (&st, entry, &wrong.sections, 0,
					     track_error, &err));
  TASK_CHECK (err.count >= 1);
  query (&st, 0x1001, &e, &err);
  TASK_CHECK (e.n == 1);
  TASK_CHECK (e.frames[0].function == NULL);
  TASK_CHECK (e.frames[0].line == 7);
  report_evidence ("rejected", &e);

  /* Correct identity.  */
  TASK_CHECK (backtrace_dwarf_attach_split (&st, entry, &right.sections, 0,
					    track_error, &err));

  /* Duplicate binding is rejected.  */
  TASK_CHECK (!backtrace_dwarf_attach_split (&st, entry, &right.sections, 0,
					     track_error, &err));

  TASK_CHECK (check_full_chain (&st, &sr, &err) == 0);
  return 0;
}

/* pairing: two skeletons carrying the same dwo_id make the match
   ambiguous; attachment must fail while existing units stay readable.  */

static int
case_pairing_ambiguous (void)
{
  static const uint64_t id_a = UINT64_C (0x1122334455667788);
  struct backtrace_state st;
  struct error_tracker err;
  struct main_fx m;
  struct dwo_fx d;
  struct dwo_spec s;
  struct dwarf_data *entry;
  struct evidence ev;

  memset (&st, 0, sizeof st);
  memset (&err, 0, sizeof err);

  /* One main object, two skeletons sharing the same dwo_id.  */
  make_ambiguous_main (&m, id_a, 0x1000, 0x2000);
  TASK_CHECK (add_main (&st, &m, &err, &entry));

  memset (&s, 0, sizeof s);
  s.dwo_id = id_a;
  s.base_pc = 0x1000;
  s.unit_name = "sample.c";
  s.func_name = "alpha";
  s.inline_name = "beta";
  s.call_file = "calls.c";
  s.call_line = 9;
  make_dwo (&d, &s);

  /* Ambiguous identity: reject, report, bind nothing.  */
  TASK_CHECK (!backtrace_dwarf_attach_split (&st, entry, &d.sections, 0,
					     track_error, &err));
  TASK_CHECK (err.count >= 1);

  /* Both skeletons remain line-only and usable.  */
  query (&st, 0x1001, &ev, &err);
  TASK_CHECK (ev.n == 1 && ev.frames[0].function == NULL
	      && ev.frames[0].line == 7);
  query (&st, 0x2001, &ev, &err);
  TASK_CHECK (ev.n == 1 && ev.frames[0].function == NULL
	      && ev.frames[0].line == 7);
  report_evidence ("ambiguous", &ev);
  return 0;
}

/* pairing: the same reject-then-pair flow through the ELF64 entry
   point.  Both DWO files are deleted before the delayed queries.  */

static int
case_pairing_elf (void)
{
  static const uint64_t id_a = UINT64_C (0x2233445566778899);
  static const uint64_t id_b = UINT64_C (0x9988776655443322);
  struct backtrace_state st;
  struct error_tracker err;
  struct main_fx m;
  struct dwo_fx right;
  struct dwo_fx wrong;
  struct dwo_spec sr;
  struct dwo_spec sw;
  struct dwarf_data *entry;
  struct evidence e;
  char path_right[256];
  char path_wrong[256];

  memset (&st, 0, sizeof st);
  memset (&err, 0, sizeof err);
  make_main_skeleton (&m, id_a, 0x3000, 1);

  memset (&sr, 0, sizeof sr);
  sr.dwo_id = id_a;
  sr.base_pc = 0x3000;
  sr.unit_name = "sample.c";
  sr.func_name = "elf_alpha";
  sr.inline_name = "elf_beta";
  sr.call_file = "calls.c";
  sr.call_line = 11;
  make_dwo (&right, &sr);
  sw = sr;
  sw.dwo_id = id_b;
  sw.func_name = "elf_alpha_OTHER";
  make_dwo (&wrong, &sw);

  dwo_path (path_right, sizeof path_right, "right");
  dwo_path (path_wrong, sizeof path_wrong, "wrong");
  TASK_CHECK (write_basic_elf (path_right, &right.sections, ".dwo"));
  TASK_CHECK (write_basic_elf (path_wrong, &wrong.sections, ".dwo"));

  TASK_CHECK (add_main (&st, &m, &err, &entry));

  TASK_CHECK (!backtrace_elf_attach_split (&st, entry, path_wrong,
					   track_error, &err));
  TASK_CHECK (err.count >= 1);
  TASK_CHECK (backtrace_elf_attach_split (&st, entry, path_right,
					  track_error, &err));

  /* The descriptors and views are gone; delete the files too.  Later
     queries must still resolve from the owned section copies.  */
  TASK_CHECK (unlink (path_wrong) == 0);
  TASK_CHECK (unlink (path_right) == 0);

  query (&st, 0x3001, &e, &err);
  TASK_CHECK (e.n == 1);
  TASK_CHECK (e.frames[0].function != NULL
	      && strcmp (e.frames[0].function, "elf_alpha") == 0);
  query (&st, 0x300a, &e, &err);
  TASK_CHECK (e.n == 2);
  TASK_CHECK (strcmp (e.frames[0].function, "elf_beta") == 0);
  TASK_CHECK (strcmp (e.frames[1].function, "elf_alpha") == 0);
  TASK_CHECK (strcmp (e.frames[1].file, "calls.c") == 0);
  TASK_CHECK (e.frames[1].line == 11);
  report_evidence ("elf-attached", &e);
  return 0;
}

/* contexts: two skeletons bound to two different DWOs that share the
   same short file name, local DIE offsets and string indices but hold
   different strings and dwo_ids.  The first bundle enters through the
   section API, the second through the ELF API.  */

static int
case_contexts_dual_isolation (void)
{
  static const uint64_t id_a = UINT64_C (0xaaaabbbbccccdddd);
  static const uint64_t id_b = UINT64_C (0xddddccccbbbbaaaa);
  struct backtrace_state st;
  struct error_tracker err;
  struct main_fx ma;
  struct main_fx mb;
  struct dwo_fx da;
  struct dwo_fx db;
  struct dwo_spec sa;
  struct dwo_spec sb;
  struct dwarf_data *ea;
  struct dwarf_data *eb;
  struct evidence e;
  char path_b[256];

  memset (&st, 0, sizeof st);
  memset (&err, 0, sizeof err);
  make_main_skeleton (&ma, id_a, 0x4000, 1);
  make_main_skeleton (&mb, id_b, 0x5000, 1);
  TASK_CHECK (add_main (&st, &ma, &err, &ea));
  TASK_CHECK (add_main (&st, &mb, &err, &eb));

  memset (&sa, 0, sizeof sa);
  sa.dwo_id = id_a;
  sa.base_pc = 0x4000;
  sa.unit_name = "sample.c";
  sa.func_name = "alpha_A";
  sa.inline_name = "helper_A";
  sa.call_file = "calls.c";
  sa.call_line = 21;
  make_dwo (&da, &sa);

  /* Identical indices/offsets/short names, different contents and id.  */
  memset (&sb, 0, sizeof sb);
  sb.dwo_id = id_b;
  sb.base_pc = 0x5000;
  sb.unit_name = "sample.c";
  sb.func_name = "alpha_B";
  sb.inline_name = "helper_B";
  sb.call_file = "calls.c";
  sb.call_line = 21;
  make_dwo (&db, &sb);

  TASK_CHECK (backtrace_dwarf_attach_split (&st, ea, &da.sections, 0,
					    track_error, &err));

  dwo_path (path_b, sizeof path_b, "dual");
  TASK_CHECK (write_basic_elf (path_b, &db.sections, ".dwo"));
  TASK_CHECK (backtrace_elf_attach_split (&st, eb, path_b, track_error,
					  &err));
  TASK_CHECK (unlink (path_b) == 0);

  query (&st, 0x400a, &e, &err);
  TASK_CHECK (e.n == 2);
  TASK_CHECK (strcmp (e.frames[0].function, "helper_A") == 0);
  TASK_CHECK (strcmp (e.frames[1].function, "alpha_A") == 0);
  TASK_CHECK (strcmp (e.frames[1].file, "calls.c") == 0);
  report_evidence ("build-A", &e);

  query (&st, 0x500a, &e, &err);
  TASK_CHECK (e.n == 2);
  TASK_CHECK (strcmp (e.frames[0].function, "helper_B") == 0);
  TASK_CHECK (strcmp (e.frames[1].function, "alpha_B") == 0);
  TASK_CHECK (strcmp (e.frames[1].file, "calls.c") == 0);
  report_evidence ("build-B", &e);

  /* No borrowing: build A never resolves a build-B name.  */
  query (&st, 0x4001, &e, &err);
  TASK_CHECK (strcmp (e.frames[0].function, "alpha_A") == 0);
  return 0;
}

/* contexts: addrx resolution through the skeleton addr_base, constant
   high_pc lengths and the default (omitted) str_offsets_base, in a
   DWARF 64 split unit.  */

static int
case_contexts_dwarf64_addrx (void)
{
  static const uint64_t id_a = UINT64_C (0x13579bdf2468ace0);
  struct backtrace_state st;
  struct error_tracker err;
  struct main_fx m;
  struct dwo_fx d;
  struct dwo_spec s;
  struct dwarf_data *entry;
  struct evidence e;

  memset (&st, 0, sizeof st);
  memset (&err, 0, sizeof err);
  make_main_skeleton (&m, id_a, 0x6000, 1);
  TASK_CHECK (add_main (&st, &m, &err, &entry));

  memset (&s, 0, sizeof s);
  s.dwo_id = id_a;
  s.dwarf64 = 1;
  s.use_addrx = 1;
  s.base_pc = 0x6000;
  s.unit_name = "sample.c";
  s.func_name = "wide_alpha";
  s.inline_name = "wide_beta";
  s.call_file = "calls.c";
  s.call_line = 31;
  make_dwo (&d, &s);

  TASK_CHECK (backtrace_dwarf_attach_split (&st, entry, &d.sections, 0,
					    track_error, &err));
  TASK_CHECK (err.count == 0);

  query (&st, 0x6001, &e, &err);
  TASK_CHECK (e.n == 1);
  TASK_CHECK (strcmp (e.frames[0].function, "wide_alpha") == 0);
  query (&st, 0x600a, &e, &err);
  TASK_CHECK (e.n == 2);
  TASK_CHECK (strcmp (e.frames[0].function, "wide_beta") == 0);
  TASK_CHECK (strcmp (e.frames[1].function, "wide_alpha") == 0);
  report_evidence ("dwarf64-addrx", &e);
  return 0;
}

/* contexts: an explicit DW_AT_str_offsets_base in a DWARF 32 split
   unit.  */

static int
case_contexts_explicit_strbase (void)
{
  static const uint64_t id_a = UINT64_C (0x0fedcba987654321);
  struct backtrace_state st;
  struct error_tracker err;
  struct main_fx m;
  struct dwo_fx d;
  struct dwo_spec s;
  struct dwarf_data *entry;
  struct evidence e;

  memset (&st, 0, sizeof st);
  memset (&err, 0, sizeof err);
  make_main_skeleton (&m, id_a, 0x7000, 0);
  TASK_CHECK (add_main (&st, &m, &err, &entry));

  memset (&s, 0, sizeof s);
  s.dwo_id = id_a;
  s.explicit_str_base = 1;
  s.base_pc = 0x7000;
  s.unit_name = "sample.c";
  s.func_name = "strbase_alpha";
  s.inline_name = "strbase_beta";
  s.call_file = "calls.c";
  s.call_line = 41;
  make_dwo (&d, &s);

  TASK_CHECK (backtrace_dwarf_attach_split (&st, entry, &d.sections, 0,
					    track_error, &err));
  query (&st, 0x700a, &e, &err);
  TASK_CHECK (e.n == 2);
  TASK_CHECK (strcmp (e.frames[0].function, "strbase_beta") == 0);
  TASK_CHECK (strcmp (e.frames[1].function, "strbase_alpha") == 0);
  report_evidence ("explicit-strbase", &e);
  return 0;
}

/* late_attach: alternate queries around the attachment, half-open
   ranges, early callback termination and ordinary-unit regression.  */

static int
case_late_attach_alternating (void)
{
  static const uint64_t id_a = UINT64_C (0x5555666677778888);
  struct backtrace_state st;
  struct backtrace_state ordinary_st;
  struct error_tracker err;
  struct main_fx m;
  struct basic_fixture ordinary;
  struct dwo_fx d;
  struct dwo_spec s;
  struct dwarf_data *entry;
  struct dwarf_data *ordinary_entry;
  struct evidence e;
  struct libbacktrace_base_address base;
  fileline fn;

  memset (&st, 0, sizeof st);
  memset (&ordinary_st, 0, sizeof ordinary_st);
  memset (&err, 0, sizeof err);
  make_main_skeleton (&m, id_a, 0x8000, 1);
  TASK_CHECK (add_main (&st, &m, &err, &entry));

  memset (&s, 0, sizeof s);
  s.dwo_id = id_a;
  s.use_addrx = 1;
  s.base_pc = 0x8000;
  s.unit_name = "sample.c";
  s.func_name = "late_alpha";
  s.inline_name = "late_beta";
  s.call_file = "calls.c";
  s.call_line = 51;
  make_dwo (&d, &s);

  /* Before attachment: line evidence survives, function missing.  */
  query (&st, 0x8001, &e, &err);
  TASK_CHECK (e.n == 1 && e.frames[0].function == NULL
	      && e.frames[0].line == 7);
  report_evidence ("late-unbound", &e);

  /* Ordinary single-object module keeps its original behavior, in a
     separate state so its identical PC range cannot overlap.  */
  make_basic_fixture (&ordinary, 0);
  memset (&base, 0, sizeof base);
  TASK_CHECK (backtrace_dwarf_add (&ordinary_st, base, &ordinary.sections, 0,
				   NULL, track_error, &err, &fn,
				   &ordinary_entry));
  ordinary_st.fileline_fn = fn;
  query (&ordinary_st, 0x1001, &e, &err);
  TASK_CHECK (e.n == 1);
  TASK_CHECK (e.frames[0].function != NULL
	      && strcmp (e.frames[0].function, "ordinary") == 0);

  /* The skeleton entry captured above is untouched by the second
     module; bind the split unit to it now.  */
  TASK_CHECK (backtrace_dwarf_attach_split (&st, entry, &d.sections, 0,
					    track_error, &err));

  /* After attachment the same PC immediately shows the function.  */
  query (&st, 0x8001, &e, &err);
  TASK_CHECK (e.n == 1);
  TASK_CHECK (strcmp (e.frames[0].function, "late_alpha") == 0);

  /* And an unbound-style query on the ordinary module is unchanged.  */
  query (&ordinary_st, 0x1001, &e, &err);
  TASK_CHECK (strcmp (e.frames[0].function, "ordinary") == 0);
  report_evidence ("late-bound", &e);

  /* Half-open range: PC at the high boundary is outside the unit.  */
  query (&st, 0x8020, &e, &err);
  TASK_CHECK (e.n == 1);
  TASK_CHECK (e.frames[0].file == NULL);
  TASK_CHECK (e.frames[0].function == NULL);

  /* Early callback termination is honored, including for inline frames.  */
  memset (&e, 0, sizeof e);
  e.stop_after_first = 1;
  {
    struct query_context ctx;

    ctx.e = &e;
    ctx.err = &err;
    backtrace_pcinfo (&st, 0x800a, evidence_callback, query_error, &ctx);
  }
  TASK_CHECK (e.n == 1);
  TASK_CHECK (strcmp (e.frames[0].function, "late_beta") == 0);
  return 0;
}

/* late_attach: ELF lifecycle (file removed before delayed queries) and
   the production ELF reader path for an ordinary module.  */

static int
case_late_attach_elf_lifecycle (void)
{
  static const uint64_t id_a = UINT64_C (0x9999aaaabbbbcccc);
  struct backtrace_state st;
  struct backtrace_state elf_st;
  struct error_tracker err;
  struct main_fx m;
  struct basic_fixture basic;
  struct dwo_fx d;
  struct dwo_spec s;
  struct dwarf_data *entry;
  struct evidence e;
  char path_dwo[256];
  char path_elf[256];
  int fd;
  fileline fn;

  memset (&st, 0, sizeof st);
  memset (&elf_st, 0, sizeof elf_st);
  memset (&err, 0, sizeof err);
  make_main_skeleton (&m, id_a, 0x9000, 1);
  TASK_CHECK (add_main (&st, &m, &err, &entry));

  memset (&s, 0, sizeof s);
  s.dwo_id = id_a;
  s.base_pc = 0x9000;
  s.unit_name = "sample.c";
  s.func_name = "life_alpha";
  s.inline_name = "life_beta";
  s.call_file = "calls.c";
  s.call_line = 61;
  make_dwo (&d, &s);

  dwo_path (path_dwo, sizeof path_dwo, "life");
  TASK_CHECK (write_basic_elf (path_dwo, &d.sections, ".dwo"));
  TASK_CHECK (backtrace_elf_attach_split (&st, entry, path_dwo,
					  track_error, &err));
  TASK_CHECK (unlink (path_dwo) == 0);

  /* Delayed queries after the file is gone.  */
  query (&st, 0x9001, &e, &err);
  TASK_CHECK (e.n == 1);
  TASK_CHECK (strcmp (e.frames[0].function, "life_alpha") == 0);
  query (&st, 0x900a, &e, &err);
  TASK_CHECK (e.n == 2);
  TASK_CHECK (strcmp (e.frames[0].function, "life_beta") == 0);
  TASK_CHECK (strcmp (e.frames[1].file, "calls.c") == 0);
  report_evidence ("elf-lifecycle", &e);

  /* Ordinary ELF production reader regression.  */
  dwo_path (path_elf, sizeof path_elf, "basic");
  make_basic_fixture (&basic, 0);
  TASK_CHECK (write_basic_elf (path_elf, &basic.sections, ""));
  fd = backtrace_open (path_elf, track_error, &err, NULL);
  TASK_CHECK (fd >= 0);
  TASK_CHECK (backtrace_initialize (&elf_st, path_elf, fd, track_error, &err,
				    &fn));
  elf_st.fileline_fn = fn;
  query (&elf_st, 0x1001, &e, &err);
  TASK_CHECK (e.n == 1);
  TASK_CHECK (e.frames[0].function != NULL
	      && strcmp (e.frames[0].function, "ordinary") == 0);
  TASK_CHECK (unlink (path_elf) == 0);
  return 0;
}

const struct task_case task_cases[] =
{
  {"pairing", "wrong_id_then_pair_sections", case_pairing_sections},
  {"pairing", "ambiguous_skeleton_identity", case_pairing_ambiguous},
  {"pairing", "wrong_id_then_pair_elf", case_pairing_elf},

  {"contexts", "dual_units_string_isolation", case_contexts_dual_isolation},
  {"contexts", "dwarf64_addrx_default_strbase", case_contexts_dwarf64_addrx},
  {"contexts", "dwarf32_explicit_strbase", case_contexts_explicit_strbase},

  {"late_attach", "alternating_queries_and_regression",
   case_late_attach_alternating},
  {"late_attach", "elf_lifecycle_and_reader_regression",
   case_late_attach_elf_lifecycle},
};

const size_t task_case_count
  = sizeof task_cases / sizeof task_cases[0];
