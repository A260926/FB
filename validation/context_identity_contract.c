/*
 * Contract tests for -Wcontext-identity.
 *
 * These tests do not link against the analyzer: each case writes a
 * small C source file, runs the real sparse command line interface on
 * it and checks the diagnostics sparse actually emits.  The cases are
 * split into three groups:
 *
 *   substitution - parameter substitution, same-call aggregation,
 *                  order independence and anonymous isolation
 *   joins        - control flow joins, loops and unknown identities
 *   diagnostics  - diagnostic contents, sources and the off switch
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "task_support/check.h"

#define OUT_CAP 32768

/* Locate the sparse binary: an explicit override, else the build tree. */
static const char *sparse_bin(void)
{
	const char *env = getenv("SPARSE_BIN");

	return env ? env : "./sparse";
}

static int count_occurrences(const char *hay, const char *needle)
{
	int n = 0;
	size_t len = strlen(needle);
	const char *p = hay;

	if (!len)
		return 0;
	while ((p = strstr(p, needle)) != NULL) {
		n++;
		p += len;
	}
	return n;
}

/*
 * Write SRC to a temporary .c file, run sparse with FLAGS (may be "")
 * and capture the combined stdout/stderr into OUT.  Returns sparse's
 * exit status.
 */
static int run_sparse(const char *flags, const char *src,
		      char *out, size_t cap)
{
	const char *tmpdir = getenv("TMPDIR");
	char tmpl[512];
	char cmd[1024];
	int fd, status;
	FILE *pipe;
	size_t used = 0;
	ssize_t n;

	if (!tmpdir || !*tmpdir)
		tmpdir = "/tmp";
	snprintf(tmpl, sizeof(tmpl), "%s/cid-contract-XXXXXX.c", tmpdir);
	fd = mkstemps(tmpl, 2);
	if (fd < 0)
		return -1;
	n = write(fd, src, strlen(src));
	close(fd);
	if (n < 0) {
		unlink(tmpl);
		return -1;
	}

	out[0] = '\0';
	snprintf(cmd, sizeof(cmd), "\"%s\" -Wno-decl %s \"%s\" 2>&1",
		 sparse_bin(), flags, tmpl);
	pipe = popen(cmd, "r");
	if (!pipe) {
		unlink(tmpl);
		return -1;
	}
	while ((n = fread(out + used, 1, cap - 1 - used, pipe)) > 0) {
		used += (size_t)n;
		if (used == cap - 1)
			break;
	}
	out[used] = '\0';
	status = pclose(pipe);
	unlink(tmpl);
	return status;
}

#define PRELUDE \
"struct resource { int value; };\n" \
"static struct resource a, b;\n" \
"extern void keep(struct resource *p) __attribute__((context(p,0,1)));\n" \
"extern void give(struct resource *p) __attribute__((context(p,1,0)));\n" \
"extern void ex1(struct resource *from, struct resource *to)\n" \
" __attribute__((context(from,1,0),context(to,0,1)));\n" \
"extern void ex2(struct resource *from, struct resource *to)\n" \
" __attribute__((context(to,0,1),context(from,1,0)));\n"

/* ---------------------------------------------------------------- */
/* substitution group                                               */
/* ---------------------------------------------------------------- */

/* A fully balanced function must stay silent. */
static int sub_balanced(void)
{
	char out[OUT_CAP];
	const char *src = PRELUDE
"static void balanced(int fail)\n"
"{\n"
"	keep(&a);\n"
"	if (fail) { give(&a); return; }\n"
"	ex1(&a, &a);\n"
"	give(&a);\n"
"}\n";

	run_sparse("-Wcontext-identity", src, out, sizeof(out));
	TASK_CHECK(out[0] == '\0');
	TASK_CHECK(strstr(out, "resource context") == NULL);
	return 0;
}

/* Pointer arguments and direct local copies share one identity. */
static int sub_local_copy(void)
{
	char out[OUT_CAP];
	const char *src = PRELUDE
"static void copy_ok(struct resource *p)\n"
"{\n"
"	struct resource *q = p;\n"
"	keep(q);\n"
"	give(p);\n"
"}\n";

	run_sparse("-Wcontext-identity", src, out, sizeof(out));
	TASK_CHECK(out[0] == '\0');
	return 0;
}

