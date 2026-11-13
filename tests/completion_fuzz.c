/*
	Structured completion-model fuzz for MuJS try/catch/finally semantics.

	A statement tree is generated from meaningful templates, then fed to two
	independent oracles:

	  * a small C interpreter (the "model") that computes the expected
	    completion record (normal / return / throw / labelled break-continue),
	    integer variables, catch binding environments and an event string;
	  * the real MuJS engine through js_ploadstring + js_pcall, executing
	    rendered JavaScript that records the same events.

	The expected results are never derived from MuJS ASTs, bytecode or another
	JS engine.  Only integers, strings, functions and the covered control
	structures are used; there is no eval, with, prototype poking, async or
	host resource.
*/

#include "../task_support/check.h"
#include "mujs.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Fixed limits: every loop bound is supplied by the model, nothing    */
/* here relies on timeouts to terminate the generated programs.        */
/* ------------------------------------------------------------------ */

#define NVAR        8   /* integer model variables z0..z7               */
#define NLBL        3   /* labels L0..L2                                */
#define NPROG     140   /* distinct statement trees per group           */
#define NTRANSFORM  3   /* identity, rename, block-wrap                 */
#define MAXKIDS    12
#define MAXEVENTS 256
#define MAXNODES  600
#define SRCMAX   65536
#define LOGMAX   4096
#define MAXDEPTH    5
#define LOOPSAVG   16

enum {
	K_BLOCK = 1,
	K_EVENT,        /* append one event to the event string            */
	K_ASSIGN,       /* integer variable assignment                     */
	K_RETURN,
	K_THROW,
	K_BREAK,
	K_CONTINUE,
	K_TRY,          /* try/catch/finally, any non-empty combination    */
	K_LOOP          /* labelled finite-count loop                      */
};

enum {
	CP_NORMAL = 1,
	CP_RETURN,
	CP_THROW,
	CP_BREAK,
	CP_CONTINUE
};

typedef struct Node Node;
struct Node {
	int kind;
	int id;                 /* stable node number                        */
	int event;              /* stable event id (K_EVENT)                 */
	int var;                /* K_ASSIGN / K_THROW / K_TRY catch var      */
	int value;              /* literal used by the node                  */
	int label;              /* K_BREAK/K_CONTINUE/K_LOOP target label    */
	int rounds;             /* K_LOOP fixed iteration count              */
	int has_catch;          /* K_TRY                                     */
	int has_finally;        /* K_TRY                                     */
	int catch_var;          /* K_TRY                                     */
	Node *kids[MAXKIDS];
	int nkids;
};

/* Completion record produced by the C model. */
typedef struct {
	int kind;
	int value;              /* return/throw integer                      */
	int label;              /* break/continue target label               */
} Completion;

typedef struct {
	int bound;              /* 1 if this frame binds the variable        */
	int val;
} FrameVar;

typedef struct Frame {
	FrameVar v[NVAR];
	struct Frame *outer;
} Frame;

typedef struct {
	unsigned int s;         /* deterministic xorshift state              */
	Node *all[MAXNODES];
	int nall;
	int next_event;
	int next_node;
	int lblstack[LOOPSAVG];/* enclosing labelled loops during generation */
	int nlbl;
	int policy;             /* GRP_*: which statement shapes are allowed */
} Gen;

/*
 * Group policies keep the two mutation targets independent:
 *   GRP_FINALLY / GRP_ABRUPT only build try/finally (never a catch clause),
 *   so the OP_ENDCATCH mutation is inert for them;
 *   GRP_CATCH builds try/catch shapes that exercise binding environments.
 */
enum { GRP_FINALLY = 0, GRP_CATCH = 1, GRP_ABRUPT = 2 };

/* Model interpretation state. */
typedef struct {
	int globals[NVAR];
	Frame *env;
	int events[MAXEVENTS];
	int nevents;
	int steps;
	int loop_depth;       /* bounded nesting depth of active loops      */
	/* coverage observed while interpreting */
	int cov_ret_finally;    /* return travels through a finally          */
	int cov_fin_overwrite;  /* finally replaces an abrupt completion     */
	int cov_rethrow;        /* catch block throws again                  */
	int cov_shadow;         /* catch param shadows an outer binding      */
	int cov_jump_outer;     /* break/continue through a finally          */
} Model;

/* Observed result of one engine run; strings are owned copies, never   */
/* borrowed MuJS pointers kept across allocations.                      */
typedef struct {
	int didthrow;
	int thrown;
	int r;
	int vars[NVAR];
	char log[LOGMAX];
} Outcome;

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static void die(const char *msg)
{
	fprintf(stderr, "completion_fuzz: %s\n", msg);
	exit(2);
}

static unsigned int rnd(Gen *g)
{
	unsigned int x = g->s;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	g->s = x ? x : 0x9e3779b9u;
	return g->s;
}

static int rbetween(Gen *g, int lo, int hi)
{
	if (hi <= lo)
		return lo;
	return lo + (int)(rnd(g) % (unsigned)(hi - lo + 1));
}

static int rpick(Gen *g, int n)
{
	return (int)(rnd(g) % (unsigned)n);
}

/* ------------------------------------------------------------------ */
/* Tree construction and destruction.                                  */
/* ------------------------------------------------------------------ */

static Node *new_node(Gen *g, int kind)
{
	Node *n;
	if (g->nall >= MAXNODES)
		die("node table overflow");
	n = calloc(1, sizeof *n);
	if (!n)
		die("out of memory");
	n->kind = kind;
	n->id = g->next_node++;
	n->event = -1;
	n->label = -1;
	n->var = -1;
	n->catch_var = -1;
	g->all[g->nall++] = n;
	return n;
}

static void add_kid(Node *parent, Node *kid)
{
	if (parent->nkids >= MAXKIDS)
		die("kid table overflow");
	parent->kids[parent->nkids++] = kid;
}

