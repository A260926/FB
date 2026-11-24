/*
** result_state_contract.c
**
** End-to-end contract tests for @transferswhen ownership hand-off.
** Every case invokes the freshly compiled Splint binary rather than
** testing an internal implementation function.
*/

#include "../task_support/check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RS_DIR ".task-build/contract-cases"
#define RS_TMP ".task-build/contract-tmp"
#define RS_OUT_SIZE 200000

static char rs_output[RS_OUT_SIZE];
static int rs_exit_status;
static unsigned rs_case_number;

static void rs_prepare(void)
{
  (void)system("mkdir -p " RS_DIR " " RS_TMP);
}

static int rs_write_file(const char *path, const char *source)
{
  FILE *f = fopen(path, "w");
  if (!f)
    return 0;
  fputs(source, f);
  return fclose(f) == 0;
}

static int rs_append_file(const char *path, const char *text)
{
  FILE *f = fopen(path, "a");
  if (!f)
    return 0;
  fputs(text, f);
  return fclose(f) == 0;
}

/* Run ./src/splint on a newly written case file. */
static int rs_invoke(const char *name, const char *source, const char *extra)
{
  char path[512];
  char command[2400];
  FILE *pipe;
  size_t got;
  char *marker;

  rs_prepare();
  ++rs_case_number;
  snprintf(path, sizeof(path), "%s/%s.c", RS_DIR, name);
  if (!rs_write_file(path, source))
    return 0;

  snprintf(command, sizeof(command),
           "./src/splint -nof -nolib -tmpdir %s "
           "-exportlocal -exportfcn -paramuse -modnomods %s \"%s\" 2>&1; "
           "printf '\\nRS_EXIT:%%d\\n' \"$?\"",
           RS_TMP, extra ? extra : "", path);

  pipe = popen(command, "r");
  if (!pipe)
    return 0;
  got = fread(rs_output, 1, sizeof(rs_output) - 1, pipe);
  rs_output[got] = '\0';
  (void)pclose(pipe);

  marker = strstr(rs_output, "RS_EXIT:");
  if (!marker)
    {
      rs_exit_status = -999;
      return 1;
    }
  rs_exit_status = atoi(marker + 8);
  *marker = '\0';
  return 1;
}

static int rs_invoke_named(const char *path, const char *extra)
{
  char command[2400];
  FILE *pipe;
  size_t got;
  char *marker;

  rs_prepare();
  snprintf(command, sizeof(command),
           "./src/splint -nof -nolib -tmpdir %s "
           "-exportlocal -exportfcn -paramuse -modnomods %s \"%s\" 2>&1; "
           "printf '\\nRS_EXIT:%%d\\n' \"$?\"",
           RS_TMP, extra ? extra : "", path);
  pipe = popen(command, "r");
  if (!pipe)
    return 0;
  got = fread(rs_output, 1, sizeof(rs_output) - 1, pipe);
  rs_output[got] = '\0';
  (void)pclose(pipe);
  marker = strstr(rs_output, "RS_EXIT:");
  if (!marker)
    {
      rs_exit_status = -999;
      return 1;
    }
  rs_exit_status = atoi(marker + 8);
  *marker = '\0';
  return 1;
}

#define RS_RUN(name, src, extra) TASK_CHECK(rs_invoke((name), (src), (extra)))
#define RS_NAMED(path, extra) TASK_CHECK(rs_invoke_named((path), (extra)))
#define RS_EXIT_ZERO TASK_CHECK(rs_exit_status == 0)
#define RS_EXIT_NONZERO TASK_CHECK(rs_exit_status != 0)
#define RS_HAS(text) TASK_CHECK(strstr(rs_output, (text)) != NULL)
#define RS_LACKS(text) TASK_CHECK(strstr(rs_output, (text)) == NULL)

#define RS_FLOW_FLAGS \
  "-nullpass -branchstate -mustfreefresh -usereleased -compdef " \
  "-onlytrans -mustfreeonly -boolops -noeffect"

