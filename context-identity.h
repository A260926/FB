#ifndef CONTEXT_IDENTITY_H
#define CONTEXT_IDENTITY_H
/*
 * Per-resource context (reference counting) identity tracking.
 *
 * The ordinary -Wcontext machinery only counts the total number of
 * contexts held on each control flow path.  When a function's
 * context(expr, in, out) attributes name a resource through their
 * first argument, -Wcontext-identity keeps a separate ledger for every
 * distinct resource identity instead, so that taking a reference on
 * one object can never repay a reference owed on another one.
 *
 * The analysis is purely static: it reads the declarations and the
 * linearized IR of the program but never executes the analyzed code.
 */

struct entrypoint;
struct expression;
struct instruction;
struct pseudo;
struct symbol;

/*
 * Substitute a parameter expression of a callee declaration with the
 * identity-bearing pseudo actually passed at a call instruction.
 * Returns the VOID pseudo when the annotation does not name one of the
 * callee's parameters.
 */
pseudo_t cid_argument_pseudo(struct symbol *fn_type,
			     struct expression *expr,
			     struct instruction *call);

/*
 * Run the per-resource context balance check for one function.
 * Called from the sparse checking entry when -Wcontext-identity is on.
 */
void cid_check_entrypoint(struct entrypoint *ep);

#endif /* CONTEXT_IDENTITY_H */
