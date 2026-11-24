// SPDX-License-Identifier: MIT
//
// Per-resource context identity tracking for -Wcontext-identity.
//
// The regular context check only knows one anonymous counter: all
// context(expr, in, out) attributes and all __context__() statements
// contribute to the same total.  This module instead keeps a separate
// ledger entry for every resource identity that can be statically
// named, so that a reference taken on one object can never repay an
// obligation owed on another one.
//
// The analysis is entirely static: it only reads the parsed
// declarations and the linearized IR, it never executes the program
// under analysis.
//
// Supported resource identities:
//   - pointer arguments of the enclosing function,
//   - the address of a named object (global/static/local variable),
//   - direct copies of such pointers (plain SSA copies, casts and
//     address-of are transparent to identity).
// Identity is never derived from the spelling of a variable.
//
// When an identity cannot be determined (a PHI/select of different
// sources, an indirect call, an unsupported address expression), the
// analysis says so and keeps checking every resource whose ledger is
// still known instead of silently picking an object or dropping the
// known state.

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "lib.h"
#include "allocate.h"
#include "symbol.h"
#include "expression.h"
#include "linearize.h"
#include "token.h"
#include "context-identity.h"

/*
 * A resource identity.  Two pseudos name the same resource when they
 * resolve to the same identity.
 */
struct cid_identity {
	enum { CID_ID_ARG, CID_ID_OBJ } kind;
	union {
		int arg_nr;		/* 1-based function argument number */
		struct symbol *obj;	/* named object symbol */
	};
};

/* One ledger line: the count held for a single resource. */
struct cid_count {
	struct cid_identity id;
	int count;
	int unknown;		/* count can no longer be determined */
	struct position origin;	/* position of the last obligation change */
	enum {
		CID_ORIGIN_NONE,
		CID_ORIGIN_CALL,
		CID_ORIGIN_STMT,
		CID_ORIGIN_ENTRY,
	} origin_kind;
	struct symbol *origin_fn; /* function named by the originating call */
};

/* The abstract state carried by a basic block. */
struct cid_state {
	struct cid_count *items;
	int nr, cap;
	int tainted;		/* an unresolvable identity was encountered */
};

/* Per-basic-block bookkeeping for the worklist walk. */
struct cid_bb {
	struct cid_state in;
	int queued;
	int seen;
};

/* Diagnostic de-duplication. */
enum cid_diag_kind {
	CID_DIAG_INPUT,
	CID_DIAG_EXIT,
	CID_DIAG_MERGE,
};

struct cid_seen {
	struct cid_identity id;
	struct position pos;
	enum cid_diag_kind kind;
};

struct cid_analysis {
	struct entrypoint *ep;
	char fname[256];	/* stable copy, show_ident uses rotating buffers */
	struct cid_bb *bbs;
	struct cid_seen *seen;
	int seen_nr, seen_cap;
};

/* ---------------------------------------------------------------- */
/* Name handling                                                    */
/* ---------------------------------------------------------------- */

static struct symbol *cid_symbol_base(struct symbol *sym)
{
	if (sym && sym->type == SYM_NODE)
		return sym->ctype.base_type;
	return sym;
}

/*
 * The first argument of a context attribute is parsed but never
 * evaluated, so the resulting expression is an unevaluated EXPR_SYMBOL
 * carrying only its ident, not the resolved symbol.  Identity therefore
 * follows the ident (idents are interned, so pointer comparison works).
 */
static struct ident *cid_expr_ident(struct expression *expr)
{
	if (!expr)
		return NULL;
	while (expr->type == EXPR_IMPLIED_CAST ||
	       expr->type == EXPR_CAST ||
	       expr->type == EXPR_FORCE_CAST)
		expr = expr->cast_expression;
	if (expr->type == EXPR_PREOP && expr->op == '&')
		expr = expr->unop;
	if (expr->type != EXPR_SYMBOL)
		return NULL;
	if (expr->symbol)
		return expr->symbol->ident;
	return expr->symbol_name;
}

/*
 * Substitute a parameter expression of a callee declaration with the
 * identity-bearing pseudo actually passed at a call instruction.
 * Returns the VOID pseudo when the annotation does not name one of the
 * callee's parameters.
 */
