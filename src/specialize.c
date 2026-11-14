#include <spvm/specialize.h>
#include <spvm/spirv.h>

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>

/* ------------------------------------------------------------------ */
/* Internal model                                                      */
/* ------------------------------------------------------------------ */

enum spec_type_kind {
	TK_NONE = 0,
	TK_VOID,
	TK_BOOL,
	TK_INT,
	TK_FLOAT,
	TK_VECTOR,
	TK_MATRIX,
	TK_ARRAY,
	TK_RUNTIME_ARRAY,
	TK_STRUCT,
	TK_POINTER,
	TK_OTHER
};

typedef struct spec_type {
	int kind;
	int bitcount;
	int sign;
	spvm_word comp;       /* component / element / pointee type id        */
	spvm_word count;      /* vector width / array length / struct members */
	spvm_word length_id;  /* OpTypeArray length constant id               */
	spvm_word* members;   /* struct member type ids                       */
} spec_type;

typedef struct spec_const {
	int opcode;           /* 0 when this id is not a constant definition  */
	spvm_word type;
	spvm_word op_start;   /* index of first data operand in src           */
	spvm_word op_count;   /* number of data operand words                 */
} spec_const;

typedef struct spec_val spec_val;
struct spec_val {
	spvm_word count;      /* 0 => leaf, otherwise number of children      */
	uint32_t bits;        /* valid when count == 0                        */
	spec_val** kids;
};

typedef struct spec_pair {
	spvm_word spec_id;
	spvm_word target;
} spec_pair;

typedef struct spec_ctx {
	spvm_context_t context;
	const spvm_word* src;
	size_t word_count;
	spvm_word bound;

	spec_type* types;                 /* size bound                     */
	spec_const* consts;               /* size bound                     */
	spvm_word* var_ptr_type;         /* size bound, 0 if not a variable */
	spvm_word* var_init;             /* size bound, 0 / (spvm_word)-1  */

	spec_pair* pairs;                /* SpecId decoration bindings     */
	size_t pair_count;

	uint32_t* override_bits;         /* size bound, per result id      */
	unsigned char* has_override;      /* size bound                     */

	spec_val** values;               /* size bound, memoized results   */
	unsigned char* visit;            /* 0 unseen, 1 visiting, 2 done   */

	void** arena;
	size_t arena_count, arena_cap;

	int err_code;
	spvm_word err_rid;
} spec_ctx;

static void* spec_alloc(spec_ctx* c, size_t size)
{
	void* p = calloc(1, size);
	if (p == NULL) {
		c->err_code = SPVM_SPEC_ALLOCATION;
		c->err_rid = 0;
		return NULL;
	}
	if (c->arena_count == c->arena_cap) {
		size_t cap = c->arena_cap ? c->arena_cap * 2 : 64;
		void** grow = (void**)realloc(c->arena, cap * sizeof(void*));
		if (grow == NULL) {
			free(p);
			c->err_code = SPVM_SPEC_ALLOCATION;
			c->err_rid = 0;
			return NULL;
		}
		c->arena = grow;
		c->arena_cap = cap;
	}
	c->arena[c->arena_count++] = p;
	return p;
}

static void spec_fail(spec_ctx* c, int code, spvm_word rid)
{
	c->err_code = code;
	c->err_rid = rid;
}

/* ------------------------------------------------------------------ */
/* Module parsing                                                      */
/* ------------------------------------------------------------------ */

