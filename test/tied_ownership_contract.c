/*  Contract tests for tied Dwarf_Debug relations and error
    ownership, using only public libdwarf API on legal
    compiler-generated split-DWARF objects.

    Two objects can be bound as a main end and a tied end.
    While bound, errors created from either end are allocated
    on the main end.  Errors keep the allocation they were
    born with across binding changes, so dwarf_dealloc_error
    works on whichever still-open Dwarf_Debug the error was
    created through (or, for an error collected on the main
    end, on that main end).  Closing either end detaches the
    pair and the survivor remains fully usable. */

#include "task_support/check.h"
#include "dwarf.h"
#include "libdwarf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DWO_PATH ".task-build/fixture/split.dwo"
#define OBJ_PATH ".task-build/fixture/split.o"

/*  Two callback tags, so a test can tell which object's
    de_errarg was used for a handler delivery. */
struct tag {
    unsigned delivered;
    unsigned which;
};

static struct tag tag_a = {0,0};
static struct tag tag_b = {0,1};
static struct tag tag_c = {0,2};
static struct tag tag_d = {0,3};

static void
handler(Dwarf_Error error, Dwarf_Ptr arg)
{
    struct tag *t = (struct tag *)arg;
    if (!t || !error) {
        exit(41);
    }
    ++t->delivered;
}

static Dwarf_Debug
open_one(const char *path, struct tag *t)
{
    Dwarf_Debug dbg = 0;
    Dwarf_Error err = 0;
    int r = dwarf_init_path(path,0,0,DW_GROUPNUMBER_ANY,
        handler,(Dwarf_Ptr)t,&dbg,&err);
    if (r != DW_DLV_OK || !dbg || err) {
        return 0;
    }
    return dbg;
}

static int
close_one(Dwarf_Debug dbg)
{
    return dwarf_finish(dbg);
}

/*  A read that exercises both local sections and (for a tied
    pair) the tied object's sections.  Must keep working. */
static int
healthy_read(Dwarf_Debug dbg)
{
    Dwarf_Unsigned len = 0;
    Dwarf_Unsigned typeoff = 0;
    Dwarf_Unsigned next = 0;
    Dwarf_Half version = 0;
    Dwarf_Half addr = 0;
    Dwarf_Half length = 0;
    Dwarf_Half extension = 0;
    Dwarf_Half kind = 0;
    Dwarf_Off abbrev = 0;
    Dwarf_Sig8 sig;
    Dwarf_Error err = 0;
    Dwarf_Die die = 0;
    int r = 0;

    memset(&sig,0,sizeof(sig));
    r = dwarf_next_cu_header_d(dbg,1,&len,&version,&abbrev,
        &addr,&length,&extension,&sig,&typeoff,&next,&kind,&err);
    if (r != DW_DLV_OK || version != 5 || len == 0 ||
        addr != 8 || err) {
        return 0;
    }
    r = dwarf_siblingof_b(dbg,0,1,&die,&err);
    if (r != DW_DLV_OK || !die || err) {
        return 0;
    }
    dwarf_dealloc_die(die);
    return 1;
}

/*  Lookup contract: only a bound main end yields the tied
    object; every other case yields NULL through the out
    pointer with DW_DLV_OK. */
static int
expect_lookup(Dwarf_Debug dbg, Dwarf_Debug expected)
{
    Dwarf_Debug out = (Dwarf_Debug)(uintptr_t)0x1;
    Dwarf_Error err = 0;
    if (dwarf_get_tied_dbg(dbg,&out,&err) != DW_DLV_OK) {
        return 0;
    }
    if (err || out != expected) {
        return 0;
    }
    return 1;
}

/*  Create an error through dbg and, after checking that it
    reports the expected text, free it through the Dwarf_Debug
    the caller says owns the allocation. */
static int
make_and_release(Dwarf_Debug create_via,
    Dwarf_Debug release_via,
    const char *text)
{
    Dwarf_Error err = 0;
    dwarf_error_creation(create_via,&err,(char *)text);
    if (!err) {
        return 0;
    }
    if (!strstr(dwarf_errmsg(err),text)) {
        return 0;
    }
    dwarf_dealloc_error(release_via,err);
    return 1;
}

/*  ---------- relations group ---------- */