pseudo_t cid_argument_pseudo(struct symbol *fn_type,
			     struct expression *expr,
			     struct instruction *call)
{
	struct ident *id;
	struct symbol *arg, *base;
	int idx = 0;

	if (!call || call->opcode != OP_CALL)
		return VOID;
	id = cid_expr_ident(expr);
	base = cid_symbol_base(fn_type);
	if (!id || !base)
		return VOID;

	FOR_EACH_PTR(base->arguments, arg) {
		struct symbol *arg_base = cid_symbol_base(arg);
		idx++;
		if ((arg->ident && arg->ident == id) ||
		    (arg_base && arg_base->ident == id))
			return ptr_list_nth(call->arguments, idx - 1);
	} END_FOR_EACH_PTR(arg);
	return VOID;
}

/* ---------------------------------------------------------------- */
/* Identity resolution                                              */
/* ---------------------------------------------------------------- */

static int cid_identity_equal(const struct cid_identity *a,
			      const struct cid_identity *b)
{
	if (a->kind != b->kind)
		return 0;
	if (a->kind == CID_ID_ARG)
		return a->arg_nr == b->arg_nr;
	return a->obj == b->obj;
}

static const char *cid_identity_name(const struct cid_identity *id)
{
	static char argbuf[4][40];
	static int n;
	char *buf;

	if (id->kind == CID_ID_ARG) {
		buf = argbuf[3 & ++n];
		snprintf(buf, sizeof(argbuf[0]), "argument %d", id->arg_nr);
		return buf;
	}
	if (id->obj->ident)
		return show_ident(id->obj->ident);
	return "<anonymous object>";
}

static int cid_resolve_root(struct pseudo *pseudo, struct cid_identity *out)
{
	if (!pseudo)
		return 0;
	switch (pseudo->type) {
	case PSEUDO_ARG:
		out->kind = CID_ID_ARG;
		out->arg_nr = pseudo->nr;
		return 1;
	case PSEUDO_SYM:
		if (!pseudo->sym)
			return 0;
		out->kind = CID_ID_OBJ;
		out->obj = pseudo->sym;
		return 1;
	default:
		return 0;
	}
}

/*
 * Follow the transparent definitions (plain copies, casts,
 * address-of) down to a tracked root pseudo.  Anything else (PHI,
 * select, arithmetic, a load of an unknown pointer, ...) is an
 * unknown source.
 */
static int cid_trace(struct pseudo *pseudo, struct cid_identity *out)
{
	struct pseudo *seen[64];
	int nr_seen = 0;
	int i;

	for (;;) {
		struct instruction *def;

		if (cid_resolve_root(pseudo, out))
			return 1;
		if (!pseudo || (pseudo->type != PSEUDO_REG &&
				pseudo->type != PSEUDO_PHI))
			return 0;
		for (i = 0; i < nr_seen; i++) {
			if (seen[i] == pseudo)
				return 0;
		}
		if (nr_seen >= (int)ARRAY_SIZE(seen))
			return 0;
		seen[nr_seen++] = pseudo;

		def = pseudo->def;
		if (!def || !def->bb)
			return 0;
		switch (def->opcode) {
		case OP_COPY:
		case OP_PTRCAST:
		case OP_PTRTU:
		case OP_UTPTR:
		case OP_SEXT:
		case OP_ZEXT:
		case OP_TRUNC:
		case OP_SYMADDR:
			pseudo = def->src;
			continue;
		default:
			return 0;
		}
	}
}

/*
 * Resolve an annotation expression of the function being analyzed
 * (its own declaration attributes or a standalone __context__
 * statement).  Such an expression is not tied to a call, so it is
 * resolved against the parameters and objects visible in the function.
 */
static int cid_resolve_local_expr(struct entrypoint *ep,
				  struct expression *expr,
				  struct cid_identity *out)
{
	struct ident *id;
	struct symbol *arg;
	int idx = 0;
	struct symbol *base;

	id = cid_expr_ident(expr);
	if (!id)
		return 0;
	base = cid_symbol_base(ep->name->ctype.base_type);