static int parse_module(spec_ctx* c)
{
	const spvm_word* p = c->src + 5;
	const spvm_word* end = c->src + c->word_count;

	while (p < end) {
		unsigned int header = (unsigned int)(*p);
		int words = (int)((header >> SpvWordCountShift) & 0xffffu);
		int opcode = (int)(header & SpvOpCodeMask);

		if (words < 1 || (size_t)(end - p) < (size_t)words) {
			spec_fail(c, SPVM_SPEC_INVALID_MODULE, 0);
			return 0;
		}

		const spvm_word* args = p + 1;
		int argc = words - 1;

		switch (opcode) {
		case SpvOpTypeVoid:
			if (argc < 1) break;
			c->types[args[0]].kind = TK_VOID;
			break;
		case SpvOpTypeBool:
			if (argc < 1) break;
			c->types[args[0]].kind = TK_BOOL;
			c->types[args[0]].bitcount = 1;
			c->types[args[0]].count = 1;
			break;
		case SpvOpTypeInt:
			if (argc < 3) break;
			c->types[args[0]].kind = TK_INT;
			c->types[args[0]].bitcount = args[1];
			c->types[args[0]].sign = args[2];
			c->types[args[0]].count = 1;
			break;
		case SpvOpTypeFloat:
			if (argc < 2) break;
			c->types[args[0]].kind = TK_FLOAT;
			c->types[args[0]].bitcount = args[1];
			c->types[args[0]].count = 1;
			break;
		case SpvOpTypeVector:
			if (argc < 3) break;
			c->types[args[0]].kind = TK_VECTOR;
			c->types[args[0]].comp = args[1];
			c->types[args[0]].count = (spvm_word)args[2];
			break;
		case SpvOpTypeMatrix:
			if (argc < 3) break;
			c->types[args[0]].kind = TK_MATRIX;
			c->types[args[0]].comp = args[1];
			c->types[args[0]].count = (spvm_word)args[2];
			break;
		case SpvOpTypeArray:
			if (argc < 3) break;
			c->types[args[0]].kind = TK_ARRAY;
			c->types[args[0]].comp = args[1];
			c->types[args[0]].length_id = args[2];
			break;
		case SpvOpTypeRuntimeArray:
			if (argc < 2) break;
			c->types[args[0]].kind = TK_RUNTIME_ARRAY;
			c->types[args[0]].comp = args[1];
			break;
		case SpvOpTypeStruct: {
			if (argc < 1) break;
			spvm_word id = args[0];
			spvm_word mc = (spvm_word)(argc - 1);
			c->types[id].kind = TK_STRUCT;
			c->types[id].count = mc;
			spvm_word* mem = (spvm_word*)spec_alloc(c, (size_t)mc * sizeof(spvm_word));
			if (mem == NULL) return 0;
			for (spvm_word i = 0; i < mc; i++)
				mem[i] = args[1 + i];
			c->types[id].members = mem;
			break;
		}
		case SpvOpTypePointer:
			if (argc < 3) break;
			c->types[args[0]].kind = TK_POINTER;
			c->types[args[0]].comp = args[2];
			break;
		case SpvOpDecorate:
			/* OpDecorate <target> <decoration> [literals...] */
			if (argc >= 2 && args[1] == SpvDecorationSpecId) {
				spec_pair pr;
				pr.target = args[0];
				pr.spec_id = args[2];
				spec_pair* grow = (spec_pair*)realloc(c->pairs, (c->pair_count + 1) * sizeof(spec_pair));
				if (grow == NULL) { spec_fail(c, SPVM_SPEC_ALLOCATION, 0); return 0; }
				c->pairs = grow;
				c->pairs[c->pair_count++] = pr;
			}
			break;
		case SpvOpVariable: {
			if (argc < 3) break;
			spvm_word id = args[1];
			c->var_ptr_type[id] = args[0];
			c->var_init[id] = (argc >= 4) ? args[3] : (spvm_word)-1;
			break;
		}
		default:
			break;
		}

		/* Constant-creation instructions all start with result type, result id. */
		switch (opcode) {
		case SpvOpUndef:
		case SpvOpConstantTrue:
		case SpvOpConstantFalse:
		case SpvOpConstant:
		case SpvOpConstantComposite:
		case SpvOpConstantNull:
		case SpvOpSpecConstantTrue:
		case SpvOpSpecConstantFalse:
		case SpvOpSpecConstant:
		case SpvOpSpecConstantComposite:
		case SpvOpSpecConstantOp: {
			if (argc < 2) break;
			spvm_word id = args[1];
			spec_const* sc = &c->consts[id];
			sc->opcode = opcode;
			sc->type = args[0];
			/* operands: for ordinary constants everything after type,id;
			 * for SpecConstantOp skip the wrapped opcode word as well. */
			if (opcode == SpvOpSpecConstantOp) {
				if (argc < 3) {
					spec_fail(c, SPVM_SPEC_INVALID_MODULE, args[1]);
					return 0;
				}
				sc->op_start = (spvm_word)((p + 1 + 3) - c->src); /* after type,id,wrapped-op */
				sc->op_count = (spvm_word)(argc - 3);
			} else {
				sc->op_start = (spvm_word)((p + 1 + 2) - c->src);
				sc->op_count = (spvm_word)(argc - 2);
			}
			break;
		}
		default:
			break;
		}

		p += words;
	}

	return 1;
}

/* ------------------------------------------------------------------ */
/* Type universe                                                       */
/* ------------------------------------------------------------------ */

static int type_supported(spec_ctx* c, spvm_word tid, spvm_word rid);

static int type_supported(spec_ctx* c, spvm_word tid, spvm_word rid)
{
	if (tid == 0 || tid >= c->bound) {
		spec_fail(c, SPVM_SPEC_BAD_REFERENCE, rid);
		return 0;
	}
	spec_type* t = &c->types[tid];
	switch (t->kind) {
	case TK_BOOL:
		return 1;
	case TK_INT:
	case TK_FLOAT:
		if (t->bitcount != 32) {
			spec_fail(c, SPVM_SPEC_UNSUPPORTED_TYPE, rid);
			return 0;
		}
		return 1;
	case TK_VECTOR:
		return type_supported(c, t->comp, rid);
	case TK_ARRAY:
		return type_supported(c, t->comp, rid);
	case TK_STRUCT:
		for (spvm_word i = 0; i < t->count; i++)
			if (!type_supported(c, t->members[i], rid))
				return 0;
		return 1;
	case TK_MATRIX:
	case TK_RUNTIME_ARRAY:
	default:
		spec_fail(c, SPVM_SPEC_UNSUPPORTED_TYPE, rid);
		return 0;
	}
}

/* Type of the member reached by following literal indices from tid. Returns
 * 0 (and reports a shape error on rid) if any index is out of bounds. */
static spvm_word member_type_at(spec_ctx* c, spvm_word tid, const spvm_word* idx,
	spvm_word depth, spvm_word rid)
{
	spvm_word cur = tid;
	for (spvm_word k = 0; k < depth; k++) {
		spec_type* t = &c->types[cur];
		spvm_word i = idx[k];
		if (t->kind == TK_STRUCT) {
			if (i >= t->count) { spec_fail(c, SPVM_SPEC_BAD_SHAPE, rid); return 0; }
			cur = t->members[i];
		} else if (t->kind == TK_VECTOR || t->kind == TK_ARRAY) {
			if (i >= t->count) { spec_fail(c, SPVM_SPEC_BAD_SHAPE, rid); return 0; }
			cur = t->comp;
		} else {
			spec_fail(c, SPVM_SPEC_BAD_SHAPE, rid);
			return 0;
		}
	}
	return cur;
}