static int
case_bind_and_lookup(void)
{
    Dwarf_Debug a = open_one(DWO_PATH,&tag_a);
    Dwarf_Debug b = open_one(OBJ_PATH,&tag_b);
    Dwarf_Error err = 0;

    TASK_CHECK(a != 0);
    TASK_CHECK(b != 0);
    TASK_CHECK(expect_lookup(a,0) == 1);
    TASK_CHECK(expect_lookup(b,0) == 1);
    TASK_CHECK(dwarf_set_tied_dbg(a,b,&err) == DW_DLV_OK);
    TASK_CHECK(err == 0);
    TASK_CHECK(expect_lookup(a,b) == 1);
    TASK_CHECK(expect_lookup(b,0) == 1);
    TASK_CHECK(close_one(b) == DW_DLV_OK);
    TASK_CHECK(close_one(a) == DW_DLV_OK);
    return 0;
}

static int
case_self_bind_fails(void)
{
    Dwarf_Debug a = open_one(DWO_PATH,&tag_a);
    Dwarf_Error err = 0;

    TASK_CHECK(a != 0);
    TASK_CHECK(dwarf_set_tied_dbg(a,a,&err) == DW_DLV_ERROR);
    TASK_CHECK(err != 0);
    dwarf_dealloc_error(a,err);
    err = 0;
    TASK_CHECK(expect_lookup(a,0) == 1);
    TASK_CHECK(close_one(a) == DW_DLV_OK);
    return 0;
}

static int
case_failed_rebind_keeps_old_relation(void)
{
    Dwarf_Debug a = open_one(DWO_PATH,&tag_a);
    Dwarf_Debug b = open_one(OBJ_PATH,&tag_b);
    Dwarf_Debug c = open_one(DWO_PATH,&tag_c);
    Dwarf_Error err = 0;

    TASK_CHECK(a != 0);
    TASK_CHECK(b != 0);
    TASK_CHECK(c != 0);
    TASK_CHECK(dwarf_set_tied_dbg(a,b,&err) == DW_DLV_OK);
    TASK_CHECK(err == 0);
    /*  a already has a tied end. */
    err = 0;
    TASK_CHECK(dwarf_set_tied_dbg(a,c,&err) == DW_DLV_ERROR);
    TASK_CHECK(err != 0);
    dwarf_dealloc_error(a,err);
    err = 0;
    TASK_CHECK(expect_lookup(a,b) == 1);
    TASK_CHECK(expect_lookup(b,0) == 1);
    /*  c is not part of the old relation: it may be bound. */
    TASK_CHECK(expect_lookup(c,0) == 1);
    /*  b already is the tied end of a. */
    TASK_CHECK(dwarf_set_tied_dbg(c,b,&err) == DW_DLV_ERROR);
    TASK_CHECK(err != 0);
    dwarf_dealloc_error(c,err);
    err = 0;
    TASK_CHECK(expect_lookup(a,b) == 1);
    TASK_CHECK(expect_lookup(c,0) == 1);
    /*  b may not be used as a main end either. */
    TASK_CHECK(dwarf_set_tied_dbg(b,c,&err) == DW_DLV_ERROR);
    TASK_CHECK(err != 0);
    dwarf_dealloc_error(b,err);
    err = 0;
    TASK_CHECK(expect_lookup(a,b) == 1);
    TASK_CHECK(close_one(c) == DW_DLV_OK);
    TASK_CHECK(close_one(b) == DW_DLV_OK);
    TASK_CHECK(close_one(a) == DW_DLV_OK);
    return 0;
}

static int
case_repeated_detach_succeeds(void)
{
    Dwarf_Debug a = open_one(DWO_PATH,&tag_a);
    Dwarf_Debug b = open_one(OBJ_PATH,&tag_b);
    Dwarf_Error err = 0;

    TASK_CHECK(a != 0);
    TASK_CHECK(b != 0);
    /*  Detach of an unbound object succeeds and is a no-op. */
    TASK_CHECK(dwarf_set_tied_dbg(a,0,&err) == DW_DLV_OK);
    TASK_CHECK(err == 0);
    TASK_CHECK(expect_lookup(a,0) == 1);
    TASK_CHECK(dwarf_set_tied_dbg(a,b,&err) == DW_DLV_OK);
    TASK_CHECK(err == 0);
    TASK_CHECK(dwarf_set_tied_dbg(a,0,&err) == DW_DLV_OK);
    TASK_CHECK(err == 0);
    TASK_CHECK(expect_lookup(a,0) == 1);
    TASK_CHECK(expect_lookup(b,0) == 1);
    /*  Repeating the detach on either end still succeeds. */
    TASK_CHECK(dwarf_set_tied_dbg(a,0,&err) == DW_DLV_OK);
    TASK_CHECK(err == 0);
    TASK_CHECK(dwarf_set_tied_dbg(b,0,&err) == DW_DLV_OK);
    TASK_CHECK(err == 0);
    /*  Either end may be passed to detach a live relation. */
    TASK_CHECK(dwarf_set_tied_dbg(a,b,&err) == DW_DLV_OK);
    TASK_CHECK(err == 0);
    TASK_CHECK(dwarf_set_tied_dbg(b,0,&err) == DW_DLV_OK);
    TASK_CHECK(err == 0);
    TASK_CHECK(expect_lookup(a,0) == 1);
    TASK_CHECK(expect_lookup(b,0) == 1);
    TASK_CHECK(close_one(b) == DW_DLV_OK);
    TASK_CHECK(close_one(a) == DW_DLV_OK);
    return 0;
}