	/* a parameter name resolves to the corresponding argument identity */
	if (base) {
		FOR_EACH_PTR(base->arguments, arg) {
			struct symbol *arg_base = cid_symbol_base(arg);
			idx++;
			if ((arg->ident && arg->ident == id) ||
			    (arg_base && arg_base->ident == id)) {
				out->kind = CID_ID_ARG;
				out->arg_nr = idx;
				return 1;
			}
		} END_FOR_EACH_PTR(arg);
	}

	/* otherwise it must name a visible object */
	{
		struct symbol *sym = lookup_symbol(id, NS_SYMBOL);
		if (sym && sym->ident) {
			out->kind = CID_ID_OBJ;
			out->obj = sym;
			return 1;
		}
		/*
		 * The annotation is parsed in the declaration scope, which
		 * may already be closed by the time the analysis runs.  The
		 * ident still carries every symbol ever bound to it; take
		 * the innermost non-keyword one rather than giving up.
		 */
		for (sym = id->symbols; sym; sym = sym->next_id) {
			if (!(sym->namespace & NS_SYMBOL))
				continue;
			out->kind = CID_ID_OBJ;
			out->obj = sym;
			return 1;
		}
	}
	return 0;
}

/* ---------------------------------------------------------------- */
/* Ledger state                                                     */
/* ---------------------------------------------------------------- */

static struct cid_count *cid_find(struct cid_state *st,
				  const struct cid_identity *id)
{
	int i;

	for (i = 0; i < st->nr; i++) {
		if (cid_identity_equal(&st->items[i].id, id))
			return &st->items[i];
	}
	return NULL;
}

static struct cid_count *cid_get(struct cid_state *st,
				 const struct cid_identity *id)
{
	struct cid_count *slot = cid_find(st, id);

	if (slot)
		return slot;
	if (st->nr == st->cap) {
		int ncap = st->cap ? st->cap * 2 : 4;
		st->items = realloc(st->items, ncap * sizeof(*st->items));
		st->cap = ncap;
	}
	slot = &st->items[st->nr++];
	memset(slot, 0, sizeof(*slot));
	slot->id = *id;
	return slot;
}

static void cid_state_init(struct cid_state *st)
{
	memset(st, 0, sizeof(*st));
}

static void cid_state_free(struct cid_state *st)
{
	free(st->items);
	cid_state_init(st);
}

static int cid_state_copy(struct cid_state *dst, const struct cid_state *src)
{
	cid_state_init(dst);
	dst->tainted = src->tainted;
	if (src->nr) {
		dst->items = malloc(src->nr * sizeof(*dst->items));
		if (!dst->items)
			return 0;
		memcpy(dst->items, src->items, src->nr * sizeof(*dst->items));
		dst->nr = dst->cap = src->nr;
	}
	return 1;
}

static int cid_state_equal(const struct cid_state *a,
			   const struct cid_state *b)
{
	if (a->tainted != b->tainted || a->nr != b->nr)
		return 0;
	return memcmp(a->items, b->items, a->nr * sizeof(*a->items)) == 0;
}

/* ---------------------------------------------------------------- */
/* Diagnostics                                                      */
/* ---------------------------------------------------------------- */

static int cid_already_reported(struct cid_analysis *a,
				const struct cid_identity *id,
				struct position pos,
				enum cid_diag_kind kind)
{
	int i;

	for (i = 0; i < a->seen_nr; i++) {
		if (a->seen[i].kind == kind &&
		    a->seen[i].pos.stream == pos.stream &&
		    a->seen[i].pos.line == pos.line &&
		    a->seen[i].pos.pos == pos.pos &&
		    cid_identity_equal(&a->seen[i].id, id))
			return 1;
	}
	if (a->seen_nr == a->seen_cap) {
		int ncap = a->seen_cap ? a->seen_cap * 2 : 16;
		a->seen = realloc(a->seen, ncap * sizeof(*a->seen));
		a->seen_cap = ncap;
	}
	a->seen[a->seen_nr].id = *id;
	a->seen[a->seen_nr].pos = pos;
	a->seen[a->seen_nr].kind = kind;
	a->seen_nr++;
	return 0;
}

