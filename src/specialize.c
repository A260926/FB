#include <spvm/specialize.h>
#include <spvm/result.h>
#include <spvm/state.h>
#include <spvm/value.h>
#include <spvm/spirv.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

/* Specialization engine.
 *
 * The module is parsed once into an instruction table. Type declarations are
 * recorded in a private spvm_result array so the existing typed-value
 * allocation helpers can build constant value trees of exactly the right
 * shape. Every specialization constant (plus the ordinary constants and
 * spec ops it depends on) is evaluated by an explicit dependency walk, then
 * the spec instructions are rewritten into ordinary constants over an owned
 * copy of the source. State creation afterwards goes through the unchanged
 * setup/executor, so an AccessChain, Load or helper argument sees exactly what
 * an equivalent ordinary-constant module would contain. */

typedef struct {
	SpvOp op;
	spvm_word word_count;   /* words following the opcode header word */
	spvm_source start;      /* points at the header word */
	spvm_word result_id;    /* 0 when the instruction has no result <id> */
} spec_instr;

typedef struct {
	spvm_word spec_id;
	spvm_word result_id;
} spec_def;

typedef struct {
	spvm_context_t ctx;
	spvm_source words;
	size_t word_count;
	spvm_word bound;

	spec_instr* instrs;
	size_t instr_count;
	spvm_word* result_instr; /* result <id> -> instruction index, -1 if none */

	spvm_result* results;    /* scratch slots: types, decorations and constant values */
	unsigned char* eval_state; /* 0 unseen, 1 visiting, 2 done */

	spec_def* defs;
	size_t def_count;

	const spvm_spec_entry* entries;
	size_t entry_count;

	spvm_spec_error err;
} spec_engine;

static void spec_fail(spec_engine* eng, int code, spvm_word result_id)
{
	if (eng->err.code == SPVM_SPEC_OK) {
		eng->err.code = code;
		eng->err.result_id = result_id;
	}
}

/* Tear down the scratch result array. OpTypeArray lengths are kept in params,
 * which spvm_result_delete does not know about, so free those first. */
static void spec_cleanup_results(spec_engine* eng)
{
	for (spvm_word i = 0; i < eng->bound; i++) {
		spvm_result* r = &eng->results[i];
		if (r->type == spvm_result_type_type && r->value_type == spvm_value_type_array && r->params != NULL)
			free(r->params);
		spvm_result_delete(r);
	}
}

/* ---- member tree helpers ------------------------------------------------ */

static int spec_is_leaf(const spvm_member* m)
{
	return m->member_count == 0;
}

/* Recursive shape comparison: two trees match when every level has the same
 * child count down to the leaves. */
static int spec_shape_equal(const spvm_member* a, const spvm_member* b)
{
	if (spec_is_leaf(a) || spec_is_leaf(b))
		return spec_is_leaf(a) && spec_is_leaf(b);
	if (a->member_count != b->member_count)
		return 0;
	for (spvm_word i = 0; i < a->member_count; i++)
		if (!spec_shape_equal(&a->members[i], &b->members[i]))
			return 0;
	return 1;
}

static void spec_copy_leaves(spvm_member* dst, const spvm_member* src)
{
	if (spec_is_leaf(dst)) {
		dst->value.u64 = src->value.u64;
		return;
	}
	for (spvm_word i = 0; i < dst->member_count; i++)
		spec_copy_leaves(&dst->members[i], &src->members[i]);
}

/* checked version of spvm_member_memcpy that reports instead of asserting */
static int spec_checked_copy(spvm_member* dst, const spvm_member* src)
{
	if (!spec_shape_equal(dst, src))
		return 0;
	spec_copy_leaves(dst, src);
	return 1;
}

/* Copy every top-level member of one result into another (the member-array
 * base is itself a leaf for vectors, so it cannot be copied as a single tree). */
static int spec_copy_result_members(spvm_result* dst, const spvm_result* src)
{
	if (dst->member_count != src->member_count)
		return 0;
	for (spvm_word i = 0; i < dst->member_count; i++)
		if (!spec_checked_copy(&dst->members[i], &src->members[i]))
			return 0;
	return 1;
}

/* Copy one composite constituent into slot i of a composite value. Mirrors
 * spvm_setup_OpConstantComposite: a scalar constituent contributes its single
 * leaf, an aggregate constituent fills the matching subtree. */
static int spec_copy_constituent(spvm_member* dst, const spvm_result* src)
{
	if (spec_is_leaf(dst)) {
		if (src->member_count != 1 || !spec_is_leaf(&src->members[0]))
			return 0;
		dst->value.u64 = src->members[0].value.u64;
		return 1;
	}
	if (src->member_count != dst->member_count)
		return 0;
	for (spvm_word i = 0; i < dst->member_count; i++)
		if (!spec_checked_copy(&dst->members[i], &src->members[i]))
			return 0;
	return 1;
}

static void spec_set_leaves(spvm_member* m, spvm_word count, uint32_t bits)
{
	for (spvm_word i = 0; i < count; i++) {
		if (spec_is_leaf(&m[i]))
			m[i].value.u = (spvm_word)bits;
		else
			spec_set_leaves(m[i].members, m[i].member_count, bits);
	}
}

/* ---- type handling ------------------------------------------------------ */

static spvm_result* spec_type_info(spec_engine* eng, spvm_word type_id)
{
	if (type_id == 0 || type_id >= eng->bound)
		return NULL;
	return spvm_state_get_type_info(eng->results, &eng->results[type_id]);
}

/* Only bool, 32-bit ints, float32 and vectors / fixed arrays / nested structs
 * built from those are legal specialization values. Matrices, 64-bit types
 * and the opaque/pointer kinds are rejected explicitly. */
static int spec_type_supported(spec_engine* eng, spvm_word type_id)
{
	spvm_result* t = spec_type_info(eng, type_id);
	if (t == NULL)
		return 0;

	switch (t->value_type) {
	case spvm_value_type_bool:
		return 1;
	case spvm_value_type_int:
		return t->value_bitcount == 32;
	case spvm_value_type_float:
		return t->value_bitcount == 32;
	case spvm_value_type_vector:
		return spec_type_supported(eng, t->pointer);
	case spvm_value_type_array:
		if (t->member_count == 0)
			return 0;
		return spec_type_supported(eng, t->pointer);
	case spvm_value_type_struct:
		for (spvm_word i = 0; i < t->member_count; i++)
			if (!spec_type_supported(eng, t->params[i]))
				return 0;
		return 1;
	default:
		return 0;
	}
}