/* Two distinct arguments cannot settle each other's obligations. */
static int sub_arg_mismatch(void)
{
	char out[OUT_CAP];
	const char *src = PRELUDE
"static void arg_mix(struct resource *p, struct resource *q)\n"
"{\n"
"	keep(p);\n"
"	give(q);\n"
"}\n";

	run_sparse("-Wcontext-identity", src, out, sizeof(out));
	TASK_CHECK(strstr(out, "resource context") != NULL);
	TASK_CHECK(strstr(out, "arg_mix") != NULL);
	TASK_CHECK(strstr(out, "requires 1") != NULL);
	TASK_CHECK(strstr(out, "holds 0 reference") != NULL);
	TASK_CHECK(strstr(out, "leaves with count 1, expected 0") != NULL);
	TASK_CHECK(strstr(out, "leaves with count -1, expected 0") != NULL);
	return 0;
}

/*
 * Both parameters naming the same object: the two contexts of one call
 * aggregate into one obligation, so the missing entry reference is
 * reported exactly once, with the actual and required counts.
 */
static int sub_same_param_aggregate(void)
{
	char out[OUT_CAP];
	const char *src = PRELUDE
"static void aggregate(void)\n"
"{\n"
"	ex1(&a, &a);\n"
"}\n";

	run_sparse("-Wcontext-identity", src, out, sizeof(out));
	TASK_CHECK(count_occurrences(out, "requires 1") == 1);
	TASK_CHECK(count_occurrences(out, "holds 0 reference") == 1);
	TASK_CHECK(strstr(out, "resource 'a'") != NULL);
	return 0;
}

/*
 * The aggregated conclusion must not depend on the order in which the
 * context attributes are written on the callee declaration.
 */
static int sub_order_independent(void)
{
	char out1[OUT_CAP], out2[OUT_CAP];
	const char *src1 = PRELUDE
"static void order1(void) { ex1(&a, &a); }\n";
	const char *src2 = PRELUDE
"static void order2(void) { ex2(&a, &a); }\n";

	run_sparse("-Wcontext-identity", src1, out1, sizeof(out1));
	run_sparse("-Wcontext-identity", src2, out2, sizeof(out2));
	TASK_CHECK(count_occurrences(out1, "requires 1") ==
		   count_occurrences(out2, "requires 1"));
	TASK_CHECK(count_occurrences(out1, "resource 'a'") ==
		   count_occurrences(out2, "resource 'a'"));
	TASK_CHECK(count_occurrences(out1, "requires 1") == 1);

	/* with the entry reference held first, both orderings balance */
	{
		const char *ok1 = PRELUDE
"static void ok1(void) { keep(&a); ex1(&a, &a); give(&a); }\n";
		const char *ok2 = PRELUDE
"static void ok2(void) { keep(&a); ex2(&a, &a); give(&a); }\n";
		char clean1[OUT_CAP], clean2[OUT_CAP];

		run_sparse("-Wcontext-identity", ok1, clean1, sizeof(clean1));
		run_sparse("-Wcontext-identity", ok2, clean2, sizeof(clean2));
		TASK_CHECK(clean1[0] == '\0');
		TASK_CHECK(clean2[0] == '\0');
	}
	return 0;
}

/* A zero net change must not hide a missing entry reference. */
static int sub_zero_net_still_checked(void)
{
	char out[OUT_CAP];
	const char *src = PRELUDE
"static void zero_net(void) { ex1(&a, &a); }\n";

	run_sparse("-Wcontext-identity", src, out, sizeof(out));
	TASK_CHECK(strstr(out, "resource context mismatch") != NULL);
	TASK_CHECK(strstr(out, "zero_net") != NULL);
	/* the scalar totals cancel, so the old check stays quiet */
	TASK_CHECK(strstr(out, "context imbalance in 'zero_net'") == NULL);
	return 0;
}

/* The anonymous counter never repays a named resource. */
static int sub_anonymous_isolated(void)
{
	char out[OUT_CAP];
	const char *src = PRELUDE
"static void anon_does_not_pay(void)\n"
"{\n"
"	keep(&a);\n"
"	__context__(-1);\n"
"}\n"
"static void named_statements_balance(void)\n"
"{\n"
"	__context__(&a, 1);\n"
"	__context__(&a, -1);\n"
"}\n";

	run_sparse("-Wcontext-identity", src, out, sizeof(out));
	TASK_CHECK(strstr(out, "anon_does_not_pay") != NULL);
	TASK_CHECK(strstr(out, "resource 'a'") != NULL);
	TASK_CHECK(strstr(out, "leaves with count 1, expected 0") != NULL);
	TASK_CHECK(strstr(out, "named_statements_balance") == NULL);
	return 0;
}