static void cid_note_origin(struct cid_analysis *a,
			    const struct cid_count *slot,
			    const char *rname)
{
	(void)a;
	if (!slot || slot->origin.stream == 0)
		return;
	switch (slot->origin_kind) {
	case CID_ORIGIN_CALL:
		info(slot->origin,
		     "resource context: the obligation on '%s' was last changed by the call to '%s' here",
		     rname, show_ident(slot->origin_fn->ident));
		break;
	case CID_ORIGIN_STMT:
		info(slot->origin,
		     "resource context: the obligation on '%s' was last changed by a __context__ statement here",
		     rname);
		break;
	case CID_ORIGIN_ENTRY:
		info(slot->origin,
		     "resource context: the obligation on '%s' was created by the entry context of this function",
		     rname);
		break;
	case CID_ORIGIN_NONE:
		break;
	}
}

static void cid_report_input(struct cid_analysis *a,
			     const struct cid_identity *id,
			     int have, int need,
			     struct position pos)
{
	if (cid_already_reported(a, id, pos, CID_DIAG_INPUT))
		return;
	warning(pos,
		"resource context mismatch in '%s': resource '%s' holds %d reference(s), the call requires %d",
		a->fname, cid_identity_name(id), have, need);
}

static void cid_report_exit(struct cid_analysis *a,
			    const struct cid_count *slot,
			    const struct cid_identity *id,
			    int have, int want,
			    struct position pos)
{
	const char *rname = cid_identity_name(id);

	if (cid_already_reported(a, id, pos, CID_DIAG_EXIT))
		return;
	warning(pos,
		"resource context imbalance in '%s': resource '%s' leaves with count %d, expected %d",
		a->fname, rname, have, want);
	cid_note_origin(a, slot, rname);
}

static void cid_report_merge(struct cid_analysis *a,
			     const struct cid_identity *id,
			     int c1, int c2,
			     struct basic_block *bb)
{
	if (cid_already_reported(a, id, bb->pos, CID_DIAG_MERGE))
		return;
	warning(bb->pos,
		"resource context conflict in '%s': resource '%s' reaches this block join with unequal counts (%d and %d); its ledger can no longer be determined",
		a->fname, cid_identity_name(id), c1, c2);
}

static void cid_note_unknown(struct position pos)
{
	info(pos,
	     "resource context: the resource identity cannot be determined here; the known resource ledgers continue to be checked");
}

/* ---------------------------------------------------------------- */
/* Ledger updates                                                   */
/* ---------------------------------------------------------------- */

/*
 * Change a ledger entry.  Entries once marked unknown are frozen: this
 * keeps the dataflow walk terminating even on cyclic control flow.
 */
static void cid_apply(struct cid_state *st, const struct cid_identity *id,
		      int delta, struct position origin, int origin_kind,
		      struct symbol *fn)
{
	struct cid_count *slot = cid_get(st, id);

	if (slot->unknown)
		return;
	slot->count += delta;
	slot->origin = origin;
	slot->origin_kind = origin_kind;
	slot->origin_fn = fn;
}

/* One aggregated obligation of a single call, per substituted identity. */
struct cid_obligation {
	struct cid_identity id;
	int in, out;
	int known;
};

static struct symbol *cid_call_fn_sym(struct instruction *call)
{
	pseudo_t fn = call->func;

	if (fn->type != PSEUDO_SYM)
		return NULL;
	return fn->sym;
}

static struct context_list *cid_fn_contexts(struct symbol *fn_sym)
{
	struct symbol *base;

	if (!fn_sym)
		return NULL;
	if (fn_sym->ctype.contexts)
		return fn_sym->ctype.contexts;
	base = cid_symbol_base(fn_sym);
	if (base && base->ctype.contexts)
		return base->ctype.contexts;
	return NULL;
}

/*
 * Apply every named context of a direct call.  Contexts belonging to
 * the same call are first substituted with the argument identities and
 * aggregated by identity, then the entry counts are verified, and only
 * then are the net changes applied.  Reordering the attributes can
 * therefore not change the result, and a zero net change cannot hide a
 * missing entry reference.
 */