static void free_trees(Gen *g)
{
	int i;
	for (i = 0; i < g->nall; ++i)
		free(g->all[i]);
	g->nall = 0;
}

static Node *mk_event(Gen *g)
{
	Node *n = new_node(g, K_EVENT);
	n->event = g->next_event++;
	if (n->event >= MAXEVENTS)
		die("event table overflow");
	return n;
}

static Node *mk_block(Gen *g, int nkids)
{
	Node *n = new_node(g, K_BLOCK);
	int i;
	for (i = 0; i < nkids; ++i)
		add_kid(n, mk_event(g));
	return n;
}

/* ------------------------------------------------------------------ */
/* Statement tree generator.                                           */
/*                                                                     */
/* Templates are enumerated by index within each shape and jittered    */
/* with a fixed seed, so reruns reproduce every tree exactly.  Shapes  */
/* include nested (>=2 levels) try statements and labelled loops;      */
/* break/continue labels are only ever chosen from enclosing loops so  */
/* every rendered program is valid JavaScript.                         */
/* ------------------------------------------------------------------ */

static Node *gen_stmt(Gen *g, int depth);

static void push_label(Gen *g, int label)
{
	if (g->nlbl >= LOOPSAVG)
		die("label stack overflow");
	g->lblstack[g->nlbl++] = label;
}

static Node *gen_loop(Gen *g, int depth)
{
	Node *loop = new_node(g, K_LOOP);
	Node *body;
	loop->label = rpick(g, NLBL);
	loop->rounds = rbetween(g, 1, 3);
	body = mk_block(g, rbetween(g, 1, 2));
	push_label(g, loop->label);
	if (depth < MAXDEPTH)
		add_kid(body, gen_stmt(g, depth + 1));
	g->nlbl--;
	add_kid(loop, body);
	return loop;
}

/* A jump that can only reference a currently enclosing labelled loop. */
static Node *gen_jump(Gen *g)
{
	Node *n;
	int jk;
	if (g->nlbl == 0) {
		/* no loop in scope: a jump would be invalid, use an assignment */
		n = new_node(g, K_ASSIGN);
		n->var = rpick(g, NVAR);
		n->value = rbetween(g, 1, 9);
		return n;
	}
	jk = rpick(g, 4);
	if (jk == 0) {
		n = new_node(g, K_RETURN);
		n->value = rbetween(g, 1, 9);
	} else if (jk == 1) {
		n = new_node(g, K_THROW);
		n->value = rbetween(g, 20, 39);
	} else if (jk == 2) {
		n = new_node(g, K_BREAK);
		n->label = g->lblstack[rpick(g, g->nlbl)];
	} else {
		n = new_node(g, K_CONTINUE);
		n->label = g->lblstack[rpick(g, g->nlbl)];
	}
	return n;
}

static Node *gen_try(Gen *g, int depth)
{
	Node *t = new_node(g, K_TRY);
	Node *trystm, *catchstm, *finstm;
	int shape = rpick(g, 4);

	if (g->policy == GRP_FINALLY || g->policy == GRP_ABRUPT) {
		/* these groups never emit a catch clause */
		t->has_catch = 0;
		t->has_finally = 1;
	} else {
		t->has_catch = (shape != 0);      /* shape 0: try/finally only  */
		t->has_finally = (shape != 1);    /* shape 1: try/catch only    */
		if (!t->has_catch && !t->has_finally)
			t->has_finally = 1;
	}

	trystm = mk_block(g, rbetween(g, 1, 2));
	if (rbetween(g, 0, 3) != 0)
		add_kid(trystm, gen_stmt(g, depth + 1));
	add_kid(t, trystm);

	if (t->has_catch) {
		Node *use;
		t->catch_var = rpick(g, NVAR);
		catchstm = mk_block(g, rbetween(g, 1, 2));
		/* touch the caught binding: a leaked catch environment makes   */
		/* this assignment observable to the rest of the function.      */
		use = new_node(g, K_ASSIGN);
		use->var = t->catch_var;
		use->value = 0;                  /* z = caught value            */
		add_kid(catchstm, use);
		if (rbetween(g, 0, 2) == 0) {
			/* rethrow the caught integer from the catch block */
			Node *th = new_node(g, K_THROW);
			th->var = t->catch_var;
			th->value = 0;
			add_kid(catchstm, th);
		} else if (depth < MAXDEPTH && rbetween(g, 0, 2) == 0) {
			add_kid(catchstm, gen_stmt(g, depth + 1));
		}
		add_kid(t, catchstm);
	}

	if (t->has_finally) {
		finstm = mk_block(g, rbetween(g, 1, 2));
		if (rbetween(g, 0, 3) == 0) {
			/* a finally that itself jumps replaces the incoming record */
			Node *j;
			int pick = rpick(g, 3);
			if (pick == 0) {
				j = new_node(g, K_RETURN);
				j->value = rbetween(g, 1, 9);
			} else if (pick == 1) {
				j = new_node(g, K_THROW);
				j->value = rbetween(g, 10, 19);
			} else {
				j = gen_jump(g);      /* valid label or an assignment   */
				if (j->kind != K_BREAK && j->kind != K_CONTINUE) {
					j = new_node(g, K_RETURN);
					j->value = rbetween(g, 1, 9);
				}
			}
			add_kid(finstm, j);
		}
		add_kid(t, finstm);
	}

	if (t->has_catch) {
		/* Wrap the try/catch and write the catch variable again from the
		   outer scope.  The model restores the outer binding on catch
		   exit, so this hits the outer variable; if OP_ENDCATCH fails to
		   pop the catch environment the write leaks into that frame. */
		Node *wrap = new_node(g, K_BLOCK);
		Node *after = new_node(g, K_ASSIGN);
		after->var = t->catch_var;
		after->value = rbetween(g, 40, 49);
		add_kid(wrap, t);
		add_kid(wrap, after);
		return wrap;
	}
	return t;
}