/* ---------------------------------------------------------------- */
/* joins group                                                      */
/* ---------------------------------------------------------------- */

/* A PHI of two branches naming the same object stays fully tracked. */
static int join_same_identity(void)
{
	char out[OUT_CAP];
	const char *src = PRELUDE
"static void same_join(int c)\n"
"{\n"
"	struct resource *r;\n"
"	if (c)\n"
"		r = &a;\n"
"	else\n"
"		r = &a;\n"
"	keep(r);\n"
"	give(&a);\n"
"}\n";

	run_sparse("-Wcontext-identity", src, out, sizeof(out));
	TASK_CHECK(out[0] == '\0');
	TASK_CHECK(strstr(out, "cannot be determined") == NULL);
	return 0;
}

/* A PHI of different objects is unknown; the known ledger survives. */
static int join_different_unknown(void)
{
	char out[OUT_CAP];
	const char *src = PRELUDE
"static void diff_join(int c)\n"
"{\n"
"	struct resource *r = c ? &a : &b;\n"
"	keep(&a);\n"
"	keep(r);\n"
"	give(&a);\n"
"}\n";

	run_sparse("-Wcontext-identity", src, out, sizeof(out));
	TASK_CHECK(strstr(out, "cannot be determined") != NULL);
	/* the known 'a' ledger is balanced and must not be misreported */
	TASK_CHECK(strstr(out, "resource 'a' leaves") == NULL);
	/* the unknown note never clears the books: analysis still runs */
	TASK_CHECK(strstr(out, "resource context") != NULL);
	return 0;
}

/* A balanced loop converges and produces no diagnostics. */
static int join_balanced_loop(void)
{
	char out[OUT_CAP];
	const char *src = PRELUDE
"static void loop_ok(int n)\n"
"{\n"
"	int i;\n"
"	keep(&a);\n"
"	for (i = 0; i < n; i++) {\n"
"		keep(&a);\n"
"		give(&a);\n"
"	}\n"
"	give(&a);\n"
"}\n";

	run_sparse("-Wcontext-identity", src, out, sizeof(out));
	TASK_CHECK(out[0] == '\0');
	return 0;
}

/* A leaking loop reports the gap, but never repeats it without bound. */
static int join_leaking_loop_bounded(void)
{
	char out[OUT_CAP];
	const char *src = PRELUDE
"static void loop_leak(int n)\n"
"{\n"
"	int i;\n"
"	for (i = 0; i < n; i++)\n"
"		keep(&a);\n"
"}\n";

	run_sparse("-Wcontext-identity", src, out, sizeof(out));
	TASK_CHECK(strstr(out, "resource context") != NULL);
	TASK_CHECK(strstr(out, "loop_leak") != NULL);
	TASK_CHECK(count_occurrences(out, "resource context conflict") <= 1);
	TASK_CHECK((int)strlen(out) < 4000);
	return 0;
}

/* Indirect calls and unsupported addresses are conservative, not fatal. */
static int join_unknown_sources_safe(void)
{
	char out[OUT_CAP];
	const char *src = PRELUDE
"typedef void (*fn_t)(struct resource *);\n"
"static void indirect(fn_t fn)\n"
"{\n"
"	fn(&a);\n"
"	keep(&a);\n"
"}\n"
"static void arith(void)\n"
"{\n"
"	keep(&a + 1);\n"
"}\n";

	run_sparse("-Wcontext-identity", src, out, sizeof(out));
	TASK_CHECK(strstr(out, "cannot be determined") != NULL);
	TASK_CHECK(strstr(out, "resource 'a' leaves with count 1") != NULL);
	return 0;
}

/* ---------------------------------------------------------------- */
/* diagnostics group                                                */
/* ---------------------------------------------------------------- */