static void cid_process_call(struct cid_analysis *a, struct cid_state *st,
			     struct instruction *call)
{
	struct symbol *fn_sym = cid_call_fn_sym(call);
	struct symbol *fn_base;
	struct context *ctx;
	struct cid_obligation *obls = NULL;
	int nr_obls = 0, cap_obls = 0;
	int i;

	if (!fn_sym) {
		/* indirect call: no named identity can be derived */
		st->tainted = 1;
		cid_note_unknown(call->pos);
		return;
	}
	fn_base = cid_symbol_base(fn_sym);

	FOR_EACH_PTR(cid_fn_contexts(fn_sym), ctx) {
		struct cid_obligation *obl;
		pseudo_t arg;
		int in = (int)ctx->in;
		int out = (int)ctx->out;

		if (in < 0)
			in = 0;
		if (out < 0)
			out = 0;

		/* anonymous call contexts never touch named ledgers */
		if (!ctx->context)
			continue;

		if (nr_obls == cap_obls) {
			int ncap = cap_obls ? cap_obls * 2 : 4;
			obls = realloc(obls, ncap * sizeof(*obls));
			cap_obls = ncap;
		}
		obl = &obls[nr_obls++];
		memset(obl, 0, sizeof(*obl));
		obl->in = in;
		obl->out = out;
		arg = cid_argument_pseudo(fn_base, ctx->context, call);
		if (arg && arg != VOID && cid_trace(arg, &obl->id)) {
			obl->known = 1;
		} else {
			st->tainted = 1;
			cid_note_unknown(call->pos);
		}
	} END_FOR_EACH_PTR(ctx);

	/* aggregate the substituted inputs/outputs per resource identity */
	for (i = 0; i < nr_obls; i++) {
		int j;

		if (!obls[i].known)
			continue;
		for (j = 0; j < i; j++) {
			if (obls[j].known &&
			    cid_identity_equal(&obls[j].id, &obls[i].id)) {
				obls[j].in += obls[i].in;
				obls[j].out += obls[i].out;
				obls[i].known = 0;
				break;
			}
		}
	}

	/* phase 1: verify every entry obligation against the current state */
	for (i = 0; i < nr_obls; i++) {
		struct cid_count *slot;
		int have;

		if (!obls[i].known)
			continue;
		slot = cid_find(st, &obls[i].id);
		have = slot ? slot->count : 0;
		if (slot && slot->unknown)
			continue;
		if (have < obls[i].in)
			cid_report_input(a, &obls[i].id, have, obls[i].in,
					 call->pos);
	}

	/* phase 2: apply the net changes all together */
	for (i = 0; i < nr_obls; i++) {
		if (!obls[i].known)
			continue;
		cid_apply(st, &obls[i].id, obls[i].out - obls[i].in,
			  call->pos, CID_ORIGIN_CALL, fn_sym);
	}

	free(obls);
}

/*
 * A standalone OP_CONTEXT instruction comes from a __context__
 * statement.  A named one directly adjusts the ledger of the resource
 * named in the current function; an unnamed one is anonymous and never
 * repays a named resource.
 */
static void cid_process_statement_context(struct cid_analysis *a,
					  struct cid_state *st,
					  struct instruction *insn)
{
	struct cid_identity id;

	if (!insn->context_expr)
		return;
	if (cid_resolve_local_expr(a->ep, insn->context_expr, &id)) {
		cid_apply(st, &id, insn->increment, insn->pos,
			  CID_ORIGIN_STMT, NULL);
		return;
	}
	st->tainted = 1;
	cid_note_unknown(insn->pos);
}

/* ---------------------------------------------------------------- */
/* CFG walk                                                         */
/* ---------------------------------------------------------------- */

/*
 * Merge SRC into DST at a block join.  A resource whose incoming
 * counts disagree can no longer be tracked: it is marked unknown but
 * every other ledger is retained.  When the join feeds a return block
 * the precise per-path exit check has already been done on each
 * incoming edge, so no conflict warning is emitted there.
 */