static const char *RS_PRELUDE =
  "typedef struct object { int value; } object;\n"
  "extern /*@only@*/ object *make_object(void);\n"
  "extern void drop_object(/*@only@*/ object *obj);\n";

/* ----------------------------- declaration ----------------------------- */

static int case_decl_good_fixture(void)
{
  RS_RUN("decl_good_fixture",
         "typedef struct object { int value; } object;\n"
         "int move_object(/*@special@*/ object *src,\n"
         "                /*@special@*/ object **dst)\n"
         "  /*@transferswhen 0 src -> *dst@*/\n"
         "{\n  *dst = src;\n  return 0;\n}\n",
         "");
  RS_EXIT_ZERO;
  RS_HAS("Finished checking");
  RS_LACKS("Semantic comment unrecognized");
  RS_LACKS("conditional ownership");
  return 0;
}

static int case_decl_negative_status(void)
{
  RS_RUN("decl_negative_status",
         "typedef struct object { int value; } object;\n"
         "int acquire_or_busy(/*@special@*/ object *src,\n"
         "                   /*@special@*/ object **dst)\n"
         "  /*@transferswhen -4 src -> *dst@*/\n"
         "{\n"
         "  if (src->value == 0) return 0;\n"
         "  *dst = src;\n"
         "  return -4;\n"
         "}\n",
         RS_FLOW_FLAGS);
  RS_EXIT_ZERO;
  RS_LACKS("conditional ownership");
  RS_LACKS("Semantic comment unrecognized");
  return 0;
}

static int case_decl_duplicate_source(void)
{
  RS_RUN("decl_duplicate_source",
         "typedef struct object { int value; } object;\n"
         "int split_twice(/*@special@*/ object *src,\n"
         "                /*@special@*/ object **a,\n"
         "                /*@special@*/ object **b)\n"
         "  /*@transferswhen 0 src -> *a@*/\n"
         "  /*@transferswhen 0 src -> *b@*/\n"
         "{\n  *a = src; *b = src; return 0;\n}\n",
         RS_FLOW_FLAGS);
  RS_EXIT_NONZERO;
  RS_HAS("duplicate transferswhen declaration");
  RS_HAS("source 'src' is consumed twice");
  RS_HAS("decl_duplicate_source.c");
  return 0;
}

static int case_decl_duplicate_target(void)
{
  RS_RUN("decl_duplicate_target",
         "typedef struct object { int value; } object;\n"
         "int merge_twice(/*@special@*/ object *a,\n"
         "                /*@special@*/ object *b,\n"
         "                /*@special@*/ object **out)\n"
         "  /*@transferswhen 7 a -> *out@*/\n"
         "  /*@transferswhen 7 b -> *out@*/\n"
         "{\n  *out = a; (void)b; return 7;\n}\n",
         RS_FLOW_FLAGS);
  RS_EXIT_NONZERO;
  RS_HAS("duplicate transferswhen declaration");
  RS_HAS("target '*out' is written twice");
  RS_HAS("status 7");
  return 0;
}

static int case_decl_multiple_distinct_ok(void)
{
  RS_RUN("decl_multiple_distinct_ok",
         "typedef struct object { int value; } object;\n"
         "int pair_move(/*@special@*/ object *a,\n"
         "              /*@special@*/ object *b,\n"
         "              /*@special@*/ object **pa,\n"
         "              /*@special@*/ object **pb)\n"
         "  /*@transferswhen 0 a -> *pa@*/\n"
         "  /*@transferswhen 0 b -> *pb@*/\n"
         "{\n  *pa = a; *pb = b; return 0;\n}\n",
         RS_FLOW_FLAGS);
  RS_EXIT_ZERO;
  RS_LACKS("conflicting transferswhen declaration");
  return 0;
}

static int case_decl_requires_special_source(void)
{
  RS_RUN("decl_requires_special_source",
         "typedef struct object { int value; } object;\n"
         "int move_object(object *src,\n"
         "                /*@special@*/ object **dst)\n"
         "  /*@transferswhen 0 src -> *dst@*/\n"
         "{\n  *dst = src; return 0;\n}\n",
         RS_FLOW_FLAGS);
  RS_EXIT_NONZERO;
  RS_HAS("must be declared special");
  RS_HAS("'src'");
  return 0;
}