/* Recursive structural comparison of an evaluated value against a type. */
static int shape_matches(spec_ctx* c, const spec_val* v, spvm_word tid, spvm_word rid)
{
	spec_type* t = &c->types[tid];
	switch (t->kind) {
	case TK_BOOL:
	case TK_INT:
	case TK_FLOAT:
		if (v->count != 0) { spec_fail(c, SPVM_SPEC_BAD_SHAPE, rid); return 0; }
		return 1;
	case TK_VECTOR:
		if (v->count != t->count) { spec_fail(c, SPVM_SPEC_BAD_SHAPE, rid); return 0; }
		for (spvm_word i = 0; i < v->count; i++)
			if (!shape_matches(c, v->kids[i], t->comp, rid))
				return 0;
		return 1;
	case TK_ARRAY: {
		if (v->count != t->count) { spec_fail(c, SPVM_SPEC_BAD_SHAPE, rid); return 0; }
		for (spvm_word i = 0; i < v->count; i++)
			if (!shape_matches(c, v->kids[i], t->comp, rid))
				return 0;
		return 1;
	}
	case TK_STRUCT:
		if (v->count != t->count) { spec_fail(c, SPVM_SPEC_BAD_SHAPE, rid); return 0; }
		for (spvm_word i = 0; i < v->count; i++)
			if (!shape_matches(c, v->kids[i], t->members[i], rid))
				return 0;
		return 1;
	default:
		spec_fail(c, SPVM_SPEC_UNSUPPORTED_TYPE, rid);
		return 0;
	}
}

/* ------------------------------------------------------------------ */
/* Value construction                                                  */
/* ------------------------------------------------------------------ */

static spec_val* make_leaf(spec_ctx* c, uint32_t bits)
{
	spec_val* v = (spec_val*)spec_alloc(c, sizeof(spec_val));
	if (v == NULL) return NULL;
	v->count = 0;
	v->bits = bits;
	v->kids = NULL;
	return v;
}

static spec_val* make_node(spec_ctx* c, spvm_word count)
{
	spec_val* v = (spec_val*)spec_alloc(c, sizeof(spec_val));
	if (v == NULL) return NULL;
	v->count = count;
	v->bits = 0;
	if (count) {
		v->kids = (spec_val**)spec_alloc(c, (size_t)count * sizeof(spec_val*));
		if (v->kids == NULL) return NULL;
	}
	return v;
}

static spec_val* copy_val(spec_ctx* c, const spec_val* src)
{
	if (src->count == 0)
		return make_leaf(c, src->bits);
	spec_val* v = make_node(c, src->count);
	if (v == NULL) return NULL;
	for (spvm_word i = 0; i < src->count; i++) {
		v->kids[i] = copy_val(c, src->kids[i]);
		if (v->kids[i] == NULL) return NULL;
	}
	return v;
}

static spec_val* zero_value(spec_ctx* c, spvm_word tid, spvm_word rid)
{
	spec_type* t = &c->types[tid];
	switch (t->kind) {
	case TK_BOOL:
	case TK_INT:
	case TK_FLOAT:
		return make_leaf(c, 0u);
	case TK_VECTOR: {
		spec_val* v = make_node(c, t->count);
		if (v == NULL) return NULL;
		for (spvm_word i = 0; i < t->count; i++) {
			v->kids[i] = zero_value(c, t->comp, rid);
			if (v->kids[i] == NULL) return NULL;
		}
		return v;
	}
	case TK_ARRAY: {
		spec_val* v = make_node(c, t->count);
		if (v == NULL) return NULL;
		for (spvm_word i = 0; i < t->count; i++) {
			v->kids[i] = zero_value(c, t->comp, rid);
			if (v->kids[i] == NULL) return NULL;
		}
		return v;
	}
	case TK_STRUCT: {
		spec_val* v = make_node(c, t->count);
		if (v == NULL) return NULL;
		for (spvm_word i = 0; i < t->count; i++) {
			v->kids[i] = zero_value(c, t->members[i], rid);
			if (v->kids[i] == NULL) return NULL;
		}
		return v;
	}
	default:
		spec_fail(c, SPVM_SPEC_UNSUPPORTED_TYPE, rid);
		return NULL;
	}
}

/* ------------------------------------------------------------------ */
/* DFS evaluation                                                      */
/* ------------------------------------------------------------------ */

static int eval_id(spec_ctx* c, spvm_word id, spec_val** out);

static spvm_word operand(spec_ctx* c, const spec_const* sc, spvm_word i)
{
	return (spvm_word)c->src[sc->op_start + i];
}

static int resolve_array_count(spec_ctx* c, spvm_word array_type, spvm_word rid)
{
	spec_type* t = &c->types[array_type];
	spvm_word len_id = t->length_id;
	spec_val* lv;
	if (!eval_id(c, len_id, &lv))
		return 0;
	if (lv->count != 0) {
		spec_fail(c, SPVM_SPEC_BAD_SHAPE, rid);
		return 0;
	}
	int32_t len = (int32_t)lv->bits;
	if (len <= 0) {
		spec_fail(c, SPVM_SPEC_BAD_SHAPE, rid);
		return 0;
	}
	t->count = (spvm_word)len;
	return 1;
}

/* Component-wise leaf helpers: operands are scalars or vectors only. */
static int leaf_vector(spec_ctx* c, spec_val* v, spvm_word rid)
{
	if (v->count == 0)
		return 1;
	for (spvm_word i = 0; i < v->count; i++)
		if (v->kids[i]->count != 0) {
			spec_fail(c, SPVM_SPEC_BAD_SHAPE, rid);
			return 0;
		}
	return 1;
}

static uint32_t leaf_bits(const spec_val* v, spvm_word i)
{
	return (v->count == 0) ? v->bits : v->kids[i]->bits;
}