static Node *gen_stmt(Gen *g, int depth)
{
	int choice;
	Node *n;
	if (depth >= MAXDEPTH)
		return mk_event(g);
	choice = rpick(g, 6);
	switch (choice) {
	case 0:
		n = new_node(g, K_ASSIGN);
		n->var = rpick(g, NVAR);
		n->value = rbetween(g, 1, 9);
		return n;
	case 1:
		return gen_try(g, depth);
	case 2:
		return gen_loop(g, depth);
	case 3: {
		Node *b = mk_block(g, rbetween(g, 1, 2));
		add_kid(b, gen_stmt(g, depth + 1));
		return b;
	}
	default:
		return gen_jump(g);
	}
}

/* ------------------------------------------------------------------ */
/* Hand-shaped templates guaranteeing the mandatory combinations.      */
/* ------------------------------------------------------------------ */

/* return crosses a finally: finally event must appear, value kept. */
static Node *template_return_through_finally(Gen *g)
{
	Node *t = new_node(g, K_TRY);
	Node *tryb = mk_block(g, 1);
	Node *fin = mk_block(g, 1);
	Node *ret = new_node(g, K_RETURN);
	ret->value = 77;
	add_kid(tryb, ret);
	t->has_finally = 1;
	add_kid(t, tryb);
	add_kid(t, fin);
	return t;
}

/* finally throws and overwrites the incoming return completion. */
static Node *template_finally_overwrites(Gen *g)
{
	Node *t = new_node(g, K_TRY);
	Node *tryb = mk_block(g, 1);
	Node *fin = mk_block(g, 1);
	Node *ret = new_node(g, K_RETURN);
	Node *over = new_node(g, K_THROW);
	ret->value = 77;
	over->value = 88;
	add_kid(tryb, ret);
	add_kid(fin, over);
	t->has_finally = 1;
	add_kid(t, tryb);
	add_kid(t, fin);
	return t;
}

/* catch shadows same-named outer binding, then restores it on exit. */
static Node *template_catch_shadow_restore(Gen *g)
{
	Node *b = new_node(g, K_BLOCK);
	Node *t = new_node(g, K_TRY);
	Node *tryb = mk_block(g, 1);
	Node *catchb = mk_block(g, 1);
	Node *th = new_node(g, K_THROW);
	Node *use, *after;
	th->value = 55;
	add_kid(tryb, th);
	t->has_catch = 1;
	t->catch_var = 0;                  /* shadows outer z0 */
	use = new_node(g, K_ASSIGN);
	use->var = 0;
	use->value = 0;
	add_kid(catchb, use);
	add_kid(t, tryb);
	add_kid(t, catchb);
	add_kid(b, t);
	add_kid(b, mk_event(g));
	after = new_node(g, K_ASSIGN);
	after->var = 0;
	after->value = 66;                 /* writes the restored outer z0 */
	add_kid(b, after);
	return b;
}

/* catch rethrows the caught integer. */
static Node *template_catch_rethrow(Gen *g)
{
	Node *t = new_node(g, K_TRY);
	Node *tryb = mk_block(g, 1);
	Node *catchb = mk_block(g, 1);
	Node *th = new_node(g, K_THROW);
	Node *use, *rethrow;
	th->value = 56;
	add_kid(tryb, th);
	t->has_catch = 1;
	t->catch_var = 1;
	use = new_node(g, K_ASSIGN);
	use->var = 1;
	use->value = 0;
	add_kid(catchb, use);
	rethrow = new_node(g, K_THROW);
	rethrow->var = 1;
	rethrow->value = 0;
	add_kid(catchb, rethrow);
	add_kid(t, tryb);
	add_kid(t, catchb);
	return t;
}

/* break crosses an inner finally and reaches the outer labelled loop. */
static Node *template_break_through_finally(Gen *g)
{
	Node *outer = new_node(g, K_LOOP);
	Node *obody = mk_block(g, 1);
	Node *inner = new_node(g, K_TRY);
	Node *itry = mk_block(g, 1);
	Node *ifin = mk_block(g, 1);
	Node *brk = new_node(g, K_BREAK);
	outer->label = 0;
	outer->rounds = 2;
	brk->label = 0;
	add_kid(itry, brk);
	inner->has_finally = 1;
	add_kid(inner, itry);
	add_kid(inner, ifin);
	add_kid(obody, inner);
	add_kid(outer, obody);
	return outer;
}

/* continue crosses an inner finally and reaches the outer labelled loop. */
static Node *template_continue_through_finally(Gen *g)
{
	Node *outer = new_node(g, K_LOOP);
	Node *obody = mk_block(g, 1);
	Node *inner = new_node(g, K_TRY);
	Node *itry = mk_block(g, 1);
	Node *ifin = mk_block(g, 1);
	Node *cont = new_node(g, K_CONTINUE);
	outer->label = 1;
	outer->rounds = 2;
	cont->label = 1;
	add_kid(itry, cont);
	inner->has_finally = 1;
	add_kid(inner, itry);
	add_kid(inner, ifin);
	add_kid(obody, inner);
	add_kid(outer, obody);
	return outer;
}

/* two nested try/finally levels: a return must cross both finally blocks */
static Node *template_nested_finally(Gen *g)
{
	Node *outer = new_node(g, K_TRY);
	Node *otry = mk_block(g, 1);
	Node *ofin = mk_block(g, 1);
	Node *inner = new_node(g, K_TRY);
	Node *itry = mk_block(g, 1);
	Node *ifin = mk_block(g, 1);
	Node *ret = new_node(g, K_RETURN);
	ret->value = 44;
	add_kid(itry, ret);
	inner->has_finally = 1;
	add_kid(inner, itry);
	add_kid(inner, ifin);
	add_kid(otry, inner);
	outer->has_finally = 1;
	add_kid(outer, otry);
	add_kid(outer, ofin);
	return outer;
}

/* two nested catch levels using the same binding name, then a post-catch
   write; a leaked catch environment makes that write hit the wrong scope */