static int case_decl_requires_pointer_target(void)
{
  RS_RUN("decl_requires_pointer_target",
         "typedef struct object { int value; } object;\n"
         "int move_object(/*@special@*/ object *src,\n"
         "                /*@special@*/ object *dst)\n"
         "  /*@transferswhen 0 src -> *dst@*/\n"
         "{\n  (void)dst; return 0;\n}\n",
         RS_FLOW_FLAGS);
  RS_EXIT_NONZERO;
  RS_HAS("pointer-to-pointer output parameter");
  RS_HAS("'dst'");
  return 0;
}

static int case_decl_plain_only_still_works(void)
{
  RS_RUN("decl_plain_only_still_works",
         "typedef struct object { int value; } object;\n"
         "extern /*@only@*/ object *make_object(void);\n"
         "extern void drop_object(/*@only@*/ object *);\n"
         "void plain(void) { object *p = make_object(); drop_object(p); }\n",
         "");
  RS_EXIT_ZERO;
  RS_HAS("no warnings");
  return 0;
}

/* -------------------------------- flow --------------------------------- */

static const char *RS_MOVE_DEF =
  "int move_object(/*@special@*/ object *src,\n"
  "                /*@special@*/ object **dst)\n"
  "  /*@transferswhen 0 src -> *dst@*/\n"
  "{ *dst = src; return 0; }\n";

static int case_flow_direct_compare(void)
{
  char src[4096];
  snprintf(src, sizeof(src), "%s%s%s", RS_PRELUDE, RS_MOVE_DEF,
           "void f(void) {\n"
           "  object *p = make_object(); object *out = 0;\n"
           "  if (move_object(p, &out) == 0) { drop_object(out); }\n"
           "  else { drop_object(p); }\n"
           "}\n");
  RS_RUN("flow_direct_compare", src, RS_FLOW_FLAGS);
  RS_EXIT_ZERO;
  RS_LACKS("conditional ownership");
  return 0;
}

static int case_flow_negation(void)
{
  char src[4096];
  snprintf(src, sizeof(src), "%s%s%s", RS_PRELUDE, RS_MOVE_DEF,
           "void f(void) {\n"
           "  object *p = make_object(); object *out = 0;\n"
           "  if (!move_object(p, &out)) { drop_object(out); }\n"
           "  else { drop_object(p); }\n"
           "}\n");
  RS_RUN("flow_negation", src, RS_FLOW_FLAGS);
  RS_EXIT_ZERO;
  RS_LACKS("conditional ownership");
  return 0;
}

static int case_flow_saved_compare(void)
{
  char src[4096];
  snprintf(src, sizeof(src), "%s%s%s", RS_PRELUDE, RS_MOVE_DEF,
           "void f(void) {\n"
           "  object *p = make_object(); object *out = 0;\n"
           "  int rc = move_object(p, &out);\n"
           "  if (rc == 0) { drop_object(out); } else { drop_object(p); }\n"
           "}\n");
  RS_RUN("flow_saved_compare", src, RS_FLOW_FLAGS);
  RS_EXIT_ZERO;
  RS_LACKS("conditional ownership");
  return 0;
}

static int case_flow_early_return(void)
{
  char src[4096];
  snprintf(src, sizeof(src), "%s%s%s", RS_PRELUDE, RS_MOVE_DEF,
           "int f(void) {\n"
           "  object *p = make_object(); object *out = 0;\n"
           "  int rc = move_object(p, &out);\n"
           "  if (rc != 0) { drop_object(p); return -1; }\n"
           "  drop_object(out);\n"
           "  return 0;\n"
           "}\n");
  RS_RUN("flow_early_return", src, RS_FLOW_FLAGS);
  RS_EXIT_ZERO;
  RS_LACKS("conditional ownership");
  return 0;
}