static spec_val* make_leaves(spec_ctx* c, spvm_word n, const uint32_t* bits, spvm_word rid)
{
	if (n == 1)
		return make_leaf(c, bits[0]);
	spec_val* v = make_node(c, n);
	if (v == NULL) return NULL;
	for (spvm_word i = 0; i < n; i++) {
		v->kids[i] = make_leaf(c, bits[i]);
		if (v->kids[i] == NULL) return NULL;
	}
	(void)rid;
	return v;
}

static int eval_spec_op(spec_ctx* c, spvm_word id, const spec_const* sc, spec_val** out)
{
	spvm_word wrapped = (spvm_word)c->src[sc->op_start - 1];
	spec_type* rt = &c->types[sc->type];

	switch (wrapped) {
	case SpvOpIAdd:
	case SpvOpISub:
	case SpvOpIMul:
	case SpvOpSNegate:
	case SpvOpUDiv:
	case SpvOpSDiv:
	case SpvOpIEqual:
	case SpvOpSLessThan:
	case SpvOpULessThan:
	case SpvOpLogicalAnd:
	case SpvOpLogicalOr:
	case SpvOpLogicalNot:
	case SpvOpSelect:
	case SpvOpCompositeExtract:
	case SpvOpCompositeInsert:
	case SpvOpVectorShuffle:
		break;
	default:
		spec_fail(c, SPVM_SPEC_UNSUPPORTED_OP, id);
		return 0;
	}

	/* Composite ops first: they navigate aggregate values. */
	if (wrapped == SpvOpCompositeExtract) {
		/* operands: <composite> <literal indices...> */
		spec_val* base;
		if (!eval_id(c, operand(c, sc, 0), &base))
			return 0;
		spec_val* cur = base;
		for (spvm_word i = 1; i < sc->op_count; i++) {
			spvm_word idx = operand(c, sc, i);
			if (cur->count == 0 || idx >= cur->count) {
				spec_fail(c, SPVM_SPEC_BAD_SHAPE, id);
				return 0;
			}
			cur = cur->kids[idx];
		}
		spec_val* res = copy_val(c, cur);
		if (res == NULL) return 0;
		if (!shape_matches(c, res, sc->type, id))
			return 0;
		*out = res;
		return 1;
	}

	if (wrapped == SpvOpCompositeInsert) {
		/* operands: <object> <composite> <literal indices...>; at least one
		 * literal index is required by SPIR-V. */
		spec_val* obj;
		spec_val* base;
		if (!eval_id(c, operand(c, sc, 0), &obj))
			return 0;
		if (!eval_id(c, operand(c, sc, 1), &base))
			return 0;
		if (sc->op_count < 3) {
			spec_fail(c, SPVM_SPEC_BAD_SHAPE, id);
			return 0;
		}
		/* validate the inserted object against the indexed member's type */
		{
			spvm_word idx[16];
			spvm_word depth = sc->op_count - 2;
			if (depth > 16) { spec_fail(c, SPVM_SPEC_BAD_SHAPE, id); return 0; }
			for (spvm_word k = 0; k < depth; k++)
				idx[k] = operand(c, sc, 2 + k);
			spvm_word slot_type = member_type_at(c, sc->type, idx, depth, id);
			if (slot_type == 0)
				return 0;
			if (!shape_matches(c, obj, slot_type, id))
				return 0;
		}
		spec_val* res = copy_val(c, base);
		if (res == NULL) return 0;
		spec_val* cur = res;
		for (spvm_word i = 2; i < sc->op_count; i++) {
			spvm_word idxv = operand(c, sc, i);
			if (cur->count == 0 || idxv >= cur->count) {
				spec_fail(c, SPVM_SPEC_BAD_SHAPE, id);
				return 0;
			}
			if (i + 1 == sc->op_count) {
				cur->kids[idxv] = copy_val(c, obj);
				if (cur->kids[idxv] == NULL) return 0;
			} else {
				cur = cur->kids[idxv];
			}
		}
		if (!shape_matches(c, res, sc->type, id))
			return 0;
		*out = res;
		return 1;
	}

	if (wrapped == SpvOpVectorShuffle) {
		/* operands: <vector1> <vector2> <selectors...> */
		spec_val* v1;
		spec_val* v2;
		if (!eval_id(c, operand(c, sc, 0), &v1))
			return 0;
		if (!eval_id(c, operand(c, sc, 1), &v2))
			return 0;
		if (!leaf_vector(c, v1, id) || !leaf_vector(c, v2, id))
			return 0;
		uint32_t c1 = (v1->count == 0) ? 1u : (uint32_t)v1->count;
		uint32_t c2 = (v2->count == 0) ? 1u : (uint32_t)v2->count;
		spvm_word sel = sc->op_count - 2;
		if (rt->kind != TK_VECTOR || rt->count != sel) {
			spec_fail(c, SPVM_SPEC_BAD_SHAPE, id);
			return 0;
		}
		uint32_t* bits = (uint32_t*)spec_alloc(c, (size_t)sel * sizeof(uint32_t));
		if (bits == NULL) return 0;
		for (spvm_word i = 0; i < sel; i++) {
			uint32_t s = (uint32_t)operand(c, sc, 2 + i);
			if (s == 0xffffffffu) {
				spec_fail(c, SPVM_SPEC_BAD_REFERENCE, id);
				return 0;
			}
			if (s < c1)
				bits[i] = leaf_bits(v1, s);
			else if (s < c1 + c2)
				bits[i] = leaf_bits(v2, s - c1);
			else {
				spec_fail(c, SPVM_SPEC_BAD_SHAPE, id);
				return 0;
			}
		}
		spec_val* res = make_leaves(c, sel, bits, id);
		if (res == NULL) return 0;
		if (!shape_matches(c, res, sc->type, id))
			return 0;
		*out = res;
		return 1;
	}

	if (wrapped == SpvOpSelect) {
		/* operands: <condition> <obj1> <obj2> */
		spec_val* cond;
		spec_val* a;
		spec_val* b;
		if (!eval_id(c, operand(c, sc, 0), &cond))
			return 0;
		if (!eval_id(c, operand(c, sc, 1), &a))
			return 0;
		if (!eval_id(c, operand(c, sc, 2), &b))
			return 0;
		if (cond->count == 0) {
			spec_val* res = copy_val(c, cond->bits ? a : b);
			if (res == NULL) return 0;
			if (!shape_matches(c, res, sc->type, id))
				return 0;
			*out = res;
			return 1;
		}
		if (!leaf_vector(c, cond, id) || !leaf_vector(c, a, id) || !leaf_vector(c, b, id))
			return 0;
		if (cond->count != a->count || a->count != b->count) {
			spec_fail(c, SPVM_SPEC_BAD_SHAPE, id);
			return 0;
		}
		uint32_t* bits = (uint32_t*)spec_alloc(c, (size_t)cond->count * sizeof(uint32_t));
		if (bits == NULL) return 0;
		for (spvm_word i = 0; i < cond->count; i++)
			bits[i] = cond->kids[i]->bits ? a->kids[i]->bits : b->kids[i]->bits;
		spec_val* res = make_leaves(c, cond->count, bits, id);
		if (res == NULL) return 0;
		if (!shape_matches(c, res, sc->type, id))
			return 0;
		*out = res;
		return 1;
	}

	/* Remaining ops are component-wise over scalar/vector leaves. */
	spvm_word nargs = 1;
	if (wrapped == SpvOpIAdd || wrapped == SpvOpISub || wrapped == SpvOpIMul ||
		wrapped == SpvOpUDiv || wrapped == SpvOpSDiv ||
		wrapped == SpvOpIEqual || wrapped == SpvOpSLessThan || wrapped == SpvOpULessThan ||
		wrapped == SpvOpLogicalAnd || wrapped == SpvOpLogicalOr)
		nargs = 2;

	spec_val* args[2];
	for (spvm_word i = 0; i < nargs; i++) {
		if (!eval_id(c, operand(c, sc, i), &args[i]))
			return 0;
		if (!leaf_vector(c, args[i], id))
			return 0;
	}

	spvm_word comps = 1;
	if (nargs == 2) {
		spvm_word c0 = (args[0]->count == 0) ? 1 : args[0]->count;
		spvm_word c1 = (args[1]->count == 0) ? 1 : args[1]->count;
		if (c0 != c1) {
			spec_fail(c, SPVM_SPEC_BAD_SHAPE, id);
			return 0;
		}
		comps = c0;
	} else {
		comps = (args[0]->count == 0) ? 1 : args[0]->count;
	}

	uint32_t* bits = (uint32_t*)spec_alloc(c, (size_t)comps * sizeof(uint32_t));
	if (bits == NULL) return 0;

	for (spvm_word i = 0; i < comps; i++) {
		uint32_t x = leaf_bits(args[0], i);
		uint32_t y = (nargs == 2) ? leaf_bits(args[1], i) : 0u;

		switch (wrapped) {
		case SpvOpIAdd:
			bits[i] = x + y; /* unsigned wrap, defined for uint32 */
			break;
		case SpvOpISub:
			bits[i] = x - y;
			break;
		case SpvOpIMul:
			bits[i] = x * y;
			break;
		case SpvOpSNegate:
			bits[i] = 0u - x;
			break;
		case SpvOpUDiv:
			if (y == 0u) { spec_fail(c, SPVM_SPEC_DIVISION_BY_ZERO, id); return 0; }
			bits[i] = x / y;
			break;
		case SpvOpSDiv: {
			int32_t sx = (int32_t)x, sy = (int32_t)y;
			if (sy == 0) { spec_fail(c, SPVM_SPEC_DIVISION_BY_ZERO, id); return 0; }
			if (sx == INT32_MIN && sy == -1) {
				spec_fail(c, SPVM_SPEC_INT_OVERFLOW, id);
				return 0;
			}
			bits[i] = (uint32_t)(sx / sy);
			break;
		}
		case SpvOpIEqual:
			bits[i] = (x == y) ? 1u : 0u;
			break;
		case SpvOpSLessThan:
			bits[i] = ((int32_t)x < (int32_t)y) ? 1u : 0u;
			break;
		case SpvOpULessThan:
			bits[i] = (x < y) ? 1u : 0u;
			break;
		case SpvOpLogicalAnd:
			bits[i] = ((x != 0u) && (y != 0u)) ? 1u : 0u;
			break;
		case SpvOpLogicalOr:
			bits[i] = ((x != 0u) || (y != 0u)) ? 1u : 0u;
			break;
		case SpvOpLogicalNot:
			bits[i] = (x == 0u) ? 1u : 0u;
			break;
		default:
			spec_fail(c, SPVM_SPEC_UNSUPPORTED_OP, id);
			return 0;
		}
	}

	spec_val* res = make_leaves(c, comps, bits, id);
	if (res == NULL) return 0;
	if (!shape_matches(c, res, sc->type, id))
		return 0;
	*out = res;
	return 1;
}