static int spec_is_int_type(const spvm_result* t)
{
	return t->value_type == spvm_value_type_int && t->value_bitcount == 32;
}
static int spec_is_float_type(const spvm_result* t)
{
	return t->value_type == spvm_value_type_float && t->value_bitcount == 32;
}
static int spec_is_bool_type(const spvm_result* t)
{
	return t->value_type == spvm_value_type_bool;
}

/* Resolve a type to its scalar element type (unwraps vectors). */
static spvm_result* spec_leaf_type(spec_engine* eng, spvm_word type_id)
{
	spvm_result* t = spec_type_info(eng, type_id);
	while (t != NULL && t->value_type == spvm_value_type_vector)
		t = spec_type_info(eng, t->pointer);
	return t;
}

/* Record a type declaration into the scratch result array. Mirrors the type
 * cases of opcode_setup.c without needing a fully built state. */
static void spec_record_type(spec_engine* eng, const spec_instr* in)
{
	spvm_source p = in->start + 1; /* skip header */
	spvm_word id = SPVM_READ_WORD(p);
	spvm_result* r = &eng->results[id];
	r->type = spvm_result_type_type;

	switch (in->op) {
	case SpvOpTypeVoid:
		r->value_type = spvm_value_type_void;
		break;
	case SpvOpTypeBool:
		r->value_type = spvm_value_type_bool;
		r->value_bitcount = 1;
		r->member_count = 1;
		break;
	case SpvOpTypeInt:
		r->value_type = spvm_value_type_int;
		r->value_bitcount = SPVM_READ_WORD(p);
		r->value_sign = SPVM_READ_WORD(p);
		r->member_count = 1;
		break;
	case SpvOpTypeFloat:
		r->value_type = spvm_value_type_float;
		r->value_bitcount = SPVM_READ_WORD(p);
		r->member_count = 1;
		break;
	case SpvOpTypeVector:
	case SpvOpTypeMatrix:
		r->value_type = (in->op == SpvOpTypeVector) ? spvm_value_type_vector : spvm_value_type_matrix;
		r->pointer = SPVM_READ_WORD(p);
		r->member_count = SPVM_READ_WORD(p);
		break;
	case SpvOpTypeImage:
		r->value_type = spvm_value_type_image;
		r->pointer = SPVM_READ_WORD(p);
		r->member_count = 1;
		break;
	case SpvOpTypeSampler:
		r->value_type = spvm_value_type_sampler;
		r->member_count = 1;
		break;
	case SpvOpTypeSampledImage:
		r->value_type = spvm_value_type_sampled_image;
		r->pointer = SPVM_READ_WORD(p);
		r->member_count = 2;
		break;
	case SpvOpTypeArray: {
		r->value_type = spvm_value_type_array;
		r->pointer = SPVM_READ_WORD(p);
		spvm_word length_id = SPVM_READ_WORD(p);
		/* length is resolved after constant evaluation; remember it in params */
		r->params = (spvm_word*)malloc(sizeof(spvm_word));
		r->params[0] = length_id;
		break;
	}
	case SpvOpTypeRuntimeArray:
		r->value_type = spvm_value_type_runtime_array;
		r->pointer = SPVM_READ_WORD(p);
		break;
	case SpvOpTypeStruct: {
		r->value_type = spvm_value_type_struct;
		spvm_word mcnt = in->word_count - 1;
		r->member_count = mcnt;
		r->params = (spvm_word*)malloc(mcnt * sizeof(spvm_word));
		for (spvm_word i = 0; i < mcnt; i++)
			r->params[i] = SPVM_READ_WORD(p);
		break;
	}
	case SpvOpTypePointer:
		r->value_type = spvm_value_type_pointer;
		r->storage_class = (SpvStorageClass)SPVM_READ_WORD(p);
		r->pointer = SPVM_READ_WORD(p);
		break;
	case SpvOpTypeFunction: {
		spvm_word ret = SPVM_READ_WORD(p);
		spvm_word param_count = in->word_count - 2;
		r->type = spvm_result_type_function_type;
		r->pointer = ret;
		r->member_count = param_count;
		r->params = (spvm_word*)malloc((param_count ? param_count : 1) * sizeof(spvm_word));
		for (spvm_word i = 0; i < param_count; i++)
			r->params[i] = SPVM_READ_WORD(p);
		break;
	}
	default:
		break;
	}
}

/* ---- forward declaration ------------------------------------------------ */

static int spec_eval(spec_engine* eng, spvm_word id);

/* ---- OpSpecConstantOp implementation ------------------------------------ */

static int spec_vector_component_count(spec_engine* eng, spvm_word type_id, spvm_word* count, spvm_result** leaf_type)
{
	spvm_result* t = spec_type_info(eng, type_id);
	if (t == NULL)
		return 0;
	if (t->value_type == spvm_value_type_vector) {
		*count = t->member_count;
		*leaf_type = spec_type_info(eng, t->pointer);
	} else {
		*count = 1;
		*leaf_type = t;
	}
	return *leaf_type != NULL;
}