static Node *template_nested_catch(Gen *g)
{
	Node *b = new_node(g, K_BLOCK);
	Node *outer = new_node(g, K_TRY);
	Node *otry = mk_block(g, 1);
	Node *ocatch = mk_block(g, 1);
	Node *inner = new_node(g, K_TRY);
	Node *itry = mk_block(g, 1);
	Node *icatch = mk_block(g, 1);
	Node *th = new_node(g, K_THROW);
	Node *use1, *rethrow, *use2, *after;
	th->value = 50;
	add_kid(itry, th);
	inner->has_catch = 1;
	inner->catch_var = 0;
	use1 = new_node(g, K_ASSIGN);
	use1->var = 0;
	use1->value = 0;
	add_kid(icatch, use1);
	rethrow = new_node(g, K_THROW);
	rethrow->var = 0;
	rethrow->value = 0;
	add_kid(icatch, rethrow);
	add_kid(inner, itry);
	add_kid(inner, icatch);
	add_kid(otry, inner);
	outer->has_catch = 1;
	outer->catch_var = 0;
	use2 = new_node(g, K_ASSIGN);
	use2->var = 0;
	use2->value = 0;
	add_kid(ocatch, use2);
	add_kid(outer, otry);
	add_kid(outer, ocatch);
	add_kid(b, outer);
	add_kid(b, mk_event(g));
	after = new_node(g, K_ASSIGN);
	after->var = 0;
	after->value = 61;           /* model: outer global; mutant: leaked env */
	add_kid(b, after);
	return b;
}

/* throw is caught and handled; execution continues normally afterwards. */
static Node *template_catch_normal(Gen *g)
{
	Node *b = new_node(g, K_BLOCK);
	Node *t = new_node(g, K_TRY);
	Node *tryb = mk_block(g, 1);
	Node *catchb = mk_block(g, 1);
	Node *th = new_node(g, K_THROW);
	Node *use, *after;
	th->value = 57;
	add_kid(tryb, th);
	t->has_catch = 1;
	t->catch_var = 2;
	use = new_node(g, K_ASSIGN);
	use->var = 2;
	use->value = 0;
	add_kid(catchb, use);
	add_kid(t, tryb);
	add_kid(t, catchb);
	add_kid(b, t);
	add_kid(b, mk_event(g));
	after = new_node(g, K_ASSIGN);
	after->var = 2;
	after->value = 62;
	add_kid(b, after);
	return b;
}

/* Policy-specific guaranteed templates, indexed by the first few programs. */
static Node *template_for(Gen *g, int program_index)
{
	switch (g->policy) {
	case GRP_FINALLY:
		switch (program_index) {
		case 0: return template_return_through_finally(g);
		case 1: return template_finally_overwrites(g);
		case 2: return template_nested_finally(g);
		default: return template_break_through_finally(g);
		}
	case GRP_ABRUPT:
		switch (program_index) {
		case 0: return template_break_through_finally(g);
		case 1: return template_continue_through_finally(g);
		case 2: return template_return_through_finally(g);
		default: return template_finally_overwrites(g);
		}
	default: /* GRP_CATCH */
		switch (program_index) {
		case 0: return template_catch_shadow_restore(g);
		case 1: return template_catch_rethrow(g);
		case 2: return template_nested_catch(g);
		default: return template_catch_normal(g);
		}
	}
}

static Node *gen_program_tree(Gen *g, unsigned seed, int program_index)
{
	Node *root;
	int i, nstmt;

	/* reset all counters before allocating anything, so node/event ids
	   are unique and stable across the whole tree */
	g->s = seed * 2654435761u + (unsigned)program_index * 40503u + 1u;
	g->next_event = 0;
	g->next_node = 0;
	g->nlbl = 0;

	root = new_node(g, K_BLOCK);

	/* the first four programs of every group are policy-specific
	   guaranteed templates; the rest are randomly generated */
	if (program_index < 4)
		add_kid(root, template_for(g, program_index));

	nstmt = rbetween(g, 2, 4);
	for (i = 0; i < nstmt; ++i)
		add_kid(root, gen_stmt(g, 0));

	/* trailing event proves statements after an abrupt exit do not run */
	add_kid(root, mk_event(g));
	return root;
}

/* ------------------------------------------------------------------ */
/* The independent C model: interpret the tree, no JS text involved.   */
/* ------------------------------------------------------------------ */

static Completion normal_comp(void)
{
	Completion c = { CP_NORMAL, 0, -1 };
	return c;
}

static void model_event(Model *m, int id)
{
	if (m->nevents >= MAXEVENTS)
		die("model event overflow");
	m->events[m->nevents++] = id;
}

static int var_get(Model *m, int v)
{
	Frame *f = m->env;
	while (f) {
		if (f->v[v].bound)
			return f->v[v].val;
		f = f->outer;
	}
	return m->globals[v];
}

static void var_set(Model *m, int v, int x)
{
	Frame *f = m->env;
	while (f) {
		if (f->v[v].bound) {
			f->v[v].val = x;
			return;
		}
		f = f->outer;
	}
	m->globals[v] = x;
}

static Frame *push_frame(Model *m)
{
	Frame *f = calloc(1, sizeof *f);
	if (!f)
		die("out of memory");
	f->outer = m->env;
	m->env = f;
	return f;
}

static void pop_frame(Model *m)
{
	Frame *f = m->env;
	m->env = f->outer;
	free(f);
}

static Completion interp(Model *m, const Node *n);

static Completion interp_block(Model *m, const Node *n)
{
	int i;
	for (i = 0; i < n->nkids; ++i) {
		Completion c = interp(m, n->kids[i]);
		if (c.kind != CP_NORMAL)
			return c; /* later statements must not append events */
	}
	return normal_comp();
}