static int eval_id(spec_ctx* c, spvm_word id, spec_val** out)
{
	if (id == 0 || id >= c->bound) {
		spec_fail(c, SPVM_SPEC_BAD_REFERENCE, id ? id : 0);
		return 0;
	}
	if (c->visit[id] == 2) {
		*out = c->values[id];
		return 1;
	}
	if (c->visit[id] == 1) {
		spec_fail(c, SPVM_SPEC_CYCLE, id);
		return 0;
	}

	spec_const* sc = &c->consts[id];
	if (sc->opcode == 0) {
		/* not a constant definition: unknown reference */
		spec_fail(c, SPVM_SPEC_BAD_REFERENCE, id);
		return 0;
	}
	if (sc->opcode == SpvOpUndef) {
		spec_fail(c, SPVM_SPEC_BAD_REFERENCE, id);
		return 0;
	}

	c->visit[id] = 1;
	spec_val* v = NULL;

	switch (sc->opcode) {
	case SpvOpConstantTrue:
	case SpvOpConstantFalse:
	case SpvOpSpecConstantTrue:
	case SpvOpSpecConstantFalse:
		if (!type_supported(c, sc->type, id))
			return 0;
		if (c->types[sc->type].kind != TK_BOOL) {
			spec_fail(c, SPVM_SPEC_INVALID_VALUE, id);
			return 0;
		}
		if (c->has_override[id])
			v = make_leaf(c, c->override_bits[id] ? 1u : 0u);
		else
			v = make_leaf(c, (sc->opcode == SpvOpConstantTrue || sc->opcode == SpvOpSpecConstantTrue) ? 1u : 0u);
		break;

	case SpvOpConstant:
	case SpvOpSpecConstant:
		if (!type_supported(c, sc->type, id))
			return 0;
		{
			spec_type* t = &c->types[sc->type];
			if (t->kind != TK_INT && t->kind != TK_FLOAT) {
				spec_fail(c, SPVM_SPEC_INVALID_VALUE, id);
				return 0;
			}
			uint32_t bits;
			if (sc->opcode == SpvOpSpecConstant && c->has_override[id])
				bits = c->override_bits[id];
			else {
				if (sc->op_count < 1) { spec_fail(c, SPVM_SPEC_INVALID_MODULE, id); return 0; }
				bits = (uint32_t)c->src[sc->op_start];
			}
			v = make_leaf(c, bits);
		}
		break;

	case SpvOpConstantNull:
		if (!type_supported(c, sc->type, id))
			return 0;
		v = zero_value(c, sc->type, id);
		break;

	case SpvOpConstantComposite:
	case SpvOpSpecConstantComposite: {
		if (!type_supported(c, sc->type, id))
			return 0;
		spec_type* t = &c->types[sc->type];
		if (t->kind == TK_ARRAY && !resolve_array_count(c, sc->type, id))
			return 0;
		spvm_word expect = t->count;
		if (sc->op_count != expect) {
			spec_fail(c, SPVM_SPEC_BAD_SHAPE, id);
			return 0;
		}
		v = make_node(c, expect);
		if (v == NULL) return 0;
		for (spvm_word i = 0; i < expect; i++) {
			spec_val* child;
			if (!eval_id(c, operand(c, sc, i), &child))
				return 0;
			spvm_word child_type = 0;
			if (t->kind == TK_VECTOR) child_type = t->comp;
			else if (t->kind == TK_ARRAY) child_type = t->comp;
			else if (t->kind == TK_STRUCT) child_type = t->members[i];
			if (!shape_matches(c, child, child_type, id))
				return 0;
			spec_val* cp = copy_val(c, child);
			if (cp == NULL) return 0;
			v->kids[i] = cp;
		}
		break;
	}

	case SpvOpSpecConstantOp:
		if (!type_supported(c, sc->type, id))
			return 0;
		if (!eval_spec_op(c, id, sc, &v))
			return 0;
		break;

	default:
		spec_fail(c, SPVM_SPEC_BAD_REFERENCE, id);
		return 0;
	}

	if (v == NULL)
		return 0;
	if (!shape_matches(c, v, sc->type, id))
		return 0;

	c->visit[id] = 2;
	c->values[id] = v;
	*out = v;
	return 1;
}