static int spec_eval_spec_op(spec_engine* eng, const spec_instr* in)
{
	spvm_word result_type = in->start[1];
	spvm_word id = in->start[2];
	SpvOp wrapped = (SpvOp)in->start[3];

	spvm_result* dst = &eng->results[id];
	spvm_result_allocate_typed_value(dst, eng->results, result_type);

	spvm_word dst_count;
	spvm_result* dst_leaf;
	if (!spec_vector_component_count(eng, result_type, &dst_count, &dst_leaf)) {
		spec_fail(eng, SPVM_SPEC_ERR_UNSUPPORTED_TYPE, id);
		return eng->err.code;
	}

	switch (wrapped) {
	/* integer scalar/vector arithmetic, 32-bit wrap-around */
	case SpvOpIAdd:
	case SpvOpISub:
	case SpvOpIMul:
	case SpvOpUDiv:
	case SpvOpSDiv: {
		if (in->word_count != 5) { spec_fail(eng, SPVM_SPEC_ERR_SHAPE_MISMATCH, id); return eng->err.code; }
		spvm_word a_id = in->start[4], b_id = in->start[5];
		if (spec_eval(eng, a_id) || spec_eval(eng, b_id)) return eng->err.code;
		spvm_result *ra = &eng->results[a_id], *rb = &eng->results[b_id];
		if (!spec_shape_equal(dst->members, ra->members) || !spec_shape_equal(dst->members, rb->members)) {
			spec_fail(eng, SPVM_SPEC_ERR_SHAPE_MISMATCH, id); return eng->err.code;
		}
		spvm_result* la = spec_leaf_type(eng, ra->pointer);
		if (la == NULL || !spec_is_int_type(la) || !spec_is_int_type(dst_leaf)) {
			spec_fail(eng, SPVM_SPEC_ERR_UNSUPPORTED_TYPE, id); return eng->err.code;
		}
		for (spvm_word i = 0; i < dst_count; i++) {
			uint32_t a = (uint32_t)ra->members[i].value.u;
			uint32_t b = (uint32_t)rb->members[i].value.u;
			uint32_t r = 0;
			if (wrapped == SpvOpIAdd) r = a + b;
			else if (wrapped == SpvOpISub) r = a - b;
			else if (wrapped == SpvOpIMul) r = a * b;
			else if (wrapped == SpvOpUDiv) {
				if (b == 0) { spec_fail(eng, SPVM_SPEC_ERR_DIVIDE_BY_ZERO, id); return eng->err.code; }
				r = a / b;
			} else { /* SpvOpSDiv */
				int32_t sa = (int32_t)a, sb = (int32_t)b;
				if (sb == 0) { spec_fail(eng, SPVM_SPEC_ERR_DIVIDE_BY_ZERO, id); return eng->err.code; }
				if (sa == INT32_MIN && sb == -1) {
					spec_fail(eng, SPVM_SPEC_ERR_SIGNED_OVERFLOW, id); return eng->err.code;
				}
				r = (uint32_t)(sa / sb);
			}
			dst->members[i].value.u = (spvm_word)r;
		}
		return 0;
	}
	case SpvOpSNegate: {
		if (in->word_count != 4) { spec_fail(eng, SPVM_SPEC_ERR_SHAPE_MISMATCH, id); return eng->err.code; }
		spvm_word a_id = in->start[4];
		if (spec_eval(eng, a_id)) return eng->err.code;
		spvm_result* ra = &eng->results[a_id];
		if (!spec_shape_equal(dst->members, ra->members) ||
			!spec_is_int_type(dst_leaf) || !spec_is_int_type(spec_leaf_type(eng, ra->pointer))) {
			spec_fail(eng, SPVM_SPEC_ERR_SHAPE_MISMATCH, id); return eng->err.code;
		}
		for (spvm_word i = 0; i < dst_count; i++)
			dst->members[i].value.u = (spvm_word)(0u - (uint32_t)ra->members[i].value.u);
		return 0;
	}
	/* integer comparisons, component-wise bool results */
	case SpvOpIEqual:
	case SpvOpSLessThan:
	case SpvOpULessThan: {
		if (in->word_count != 5) { spec_fail(eng, SPVM_SPEC_ERR_SHAPE_MISMATCH, id); return eng->err.code; }
		spvm_word a_id = in->start[4], b_id = in->start[5];
		if (spec_eval(eng, a_id) || spec_eval(eng, b_id)) return eng->err.code;
		spvm_result *ra = &eng->results[a_id], *rb = &eng->results[b_id];
		if (!spec_shape_equal(dst->members, ra->members) || !spec_shape_equal(dst->members, rb->members) ||
			!spec_is_bool_type(dst_leaf) || !spec_is_int_type(spec_leaf_type(eng, ra->pointer))) {
			spec_fail(eng, SPVM_SPEC_ERR_SHAPE_MISMATCH, id); return eng->err.code;
		}
		for (spvm_word i = 0; i < dst_count; i++) {
			uint32_t a = (uint32_t)ra->members[i].value.u;
			uint32_t b = (uint32_t)rb->members[i].value.u;
			int r = 0;
			if (wrapped == SpvOpIEqual) r = (a == b);
			else if (wrapped == SpvOpULessThan) r = (a < b);
			else r = ((int32_t)a < (int32_t)b);
			dst->members[i].value.b = (char)r;
		}
		return 0;
	}
	case SpvOpLogicalAnd:
	case SpvOpLogicalOr: {
		if (in->word_count != 5) { spec_fail(eng, SPVM_SPEC_ERR_SHAPE_MISMATCH, id); return eng->err.code; }
		spvm_word a_id = in->start[4], b_id = in->start[5];
		if (spec_eval(eng, a_id) || spec_eval(eng, b_id)) return eng->err.code;
		spvm_result *ra = &eng->results[a_id], *rb = &eng->results[b_id];
		if (!spec_shape_equal(dst->members, ra->members) || !spec_shape_equal(dst->members, rb->members) ||
			!spec_is_bool_type(dst_leaf) || !spec_is_bool_type(spec_leaf_type(eng, ra->pointer))) {
			spec_fail(eng, SPVM_SPEC_ERR_SHAPE_MISMATCH, id); return eng->err.code;
		}
		for (spvm_word i = 0; i < dst_count; i++) {
			int a = ra->members[i].value.b != 0;
			int b = rb->members[i].value.b != 0;
			dst->members[i].value.b = (char)(wrapped == SpvOpLogicalAnd ? (a && b) : (a || b));
		}
		return 0;
	}
	case SpvOpLogicalNot: {
		if (in->word_count != 4) { spec_fail(eng, SPVM_SPEC_ERR_SHAPE_MISMATCH, id); return eng->err.code; }
		spvm_word a_id = in->start[4];
		if (spec_eval(eng, a_id)) return eng->err.code;
		spvm_result* ra = &eng->results[a_id];
		if (!spec_shape_equal(dst->members, ra->members) ||
			!spec_is_bool_type(dst_leaf) || !spec_is_bool_type(spec_leaf_type(eng, ra->pointer))) {
			spec_fail(eng, SPVM_SPEC_ERR_SHAPE_MISMATCH, id); return eng->err.code;
		}
		for (spvm_word i = 0; i < dst_count; i++)
			dst->members[i].value.b = (char)(ra->members[i].value.b == 0);
		return 0;
	}
	case SpvOpSelect: {
		if (in->word_count != 6) { spec_fail(eng, SPVM_SPEC_ERR_SHAPE_MISMATCH, id); return eng->err.code; }
		spvm_word cond_id = in->start[4], x_id = in->start[5], y_id = in->start[6];
		if (spec_eval(eng, cond_id) || spec_eval(eng, x_id) || spec_eval(eng, y_id)) return eng->err.code;
		spvm_result *rc = &eng->results[cond_id], *rx = &eng->results[x_id], *ry = &eng->results[y_id];
		if (!spec_shape_equal(dst->members, rx->members) || !spec_shape_equal(dst->members, ry->members)) {
			spec_fail(eng, SPVM_SPEC_ERR_SHAPE_MISMATCH, id); return eng->err.code;
		}
		spvm_result* ctype = spec_leaf_type(eng, rc->pointer);
		if (ctype == NULL || !spec_is_bool_type(ctype)) {
			spec_fail(eng, SPVM_SPEC_ERR_UNSUPPORTED_TYPE, id); return eng->err.code;
		}
		if (rc->member_count == 1) {
			int pick = rc->members[0].value.b != 0;
			if (!spec_copy_result_members(dst, pick ? rx : ry)) {
				spec_fail(eng, SPVM_SPEC_ERR_SHAPE_MISMATCH, id); return eng->err.code;
			}
		} else {
			if (rc->member_count != dst_count) {
				spec_fail(eng, SPVM_SPEC_ERR_SHAPE_MISMATCH, id); return eng->err.code;
			}
			for (spvm_word i = 0; i < dst_count; i++)
				dst->members[i].value.u64 = rc->members[i].value.b ? rx->members[i].value.u64 : ry->members[i].value.u64;
		}
		return 0;
	}
	case SpvOpCompositeExtract: {
		if (in->word_count < 4) { spec_fail(eng, SPVM_SPEC_ERR_SHAPE_MISMATCH, id); return eng->err.code; }
		spvm_word obj_id = in->start[4];
		if (spec_eval(eng, obj_id)) return eng->err.code;
		spvm_result* ro = &eng->results[obj_id];
		size_t idx_count = (size_t)in->word_count - 4;
		const spvm_word* indices = in->start + 5;

		/* the first literal indexes the object's own member array, then each
		 * further literal descends one level (matches OpCompositeExtract) */
		if (idx_count == 0 || indices[0] >= ro->member_count) {
			spec_fail(eng, SPVM_SPEC_ERR_INDEX_OUT_OF_RANGE, id); return eng->err.code;
		}
		const spvm_member* cur = &ro->members[indices[0]];
		for (size_t i = 1; i < idx_count; i++) {
			if (indices[i] >= cur->member_count) {
				spec_fail(eng, SPVM_SPEC_ERR_INDEX_OUT_OF_RANGE, id); return eng->err.code;
			}
			cur = &cur->members[indices[i]];
		}
		/* cur is now the selected member (leaf or subtree) inside the object */
		if (spec_is_leaf(cur)) {
			if (dst->member_count != 1 || !spec_is_leaf(&dst->members[0])) {
				spec_fail(eng, SPVM_SPEC_ERR_SHAPE_MISMATCH, id); return eng->err.code;
			}
			dst->members[0].value.u64 = cur->value.u64;
		} else {
			if (dst->member_count != cur->member_count) {
				spec_fail(eng, SPVM_SPEC_ERR_SHAPE_MISMATCH, id); return eng->err.code;
			}
			for (spvm_word k = 0; k < dst->member_count; k++)
				if (!spec_checked_copy(&dst->members[k], &cur->members[k])) {
					spec_fail(eng, SPVM_SPEC_ERR_SHAPE_MISMATCH, id); return eng->err.code;
				}
		}
		return 0;
	}
	case SpvOpCompositeInsert: {
		if (in->word_count < 5) { spec_fail(eng, SPVM_SPEC_ERR_SHAPE_MISMATCH, id); return eng->err.code; }
		spvm_word obj_id = in->start[4];
		spvm_word comp_id = in->start[5];
		if (spec_eval(eng, obj_id) || spec_eval(eng, comp_id)) return eng->err.code;
		spvm_result *ro = &eng->results[obj_id], *rc = &eng->results[comp_id];
		size_t idx_count = (size_t)in->word_count - 5;
		const spvm_word* indices = in->start + 6;

		if (!spec_copy_result_members(dst, rc)) {
			spec_fail(eng, SPVM_SPEC_ERR_SHAPE_MISMATCH, id); return eng->err.code;
		}
		/* first literal indexes the composite's own member array, later ones descend */
		if (idx_count == 0 || indices[0] >= dst->member_count) {
			spec_fail(eng, SPVM_SPEC_ERR_INDEX_OUT_OF_RANGE, id); return eng->err.code;
		}
		spvm_member* target = &dst->members[indices[0]];
		for (size_t i = 1; i < idx_count; i++) {
			if (indices[i] >= target->member_count) {
				spec_fail(eng, SPVM_SPEC_ERR_INDEX_OUT_OF_RANGE, id); return eng->err.code;
			}
			target = &target->members[indices[i]];
		}
		if (!spec_checked_copy(target, ro->members)) {
			spec_fail(eng, SPVM_SPEC_ERR_SHAPE_MISMATCH, id); return eng->err.code;
		}
		return 0;
	}
	case SpvOpVectorShuffle: {
		if (in->word_count < 5) { spec_fail(eng, SPVM_SPEC_ERR_SHAPE_MISMATCH, id); return eng->err.code; }
		spvm_word v1_id = in->start[4], v2_id = in->start[5];
		if (spec_eval(eng, v1_id) || spec_eval(eng, v2_id)) return eng->err.code;
		spvm_result *v1 = &eng->results[v1_id], *v2 = &eng->results[v2_id];
		spvm_result* t1 = spec_type_info(eng, v1->pointer);
		spvm_result* t2 = spec_type_info(eng, v2->pointer);
		if (t1 == NULL || t2 == NULL ||
			t1->value_type != spvm_value_type_vector || t2->value_type != spvm_value_type_vector ||
			spec_type_info(eng, result_type)->value_type != spvm_value_type_vector) {
			spec_fail(eng, SPVM_SPEC_ERR_SHAPE_MISMATCH, id); return eng->err.code;
		}
		size_t selector_count = (size_t)in->word_count - 5;
		if (selector_count != (size_t)dst_count) {
			spec_fail(eng, SPVM_SPEC_ERR_SHAPE_MISMATCH, id); return eng->err.code;
		}
		uint32_t total = (uint32_t)v1->member_count + (uint32_t)v2->member_count;
		for (spvm_word i = 0; i < dst_count; i++) {
			uint32_t sel = (uint32_t)in->start[6 + i];
			if (sel >= total) { /* 0xFFFFFFFF (undefined) and any out-of-range selector */
				spec_fail(eng, SPVM_SPEC_ERR_INDEX_OUT_OF_RANGE, id); return eng->err.code;
			}
			if (sel < (uint32_t)v1->member_count)
				dst->members[i].value.u64 = v1->members[sel].value.u64;
			else
				dst->members[i].value.u64 = v2->members[sel - v1->member_count].value.u64;
		}
		return 0;
	}
	default:
		spec_fail(eng, SPVM_SPEC_ERR_UNSUPPORTED_OP, id);
		return eng->err.code;
	}
}