static void cid_merge(struct cid_analysis *a, struct cid_state *dst,
		      const struct cid_state *src, struct basic_block *bb,
		      int at_return)
{
	int i;

	dst->tainted |= src->tainted;

	for (i = 0; i < src->nr; i++) {
		const struct cid_count *s = &src->items[i];
		struct cid_count *d = cid_find(dst, &s->id);

		if (!d) {
			/* the resource is implicitly absent (count 0) on
			 * the other incoming path */
			struct cid_count *n;
			if (s->unknown || s->count != 0) {
				if (!s->unknown && !at_return)
					cid_report_merge(a, &s->id, 0,
							 s->count, bb);
				n = cid_get(dst, &s->id);
				n->unknown = 1;
			}
			continue;
		}
		if (d->unknown || s->unknown) {
			d->unknown = 1;
			continue;
		}
		if (d->count != s->count) {
			if (!at_return)
				cid_report_merge(a, &s->id, d->count,
						 s->count, bb);
			d->unknown = 1;
		}
	}

	for (i = 0; i < dst->nr; i++) {
		struct cid_count *d = &dst->items[i];
		struct cid_count *s;

		if (d->unknown)
			continue;
		s = cid_find((struct cid_state *)src, &d->id);
		if (!s) {
			if (d->count != 0) {
				if (!at_return)
					cid_report_merge(a, &d->id, d->count,
							 0, bb);
				d->unknown = 1;
			}
			continue;
		}
		if (s->unknown) {
			d->unknown = 1;
			continue;
		}
		if (d->count != s->count) {
			if (!at_return)
				cid_report_merge(a, &d->id, d->count,
						 s->count, bb);
			d->unknown = 1;
		}
	}
}

static void cid_check_exit(struct cid_analysis *a, const struct cid_state *st,
			   struct instruction *ret)
{
	struct context *ctx;
	struct cid_state required;
	int i;

	cid_state_init(&required);

	/* exit obligations declared on the function itself */
	FOR_EACH_PTR(cid_fn_contexts(a->ep->name), ctx) {
		struct cid_identity id;
		int out = (int)ctx->out;

		if (out < 0)
			out = 0;
		if (!ctx->context)
			continue;
		if (!cid_resolve_local_expr(a->ep, ctx->context, &id))
			continue;
		cid_get(&required, &id)->count = out;
	} END_FOR_EACH_PTR(ctx);

	/* every resource still on the books must meet its exit obligation */
	for (i = 0; i < st->nr; i++) {
		const struct cid_count *slot = &st->items[i];
		struct cid_count *want_slot;
		int want;

		want_slot = cid_find(&required, &slot->id);
		want = want_slot ? want_slot->count : 0;
		if (slot->unknown)
			continue;
		if (slot->count != want)
			cid_report_exit(a, slot, &slot->id, slot->count, want,
					ret->pos);
	}

	/* obligations for resources never touched in the body */
	for (i = 0; i < required.nr; i++) {
		struct cid_count *want_slot = &required.items[i];
		struct cid_count *have_slot;

		have_slot = cid_find((struct cid_state *)st, &want_slot->id);
		if (have_slot)
			continue;
		if (want_slot->count != 0)
			cid_report_exit(a, NULL, &want_slot->id, 0,
					want_slot->count, ret->pos);
	}

	cid_state_free(&required);
}

static void cid_process_bb(struct cid_analysis *a, struct basic_block *bb,
			   struct cid_state *st)
{
	struct instruction *insn;

	FOR_EACH_PTR(bb->insns, insn) {
		if (!insn->bb)
			continue;
		switch (insn->opcode) {
		case OP_CALL:
			cid_process_call(a, st, insn);
			break;
		case OP_CONTEXT:
			/*
			 * Context insns emitted for a call point back at the
			 * call; their effect is already taken from the
			 * callee's declaration above.
			 */
			if (insn->context_call)
				break;
			cid_process_statement_context(a, st, insn);
			break;
		default:
			break;
		}
	} END_FOR_EACH_PTR(insn);
}

static unsigned int cid_max_bb_nr(struct entrypoint *ep)
{
	struct basic_block *bb;
	unsigned int max = 0;

	FOR_EACH_PTR(ep->bbs, bb) {
		if (bb->nr > max)
			max = bb->nr;
	} END_FOR_EACH_PTR(bb);
	return max;
}