static int
case_four_object_pairs_independent(void)
{
    Dwarf_Debug a = open_one(DWO_PATH,&tag_a);
    Dwarf_Debug b = open_one(OBJ_PATH,&tag_b);
    Dwarf_Debug c = open_one(DWO_PATH,&tag_c);
    Dwarf_Debug d = open_one(OBJ_PATH,&tag_d);
    Dwarf_Error err = 0;

    TASK_CHECK(a != 0 && b != 0 && c != 0 && d != 0);
    TASK_CHECK(dwarf_set_tied_dbg(a,b,&err) == DW_DLV_OK);
    TASK_CHECK(err == 0);
    TASK_CHECK(dwarf_set_tied_dbg(c,d,&err) == DW_DLV_OK);
    TASK_CHECK(err == 0);
    TASK_CHECK(expect_lookup(a,b) == 1);
    TASK_CHECK(expect_lookup(c,d) == 1);
    TASK_CHECK(expect_lookup(b,0) == 1);
    TASK_CHECK(expect_lookup(d,0) == 1);
    TASK_CHECK(dwarf_set_tied_dbg(a,0,&err) == DW_DLV_OK);
    TASK_CHECK(err == 0);
    TASK_CHECK(expect_lookup(a,0) == 1);
    TASK_CHECK(expect_lookup(c,d) == 1);
    /*  a is free again, but d is still c's tied end. */
    err = 0;
    TASK_CHECK(dwarf_set_tied_dbg(a,d,&err) == DW_DLV_ERROR);
    TASK_CHECK(err != 0);
    dwarf_dealloc_error(a,err);
    err = 0;
    TASK_CHECK(expect_lookup(a,0) == 1);
    TASK_CHECK(expect_lookup(c,d) == 1);
    TASK_CHECK(close_one(d) == DW_DLV_OK);
    TASK_CHECK(close_one(c) == DW_DLV_OK);
    TASK_CHECK(close_one(b) == DW_DLV_OK);
    TASK_CHECK(close_one(a) == DW_DLV_OK);
    return 0;
}

/*  ---------- errors group ---------- */

static int
case_errors_before_bind(void)
{
    Dwarf_Debug a = open_one(DWO_PATH,&tag_a);
    Dwarf_Debug b = open_one(OBJ_PATH,&tag_b);
    Dwarf_Error ea = 0;
    Dwarf_Error eb = 0;
    Dwarf_Error err = 0;

    TASK_CHECK(a != 0 && b != 0);
    dwarf_error_creation(a,&ea,"before bind main");
    dwarf_error_creation(b,&eb,"before bind tied");
    TASK_CHECK(ea != 0 && eb != 0);
    TASK_CHECK(strstr(dwarf_errmsg(ea),"before bind main") != 0);
    TASK_CHECK(strstr(dwarf_errmsg(eb),"before bind tied") != 0);
    TASK_CHECK(dwarf_set_tied_dbg(a,b,&err) == DW_DLV_OK);
    TASK_CHECK(err == 0);
    /*  Each pre-bind error was allocated on its own object and
        may be freed there, even now that the objects are tied. */
    dwarf_dealloc_error(a,ea);
    ea = 0;
    dwarf_dealloc_error(b,eb);
    eb = 0;
    TASK_CHECK(close_one(b) == DW_DLV_OK);
    TASK_CHECK(close_one(a) == DW_DLV_OK);
    return 0;
}

