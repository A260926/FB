/*  Demonstrates tied Dwarf_Debug relations and error ownership.

    Four independently opened objects form two pairs.  The pairs
    do not interfere with one another.  One pair is detached, and
    the detached objects are used again through the ordinary
    error interface.  A short report is written to the file
    named on the command line.

    Requires the split-DWARF fixture built by task_support/run.sh
    (.task-build/fixture/split.dwo and split.o). */

#include "dwarf.h"
#include "libdwarf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned tag_a, tag_b, tag_c, tag_d;
static unsigned deliveries[4];

static void
handler(Dwarf_Error error, Dwarf_Ptr arg)
{
    unsigned *tag = (unsigned *)arg;
    if (!error || !tag || *tag > 3) {
        exit(1);
    }
    ++deliveries[*tag];
}

static Dwarf_Debug
open_object(const char *path, unsigned *tag)
{
    Dwarf_Debug dbg = 0;
    Dwarf_Error err = 0;
    int r = dwarf_init_path(path,0,0,DW_GROUPNUMBER_ANY,
        handler,(Dwarf_Ptr)tag,&dbg,&err);
    if (r != DW_DLV_OK || !dbg || err) {
        return 0;
    }
    return dbg;
}

static int
lookup_is(Dwarf_Debug dbg, Dwarf_Debug expected)
{
    Dwarf_Debug out = 0;
    Dwarf_Error err = 0;
    if (dwarf_get_tied_dbg(dbg,&out,&err) != DW_DLV_OK || err) {
        return 0;
    }
    return out == expected;
}

/*  Create an error through create_via, check the message, then
    release it through owner (the object whose allocation tree
    holds it). */
static int
round_trip_error(Dwarf_Debug create_via, Dwarf_Debug owner,
    const char *text)
{
    Dwarf_Error err = 0;
    dwarf_error_creation(create_via,&err,(char *)text);
    if (!err || !strstr(dwarf_errmsg(err),text)) {
        return 0;
    }
    dwarf_dealloc_error(owner,err);
    return 1;
}

int
main(int argc, char **argv)
{
    Dwarf_Debug a = 0, b = 0, c = 0, d = 0;
    Dwarf_Error err = 0;
    FILE *out = 0;
    int ok = 1;

    if (argc != 2) {
        fprintf(stderr,"usage: %s <report-file>\n",argv[0]);
        return 2;
    }
    a = open_object(".task-build/fixture/split.dwo",&tag_a);
    b = open_object(".task-build/fixture/split.o",&tag_b);
    c = open_object(".task-build/fixture/split.dwo",&tag_c);
    d = open_object(".task-build/fixture/split.o",&tag_d);
    if (!a || !b || !c || !d) {
        fprintf(stderr,"failed to open fixture objects\n");
        return 3;
    }

    /*  Two pairs, bound independently. */
    ok = ok && dwarf_set_tied_dbg(a,b,&err) == DW_DLV_OK && !err;
    ok = ok && dwarf_set_tied_dbg(c,d,&err) == DW_DLV_OK && !err;
    ok = ok && lookup_is(a,b) && lookup_is(c,d);
    ok = ok && lookup_is(b,0) && lookup_is(d,0);

    /*  Bind failure on an object that already has a role leaves
        the existing pair untouched. */
    err = 0;
    ok = ok && dwarf_set_tied_dbg(a,c,&err) == DW_DLV_ERROR && err;
    if (err) {
        dwarf_dealloc_error(a,err);
        err = 0;
    }
    ok = ok && lookup_is(a,b) && lookup_is(c,d);

    /*  Detach the first pair.  Both ends become independent. */
    ok = ok && dwarf_set_tied_dbg(a,0,&err) == DW_DLV_OK && !err;
    ok = ok && lookup_is(a,0) && lookup_is(b,0);
    ok = ok && lookup_is(c,d);

    /*  Errors created before and after the detach can be
        released through the object that allocated them. */
    ok = ok && round_trip_error(b,b,"detached tied end error");
    ok = ok && round_trip_error(c,a,"released through another open object");
    ok = ok && round_trip_error(a,a,"detached main end error");

    /*  The second pair is still bound and still works. */
    ok = ok && lookup_is(c,d);
    ok = ok && round_trip_error(d,c,"bound pair error");

    /*  Detach the second pair too, then close everything. */
    ok = ok && dwarf_set_tied_dbg(d,0,&err) == DW_DLV_OK && !err;
    ok = ok && lookup_is(c,0) && lookup_is(d,0);

    out = fopen(argv[1],"w");
    if (!out) {
        fprintf(stderr,"cannot write %s\n",argv[1]);
        return 4;
    }
    fprintf(out,"tied_ownership demo\n");
    fprintf(out,"two_pairs_independent=%d\n",ok);
    fprintf(out,"detach_restores_independent=%d\n",
        lookup_is(a,0) && lookup_is(b,0));
    fprintf(out,"detach_second_pair=%d\n",
        lookup_is(c,0) && lookup_is(d,0));
    fprintf(out,"error_round_trip=%d\n",ok);
    fprintf(out,"result=%s\n",ok ? "OK" : "FAILED");

    if (dwarf_finish(d) != DW_DLV_OK) {
        ok = 0;
    }
    if (dwarf_finish(c) != DW_DLV_OK) {
        ok = 0;
    }
    if (dwarf_finish(b) != DW_DLV_OK) {
        ok = 0;
    }
    if (dwarf_finish(a) != DW_DLV_OK) {
        ok = 0;
    }
    if (fclose(out)) {
        ok = 0;
    }
    return ok ? 0 : 5;
}