static int case_flow_address_alias(void)
{
  char src[4096];
  snprintf(src, sizeof(src), "%s%s%s", RS_PRELUDE, RS_MOVE_DEF,
           "void f(void) {\n"
           "  object *p = make_object(); object *out = 0;\n"
           "  object **alias = 0;\n"
           "  alias = &out;\n"
           "  if (move_object(p, alias) == 0) { drop_object(out); }\n"
           "  else { drop_object(p); }\n"
           "}\n");
  RS_RUN("flow_address_alias", src, RS_FLOW_FLAGS);
  RS_EXIT_ZERO;
  RS_LACKS("Output argument");
  RS_LACKS("conditional ownership");
  return 0;
}

static int case_flow_output_overwrite(void)
{
  char src[4096];
  snprintf(src, sizeof(src), "%s%s%s", RS_PRELUDE, RS_MOVE_DEF,
           "void f(void) {\n"
           "  object *p = make_object();\n"
           "  object *out = make_object();\n"
           "  int rc = move_object(p, &out);\n"
           "  if (rc == 0) { drop_object(out); } else { drop_object(p); }\n"
           "}\n");
  RS_RUN("flow_output_overwrite", src, RS_FLOW_FLAGS);
  RS_EXIT_NONZERO;
  RS_HAS("overwrites live object held by 'out'");
  RS_HAS("conditional ownership");
  RS_HAS("flow_output_overwrite.c:");
  return 0;
}

static int case_flow_failure_leak(void)
{
  char src[4096];
  snprintf(src, sizeof(src), "%s%s%s", RS_PRELUDE, RS_MOVE_DEF,
           "void f(void) {\n"
           "  object *p = make_object(); object *out = 0;\n"
           "  int rc = move_object(p, &out);\n"
           "  if (rc == 0) { drop_object(out); } else { return; }\n"
           "}\n");
  RS_RUN("flow_failure_leak", src, RS_FLOW_FLAGS);
  RS_EXIT_NONZERO;
  RS_HAS("conditional ownership");
  RS_HAS("obligation for 'p'");
  RS_HAS("flow_failure_leak.c:");
  return 0;
}

static int case_flow_double_call(void)
{
  char src[5000];
  snprintf(src, sizeof(src), "%s%s%s", RS_PRELUDE, RS_MOVE_DEF,
           "void f(void) {\n"
           "  object *p = make_object(); object *a = 0; object *b = 0;\n"
           "  if (move_object(p, &a) == 0) { drop_object(a); }\n"
           "  else { drop_object(p); }\n"
           "  if (move_object(p, &b) == 0) { drop_object(b); }\n"
           "  else { drop_object(p); }\n"
           "}\n");
  RS_RUN("flow_double_call", src, RS_FLOW_FLAGS);
  RS_EXIT_NONZERO;
  RS_HAS("already transferred");
  RS_HAS("flow_double_call.c:");
  return 0;
}

static int case_flow_reassignment_clears_old_relation(void)
{
  char src[5000];
  snprintf(src, sizeof(src), "%s%s%s", RS_PRELUDE, RS_MOVE_DEF,
           "void f(void) {\n"
           "  object *p = make_object(); object *a = 0; object *b = 0;\n"
           "  if (move_object(p, &a) == 0) { drop_object(a); }\n"
           "  else { drop_object(p); }\n"
           "  p = make_object();\n"
           "  if (move_object(p, &b) == 0) { drop_object(b); }\n"
           "  else { drop_object(p); }\n"
           "}\n");
  RS_RUN("flow_reassignment_clears", src, RS_FLOW_FLAGS);
  RS_EXIT_ZERO;
  RS_LACKS("already transferred");
  RS_LACKS("conditional ownership");
  return 0;
}