static int
case_errors_during_bind(void)
{
    Dwarf_Debug a = open_one(DWO_PATH,&tag_a);
    Dwarf_Debug b = open_one(OBJ_PATH,&tag_b);
    Dwarf_Error err = 0;
    Dwarf_Error from_main = 0;
    Dwarf_Error from_tied = 0;

    TASK_CHECK(a != 0 && b != 0);
    TASK_CHECK(dwarf_set_tied_dbg(a,b,&err) == DW_DLV_OK);
    TASK_CHECK(err == 0);
    /*  Both ends allocate on the main end while bound; the
        callback also uses the main end's argument. */
    tag_a.delivered = 0;
    tag_b.delivered = 0;
    dwarf_error_creation(a,&from_main,"during bind from main");
    dwarf_error_creation(b,&from_tied,"during bind from tied");
    TASK_CHECK(from_main != 0 && from_tied != 0);
    TASK_CHECK(tag_a.delivered == 0 && tag_b.delivered == 0);
    /*  A handler-delivered error comes through the main end. */
    dwarf_error_creation(b,0,"during bind handler");
    TASK_CHECK(tag_a.delivered == 1 && tag_b.delivered == 0);
    /*  Both collected on the main end: free through it. */
    dwarf_dealloc_error(a,from_main);
    from_main = 0;
    dwarf_dealloc_error(a,from_tied);
    from_tied = 0;
    /*  Detach and free another collected error afterwards. */
    dwarf_error_creation(b,&from_tied,"during bind kept");
    TASK_CHECK(from_tied != 0);
    TASK_CHECK(dwarf_set_tied_dbg(a,0,&err) == DW_DLV_OK);
    TASK_CHECK(err == 0);
    /*  Allocated while bound on the main end, so release via a
        even though it was created through b. */
    dwarf_dealloc_error(a,from_tied);
    from_tied = 0;
    TASK_CHECK(close_one(b) == DW_DLV_OK);
    TASK_CHECK(close_one(a) == DW_DLV_OK);
    return 0;
}

static int
case_errors_interleaved_phases(void)
{
    Dwarf_Debug a = open_one(DWO_PATH,&tag_a);
    Dwarf_Debug b = open_one(OBJ_PATH,&tag_b);
    Dwarf_Error err = 0;
    Dwarf_Error pre_a = 0;
    Dwarf_Error pre_b = 0;
    Dwarf_Error bound_from_b = 0;
    Dwarf_Error post_a = 0;
    Dwarf_Error post_b = 0;

    TASK_CHECK(a != 0 && b != 0);
    dwarf_error_creation(a,&pre_a,"phase pre a");
    dwarf_error_creation(b,&pre_b,"phase pre b");
    TASK_CHECK(pre_a != 0 && pre_b != 0);
    TASK_CHECK(dwarf_set_tied_dbg(a,b,&err) == DW_DLV_OK);
    TASK_CHECK(err == 0);
    dwarf_error_creation(b,&bound_from_b,"phase bound from b");
    TASK_CHECK(bound_from_b != 0);
    TASK_CHECK(dwarf_set_tied_dbg(a,0,&err) == DW_DLV_OK);
    TASK_CHECK(err == 0);
    dwarf_error_creation(a,&post_a,"phase post a");
    dwarf_error_creation(b,&post_b,"phase post b");
    TASK_CHECK(post_a != 0 && post_b != 0);
    TASK_CHECK(strstr(dwarf_errmsg(pre_a),"phase pre a") != 0);
    TASK_CHECK(strstr(dwarf_errmsg(pre_b),"phase pre b") != 0);
    TASK_CHECK(strstr(dwarf_errmsg(bound_from_b),"phase bound from b")
        != 0);
    TASK_CHECK(strstr(dwarf_errmsg(post_a),"phase post a") != 0);
    TASK_CHECK(strstr(dwarf_errmsg(post_b),"phase post b") != 0);
    /*  Release in an order unrelated to creation. */
    dwarf_dealloc_error(b,post_b);
    post_b = 0;
    dwarf_dealloc_error(a,pre_a);
    pre_a = 0;
    dwarf_dealloc_error(a,bound_from_b);
    bound_from_b = 0;
    dwarf_dealloc_error(b,pre_b);
    pre_b = 0;
    dwarf_dealloc_error(a,post_a);
    post_a = 0;
    /*  Reads still work on both independent objects. */
    TASK_CHECK(healthy_read(a) == 1);
    TASK_CHECK(healthy_read(b) == 1);
    TASK_CHECK(close_one(b) == DW_DLV_OK);
    TASK_CHECK(close_one(a) == DW_DLV_OK);
    return 0;
}