/* ------------------------------------------------------------------ */
/* Entry table and binding validation                                  */
/* ------------------------------------------------------------------ */

static int validate_bindings_and_entries(spec_ctx* c, const spvm_spec_entry* entries, size_t entry_count)
{
	/* One spec id may bind to at most one definition. */
	for (size_t i = 0; i < c->pair_count; i++) {
		for (size_t j = 0; j < i; j++) {
			if (c->pairs[i].spec_id == c->pairs[j].spec_id) {
				spec_fail(c, SPVM_SPEC_AMBIGUOUS_ID, c->pairs[i].target);
				return 0;
			}
		}
	}

	/* The decorated target must be an overridable scalar leaf. */
	for (size_t i = 0; i < c->pair_count; i++) {
		spvm_word target = c->pairs[i].target;
		spec_const* sc = &c->consts[target];
		if (sc->opcode != SpvOpSpecConstant &&
			sc->opcode != SpvOpSpecConstantTrue &&
			sc->opcode != SpvOpSpecConstantFalse) {
			spec_fail(c, SPVM_SPEC_INVALID_VALUE, target);
			return 0;
		}
		spec_type* t = &c->types[sc->type];
		if (t->kind != TK_BOOL && t->kind != TK_INT && t->kind != TK_FLOAT) {
			spec_fail(c, SPVM_SPEC_INVALID_VALUE, target);
			return 0;
		}
		if ((t->kind == TK_INT || t->kind == TK_FLOAT) && t->bitcount != 32) {
			spec_fail(c, SPVM_SPEC_UNSUPPORTED_TYPE, target);
			return 0;
		}
	}

	/* Table self-consistency: no duplicate rows, no unknown spec ids. */
	for (size_t i = 0; i < entry_count; i++) {
		for (size_t j = 0; j < i; j++) {
			if (entries[i].spec_id == entries[j].spec_id) {
				spec_fail(c, SPVM_SPEC_DUPLICATE_ENTRY, 0);
				return 0;
			}
		}
		size_t k;
		for (k = 0; k < c->pair_count; k++)
			if (c->pairs[k].spec_id == entries[i].spec_id)
				break;
		if (k == c->pair_count) {
			spec_fail(c, SPVM_SPEC_UNKNOWN_ID, 0);
			return 0;
		}
		spvm_word target = c->pairs[k].target;
		if (c->types[c->consts[target].type].kind == TK_BOOL &&
			entries[i].bits != 0u && entries[i].bits != 1u) {
			spec_fail(c, SPVM_SPEC_INVALID_VALUE, target);
			return 0;
		}
		c->has_override[target] = 1;
		c->override_bits[target] = entries[i].bits;
	}

	return 1;
}