/* Seed the entry block with the function's own obligations. */
static void cid_seed_entry(struct cid_analysis *a, struct cid_state *st)
{
	struct context *ctx;

	FOR_EACH_PTR(cid_fn_contexts(a->ep->name), ctx) {
		struct cid_identity id;
		int in = (int)ctx->in;

		if (in < 0)
			in = 0;
		if (!ctx->context)
			continue;
		if (cid_resolve_local_expr(a->ep, ctx->context, &id)) {
			if (in)
				cid_apply(st, &id, in, a->ep->name->pos,
					  CID_ORIGIN_ENTRY, NULL);
		} else {
			st->tainted = 1;
			cid_note_unknown(a->ep->name->pos);
		}
	} END_FOR_EACH_PTR(ctx);
}

void cid_check_entrypoint(struct entrypoint *ep)
{
	unsigned int max_nr;
	struct cid_bb *info;
	struct basic_block **work = NULL;
	int work_nr = 0, work_cap = 0;
	struct basic_block *bb, *entry;
	struct cid_analysis a;
	int i;

	if (!Wcontext_identity || !ep || !ep->entry)
		return;

	max_nr = cid_max_bb_nr(ep);
	info = calloc(max_nr + 1, sizeof(*info));
	if (!info)
		return;

	a.ep = ep;
	snprintf(a.fname, sizeof(a.fname), "%s",
		 show_ident(ep->name->ident));
	a.bbs = info;
	a.seen = NULL;
	a.seen_nr = a.seen_cap = 0;

	entry = ep->entry->bb;
	cid_seed_entry(&a, &info[entry->nr].in);
	info[entry->nr].seen = 1;

	work_cap = 8;
	work = malloc(work_cap * sizeof(*work));
	if (!work)
		goto out;
	work[work_nr++] = entry;
	info[entry->nr].queued = 1;

	while (work_nr) {
		struct cid_state out_state;
		struct basic_block *child;
		struct instruction *term;

		bb = work[--work_nr];
		info[bb->nr].queued = 0;

		if (!cid_state_copy(&out_state, &info[bb->nr].in))
			break;
		cid_process_bb(&a, bb, &out_state);

		term = last_instruction(bb->insns);
		if (term && term->opcode == OP_RET)
			cid_check_exit(&a, &out_state, term);

		FOR_EACH_PTR(bb->children, child) {
			struct cid_bb *ci = &info[child->nr];
			struct cid_state merged;
			struct instruction *child_term;
			int at_return;

			child_term = last_instruction(child->insns);
			at_return = child_term &&
				    child_term->opcode == OP_RET;
			/*
			 * Every edge entering a return block is checked
			 * against the function's exit obligations before
			 * the states are joined.  Apply the block's own
			 * transfer to a private copy first, so an early
			 * error return can never be hidden by a balanced
			 * sibling path.
			 */
			if (at_return) {
				struct cid_state edge_state;

				if (!cid_state_copy(&edge_state, &out_state))
					break;
				cid_process_bb(&a, child, &edge_state);
				cid_check_exit(&a, &edge_state, child_term);
				cid_state_free(&edge_state);
			}

			cid_state_init(&merged);
			if (ci->seen) {
				if (!cid_state_copy(&merged, &ci->in))
					break;
				cid_merge(&a, &merged, &out_state, child,
					  at_return);
			} else {
				if (!cid_state_copy(&merged, &out_state))
					break;
			}

			if (!ci->seen || !cid_state_equal(&ci->in, &merged)) {
				cid_state_free(&ci->in);
				ci->in = merged;
				ci->seen = 1;
				if (!ci->queued) {
					if (work_nr == work_cap) {
						work_cap *= 2;
						work = realloc(work,
							work_cap * sizeof(*work));
					}
					work[work_nr++] = child;
					ci->queued = 1;
				}
			} else {
				cid_state_free(&merged);
			}
		} END_FOR_EACH_PTR(child);

		cid_state_free(&out_state);
	}

	free(work);
out:
	for (i = 0; (unsigned int)i <= max_nr; i++)
		cid_state_free(&info[i].in);
	free(info);
	free(a.seen);
}