static int
case_errors_survive_rebinding(void)
{
    Dwarf_Debug a = open_one(DWO_PATH,&tag_a);
    Dwarf_Debug b = open_one(OBJ_PATH,&tag_b);
    Dwarf_Debug c = open_one(DWO_PATH,&tag_c);
    Dwarf_Error err = 0;
    Dwarf_Error first = 0;
    Dwarf_Error second = 0;
    Dwarf_Error third = 0;

    TASK_CHECK(a != 0 && b != 0 && c != 0);
    TASK_CHECK(dwarf_set_tied_dbg(a,b,&err) == DW_DLV_OK);
    TASK_CHECK(err == 0);
    dwarf_error_creation(b,&first,"bound a-b from b");
    TASK_CHECK(first != 0);
    TASK_CHECK(dwarf_set_tied_dbg(a,0,&err) == DW_DLV_OK);
    TASK_CHECK(err == 0);
    /*  Rebind the same objects and an error from the new pair. */
    TASK_CHECK(dwarf_set_tied_dbg(a,b,&err) == DW_DLV_OK);
    TASK_CHECK(err == 0);
    dwarf_error_creation(b,&second,"bound again from b");
    TASK_CHECK(second != 0);
    TASK_CHECK(dwarf_set_tied_dbg(a,0,&err) == DW_DLV_OK);
    TASK_CHECK(err == 0);
    dwarf_error_creation(c,&third,"independent c");
    TASK_CHECK(third != 0);
    /*  first and second were both allocated on a. */
    dwarf_dealloc_error(a,first);
    first = 0;
    dwarf_dealloc_error(a,second);
    second = 0;
    dwarf_dealloc_error(c,third);
    third = 0;
    TASK_CHECK(close_one(c) == DW_DLV_OK);
    TASK_CHECK(close_one(b) == DW_DLV_OK);
    TASK_CHECK(close_one(a) == DW_DLV_OK);
    return 0;
}

/*  ---------- survivors group ---------- */

/*  Close the tied end first; the main end survives, has no
    tied relation, and can allocate and release errors. */
static int
case_close_tied_first(void)
{
    Dwarf_Debug a = open_one(DWO_PATH,&tag_a);
    Dwarf_Debug b = open_one(OBJ_PATH,&tag_b);
    Dwarf_Error err = 0;
    Dwarf_Error old = 0;

    TASK_CHECK(a != 0 && b != 0);
    TASK_CHECK(dwarf_set_tied_dbg(a,b,&err) == DW_DLV_OK);
    TASK_CHECK(err == 0);
    dwarf_error_creation(b,&old,"collected before close");
    TASK_CHECK(old != 0);
    TASK_CHECK(close_one(b) == DW_DLV_OK);
    TASK_CHECK(expect_lookup(a,0) == 1);
    /*  The survivor allocates on itself and uses its own arg. */
    tag_a.delivered = 0;
    tag_b.delivered = 0;
    dwarf_error_creation(a,0,"after tied closed");
    TASK_CHECK(tag_a.delivered == 1 && tag_b.delivered == 0);
    TASK_CHECK(make_and_release(a,a,"survivor main") == 1);
    /*  The error collected while bound lived on a. */
    dwarf_dealloc_error(a,old);
    old = 0;
    TASK_CHECK(healthy_read(a) == 1);
    TASK_CHECK(close_one(a) == DW_DLV_OK);
    return 0;
}

/*  Close the main end first; the tied end survives, becomes
    independent, and can allocate and release errors. */
static int
case_close_main_first(void)
{
    Dwarf_Debug a = open_one(DWO_PATH,&tag_a);
    Dwarf_Debug b = open_one(OBJ_PATH,&tag_b);
    Dwarf_Error err = 0;
    Dwarf_Error old = 0;

    TASK_CHECK(a != 0 && b != 0);
    TASK_CHECK(dwarf_set_tied_dbg(a,b,&err) == DW_DLV_OK);
    TASK_CHECK(err == 0);
    dwarf_error_creation(b,&old,"collected before main close");
    TASK_CHECK(old != 0);
    TASK_CHECK(close_one(a) == DW_DLV_OK);
    TASK_CHECK(expect_lookup(b,0) == 1);
    tag_a.delivered = 0;
    tag_b.delivered = 0;
    dwarf_error_creation(b,0,"after main closed");
    TASK_CHECK(tag_b.delivered == 1 && tag_a.delivered == 0);
    TASK_CHECK(make_and_release(b,b,"survivor tied") == 1);
    /*  The error was allocated on the now-closed main object,
        so it is not usable; do not release a stale owner.
        b's own post-close errors and reads are the contract. */
    (void)old;
    TASK_CHECK(healthy_read(b) == 1);
    TASK_CHECK(close_one(b) == DW_DLV_OK);
    return 0;
}