static Completion interp_try(Model *m, const Node *n)
{
	const Node *tryb = n->kids[0];
	const Node *catchb = n->has_catch ? n->kids[1] : NULL;
	const Node *fin = n->has_finally ? n->kids[n->nkids - 1] : NULL;
	Completion in = interp(m, tryb);

	if (in.kind == CP_THROW && n->has_catch) {
		Frame *f;
		Completion cc;
		/* the new catch frame binds the name, shadowing the same-named
		   outer variable/global for the lifetime of the catch block */
		m->cov_shadow++;
		f = push_frame(m);
		f->v[n->catch_var].bound = 1;
		f->v[n->catch_var].val = in.value;
		cc = interp(m, catchb);
		pop_frame(m);
		if (cc.kind == CP_THROW)
			m->cov_rethrow++;
		if (cc.kind != CP_NORMAL)
			in = cc;       /* catch block abrupt completion (rethrow...) */
		else
			in = normal_comp();
	}

	if (n->has_finally) {
		Completion fc;
		if (in.kind == CP_RETURN)
			m->cov_ret_finally++;
		if (in.kind == CP_BREAK || in.kind == CP_CONTINUE)
			m->cov_jump_outer++;
		fc = interp(m, fin);
		if (fc.kind != CP_NORMAL) {
			m->cov_fin_overwrite++;
			in = fc;       /* finally jump replaces the incoming record */
		}
		/* normal finally keeps the incoming completion and value */
	}
	return in;
}

static Completion interp_loop(Model *m, const Node *n)
{
	const Node *body = n->kids[0];
	int round;
	if (m->loop_depth >= LOOPSAVG)
		die("loop nesting overflow");
	m->loop_depth++;
	for (round = 0; round < n->rounds; ++round) {
		Completion c = interp(m, body);
		if (c.kind == CP_BREAK && c.label == n->label) {
			m->loop_depth--;
			return normal_comp();       /* consumed: leave the loop */
		}
		if (c.kind == CP_CONTINUE && c.label == n->label)
			continue;                   /* consumed: next iteration  */
		if (c.kind != CP_NORMAL) {
			m->loop_depth--;
			return c;                   /* propagates to outer scope */
		}
	}
	m->loop_depth--;
	return normal_comp();
}

static Completion interp(Model *m, const Node *n)
{
	Completion c;
	if (++m->steps > 1000000)
		die("model step limit");
	switch (n->kind) {
	case K_BLOCK:
		return interp_block(m, n);
	case K_EVENT:
		model_event(m, n->event);
		return normal_comp();
	case K_ASSIGN:
		var_set(m, n->var, n->value ? n->value : var_get(m, n->var));
		return normal_comp();
	case K_RETURN:
		c.kind = CP_RETURN;
		c.value = n->value;
		c.label = -1;
		return c;
	case K_THROW:
		c.kind = CP_THROW;
		c.value = n->value ? n->value : var_get(m, n->var);
		c.label = -1;
		return c;
	case K_BREAK:
		c.kind = CP_BREAK;
		c.value = 0;
		c.label = n->label;
		return c;
	case K_CONTINUE:
		c.kind = CP_CONTINUE;
		c.value = 0;
		c.label = n->label;
		return c;
	case K_TRY:
		return interp_try(m, n);
	case K_LOOP:
		return interp_loop(m, n);
	default:
		die("bad node kind in model");
	}
	return normal_comp();
}

/* ------------------------------------------------------------------ */
/* Rendering of the tree to JavaScript.                                */
/* ------------------------------------------------------------------ */

typedef struct {
	char *buf;
	size_t len, cap;
	int indent;
	int renamed;       /* transformation: consistent identifier rename   */
	int wrapped;       /* transformation: side-effect-free block wrap    */
	int err;
} Src;

static void src_put(Src *s, const char *p)
{
	size_t n = strlen(p);
	if (s->err)
		return;
	if (s->len + n + 1 > s->cap) {
		size_t nc = s->cap ? s->cap * 2 : 256;
		char *nb;
		while (nc < s->len + n + 1)
			nc *= 2;
		if (nc > SRCMAX) {
			s->err = 1;
			return;
		}
		nb = realloc(s->buf, nc);
		if (!nb)
			die("out of memory");
		s->buf = nb;
		s->cap = nc;
	}
	memcpy(s->buf + s->len, p, n);
	s->len += n;
	s->buf[s->len] = 0;
}

static void src_fmt(Src *s, const char *fmt, ...)
{
	char tmp[128];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(tmp, sizeof tmp, fmt, ap);
	va_end(ap);
	src_put(s, tmp);
}

static void src_indent(Src *s)
{
	int i;
	for (i = 0; i < s->indent; ++i)
		src_put(s, "  ");
}

/* Bijective permutations applied identically at declaration and use. */
static int map_var(int renamed, int v)
{
	return renamed ? (int)(((unsigned)v * 5u + 3u) % NVAR) : v;
}

static int map_label(int renamed, int l)
{
	return renamed ? (int)(((unsigned)l * 2u + 1u) % NLBL) : l;
}

static void var_name(Src *s, int v)
{
	src_fmt(s, "z%d", map_var(s->renamed, v));
}

static void label_name(Src *s, int l)
{
	src_fmt(s, "L%d", map_label(s->renamed, l));
}

static void render_node(Src *s, const Node *n);

static void render_kids(Src *s, const Node *n)
{
	int i;
	for (i = 0; i < n->nkids; ++i) {
		src_indent(s);
		render_node(s, n->kids[i]);
	}
}

static void render_block(Src *s, const Node *n)
{
	/* wrapped form adds one extra side-effect-free block level */
	src_put(s, "{\n");
	if (s->wrapped) {
		src_indent(s);
		src_put(s, "{\n");
		s->indent++;
	}
	s->indent++;
	render_kids(s, n);
	s->indent--;
	if (s->wrapped) {
		src_indent(s);
		src_put(s, "}\n");
		s->indent--;
	}
	src_indent(s);
	src_put(s, "}\n");
}