/* The early error return must expose both resource gaps separately. */
static int diag_error_return(void)
{
	char out[OUT_CAP];
	const char *src = PRELUDE
"static int error_path(int fail)\n"
"{\n"
"	keep(&a);\n"
"	if (fail) { give(&b); return -1; }\n"
"	give(&a);\n"
"	return 0;\n"
"}\n";

	run_sparse("-Wcontext-identity", src, out, sizeof(out));
	TASK_CHECK(strstr(out, "resource context") != NULL);
	TASK_CHECK(strstr(out, "error_path") != NULL);
	TASK_CHECK(strstr(out, "resource 'a'") != NULL);
	TASK_CHECK(strstr(out, "resource 'b'") != NULL);
	TASK_CHECK(strstr(out, "holds 0 reference(s), the call requires 1")
		   != NULL);
	TASK_CHECK(strstr(out, "leaves with count 1, expected 0") != NULL);
	/* the diagnostic points back at the call creating the difference */
	TASK_CHECK(strstr(out, "call to 'give'") != NULL);
	TASK_CHECK(strstr(out, "call to 'keep'") != NULL);
	return 0;
}

/* Cross-object gaps cannot cancel; every certain gap is reported. */
static int diag_distinct_resources(void)
{
	char out[OUT_CAP];
	const char *src = PRELUDE
"static void cross_bad(void)\n"
"{\n"
"	keep(&a);\n"
"	give(&b);\n"
"}\n";

	run_sparse("-Wcontext-identity", src, out, sizeof(out));
	TASK_CHECK(count_occurrences(out, "resource 'a'") >= 1);
	TASK_CHECK(count_occurrences(out, "resource 'b'") >= 1);
	TASK_CHECK(strstr(out, "leaves with count 1, expected 0") != NULL);
	TASK_CHECK(strstr(out, "leaves with count -1, expected 0") != NULL);
	return 0;
}

/* Without the new switch the original diagnostics are unchanged. */
static int diag_switch_off(void)
{
	char identity[OUT_CAP], plain[OUT_CAP];
	const char *src = PRELUDE
"static int error_path(int fail)\n"
"{\n"
"	keep(&a);\n"
"	if (fail) { give(&b); return -1; }\n"
"	give(&a);\n"
"	return 0;\n"
"}\n"
"static void anon(void) { __context__(1); }\n";

	run_sparse("-Wcontext-identity", src, identity, sizeof(identity));
	run_sparse("-Wcontext", src, plain, sizeof(plain));
	TASK_CHECK(strstr(identity, "resource context") != NULL);
	TASK_CHECK(strstr(plain, "resource context") == NULL);
	/* the classic scalar check still fires exactly as before */
	TASK_CHECK(strstr(plain, "context imbalance in 'anon'") != NULL);
	/* the scalar totals of error_path cancel, so it stayed silent */
	TASK_CHECK(strstr(plain, "error_path") == NULL);
	return 0;
}

/* Every identity diagnostic carries function, resource and counts. */
static int diag_message_shape(void)
{
	char out[OUT_CAP];
	const char *src = PRELUDE
"static void shape(struct resource *p) { give(p); }\n";

	run_sparse("-Wcontext-identity", src, out, sizeof(out));
	TASK_CHECK(strstr(out, "resource context") != NULL);
	TASK_CHECK(strstr(out, "'shape'") != NULL);
	TASK_CHECK(strstr(out, "argument 1") != NULL);
	TASK_CHECK(strstr(out, "holds 0 reference(s)") != NULL);
	TASK_CHECK(strstr(out, "requires 1") != NULL);
	return 0;
}

const struct task_case task_cases[] = {
	{ "substitution", "balanced",		sub_balanced },
	{ "substitution", "local_copy",		sub_local_copy },
	{ "substitution", "arg_mismatch",	sub_arg_mismatch },
	{ "substitution", "same_param_aggregate", sub_same_param_aggregate },
	{ "substitution", "order_independent",	sub_order_independent },
	{ "substitution", "zero_net_checked",	sub_zero_net_still_checked },
	{ "substitution", "anonymous_isolated",	sub_anonymous_isolated },

	{ "joins", "same_identity",		join_same_identity },
	{ "joins", "different_unknown",		join_different_unknown },
	{ "joins", "balanced_loop",		join_balanced_loop },
	{ "joins", "leaking_loop_bounded",	join_leaking_loop_bounded },
	{ "joins", "unknown_sources_safe",	join_unknown_sources_safe },

	{ "diagnostics", "error_return",	diag_error_return },
	{ "diagnostics", "distinct_resources",	diag_distinct_resources },
	{ "diagnostics", "switch_off",		diag_switch_off },
	{ "diagnostics", "message_shape",	diag_message_shape },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