/* ---- constant DAG evaluation -------------------------------------------- */

static int spec_is_constant_op(SpvOp op)
{
	switch (op) {
	case SpvOpSpecConstantTrue:
	case SpvOpSpecConstantFalse:
	case SpvOpSpecConstant:
	case SpvOpSpecConstantComposite:
	case SpvOpSpecConstantOp:
	case SpvOpConstantTrue:
	case SpvOpConstantFalse:
	case SpvOpConstant:
	case SpvOpConstantNull:
	case SpvOpConstantComposite:
		return 1;
	default:
		return 0;
	}
}

/* Locate the override entry for a definition, if any. */
static const spvm_spec_entry* spec_find_override(spec_engine* eng, spvm_word result_id)
{
	for (size_t i = 0; i < eng->def_count; i++) {
		if (eng->defs[i].result_id != result_id)
			continue;
		spvm_word spec_id = eng->defs[i].spec_id;
		for (size_t j = 0; j < eng->entry_count; j++)
			if (eng->entries[j].spec_id == spec_id)
				return &eng->entries[j];
	}
	return NULL;
}

static int spec_eval(spec_engine* eng, spvm_word id)
{
	if (id == 0 || id >= eng->bound) {
		spec_fail(eng, SPVM_SPEC_ERR_UNKNOWN_REF, id);
		return eng->err.code;
	}
	if (eng->eval_state[id] == 2)
		return 0;
	if (eng->eval_state[id] == 1) {
		spec_fail(eng, SPVM_SPEC_ERR_CYCLE, id);
		return eng->err.code;
	}

	spvm_word in_index = eng->result_instr[id];
	if (in_index == (spvm_word)-1) {
		spec_fail(eng, SPVM_SPEC_ERR_UNKNOWN_REF, id);
		return eng->err.code;
	}
	const spec_instr* in = &eng->instrs[in_index];
	if (!spec_is_constant_op(in->op)) {
		spec_fail(eng, SPVM_SPEC_ERR_UNKNOWN_REF, id);
		return eng->err.code;
	}

	eng->eval_state[id] = 1;

	spvm_word type_id = in->start[1];
	spvm_result* slot = &eng->results[id];
	slot->type = spvm_result_type_constant;

	switch (in->op) {
	case SpvOpSpecConstantTrue:
	case SpvOpSpecConstantFalse:
	case SpvOpConstantTrue:
	case SpvOpConstantFalse: {
		if (!spec_type_supported(eng, type_id) ||
			!spec_is_bool_type(spec_type_info(eng, type_id))) {
			spec_fail(eng, SPVM_SPEC_ERR_UNSUPPORTED_TYPE, id); return eng->err.code;
		}
		spvm_result_allocate_typed_value(slot, eng->results, type_id);
		uint32_t bits = (in->op == SpvOpSpecConstantTrue || in->op == SpvOpConstantTrue) ? 1u : 0u;
		const spvm_spec_entry* e = spec_find_override(eng, id);
		if (e != NULL) {
			if (e->bits != 0 && e->bits != 1) {
				spec_fail(eng, SPVM_SPEC_ERR_INVALID_BOOL, id); return eng->err.code;
			}
			bits = e->bits;
		}
		spec_set_leaves(slot->members, slot->member_count, bits);
		break;
	}
	case SpvOpSpecConstant:
	case SpvOpConstant: {
		if (!spec_type_supported(eng, type_id)) {
			spec_fail(eng, SPVM_SPEC_ERR_UNSUPPORTED_TYPE, id); return eng->err.code;
		}
		spvm_result* t = spec_type_info(eng, type_id);
		if (!spec_is_int_type(t) && !spec_is_float_type(t)) {
			spec_fail(eng, SPVM_SPEC_ERR_UNSUPPORTED_TYPE, id); return eng->err.code;
		}
		if (in->word_count != 3) { /* 64-bit (two literal words) and friends */
			spec_fail(eng, SPVM_SPEC_ERR_UNSUPPORTED_TYPE, id); return eng->err.code;
		}
		spvm_result_allocate_typed_value(slot, eng->results, type_id);
		uint32_t bits = (uint32_t)in->start[3];
		if (in->op == SpvOpSpecConstant) {
			const spvm_spec_entry* e = spec_find_override(eng, id);
			if (e != NULL)
				bits = e->bits;
		}
		spec_set_leaves(slot->members, slot->member_count, bits);
		break;
	}
	case SpvOpConstantNull: {
		if (!spec_type_supported(eng, type_id)) {
			spec_fail(eng, SPVM_SPEC_ERR_UNSUPPORTED_TYPE, id); return eng->err.code;
		}
		spvm_result_allocate_typed_value(slot, eng->results, type_id);
		/* calloc leaves already hold zero; float -0.0 cannot come from null */
		break;
	}
	case SpvOpSpecConstantComposite:
	case SpvOpConstantComposite: {
		if (!spec_type_supported(eng, type_id)) {
			spec_fail(eng, SPVM_SPEC_ERR_UNSUPPORTED_TYPE, id); return eng->err.code;
		}
		spvm_result_allocate_typed_value(slot, eng->results, type_id);
		spvm_word constituent_count = in->word_count - 2; /* result type + result id precede operands */
		spvm_result* ct = spec_type_info(eng, type_id);
		if (ct->value_type == spvm_value_type_array) {
			/* specialization can grow an array past the source-listed constituents;
			 * the remaining elements stay at their zero-initialized values */
			if (constituent_count > slot->member_count) {
				spec_fail(eng, SPVM_SPEC_ERR_SHAPE_MISMATCH, id); return eng->err.code;
			}
		} else if (constituent_count != slot->member_count) {
			spec_fail(eng, SPVM_SPEC_ERR_SHAPE_MISMATCH, id); return eng->err.code;
		}
		for (spvm_word i = 0; i < constituent_count; i++) {
			spvm_word child = in->start[3 + i];
			if (spec_eval(eng, child)) return eng->err.code;
			if (!spec_copy_constituent(&slot->members[i], &eng->results[child])) {
				spec_fail(eng, SPVM_SPEC_ERR_SHAPE_MISMATCH, id); return eng->err.code;
			}
		}
		break;
	}
	case SpvOpSpecConstantOp:
		if (!spec_type_supported(eng, type_id)) {
			spec_fail(eng, SPVM_SPEC_ERR_UNSUPPORTED_TYPE, id); return eng->err.code;
		}
		if (spec_eval_spec_op(eng, in)) return eng->err.code;
		break;
	default:
		spec_fail(eng, SPVM_SPEC_ERR_UNKNOWN_REF, id);
		return eng->err.code;
	}

	eng->eval_state[id] = 2;
	return 0;
}