/*  After the survivor is detached by a close, it can be
    rebound to a fresh object and produce/release errors. */
static int
case_survivor_rebinds(void)
{
    Dwarf_Debug a = open_one(DWO_PATH,&tag_a);
    Dwarf_Debug b = open_one(OBJ_PATH,&tag_b);
    Dwarf_Debug c = open_one(OBJ_PATH,&tag_c);
    Dwarf_Error err = 0;
    Dwarf_Error collected = 0;

    TASK_CHECK(a != 0 && b != 0 && c != 0);
    TASK_CHECK(dwarf_set_tied_dbg(a,b,&err) == DW_DLV_OK);
    TASK_CHECK(err == 0);
    dwarf_error_creation(b,&collected,"collected on main");
    TASK_CHECK(collected != 0);
    TASK_CHECK(close_one(b) == DW_DLV_OK);
    TASK_CHECK(expect_lookup(a,0) == 1);
    /*  Rebind the survivor to a fresh tied object. */
    TASK_CHECK(dwarf_set_tied_dbg(a,c,&err) == DW_DLV_OK);
    TASK_CHECK(err == 0);
    TASK_CHECK(expect_lookup(a,c) == 1);
    TASK_CHECK(expect_lookup(c,0) == 1);
    /*  An error created through c now lands on a. */
    TASK_CHECK(make_and_release(c,a,"rebound tied creation") == 1);
    /*  The old collected error still lives on a. */
    dwarf_dealloc_error(a,collected);
    collected = 0;
    TASK_CHECK(healthy_read(a) == 1);
    TASK_CHECK(close_one(c) == DW_DLV_OK);
    TASK_CHECK(close_one(a) == DW_DLV_OK);
    return 0;
}

/*  A survivor whose partner was closed may itself be closed
    while the pair is not bound, and a new pair works. */
static int
case_close_survivor_then_reuse_other(void)
{
    Dwarf_Debug a = open_one(DWO_PATH,&tag_a);
    Dwarf_Debug b = open_one(OBJ_PATH,&tag_b);
    Dwarf_Debug c = open_one(DWO_PATH,&tag_c);
    Dwarf_Debug d = open_one(OBJ_PATH,&tag_d);
    Dwarf_Error err = 0;

    TASK_CHECK(a != 0 && b != 0 && c != 0 && d != 0);
    TASK_CHECK(dwarf_set_tied_dbg(a,b,&err) == DW_DLV_OK);
    TASK_CHECK(err == 0);
    TASK_CHECK(close_one(a) == DW_DLV_OK);
    TASK_CHECK(close_one(b) == DW_DLV_OK);
    /*  Two fresh objects form an unrelated pair. */
    TASK_CHECK(dwarf_set_tied_dbg(c,d,&err) == DW_DLV_OK);
    TASK_CHECK(err == 0);
    TASK_CHECK(expect_lookup(c,d) == 1);
    TASK_CHECK(make_and_release(d,c,"fresh pair") == 1);
    TASK_CHECK(close_one(d) == DW_DLV_OK);
    TASK_CHECK(close_one(c) == DW_DLV_OK);
    return 0;
}

/*  ---------- registration ---------- */

const struct task_case task_cases[] = {
    {"relations","bind_and_lookup",case_bind_and_lookup},
    {"relations","self_bind_fails",case_self_bind_fails},
    {"relations","failed_rebind_keeps_old_relation",
        case_failed_rebind_keeps_old_relation},
    {"relations","repeated_detach_succeeds",
        case_repeated_detach_succeeds},
    {"relations","four_object_pairs_independent",
        case_four_object_pairs_independent},

    {"errors","errors_before_bind",case_errors_before_bind},
    {"errors","errors_during_bind",case_errors_during_bind},
    {"errors","errors_interleaved_phases",
        case_errors_interleaved_phases},
    {"errors","errors_survive_rebinding",
        case_errors_survive_rebinding},

    {"survivors","close_tied_first",case_close_tied_first},
    {"survivors","close_main_first",case_close_main_first},
    {"survivors","survivor_rebinds",case_survivor_rebinds},
    {"survivors","close_survivor_then_reuse_other",
        case_close_survivor_then_reuse_other},
};
const size_t task_case_count =
    sizeof(task_cases)/sizeof(task_cases[0]);