static int case_flow_merge_obligation(void)
{
  char src[4096];
  snprintf(src, sizeof(src), "%s%s%s", RS_PRELUDE, RS_MOVE_DEF,
           "void f(int choose) {\n"
           "  object *p = make_object(); object *out = 0; int rc = -1;\n"
           "  if (choose) { rc = move_object(p, &out); }\n"
           "  if (rc == 0) { drop_object(out); }\n"
           "}\n");
  RS_RUN("flow_merge_obligation", src, RS_FLOW_FLAGS);
  RS_EXIT_NONZERO;
  RS_HAS("conditional ownership");
  RS_HAS("must release 'p'");
  return 0;
}

static int case_flow_callee_new_identity(void)
{
  RS_RUN("flow_callee_new_identity",
         "typedef struct object { int value; } object;\n"
         "extern /*@only@*/ object *make_object(void);\n"
         "int move_object(/*@special@*/ object *src,\n"
         "                /*@special@*/ object **dst)\n"
         "  /*@transferswhen 0 src -> *dst@*/\n"
         "{\n  (void)src;\n  *dst = make_object();\n  return 0;\n}\n",
         RS_FLOW_FLAGS);
  RS_EXIT_NONZERO;
  RS_HAS("conditional ownership");
  RS_HAS("outputs another new object");
  RS_HAS("flow_callee_new_identity.c:9");
  RS_HAS("transferswhen 0 src -> *dst");
  return 0;
}

static int case_flow_callee_failure_transfer(void)
{
  RS_RUN("flow_callee_failure_transfer",
         "typedef struct object { int value; } object;\n"
         "int move_object(/*@special@*/ object *src,\n"
         "                /*@special@*/ object **dst)\n"
         "  /*@transferswhen 0 src -> *dst@*/\n"
         "{\n  *dst = src;\n  return -2;\n}\n",
         RS_FLOW_FLAGS);
  RS_EXIT_NONZERO;
  RS_HAS("conditional ownership");
  RS_HAS("non-success return -2");
  RS_HAS("already transferred");
  RS_HAS("flow_callee_failure_transfer.c:7");
  return 0;
}

/* ----------------------------- provenance ------------------------------ */

static int case_provenance_call_and_declaration(void)
{
  char src[5000];
  snprintf(src, sizeof(src), "%s%s%s", RS_PRELUDE, RS_MOVE_DEF,
           "void provenance_caller(void) {\n"
           "  object *p = make_object(); object *out = 0;\n"
           "  int rc = move_object(p, &out);\n"
           "  if (rc == 0) { drop_object(out); } else { return; }\n"
           "}\n");
  RS_RUN("prov_call_decl", src, RS_FLOW_FLAGS);
  RS_EXIT_NONZERO;
  RS_HAS("prov_call_decl.c:");
  RS_HAS("call location");
  RS_HAS("transferswhen relation and declaration location");
  RS_HAS("prov_call_decl.c:6:");
  RS_HAS("prov_call_decl.c:10:");
  return 0;
}

static int case_provenance_dump_load_roundtrip(void)
{
  static const char *libPath = RS_DIR "/prov_lib.c";
  static const char *dbPath = RS_DIR "/prov_lib.lcd";
  static const char *userPath = RS_DIR "/prov_user.c";
  char extra[900];

  rs_prepare();
  TASK_CHECK(rs_write_file(libPath,
      "typedef struct object { int value; } object;\n"
      "int move_object(/*@special@*/ object *src,\n"
      "                /*@special@*/ object **dst)\n"
      "  /*@transferswhen 0 src -> *dst@*/;\n"));
  snprintf(extra, sizeof(extra), "-dump %s", dbPath);
  RS_NAMED(libPath, extra);
  RS_EXIT_ZERO;

  TASK_CHECK(rs_write_file(userPath,
      "typedef struct object { int value; } object;\n"
      "extern /*@only@*/ object *make_object(void);\n"
      "extern void drop_object(/*@only@*/ object *);\n"
      "void f(void) {\n"
      "  object *p = make_object(); object *out = 0;\n"
      "  int rc = move_object(p, &out);\n"
      "  if (rc == 0) { drop_object(out); } else { return; }\n"
      "}\n"));
  snprintf(extra, sizeof(extra), "-load %s %s", dbPath, RS_FLOW_FLAGS);
  RS_NAMED(userPath, extra);
  RS_EXIT_NONZERO;
  RS_HAS("conditional ownership");
  RS_HAS("obligation for 'p'");
  RS_HAS("prov_lib.c:4:6");
  RS_HAS("call location");
  return 0;
}