/* Force-evaluate every specialization definition, every spec-driven array
 * length, and every initialized variable, so the whole surface is validated
 * even with an empty entry table. Lengths are settled first because every
 * aggregate value and Private variable inherits its shape from them. */
static int evaluate_all(spec_ctx* c)
{
	for (spvm_word id = 1; id < c->bound; id++) {
		if (c->types[id].kind == TK_ARRAY) {
			if (!resolve_array_count(c, id, id))
				return 0;
			if (!type_supported(c, c->types[id].comp, id))
				return 0;
		}
	}

	for (spvm_word id = 1; id < c->bound; id++) {
		int op = c->consts[id].opcode;
		if (op == SpvOpSpecConstant || op == SpvOpSpecConstantTrue ||
			op == SpvOpSpecConstantFalse || op == SpvOpSpecConstantComposite ||
			op == SpvOpSpecConstantOp) {
			spec_val* v;
			if (!eval_id(c, id, &v))
				return 0;
		}
	}

	for (spvm_word id = 1; id < c->bound; id++) {
		if (c->var_ptr_type[id] == 0)
			continue;
		spvm_word init = c->var_init[id];
		if (init == (spvm_word)-1 || init == 0)
			continue;
		spec_val* v;
		if (!eval_id(c, init, &v))
			return 0;
		spvm_word pointee = c->types[c->var_ptr_type[id]].comp;
		if (!type_supported(c, pointee, id))
			return 0;
		if (!shape_matches(c, v, pointee, id))
			return 0;
	}

	return 1;
}

/* ------------------------------------------------------------------ */
/* Module rewriting                                                    */
/* ------------------------------------------------------------------ */

/* Global scope is only set up, never executed, so every specialization
 * definition is baked into ordinary OpConstant / OpConstantTrue / False /
 * OpConstantComposite instructions. Aggregates need constituent result ids,
 * so synthetic helper constants are emitted with fresh ids above the module's
 * original bound. */
typedef struct spec_writer {
	spvm_word* data;
	size_t len;
	size_t cap;
	spvm_word next_id;
} spec_writer;

static int writer_ensure(spec_ctx* c, spec_writer* w, size_t extra)
{
	if (w->len + extra <= w->cap)
		return 1;
	size_t cap = w->cap ? w->cap : 64;
	while (cap < w->len + extra)
		cap *= 2;
	spvm_word* grow = (spvm_word*)realloc(w->data, cap * sizeof(spvm_word));
	if (grow == NULL) {
		spec_fail(c, SPVM_SPEC_ALLOCATION, 0);
		return 0;
	}
	w->data = grow;
	w->cap = cap;
	return 1;
}

static int writer_word(spec_ctx* c, spec_writer* w, spvm_word word)
{
	if (!writer_ensure(c, w, 1))
		return 0;
	w->data[w->len++] = word;
	return 1;
}

/* Emit a constant reproducing value v with type tid and return its result id.
 * reuse_id != 0 installs the value under an existing result id (the original
 * specialization definition); otherwise a fresh synthetic id is allocated. */
static spvm_word materialize(spec_ctx* c, spec_writer* w, const spec_val* v, spvm_word tid, spvm_word reuse_id)
{
	spec_type* t = &c->types[tid];

	if (v->count == 0) {
		spvm_word id = reuse_id ? reuse_id : w->next_id++;
		if (t->kind == TK_BOOL) {
			int op = v->bits ? SpvOpConstantTrue : SpvOpConstantFalse;
			if (!writer_word(c, w, (spvm_word)((3u << SpvWordCountShift) | (unsigned int)op)) ||
				!writer_word(c, w, tid) || !writer_word(c, w, id))
				return 0;
		} else {
			if (!writer_word(c, w, (spvm_word)((4u << SpvWordCountShift) | (unsigned int)SpvOpConstant)) ||
				!writer_word(c, w, tid) || !writer_word(c, w, id) ||
				!writer_word(c, w, (spvm_word)v->bits))
				return 0;
		}
		return id;
	}

	/* Aggregate: materialize each child with its matching member type. */
	if ((uint32_t)v->count + 3u > 0xFFFFu) {
		/* no legal single SPIR-V instruction can hold this many constituents */
		spec_fail(c, SPVM_SPEC_BAD_SHAPE, reuse_id);
		return 0;
	}
	spvm_word* child_ids = (spvm_word*)spec_alloc(c, (size_t)v->count * sizeof(spvm_word));
	if (child_ids == NULL)
		return 0;

	for (spvm_word i = 0; i < v->count; i++) {
		spvm_word child_type = (t->kind == TK_STRUCT) ? t->members[i] : t->comp;
		child_ids[i] = materialize(c, w, v->kids[i], child_type, 0);
		if (child_ids[i] == 0)
			return 0;
	}

	spvm_word id = reuse_id ? reuse_id : w->next_id++;
	if (!writer_word(c, w, (spvm_word)(((3u + v->count) << SpvWordCountShift) | (unsigned int)SpvOpConstantComposite)) ||
		!writer_word(c, w, tid) || !writer_word(c, w, id))
		return 0;
	for (spvm_word i = 0; i < v->count; i++)
		if (!writer_word(c, w, child_ids[i]))
			return 0;
	return id;
}