/* ---- module rewriting --------------------------------------------------- */

static int spec_emit(spvm_word** buf, size_t* used, size_t* cap, spvm_word word);

/* Emit an already-evaluated value as literal constant instructions that the
 * existing global-scope constant setup understands. The top-level value keeps
 * its original result <id>; nested constituents get fresh <id>s above bound.
 * Runtime arithmetic opcodes are never emitted, since those only execute
 * inside functions and would leave a global-scope result at zero. */
static int spec_materialize(spec_engine* eng, spvm_word type_id, const spvm_member* tree,
	spvm_word desired_id, spvm_word* next_id, spvm_word** out, size_t* out_used, size_t* out_cap)
{
	spvm_result* t = spec_type_info(eng, type_id);
	if (t == NULL) {
		spec_fail(eng, SPVM_SPEC_ERR_UNSUPPORTED_TYPE, desired_id);
		return eng->err.code;
	}

	if (t->value_type == spvm_value_type_bool) {
		SpvOp op = tree[0].value.b ? SpvOpConstantTrue : SpvOpConstantFalse;
		if (!spec_emit(out, out_used, out_cap, (3 << SpvWordCountShift) | op) ||
			!spec_emit(out, out_used, out_cap, type_id) ||
			!spec_emit(out, out_used, out_cap, desired_id)) {
			eng->err.code = SPVM_SPEC_ERR_ALLOCATION; return eng->err.code;
		}
		return 0;
	}
	if (t->value_type == spvm_value_type_int || t->value_type == spvm_value_type_float) {
		if (!spec_emit(out, out_used, out_cap, (4 << SpvWordCountShift) | SpvOpConstant) ||
			!spec_emit(out, out_used, out_cap, type_id) ||
			!spec_emit(out, out_used, out_cap, desired_id) ||
			!spec_emit(out, out_used, out_cap, tree[0].value.u)) {
			eng->err.code = SPVM_SPEC_ERR_ALLOCATION; return eng->err.code;
		}
		return 0;
	}

	/* aggregate: materialize every constituent first, then a composite */
	spvm_word count = t->member_count;
	spvm_word* child_ids = (spvm_word*)malloc((count ? count : 1) * sizeof(spvm_word));
	if (child_ids == NULL) { eng->err.code = SPVM_SPEC_ERR_ALLOCATION; return eng->err.code; }
	for (spvm_word i = 0; i < count; i++) {
		spvm_word child_type;
		if (t->value_type == spvm_value_type_struct)
			child_type = t->params[i];
		else /* vector / fixed array share one element type */
			child_type = t->pointer;
		child_ids[i] = (*next_id)++;
		/* an aggregate child's constituent array lives in its .members, while a
		 * leaf child is the single node itself */
		const spvm_member* child_base = tree[i].member_count ? tree[i].members : &tree[i];
		if (spec_materialize(eng, child_type, child_base, child_ids[i], next_id, out, out_used, out_cap)) {
			free(child_ids);
			return eng->err.code;
		}
	}
	if (!spec_emit(out, out_used, out_cap, ((count + 3) << SpvWordCountShift) | SpvOpConstantComposite) ||
		!spec_emit(out, out_used, out_cap, type_id) ||
		!spec_emit(out, out_used, out_cap, desired_id)) {
		free(child_ids);
		eng->err.code = SPVM_SPEC_ERR_ALLOCATION; return eng->err.code;
	}
	for (spvm_word i = 0; i < count; i++)
		if (!spec_emit(out, out_used, out_cap, child_ids[i])) {
			free(child_ids);
			eng->err.code = SPVM_SPEC_ERR_ALLOCATION; return eng->err.code;
		}
	free(child_ids);
	return 0;
}