static void render_node(Src *s, const Node *n)
{
	switch (n->kind) {
	case K_BLOCK:
		render_block(s, n);
		break;
	case K_EVENT:
		src_fmt(s, "ev(%d);\n", n->event);
		break;
	case K_ASSIGN:
		var_name(s, n->var);
		src_put(s, "=");
		if (n->value)
			src_fmt(s, "%d;\n", n->value);
		else {
			var_name(s, n->var);
			src_put(s, ";\n");
		}
		break;
	case K_RETURN:
		src_fmt(s, "return %d;\n", n->value);
		break;
	case K_THROW:
		src_put(s, "throw ");
		if (n->value)
			src_fmt(s, "%d;\n", n->value);
		else {
			var_name(s, n->var);
			src_put(s, ";\n");
		}
		break;
	case K_BREAK:
		src_put(s, "break ");
		label_name(s, n->label);
		src_put(s, ";\n");
		break;
	case K_CONTINUE:
		src_put(s, "continue ");
		label_name(s, n->label);
		src_put(s, ";\n");
		break;
	case K_LOOP:
		/* unique counter name per node so nested loops never share it */
		label_name(s, n->label);
		src_fmt(s, ": for(var c%d=0;c%d<%d;c%d++){\n",
		        n->id, n->id, n->rounds, n->id);
		s->indent++;
		render_kids(s, n);
		s->indent--;
		src_indent(s);
		src_put(s, "}\n");
		break;
	case K_TRY: {
		const Node *tryb = n->kids[0];
		const Node *catchb = n->has_catch ? n->kids[1] : NULL;
		const Node *fin = n->has_finally ? n->kids[n->nkids - 1] : NULL;
		src_put(s, "try\n");
		render_node(s, tryb);
		if (n->has_catch) {
			src_indent(s);
			src_put(s, "catch(");
			var_name(s, n->catch_var);
			src_put(s, ")\n");
			render_node(s, catchb);
		}
		if (n->has_finally) {
			src_indent(s);
			src_put(s, "finally\n");
			render_node(s, fin);
		}
		break;
	}
	default:
		die("bad node kind in renderer");
	}
}

/*
 * Render a full script: an event recorder, integer variables, and one
 * function whose body is the tree.  Return values and uncaught throws are
 * exposed as globals so the C side can read them.
 */
static char *render_program(const Node *root, int renamed, int wrapped)
{
	Src s;
	int i;
	memset(&s, 0, sizeof s);
	s.renamed = renamed;
	s.wrapped = wrapped;

	src_put(&s, "var __log='';\n");
	src_put(&s, "function ev(id){__log+=id+',';}\n");
	src_put(&s, "var ");
	for (i = 0; i < NVAR; ++i) {
		if (i)
			src_put(&s, ",");
		src_fmt(&s, "z%d=%d", map_var(renamed, i), (i + 1) * 2);
	}
	src_put(&s, ";\n");
	src_put(&s, "var __r=0,__thrown=0,__didthrow=0;\n");
	src_put(&s, "function __prog(){\n");
	s.indent = 1;
	render_node(&s, root);
	src_indent(&s);
	src_put(&s, "return 0;\n");
	src_put(&s, "}\n");
	src_put(&s, "try{__r=__prog();}catch(e){__didthrow=1;__thrown=e;}\n");
	if (s.err)
		die("source buffer overflow");
	return s.buf;
}

/* ------------------------------------------------------------------ */
/* Tree and diagnostics printing                                       */
/* ------------------------------------------------------------------ */

static const char *kind_name(int kind)
{
	switch (kind) {
	case K_BLOCK: return "BLOCK";
	case K_EVENT: return "EVENT";
	case K_ASSIGN: return "ASSIGN";
	case K_RETURN: return "RETURN";
	case K_THROW: return "THROW";
	case K_BREAK: return "BREAK";
	case K_CONTINUE: return "CONTINUE";
	case K_TRY: return "TRY";
	case K_LOOP: return "LOOP";
	default: return "?";
	}
}

static void print_tree(const Node *n, int depth)
{
	int i, pad;
	for (pad = 0; pad < depth; ++pad)
		printf("  ");
	printf("%s node=%d", kind_name(n->kind), n->id);
	if (n->kind == K_EVENT)
		printf(" event=%d", n->event);
	if (n->var >= 0)
		printf(" var=z%d", n->var);
	if (n->kind == K_TRY && n->has_catch)
		printf(" catch=z%d", n->catch_var);
	if (n->value)
		printf(" value=%d", n->value);
	if (n->label >= 0)
		printf(" label=L%d", n->label);
	if (n->kind == K_LOOP)
		printf(" rounds=%d", n->rounds);
	if (n->kind == K_TRY)
		printf(" catch=%d finally=%d", n->has_catch, n->has_finally);
	printf("\n");
	for (i = 0; i < n->nkids; ++i)
		print_tree(n->kids[i], depth + 1);
}

static void model_log(const Model *m, char *out, size_t cap)
{
	size_t pos = 0;
	int i;
	out[0] = 0;
	for (i = 0; i < m->nevents; ++i) {
		int w = snprintf(out + pos, cap - pos, "%d,", m->events[i]);
		if (w < 0 || (size_t)w >= cap - pos)
			die("log buffer overflow");
		pos += (size_t)w;
	}
}

/* First event id where model and observed log diverge. */
static int first_event_diff(const Model *m, const Outcome *o)
{
	char tmp[32];
	size_t pos = 0;
	int i;
	for (i = 0; i < m->nevents; ++i) {
		int w = snprintf(tmp, sizeof tmp, "%d,", m->events[i]);
		if (w <= 0 || pos + (size_t)w >= LOGMAX ||
		        strncmp(o->log + pos, tmp, (size_t)w) != 0)
			return i;
		pos += (size_t)w;
	}
	if (o->log[pos] != 0)
		return i;
	return -1;
}

/* ------------------------------------------------------------------ */
/* Engine harness                                                      */
/* ------------------------------------------------------------------ */