static int case_provenance_old_library(void)
{
  static const char *oldSrc = RS_DIR "/old_lib_src.c";
  static const char *oldDb = RS_DIR "/old_lib.lcd";
  static const char *userPath = RS_DIR "/old_user.c";
  char extra[900];

  TASK_CHECK(rs_write_file(oldSrc,
      "typedef struct oldthing { int x; } oldthing;\n"
      "void old_keep(oldthing *p) { p->x = 1; }\n"));
  snprintf(extra, sizeof(extra), "-dump %s", oldDb);
  RS_NAMED(oldSrc, extra);
  RS_EXIT_ZERO;

  TASK_CHECK(rs_write_file(userPath,
      "typedef struct oldthing { int x; } oldthing;\n"
      "void old_keep(oldthing *);\n"
      "void f(oldthing *p) { old_keep(p); }\n"));
  snprintf(extra, sizeof(extra), "-load %s", oldDb);
  RS_NAMED(userPath, extra);
  RS_EXIT_ZERO;
  RS_LACKS("unsupported resultState library format");
  return 0;
}

static int case_provenance_future_format_rejected(void)
{
  static const char *futureSrc = RS_DIR "/future_src.c";
  static const char *badDb = RS_DIR "/future.lcd";
  static const char *userPath = RS_DIR "/future_user.c";
  char extra[900];

  TASK_CHECK(rs_write_file(futureSrc,
      "int future_library_entry(int x) { return x + 1; }\n"));
  snprintf(extra, sizeof(extra), "-dump %s", badDb);
  RS_NAMED(futureSrc, extra);
  RS_EXIT_ZERO;
  TASK_CHECK(rs_append_file(badDb, ";;RS-FORMAT 99\n"));

  TASK_CHECK(rs_write_file(userPath, "int future_user(void) { return 0; }\n"));
  snprintf(extra, sizeof(extra), "-load %s", badDb);
  RS_NAMED(userPath, extra);
  RS_EXIT_NONZERO;
  RS_HAS("unsupported resultState library format version 99");
  return 0;
}

const struct task_case task_cases[] = {
  {"declaration", "good_fixture", case_decl_good_fixture},
  {"declaration", "negative_status", case_decl_negative_status},
  {"declaration", "duplicate_source", case_decl_duplicate_source},
  {"declaration", "duplicate_target", case_decl_duplicate_target},
  {"declaration", "multiple_distinct_ok", case_decl_multiple_distinct_ok},
  {"declaration", "requires_special_source", case_decl_requires_special_source},
  {"declaration", "requires_pointer_target", case_decl_requires_pointer_target},
  {"declaration", "plain_only_still_works", case_decl_plain_only_still_works},

  {"flow", "direct_compare", case_flow_direct_compare},
  {"flow", "negation", case_flow_negation},
  {"flow", "saved_compare", case_flow_saved_compare},
  {"flow", "early_return", case_flow_early_return},
  {"flow", "address_alias", case_flow_address_alias},
  {"flow", "output_overwrite", case_flow_output_overwrite},
  {"flow", "failure_leak", case_flow_failure_leak},
  {"flow", "double_call", case_flow_double_call},
  {"flow", "reassignment_clears", case_flow_reassignment_clears_old_relation},
  {"flow", "merge_obligation", case_flow_merge_obligation},
  {"flow", "callee_new_identity", case_flow_callee_new_identity},
  {"flow", "callee_failure_transfer", case_flow_callee_failure_transfer},

  {"provenance", "call_and_declaration", case_provenance_call_and_declaration},
  {"provenance", "dump_load_roundtrip", case_provenance_dump_load_roundtrip},
  {"provenance", "old_library", case_provenance_old_library},
  {"provenance", "future_format_rejected", case_provenance_future_format_rejected},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