/* Emit one word into the growing instruction-only buffer. */
static int spec_emit(spvm_word** buf, size_t* used, size_t* cap, spvm_word word)
{
	if (*used == *cap) {
		size_t new_cap = *cap ? *cap * 2 : 64;
		spvm_word* nb = (spvm_word*)realloc(*buf, new_cap * sizeof(spvm_word));
		if (nb == NULL)
			return 0;
		*buf = nb;
		*cap = new_cap;
	}
	(*buf)[(*used)++] = word;
	return 1;
}

/* Synthetic OpConstantNull used to pad specialization-grown array composites.
 * One null constant is shared per element type and given a fresh result <id>
 * above the original bound. Returns 1 when the id was just created (the
 * matching OpConstantNull still has to be emitted before its first use),
 * 0 when it already existed, and -1 on allocation failure. */
typedef struct {
	spvm_word type_id;
	spvm_word null_id;
} spec_null_const;

static int spec_null_id_for(spec_null_const** nulls, size_t* null_count, spvm_word elem_type, spvm_word* next_id,
	spvm_word* out_id)
{
	for (size_t i = 0; i < *null_count; i++)
		if ((*nulls)[i].type_id == elem_type) {
			*out_id = (*nulls)[i].null_id;
			return 0;
		}
	spec_null_const* grown = (spec_null_const*)realloc(*nulls, (*null_count + 1) * sizeof(spec_null_const));
	if (grown == NULL)
		return -1;
	*nulls = grown;
	grown[*null_count].type_id = elem_type;
	grown[*null_count].null_id = (*next_id)++;
	*out_id = grown[*null_count].null_id;
	(*null_count)++;
	return 1;
}

/* ---- entry point -------------------------------------------------------- */