/* Run one rendered script; return 0 on success, -1 if it cannot run.   */
static int run_one(js_State *J, const char *src, int renamed, Outcome *out)
{
	int top0 = js_gettop(J);
	int i, rc;

	memset(out, 0, sizeof *out);
	if (js_ploadstring(J, "fuzz", src)) {
		fprintf(stderr, "completion_fuzz: load failure: %s\n",
		        js_trystring(J, -1, "?"));
		js_pop(J, 1);
		return -1;
	}
	js_pushundefined(J);
	rc = js_pcall(J, 0);
	if (rc) {
		/* generated scripts catch their own throws internally */
		fprintf(stderr, "completion_fuzz: unexpected uncaught: %s\n",
		        js_trystring(J, -1, "?"));
		js_pop(J, 1);
		return -1;
	}
	js_pop(J, js_gettop(J) - top0);

	js_getglobal(J, "__didthrow");
	out->didthrow = js_toboolean(J, -1);
	js_pop(J, 1);
	js_getglobal(J, "__thrown");
	out->thrown = (int)js_tonumber(J, -1);
	js_pop(J, 1);
	js_getglobal(J, "__r");
	out->r = (int)js_tonumber(J, -1);
	js_pop(J, 1);
	for (i = 0; i < NVAR; ++i) {
		char name[16];
		snprintf(name, sizeof name, "z%d", map_var(renamed, i));
		js_getglobal(J, name);
		out->vars[i] = (int)js_tonumber(J, -1);
		js_pop(J, 1);
	}
	js_getglobal(J, "__log");
	snprintf(out->log, sizeof out->log, "%s",
	         js_trystring(J, -1, "<string-error>"));
	js_pop(J, 1);

	if (js_gettop(J) != top0)
		return -1; /* unbalanced C value stack */
	return 0;
}

static const char *comp_name(int kind)
{
	switch (kind) {
	case CP_NORMAL: return "normal";
	case CP_RETURN: return "return";
	case CP_THROW: return "throw";
	case CP_BREAK: return "break";
	case CP_CONTINUE: return "continue";
	default: return "?";
	}
}

static void report_mismatch(unsigned seed, int idx, int transform,
                            const Node *root, const char *src,
                            const Model *m, const Completion *fc,
                            const Outcome *o)
{
	const char *tname[NTRANSFORM] = { "identity", "rename", "blockwrap" };
	char buf[LOGMAX];
	printf("MISMATCH seed=%u program=%d transform=%s\n",
	       seed, idx, tname[transform]);
	if (root) {
		printf("TREE:\n");
		print_tree(root, 1);
	}
	printf("SOURCE:\n%s", src);
	if (src[strlen(src) - 1] != '\n')
		printf("\n");
	printf("MODEL: completion=%s value=%d didthrow=%d nevents=%d\n",
	       comp_name(fc->kind), fc->value,
	       fc->kind == CP_THROW, m->nevents);
	printf("FIRST_DIFFERING_EVENT index=%d\n", first_event_diff(m, o));
	model_log(m, buf, sizeof buf);
	printf("MODEL_LOG=%s\n", buf);
	printf("ACTUAL: didthrow=%d thrown=%d r=%d log=%s\n",
	       o->didthrow, o->thrown, o->r, o->log);
	{
		int vi;
		for (vi = 0; vi < NVAR; ++vi) {
			if (m->globals[vi] != o->vars[vi]) {
				printf("FIRST_DIFFERING_VARIABLE z%d model=%d actual=%d\n",
				       vi, m->globals[vi], o->vars[vi]);
				break;
			}
		}
	}
	fflush(stdout);
}

/* Compare one outcome against the model; returns 1 if identical. */
static int expect_match(const Model *m, const Completion *finalc,
                        const Outcome *o, char *elog)
{
	int exp_throw = (finalc->kind == CP_THROW);
	int exp_r = (finalc->kind == CP_RETURN) ? finalc->value : 0;
	int i;
	if (o->didthrow != exp_throw)
		return 0;
	if (exp_throw && o->thrown != finalc->value)
		return 0;
	if (!exp_throw && o->r != exp_r)
		return 0;
	for (i = 0; i < NVAR; ++i)
		if (o->vars[i] != m->globals[i])
			return 0;
	model_log(m, elog, LOGMAX);
	if (strcmp(o->log, elog) != 0)
		return 0;
	return 1;
}

/* ------------------------------------------------------------------ */
/* State hygiene: uncaught exception followed by a healthy call on the */
/* same js_State, plus an explicit catch-binding restoration check.    */
/* ------------------------------------------------------------------ */

static int hygiene_check(js_State *J, int policy)
{
	int top0, rc, ncheck = 0;

	top0 = js_gettop(J);
	if (js_ploadstring(J, "boom", "function __boom(){throw 31337;}__boom();"))
		return -1;
	js_pushundefined(J);
	rc = js_pcall(J, 0);
	TASK_CHECK(rc == 1);                 /* uncaught exception reported */
	++ncheck;
	js_pop(J, 1);                       /* drop the error value */
	TASK_CHECK(js_gettop(J) == top0);   /* C stack rebalanced */
	++ncheck;

	if (js_ploadstring(J, "ping", "function __ping(){return 1234;}__ping();"))
		return -1;
	js_pushundefined(J);
	if (js_pcall(J, 0)) {
		js_pop(J, 1);
		return -1;
	}
	TASK_CHECK((int)js_tonumber(J, -1) == 1234);
	++ncheck;
	js_pop(J, 1);
	TASK_CHECK(js_gettop(J) == top0);   /* no residual exception state */
	++ncheck;

	if (policy == GRP_CATCH) {
		/* catch binding must not leak past the catch block: run only in
		   the catch group so the finally/abrupt groups stay independent
		   of the OP_ENDCATCH implementation */
		if (js_ploadstring(J, "hyg",
		    "function __h(){var z0=7;try{throw 1;}catch(z0){;}return z0;}"
		    "var __hr=__h();"))
			return -1;
		js_pushundefined(J);
		if (js_pcall(J, 0)) {
			js_pop(J, 1);
			return -1;
		}
		js_pop(J, 1);
		js_getglobal(J, "__hr");
		TASK_CHECK((int)js_tonumber(J, -1) == 7);
		++ncheck;
		js_pop(J, 1);
		TASK_CHECK(js_gettop(J) == top0);
		++ncheck;
	}
	return ncheck;
}