static int is_spec_opcode(int opcode)
{
	return opcode == SpvOpSpecConstantTrue || opcode == SpvOpSpecConstantFalse ||
		opcode == SpvOpSpecConstant || opcode == SpvOpSpecConstantComposite ||
		opcode == SpvOpSpecConstantOp;
}

static spvm_word* rewrite_module(spec_ctx* c, size_t* out_count)
{
	spec_writer w;
	memset(&w, 0, sizeof(w));
	w.next_id = c->bound;

	/* Header, including bound, is copied; the bound is patched once the
	 * highest synthetic id is known. Result IDs of the original module are
	 * preserved throughout. */
	for (size_t i = 0; i < 5; i++)
		if (!writer_word(c, &w, c->src[i]))
			goto fail;

	const spvm_word* p = c->src + 5;
	const spvm_word* end = c->src + c->word_count;

	while (p < end) {
		unsigned int header = (unsigned int)(*p);
		int words = (int)((header >> SpvWordCountShift) & 0xffffu);
		int opcode = (int)(header & SpvOpCodeMask);
		const spvm_word* args = p + 1;
		int argc = words - 1;

		if (opcode == SpvOpDecorate && argc >= 2 && args[1] == SpvDecorationSpecId) {
			/* SpecId decorations have no meaning once values are baked. */
			p += words;
			continue;
		}

		if (is_spec_opcode(opcode)) {
			spvm_word id = args[1];
			spec_val* v = c->values[id]; /* already fully evaluated */
			if (materialize(c, &w, v, c->consts[id].type, id) == 0)
				goto fail;
			p += words;
			continue;
		}

		for (int i = 0; i < words; i++)
			if (!writer_word(c, &w, p[i]))
				goto fail;
		p += words;
	}

	/* Result ids occupy [1, next_id); bound must be strictly greater. */
	w.data[3] = w.next_id;
	*out_count = w.len;
	return w.data;

fail:
	free(w.data);
	return NULL;
}

/* ------------------------------------------------------------------ */
/* Public entry point                                                  */
/* ------------------------------------------------------------------ */

spvm_program_t spvm_program_create_specialized(spvm_context_t ctx, spvm_source words, size_t word_count,
	const spvm_spec_entry *entries, size_t entry_count, spvm_spec_error *error)
{
	spvm_program_t prog = NULL;
	spvm_word* owned = NULL;
	spec_ctx c;
	memset(&c, 0, sizeof(c));
	c.context = ctx;
	c.src = words;
	c.word_count = word_count;

	if (error) {
		error->code = SPVM_SPEC_OK;
		error->result_id = 0;
	}

	if (words == NULL || word_count < 5 ||
		(unsigned int)words[0] != SpvMagicNumber) {
		if (error) { error->code = SPVM_SPEC_INVALID_MODULE; error->result_id = 0; }
		return NULL;
	}

	c.bound = (spvm_word)words[3];
	if (c.bound == 0) {
		if (error) { error->code = SPVM_SPEC_INVALID_MODULE; error->result_id = 0; }
		return NULL;
	}

	c.types = (spec_type*)calloc(c.bound, sizeof(spec_type));
	c.consts = (spec_const*)calloc(c.bound, sizeof(spec_const));
	c.var_ptr_type = (spvm_word*)calloc(c.bound, sizeof(spvm_word));
	c.var_init = (spvm_word*)malloc(c.bound * sizeof(spvm_word));
	c.override_bits = (uint32_t*)calloc(c.bound, sizeof(uint32_t));
	c.has_override = (unsigned char*)calloc(c.bound, sizeof(unsigned char));
	c.values = (spec_val**)calloc(c.bound, sizeof(spec_val*));
	c.visit = (unsigned char*)calloc(c.bound, sizeof(unsigned char));

	if (!c.types || !c.consts || !c.var_ptr_type || c.var_init == NULL ||
		!c.override_bits || !c.has_override || !c.values || !c.visit) {
		if (error) { error->code = SPVM_SPEC_ALLOCATION; error->result_id = 0; }
		goto cleanup;
	}
	for (spvm_word i = 0; i < c.bound; i++)
		c.var_init[i] = (spvm_word)-1;

	if (!parse_module(&c))
		goto report;

	if (!validate_bindings_and_entries(&c, entries, entry_count))
		goto report;

	if (!evaluate_all(&c))
		goto report;

	{
		size_t out_count = 0;
		owned = rewrite_module(&c, &out_count);
		if (owned == NULL)
			goto report;

		prog = spvm_program_create(ctx, owned, out_count);
		if (prog == NULL) {
			free(owned);
			if (error) { error->code = SPVM_SPEC_INVALID_MODULE; error->result_id = 0; }
			goto cleanup;
		}
		prog->owned_code = owned;
		prog->owned_code_length = out_count;
	}

	goto cleanup;

report:
	if (error) {
		error->code = c.err_code;
		error->result_id = c.err_rid;
	}

cleanup:
	for (size_t i = 0; i < c.arena_count; i++)
		free(c.arena[i]);
	free(c.arena);
	free(c.pairs);
	free(c.types);
	free(c.consts);
	free(c.var_ptr_type);
	free(c.var_init);
	free(c.override_bits);
	free(c.has_override);
	free(c.values);
	free(c.visit);
	return prog;
}