spvm_program_t spvm_program_create_specialized(spvm_context_t ctx, spvm_source words, size_t word_count,
	const spvm_spec_entry* entries, size_t entry_count, spvm_spec_error* error)
{
	spec_engine eng;
	memset(&eng, 0, sizeof(eng));
	if (error)
		memset(error, 0, sizeof(*error));

	if (word_count < 5 || words[0] != (spvm_word)SpvMagicNumber) {
		if (error) { error->code = SPVM_SPEC_ERR_MALFORMED; error->result_id = 0; }
		return NULL;
	}
	eng.ctx = ctx;
	eng.words = words;
	eng.word_count = word_count;
	eng.entries = entries;
	eng.entry_count = entry_count;
	eng.bound = words[3];
	if (eng.bound == 0) {
		if (error) { error->code = SPVM_SPEC_ERR_MALFORMED; error->result_id = 0; }
		return NULL;
	}

	/* instruction table */
	eng.instrs = (spec_instr*)calloc(word_count, sizeof(spec_instr));
	eng.result_instr = (spvm_word*)malloc(eng.bound * sizeof(spvm_word));
	eng.results = (spvm_result*)calloc(eng.bound, sizeof(spvm_result));
	eng.eval_state = (unsigned char*)calloc(eng.bound, sizeof(unsigned char));
	if (eng.instrs == NULL || eng.result_instr == NULL || eng.results == NULL || eng.eval_state == NULL) {
		if (error) { error->code = SPVM_SPEC_ERR_ALLOCATION; error->result_id = 0; }
		goto fail;
	}
	for (spvm_word i = 0; i < eng.bound; i++)
		eng.result_instr[i] = (spvm_word)-1;

	size_t idx = 5;
	while (idx < word_count) {
		spvm_word header = words[idx];
		spvm_word wc = (header >> SpvWordCountShift) - 1;
		SpvOp op = (SpvOp)(header & SpvOpCodeMask);
		if (wc < 0 || (size_t)idx + 1 + (size_t)wc > word_count) {
			eng.err.code = SPVM_SPEC_ERR_MALFORMED;
			eng.err.result_id = 0;
			goto fail;
		}
		spec_instr* in = &eng.instrs[eng.instr_count++];
		in->op = op;
		in->word_count = wc;
		in->start = words + idx;
		/* Result <id> placement follows two SPIR-V layouts: type declarations
		 * and OpLabel put the id right after the header; value-producing
		 * instructions carry a result type first, so the id is one word later. */
		switch (op) {
		case SpvOpTypeVoid: case SpvOpTypeBool: case SpvOpTypeInt: case SpvOpTypeFloat:
		case SpvOpTypeVector: case SpvOpTypeMatrix: case SpvOpTypeImage: case SpvOpTypeSampler:
		case SpvOpTypeSampledImage: case SpvOpTypeArray: case SpvOpTypeRuntimeArray:
		case SpvOpTypeStruct: case SpvOpTypePointer: case SpvOpTypeFunction:
		case SpvOpLabel:
			if (wc < 1) { eng.err.code = SPVM_SPEC_ERR_MALFORMED; goto fail; }
			in->result_id = words[idx + 1];
			break;
		case SpvOpSpecConstantTrue: case SpvOpSpecConstantFalse: case SpvOpSpecConstant:
		case SpvOpSpecConstantComposite: case SpvOpSpecConstantOp:
		case SpvOpConstantTrue: case SpvOpConstantFalse: case SpvOpConstant:
		case SpvOpConstantNull: case SpvOpConstantComposite: case SpvOpUndef:
		case SpvOpVariable: case SpvOpLoad: case SpvOpAccessChain:
		case SpvOpFunction: case SpvOpFunctionParameter: case SpvOpFunctionCall:
			if (wc < 2) { eng.err.code = SPVM_SPEC_ERR_MALFORMED; goto fail; }
			in->result_id = words[idx + 2];
			break;
		default:
			in->result_id = 0;
			break;
		}
		if (in->result_id >= eng.bound) {
			eng.err.code = SPVM_SPEC_ERR_MALFORMED;
			eng.err.result_id = in->result_id;
			goto fail;
		}
		if (in->result_id)
			eng.result_instr[in->result_id] = (spvm_word)(eng.instr_count - 1);
		idx += 1 + wc;
	}

	/* types first (SPIR-V guarantees forward-declared type order) */
	for (size_t i = 0; i < eng.instr_count; i++) {
		spec_instr* in = &eng.instrs[i];
		switch (in->op) {
		case SpvOpTypeVoid: case SpvOpTypeBool: case SpvOpTypeInt: case SpvOpTypeFloat:
		case SpvOpTypeVector: case SpvOpTypeMatrix: case SpvOpTypeImage: case SpvOpTypeSampler:
		case SpvOpTypeSampledImage: case SpvOpTypeArray: case SpvOpTypeRuntimeArray:
		case SpvOpTypeStruct: case SpvOpTypePointer: case SpvOpTypeFunction:
			spec_record_type(&eng, in);
			break;
		default:
			break;
		}
	}

	/* SpecId decorations -> definition list */
	for (size_t i = 0; i < eng.instr_count; i++) {
		const spec_instr* in = &eng.instrs[i];
		if (in->op != SpvOpDecorate || in->word_count < 3)
			continue;
		if ((SpvDecoration)in->start[2] != SpvDecorationSpecId)
			continue;
		spec_def def;
		def.result_id = in->start[1];
		def.spec_id = in->start[3];
		spec_def* grown = (spec_def*)realloc(eng.defs, (eng.def_count + 1) * sizeof(spec_def));
		if (grown == NULL) { eng.err.code = SPVM_SPEC_ERR_ALLOCATION; goto fail; }
		eng.defs = grown;
		eng.defs[eng.def_count++] = def;
	}

	/* validate the override table */
	for (size_t i = 0; i < entry_count; i++) {
		for (size_t j = 0; j < i; j++)
			if (entries[i].spec_id == entries[j].spec_id) {
				spec_fail(&eng, SPVM_SPEC_ERR_DUPLICATE_ENTRY, 0);
				goto fail;
			}
		const spec_def* match = NULL;
		size_t matches = 0;
		for (size_t j = 0; j < eng.def_count; j++) {
			if (eng.defs[j].spec_id == entries[i].spec_id) {
				match = &eng.defs[j];
				matches++;
			}
		}
		if (matches == 0) {
			spec_fail(&eng, SPVM_SPEC_ERR_UNKNOWN_SPEC_ID, 0);
			goto fail;
		}
		if (matches > 1) {
			spec_fail(&eng, SPVM_SPEC_ERR_AMBIGUOUS_SPEC_ID, match->result_id);
			goto fail;
		}
		/* the decorated definition must be a scalar spec constant */
		spvm_word di = eng.result_instr[match->result_id];
		if (di == (spvm_word)-1 ||
			(eng.instrs[di].op != SpvOpSpecConstantTrue &&
			 eng.instrs[di].op != SpvOpSpecConstantFalse &&
			 eng.instrs[di].op != SpvOpSpecConstant)) {
			spec_fail(&eng, SPVM_SPEC_ERR_UNSUPPORTED_TYPE, match->result_id);
			goto fail;
		}
	}

	/* ambiguous spec ids must fail even with no matching override entry */
	for (size_t i = 0; i < eng.def_count; i++) {
		size_t matches = 0;
		spvm_word conflict_id = 0;
		for (size_t j = 0; j < eng.def_count; j++)
			if (eng.defs[j].spec_id == eng.defs[i].spec_id) {
				matches++;
				conflict_id = eng.defs[j].result_id;
			}
		if (matches > 1) {
			spec_fail(&eng, SPVM_SPEC_ERR_AMBIGUOUS_SPEC_ID, conflict_id);
			goto fail;
		}
	}

	/* resolve every array length before aggregate values are allocated */
	for (size_t i = 0; i < eng.instr_count; i++) {
		const spec_instr* in = &eng.instrs[i];
		if (in->op != SpvOpTypeArray)
			continue;
		spvm_word type_id = in->start[1];
		spvm_word length_id = eng.results[type_id].params[0];
		if (spec_eval(&eng, length_id))
			goto fail;
		spvm_result* len_slot = &eng.results[length_id];
		spvm_result* lt = spec_type_info(&eng, len_slot->pointer);
		if (lt == NULL || !spec_is_int_type(lt) || len_slot->member_count != 1) {
			spec_fail(&eng, SPVM_SPEC_ERR_BAD_ARRAY_LENGTH, type_id);
			goto fail;
		}
		int32_t length = (int32_t)len_slot->members[0].value.u;
		if (length <= 0) {
			spec_fail(&eng, SPVM_SPEC_ERR_BAD_ARRAY_LENGTH, type_id);
			goto fail;
		}
		eng.results[type_id].member_count = (spvm_word)length;
	}

	/* evaluate every specialization definition (empty table => defaults) */
	for (size_t i = 0; i < eng.def_count; i++)
		if (spec_eval(&eng, eng.defs[i].result_id))
			goto fail;
	/* composite / spec-op constants not reached from a SpecId still participate */
	for (size_t i = 0; i < eng.instr_count; i++) {
		const spec_instr* in = &eng.instrs[i];
		if (in->op == SpvOpSpecConstantComposite || in->op == SpvOpSpecConstantOp)
			if (spec_eval(&eng, in->result_id))
				goto fail;
	}

	/* rewrite spec instructions into ordinary constants */
	spvm_word* out = NULL;
	size_t out_used = 0, out_cap = 0;
	spec_null_const* nulls = NULL;
	size_t null_count = 0;
	spvm_word next_id = eng.bound; /* fresh result <id>s for padding null constants */
	for (size_t i = 0; i < eng.instr_count; i++) {
		const spec_instr* in = &eng.instrs[i];
		spvm_word id = in->result_id;
		switch (in->op) {
		case SpvOpSpecConstantTrue:
		case SpvOpSpecConstantFalse: {
			/* emit the specialized boolean value, not the original default */
			SpvOp bool_op = eng.results[id].members[0].value.b ? SpvOpConstantTrue : SpvOpConstantFalse;
			if (!spec_emit(&out, &out_used, &out_cap, (3 << SpvWordCountShift) | bool_op) ||
				!spec_emit(&out, &out_used, &out_cap, in->start[1]) ||
				!spec_emit(&out, &out_used, &out_cap, in->start[2])) {
				eng.err.code = SPVM_SPEC_ERR_ALLOCATION; goto fail_rewrite;
			}
			break;
		}
		case SpvOpSpecConstant: {
			uint32_t bits = (uint32_t)eng.results[id].members[0].value.u;
			if (!spec_emit(&out, &out_used, &out_cap, (4 << SpvWordCountShift) | SpvOpConstant) ||
				!spec_emit(&out, &out_used, &out_cap, in->start[1]) ||
				!spec_emit(&out, &out_used, &out_cap, in->start[2]) ||
				!spec_emit(&out, &out_used, &out_cap, (spvm_word)bits)) {
				eng.err.code = SPVM_SPEC_ERR_ALLOCATION; goto fail_rewrite;
			}
			break;
		}
		case SpvOpSpecConstantComposite: {
			spvm_word type_id = in->start[1];
			spvm_word listed = in->word_count - 2;       /* constituents in the source instruction */
			spvm_word final_len = eng.results[id].member_count;
			spvm_word total = listed;
			spvm_word null_id = 0;
			int is_grown_array = spec_type_info(&eng, type_id)->value_type == spvm_value_type_array && final_len > listed;
			if (is_grown_array) {
				total = final_len; /* grown array: tail elements reference a synthetic null constant */
				spvm_word elem_type = spec_type_info(&eng, type_id)->pointer;
				int created = spec_null_id_for(&nulls, &null_count, elem_type, &next_id, &null_id);
				if (created < 0) { eng.err.code = SPVM_SPEC_ERR_ALLOCATION; goto fail_rewrite; }
				if (created == 1) {
					/* declare the padding constant before its first use */
					if (!spec_emit(&out, &out_used, &out_cap, (3 << SpvWordCountShift) | SpvOpConstantNull) ||
						!spec_emit(&out, &out_used, &out_cap, elem_type) ||
						!spec_emit(&out, &out_used, &out_cap, null_id)) {
						eng.err.code = SPVM_SPEC_ERR_ALLOCATION; goto fail_rewrite;
					}
				}
			}
			if (!spec_emit(&out, &out_used, &out_cap, (((total + 3)) << SpvWordCountShift) | SpvOpConstantComposite) ||
				!spec_emit(&out, &out_used, &out_cap, type_id) ||
				!spec_emit(&out, &out_used, &out_cap, id)) {
				eng.err.code = SPVM_SPEC_ERR_ALLOCATION; goto fail_rewrite;
			}
			for (spvm_word k = 0; k < listed; k++)
				if (!spec_emit(&out, &out_used, &out_cap, in->start[3 + k])) {
					eng.err.code = SPVM_SPEC_ERR_ALLOCATION; goto fail_rewrite;
				}
			for (spvm_word k = listed; k < total; k++)
				if (!spec_emit(&out, &out_used, &out_cap, null_id)) {
					eng.err.code = SPVM_SPEC_ERR_ALLOCATION; goto fail_rewrite;
				}
			break;
		}
		case SpvOpSpecConstantOp:
			/* bake the evaluated value into literal constants instead of emitting
			 * a runtime opcode, which would never run at global scope */
			if (spec_materialize(&eng, in->start[1], eng.results[id].members, id,
				&next_id, &out, &out_used, &out_cap))
				goto fail_rewrite;
			break;
		default:
			for (spvm_word w = 0; w <= in->word_count; w++)
				if (!spec_emit(&out, &out_used, &out_cap, in->start[w]))
					{ eng.err.code = SPVM_SPEC_ERR_ALLOCATION; goto fail_rewrite; }
			break;
		}
	}

	/* synthetic OpConstantNull padding constants were emitted at first use */
	free(nulls);
	nulls = NULL;

	/* hand the owned instruction stream to a normal program */
	spvm_program_t prog = _spvm_program_create_from_header(ctx, words, word_count);
	if (prog == NULL) {
		eng.err.code = SPVM_SPEC_ERR_ALLOCATION;
		goto fail_rewrite;
	}
	prog->code = out;
	prog->code_length = out_used;
	prog->owns_code = 1;
	prog->bound = next_id; /* room for the synthetic padding result <id>s */

	spec_cleanup_results(&eng);
	free(eng.eval_state);
	free(eng.result_instr);
	free(eng.defs);
	free(eng.instrs);

	if (error)
		*error = eng.err; /* code == SPVM_SPEC_OK */
	return prog;

fail_rewrite:
	free(out);
	free(nulls);
fail:
	if (error && eng.err.code != SPVM_SPEC_OK)
		*error = eng.err;
	spec_cleanup_results(&eng);
	free(eng.eval_state);
	free(eng.result_instr);
	free(eng.defs);
	free(eng.instrs);
	return NULL;
}