/* ------------------------------------------------------------------ */
/* One contract group: generate, model, execute, cross-check, replay.  */
/* ------------------------------------------------------------------ */

static int run_group(unsigned seed, int policy)
{
	js_State *J, *J2;
	static Outcome kept[NPROG * NTRANSFORM];
	static char *srcs[NPROG * NTRANSFORM];
	static Model models[NPROG];
	static Completion finals[NPROG];
	char elog[LOGMAX];
	int p, t, checks = 0;

	J = js_newstate(NULL, NULL, 0);
	if (!J)
		return -1;

	for (p = 0; p < NPROG; ++p) {
		Gen g;
		Node *root;
		Model m;
		Completion fc;
		int i;

		memset(&g, 0, sizeof g);
		g.policy = policy;
		memset(&m, 0, sizeof m);
		for (i = 0; i < NVAR; ++i)
			m.globals[i] = (i + 1) * 2;

		root = gen_program_tree(&g, seed, p);
		fc = interp(&m, root);
		if (fc.kind == CP_BREAK || fc.kind == CP_CONTINUE) {
			/* a jump escaping the function would render invalid JS */
			printf("internal: unconsumed jump at root seed=%u p=%d\n", seed, p);
			free_trees(&g);
			js_freestate(J);
			return -1;
		}
		models[p] = m;
		finals[p] = fc;

		for (t = 0; t < NTRANSFORM; ++t) {
			Outcome o;
			int idx = p * NTRANSFORM + t;
			char *src = render_program(root, t == 1, t == 2);
			srcs[idx] = src;
			if (run_one(J, src, t == 1, &o)) {
				printf("internal: run failure seed=%u p=%d t=%d\n",
				       seed, p, t);
				free_trees(&g);
				js_freestate(J);
				return -1;
			}
			kept[idx] = o;
			if (!expect_match(&m, &fc, &o, elog)) {
				report_mismatch(seed, p, t, root, src, &m, &fc, &o);
				TASK_CHECK(0);
			}
			TASK_CHECK(1);
			checks++;
			if ((idx & 31) == 0)
				js_gc(J, 0); /* no engine pointers are held across this */
		}
		free_trees(&g);
	}

	{
		int hc = hygiene_check(J, policy);
		if (hc < 0) {
			js_freestate(J);
			return -1;
		}
		checks += hc;
	}

	/* Rebuild a fresh state and replay the identical inputs. */
	J2 = js_newstate(NULL, NULL, 0);
	if (!J2) {
		js_freestate(J);
		return -1;
	}
	for (p = 0; p < NPROG; ++p) {
		for (t = 0; t < NTRANSFORM; ++t) {
			Outcome o2;
			int idx = p * NTRANSFORM + t;
			if (run_one(J2, srcs[idx], t == 1, &o2)) {
				js_freestate(J2);
				js_freestate(J);
				return -1;
			}
			/* fresh state must agree with the reused state... */
			if (memcmp(&o2, &kept[idx], sizeof o2) != 0) {
				Gen rg;
				Node *rroot;
				memset(&rg, 0, sizeof rg);
				rg.policy = policy;
				rroot = gen_program_tree(&rg, seed, p);
				report_mismatch(seed, p, t, rroot, srcs[idx],
				                &models[p], &finals[p], &o2);
				free_trees(&rg);
				TASK_CHECK(0);
			}
			TASK_CHECK(1);
			/* ...and with the independent model */
			if (!expect_match(&models[p], &finals[p], &o2, elog)) {
				Gen rg;
				Node *rroot;
				memset(&rg, 0, sizeof rg);
				rg.policy = policy;
				rroot = gen_program_tree(&rg, seed, p);
				report_mismatch(seed, p, t, rroot, srcs[idx],
				                &models[p], &finals[p], &o2);
				free_trees(&rg);
				TASK_CHECK(0);
			}
			TASK_CHECK(1);
			checks += 2;
		}
	}
	TASK_CHECK(js_gettop(J2) == 0);
	js_gc(J2, 0);
	js_freestate(J2);
	TASK_CHECK(js_gettop(J) == 0);
	js_gc(J, 0);
	js_freestate(J);

	/* Coverage: the combinations each policy is responsible for must     */
	/* really have occurred (not merely been generated as source text).   */
	{
		int cr = 0, co = 0, crt = 0, cs = 0, cj = 0;
		for (p = 0; p < NPROG; ++p) {
			cr += models[p].cov_ret_finally;
			co += models[p].cov_fin_overwrite;
			crt += models[p].cov_rethrow;
			cs += models[p].cov_shadow;
			cj += models[p].cov_jump_outer;
		}
		if (policy == GRP_CATCH) {
			TASK_CHECK(crt > 0);   /* rethrow from a catch block      */
			TASK_CHECK(cs > 0);    /* same-named catch binding        */
			checks += 2;
		} else {
			TASK_CHECK(cr > 0);    /* return travels through finally  */
			TASK_CHECK(co > 0);    /* finally overwrites a completion */
			TASK_CHECK(cj > 0);    /* break/continue through finally  */
			checks += 3;
		}
	}

	for (p = 0; p < NPROG * NTRANSFORM; ++p)
		free(srcs[p]);

	if (checks <= 0)
		return -1;
	return checks;
}

int contract_finally(void)
{
	return run_group(101u, GRP_FINALLY);
}

int contract_catch(void)
{
	return run_group(202u, GRP_CATCH);
}

int contract_abrupt(void)
{
	return run_group(303u, GRP_ABRUPT);
}
