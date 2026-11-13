/* Read-only x86-64 control flow recovery on top of the Capstone decoder.
 *
 * The engine walks executable regions starting from explicit entry points,
 * decoding every instruction with a detail enabled Capstone handle. A finite
 * constant-set lattice over the 16 general purpose registers is propagated
 * alongside the graph; the worklist runs to a fixed point so loops and late
 * discovered predecessors are handled without caching first answers.
 *
 * No machine code is executed and no host pointer is ever derived from a
 * target address: memory operands are satisfied exclusively from the caller
 * provided regions.
 */

#include <capstone/cfg.h>

#include <stdlib.h>
#include <string.h>

#define CFG_NGPR 16
#define CFG_INSN_MAX_BYTES 15

/* ------------------------------------------------------------------ */
/* Abstract register values                                           */
/* ------------------------------------------------------------------ */

typedef struct cfg_absval {
	bool unknown;
	uint8_t count;
	uint64_t vals[CS_CFG_MAX_VALUES];
} cfg_absval;

static void val_set_unknown(cfg_absval *v)
{
	v->unknown = true;
	v->count = 0;
}

static void val_set_singleton(cfg_absval *v, uint64_t x)
{
	v->unknown = false;
	v->count = 1;
	v->vals[0] = x;
}

/* Insert x into a finite set; promote to unknown when the cap is exceeded. */
static void val_insert(cfg_absval *v, uint64_t x)
{
	uint8_t i;

	if (v->unknown)
		return;
	for (i = 0; i < v->count; ++i) {
		if (v->vals[i] == x)
			return;
		if (v->vals[i] > x) {
			uint8_t j;
			if (v->count == CS_CFG_MAX_VALUES) {
				val_set_unknown(v);
				return;
			}
			for (j = v->count; j > i; --j)
				v->vals[j] = v->vals[j - 1];
			v->vals[i] = x;
			++v->count;
			return;
		}
	}
	if (v->count == CS_CFG_MAX_VALUES) {
		val_set_unknown(v);
		return;
	}
	v->vals[v->count++] = x;
}

/* Join two abstract values. Returns true when *dst changed. */
static bool val_merge(cfg_absval *dst, const cfg_absval *src)
{
	uint8_t i;

	if (src->unknown) {
		if (!dst->unknown) {
			val_set_unknown(dst);
			return true;
		}
		return false;
	}
	if (dst->unknown)
		return false;
	for (i = 0; i < src->count; ++i) {
		uint8_t j;
		bool found = false;
		for (j = 0; j < dst->count; ++j) {
			if (dst->vals[j] == src->vals[i]) {
				found = true;
				break;
			}
		}
		if (!found) {
			val_insert(dst, src->vals[i]);
			return true; /* either inserted or promoted */
		}
	}
	return false;
}

/* ------------------------------------------------------------------ */
/* General purpose register identity                                  */
/* ------------------------------------------------------------------ */

static int cfg_reg_index(x86_reg r)
{
	switch (r) {
	/* 64/32/16/8 bit aliases share one abstract identity */
	case X86_REG_RAX: case X86_REG_EAX: case X86_REG_AX:
	case X86_REG_AL: case X86_REG_AH:
		return 0;
	case X86_REG_RCX: case X86_REG_ECX: case X86_REG_CX:
	case X86_REG_CL: case X86_REG_CH:
		return 1;
	case X86_REG_RDX: case X86_REG_EDX: case X86_REG_DX:
	case X86_REG_DL: case X86_REG_DH:
		return 2;
	case X86_REG_RBX: case X86_REG_EBX: case X86_REG_BX:
	case X86_REG_BL: case X86_REG_BH:
		return 3;
	case X86_REG_RSP: case X86_REG_ESP: case X86_REG_SP:
	case X86_REG_SPL:
		return 4;
	case X86_REG_RBP: case X86_REG_EBP: case X86_REG_BP:
	case X86_REG_BPL:
		return 5;
	case X86_REG_RSI: case X86_REG_ESI: case X86_REG_SI:
	case X86_REG_SIL:
		return 6;
	case X86_REG_RDI: case X86_REG_EDI: case X86_REG_DI:
	case X86_REG_DIL:
		return 7;
	case X86_REG_R8: case X86_REG_R8D: case X86_REG_R8W:
	case X86_REG_R8B:
		return 8;
	case X86_REG_R9: case X86_REG_R9D: case X86_REG_R9W:
	case X86_REG_R9B:
		return 9;
	case X86_REG_R10: case X86_REG_R10D: case X86_REG_R10W:
	case X86_REG_R10B:
		return 10;
	case X86_REG_R11: case X86_REG_R11D: case X86_REG_R11W:
	case X86_REG_R11B:
		return 11;
	case X86_REG_R12: case X86_REG_R12D: case X86_REG_R12W:
	case X86_REG_R12B:
		return 12;
	case X86_REG_R13: case X86_REG_R13D: case X86_REG_R13W:
	case X86_REG_R13B:
		return 13;
	case X86_REG_R14: case X86_REG_R14D: case X86_REG_R14W:
	case X86_REG_R14B:
		return 14;
	case X86_REG_R15: case X86_REG_R15D: case X86_REG_R15W:
	case X86_REG_R15B:
		return 15;
	default:
		return -1;
	}
}

static const x86_reg cfg_gp_regs[CFG_NGPR] = {
	X86_REG_RAX, X86_REG_RCX, X86_REG_RDX, X86_REG_RBX,
	X86_REG_RSP, X86_REG_RBP, X86_REG_RSI, X86_REG_RDI,
	X86_REG_R8, X86_REG_R9, X86_REG_R10, X86_REG_R11,
	X86_REG_R12, X86_REG_R13, X86_REG_R14, X86_REG_R15,
};

/* ------------------------------------------------------------------ */
/* Generic dynamic array                                              */
/* ------------------------------------------------------------------ */

typedef struct cfg_arr {
	void *data;
	size_t count;
	size_t cap;
} cfg_arr;

static void *cfg_grow(void *p, size_t *cap, size_t need, size_t elemsz)
{
	size_t ncap;
	void *np;

	if (need <= *cap)
		return p;
	ncap = *cap ? *cap : 16;
	while (ncap < need) {
		if (ncap > ((size_t)-1) / 2)
			return NULL;
		ncap *= 2;
	}
	np = realloc(p, ncap * elemsz);
	if (!np)
		return NULL;
	*cap = ncap;
	return np;
}

static bool cfg_arr_push(cfg_arr *a, const void *v, size_t elemsz)
{
	void *p = cfg_grow(a->data, &a->cap, a->count + 1, elemsz);
	if (!p)
		return false;
	a->data = p;
	memcpy((uint8_t *)p + a->count * elemsz, v, elemsz);
	++a->count;
	return true;
}

/* ------------------------------------------------------------------ */
/* Decoded instructions and analysis nodes                            */
/* ------------------------------------------------------------------ */

typedef struct cfg_insn {
	bool valid;
	bool truncated;
	bool no_region;	/* address is not backed by an executable region */
	uint64_t addr;
	uint16_t size;
	unsigned int id;
	cs_x86 x;
	uint8_t groups[MAX_NUM_GROUPS];
	uint8_t groups_count;
	uint16_t regs_write[MAX_IMPL_W_REGS];
	uint8_t regs_write_count;
} cfg_insn;

typedef struct cfg_term_info {
	uint8_t kind;		/* cs_cfg_terminator */
	bool indirect_jmp;
	bool unresolved;
} cfg_term_info;

typedef struct cfg_node {
	uint64_t addr;
	bool used;		/* open addressing slot occupied */
	bool decoded;
	bool overlap_start;	/* start lies inside another decoded insn */
	cfg_insn insn;
	cfg_absval in[CFG_NGPR];
	cfg_absval out[CFG_NGPR];
	cfg_term_info term;
} cfg_node;

typedef struct cfg_work_item {
	uint64_t addr;
	cfg_absval state[CFG_NGPR];
} cfg_work_item;

typedef struct cfg_engine {
	const cs_cfg_region *regions;
	size_t region_count;
	const uint64_t *entries;
	size_t entry_count;

	csh handle;
	cs_insn *insn_cache;
	uint8_t scratch[CFG_INSN_MAX_BYTES]; /* padded decode retry buffer */

	/* node hash table, open addressing */
	cfg_node *nodes;
	size_t nodes_cap;	/* always a power of two */
	size_t nodes_count;

	/* FIFO worklist */
	cfg_work_item *work;
	size_t work_count;
	size_t work_head;
	size_t work_cap;

	/* emitted artifacts (deduplicated on insertion) */
	cfg_arr edges;
	cfg_arr indirect;
	cfg_arr overlaps;
	cfg_arr diagnostics;

	bool oom;
} cfg_engine;

/* ------------------------------------------------------------------ */
/* Region helpers                                                      */
/* ------------------------------------------------------------------ */

static const cs_cfg_region *cfg_region_at(const cfg_engine *e, uint64_t addr)
{
	size_t i;

	for (i = 0; i < e->region_count; ++i) {
		const cs_cfg_region *r = &e->regions[i];
		if (addr >= r->address && addr - r->address < r->size)
			return r;
	}
	return NULL;
}

static bool cfg_in_executable(const cfg_engine *e, uint64_t addr)
{
	const cs_cfg_region *r = cfg_region_at(e, addr);
	return r != NULL && r->executable;
}

/* Read sz (4 or 8) little endian bytes from a read only region. */
static bool cfg_read_ro(const cfg_engine *e, uint64_t addr, unsigned sz,
		uint64_t *out)
{
	const cs_cfg_region *r;
	uint64_t off;
	uint64_t v = 0;
	unsigned i;

	r = cfg_region_at(e, addr);
	if (!r || !r->read_only)
		return false;
	off = addr - r->address;
	/* wrap safe containment of the full access */
	if (off > r->size || (uint64_t)sz > r->size - off)
		return false;
	for (i = 0; i < sz; ++i)
		v |= (uint64_t)r->data[off + i] << (8 * i);
	*out = v;
	return true;
}

/* ------------------------------------------------------------------ */
/* Node hash table                                                     */
/* ------------------------------------------------------------------ */

static size_t cfg_hash64(uint64_t x)
{
	x ^= x >> 33;
	x *= 0xff51afd7ed558ccdULL;
	x ^= x >> 33;
	x *= 0xc4ceb9fe1a85ec53ULL;
	x ^= x >> 33;
	return (size_t)x;
}

static cfg_node *cfg_node_find(const cfg_engine *e, uint64_t addr)
{
	size_t i;

	if (!e->nodes_cap)
		return NULL;
	i = cfg_hash64(addr) & (e->nodes_cap - 1);
	while (e->nodes[i].used) {
		if (e->nodes[i].addr == addr)
			return &e->nodes[i];
		i = (i + 1) & (e->nodes_cap - 1);
	}
	return NULL;
}

static bool cfg_table_grow(cfg_engine *e)
{
	size_t ncap = e->nodes_cap ? e->nodes_cap * 2 : 64;
	cfg_node *nn;
	size_t i;

	nn = calloc(ncap, sizeof(cfg_node));
	if (!nn)
		return false;
	for (i = 0; i < e->nodes_cap; ++i) {
		cfg_node *s = &e->nodes[i];
		size_t h;
		if (!s->used)
			continue;
		h = cfg_hash64(s->addr) & (ncap - 1);
		while (nn[h].used)
			h = (h + 1) & (ncap - 1);
		nn[h] = *s;
	}
	free(e->nodes);
	e->nodes = nn;
	e->nodes_cap = ncap;
	return true;
}

static cfg_node *cfg_node_get(cfg_engine *e, uint64_t addr)
{
	cfg_node *n;
	size_t i;

	n = cfg_node_find(e, addr);
	if (n)
		return n;
	if (e->nodes_count + 1 > e->nodes_cap * 7 / 10) {
		if (!cfg_table_grow(e)) {
			e->oom = true;
			return NULL;
		}
	}
	i = cfg_hash64(addr) & (e->nodes_cap - 1);
	while (e->nodes[i].used)
		i = (i + 1) & (e->nodes_cap - 1);
	n = &e->nodes[i];
	n->used = true;
	n->addr = addr;
	++e->nodes_count;
	return n;
}

/* ------------------------------------------------------------------ */
/* Diagnostics, edges and overlaps                                    */
/* ------------------------------------------------------------------ */

static void cfg_diag(cfg_engine *e, uint64_t addr,
		cs_cfg_diagnostic_kind kind)
{
	cs_cfg_diagnostic *all = e->diagnostics.data;
	size_t i;
	cs_cfg_diagnostic d;

	for (i = 0; i < e->diagnostics.count; ++i) {
		if (all[i].address == addr && all[i].kind == kind)
			return;
	}
	d.address = addr;
	d.kind = kind;
	if (!cfg_arr_push(&e->diagnostics, &d, sizeof(d)))
		e->oom = true;
}

static void cfg_edge(cfg_engine *e, uint64_t src, uint64_t dst,
		cs_cfg_edge_kind kind)
{
	cs_cfg_edge *all = e->edges.data;
	size_t i;
	cs_cfg_edge g;

	for (i = 0; i < e->edges.count; ++i) {
		if (all[i].source == src && all[i].target == dst &&
				all[i].kind == kind)
			return;
	}
	g.source = src;
	g.target = dst;
	g.kind = kind;
	if (!cfg_arr_push(&e->edges, &g, sizeof(g)))
		e->oom = true;
}

static void cfg_indirect(cfg_engine *e, uint64_t site, uint64_t target)
{
	cs_cfg_indirect_target *all = e->indirect.data;
	size_t i;
	cs_cfg_indirect_target t;

	for (i = 0; i < e->indirect.count; ++i) {
		if (all[i].site == site && all[i].target == target)
			return;
	}
	t.site = site;
	t.target = target;
	if (!cfg_arr_push(&e->indirect, &t, sizeof(t)))
		e->oom = true;
}

/* Record a conflict between two decode starts covering overlapping bytes.
 * Both chains stay in the graph; nothing is discarded. */
static void cfg_record_overlap(cfg_engine *e, cfg_node *a, cfg_node *b)
{
	uint64_t a1 = a->addr, a2 = b->addr;
	uint64_t b1 = a->addr + a->insn.size;
	uint64_t b2 = b->addr + b->insn.size;
	cs_cfg_overlap o;
	cs_cfg_overlap *all = e->overlaps.data;
	size_t i;

	o.start = a1 > a2 ? a1 : a2;
	o.end = b1 < b2 ? b1 : b2;
	o.first = a1 < a2 ? a1 : a2;
	o.second = a1 < a2 ? a2 : a1;
	if (o.start >= o.end)
		return;
	/* the later start is the one landing inside an earlier instruction */
	if (a1 < a2)
		b->overlap_start = true;
	else
		a->overlap_start = true;
	for (i = 0; i < e->overlaps.count; ++i) {
		if (all[i].start == o.start && all[i].end == o.end &&
				all[i].first == o.first &&
				all[i].second == o.second)
			return;
	}
	if (!cfg_arr_push(&e->overlaps, &o, sizeof(o)))
		e->oom = true;
}

/* ------------------------------------------------------------------ */
/* Decoding                                                            */
/* ------------------------------------------------------------------ */

static bool cfg_has_group(const cfg_insn *ins, unsigned group)
{
	uint8_t i;

	for (i = 0; i < ins->groups_count; ++i)
		if (ins->groups[i] == group)
			return true;
	return false;
}

/* Decode the instruction at addr exactly once per start address. */
static cfg_node *cfg_decode(cfg_engine *e, uint64_t addr)
{
	cfg_node *n;
	const cs_cfg_region *r;
	uint64_t off, remain;
	const uint8_t *code;
	size_t size;
	uint64_t cur;
	size_t i;

	n = cfg_node_find(e, addr);
	if (n && n->decoded)
		return n;
	if (!n) {
		n = cfg_node_get(e, addr);
		if (!n)
			return NULL;
	}

	n->decoded = true;
	memset(&n->insn, 0, sizeof(n->insn));
	n->insn.addr = addr;

	r = cfg_region_at(e, addr);
	if (!r || !r->executable) {
		/* gaps are never linearly scanned; the branch that lands
		 * here already receives an OUTSIDE_REGION diagnostic */
		n->insn.valid = false;
		n->insn.no_region = true;
		return n;
	}

	off = addr - r->address;
	remain = r->size - off;
	code = r->data + off;
	size = remain > CFG_INSN_MAX_BYTES ?
		CFG_INSN_MAX_BYTES : (size_t)remain;
	cur = addr;

	if (cs_disasm_iter(e->handle, &code, &size, &cur, e->insn_cache)) {
		cs_insn *ins = e->insn_cache;
		cs_regs rr, rw;
		uint8_t nr = 0, nw = 0;

		n->insn.valid = true;
		n->insn.size = ins->size;
		n->insn.id = ins->id;
		n->insn.x = ins->detail->x86;
		n->insn.groups_count = ins->detail->groups_count;
		memcpy(n->insn.groups, ins->detail->groups,
				ins->detail->groups_count);
		if (cs_regs_access(e->handle, ins, rr, &nr, rw, &nw) ==
				CS_ERR_OK) {
			n->insn.regs_write_count = nw;
			memcpy(n->insn.regs_write, rw,
					nw * sizeof(rw[0]));
		}
	} else {
		size_t pad_n = remain > CFG_INSN_MAX_BYTES ?
			CFG_INSN_MAX_BYTES : (size_t)remain;
		const uint8_t *pcode;
		size_t psize;
		uint64_t pcur;

		n->insn.valid = false;
		/* Distinguish a stream cut inside a multi-byte instruction
		 * from a truly illegal encoding: retry with a full 15 byte
		 * scratch. Success needing bytes past the region end means
		 * truncation; failure even at the maximum instruction length
		 * means the opcode is illegal. */
		memset(e->scratch, 0, sizeof(e->scratch));
		memcpy(e->scratch, code, pad_n);
		pcode = e->scratch;
		psize = CFG_INSN_MAX_BYTES;
		pcur = addr;
		if (cs_disasm_iter(e->handle, &pcode, &psize, &pcur,
				e->insn_cache)) {
			unsigned consumed =
				(unsigned)(pcur - addr);
			n->insn.truncated = consumed > remain;
		} else {
			n->insn.truncated = false;
		}
	}

	/* overlap bookkeeping: a fresh start whose instruction bytes overlap
	 * an already decoded instruction keeps both chains visible */
	if (n->insn.valid) {
		for (i = 0; i < e->nodes_cap; ++i) {
			cfg_node *other = &e->nodes[i];
			if (!other->used || !other->decoded ||
					!other->insn.valid || other == n)
				continue;
			if (n->addr < other->addr + other->insn.size &&
					other->addr < n->addr + n->insn.size)
				cfg_record_overlap(e, other, n);
		}
	}
	return n;
}

/* ------------------------------------------------------------------ */
/* Register transfer semantics                                         */
/* ------------------------------------------------------------------ */

static uint64_t cfg_mask(unsigned width)
{
	return width == 4 ? 0xffffffffULL : 0xffffffffffffffffULL;
}

static void cfg_invalidate_writes(const cfg_insn *ins, cfg_absval *state)
{
	uint8_t i;

	for (i = 0; i < ins->regs_write_count; ++i) {
		int idx = cfg_reg_index((x86_reg)ins->regs_write[i]);
		if (idx >= 0)
			val_set_unknown(&state[idx]);
	}
}

/* Effective address set for a memory operand. RIP uses the address of the
 * next instruction; any unknown component makes the result unknown. */
static void cfg_eff_addr(const x86_op_mem *m,
		uint64_t rip_next, const cfg_absval *state, cfg_absval *out)
{
	cfg_absval bases, indices;
	int scale = m->scale ? m->scale : 1;
	uint8_t bi, ii;

	/* segment overrides (FS:/GS:) are not modeled by flat regions */
	if (m->segment != X86_REG_INVALID) {
		val_set_unknown(out);
		return;
	}

	val_set_unknown(&bases);
	val_set_unknown(&indices);

	if (m->base == X86_REG_RIP) {
		val_set_singleton(&bases, rip_next);
	} else if (m->base == X86_REG_INVALID || m->base == X86_REG_RIZ) {
		val_set_singleton(&bases, 0);
	} else {
		int idx = cfg_reg_index(m->base);
		if (idx < 0 || state[idx].unknown) {
			val_set_unknown(out);
			return;
		}
		bases = state[idx];
	}

	if (m->index == X86_REG_INVALID || m->index == X86_REG_RIZ ||
			m->index == X86_REG_EIZ) {
		val_set_singleton(&indices, 0);
	} else {
		int idx = cfg_reg_index(m->index);
		if (idx < 0 || state[idx].unknown) {
			val_set_unknown(out);
			return;
		}
		indices = state[idx];
	}

	val_set_unknown(out);
	out->unknown = false;
	out->count = 0;
	for (bi = 0; bi < bases.count && !out->unknown; ++bi) {
		for (ii = 0; ii < indices.count; ++ii) {
			uint64_t a = bases.vals[bi] +
				(uint64_t)scale * indices.vals[ii] +
				(uint64_t)m->disp;
			val_insert(out, a);
			if (out->unknown)
				break;
		}
	}
	if (out->count == 0 && !out->unknown)
		val_set_unknown(out);
}

/* Read 4/8 bytes per address in addrs, satisfying every access from a read
 * only region. A single failure or an oversize result yields unknown. */
static void cfg_mem_read(const cfg_engine *e, const cfg_absval *addrs,
		unsigned width, bool sign_extend, cfg_absval *out)
{
	uint8_t i;

	val_set_unknown(out);
	out->unknown = false;
	out->count = 0;
	if (addrs->unknown) {
		val_set_unknown(out);
		return;
	}
	for (i = 0; i < addrs->count; ++i) {
		uint64_t raw;
		if (!cfg_read_ro(e, addrs->vals[i], width, &raw)) {
			val_set_unknown(out);
			return;
		}
		if (sign_extend && width == 4 && (raw & 0x80000000ULL))
			raw |= 0xffffffff00000000ULL;
		val_insert(out, raw);
		if (out->unknown)
			return;
	}
	if (out->count == 0)
		val_set_unknown(out);
}

static void cfg_pointwise(cfg_absval *dst, const cfg_absval *a,
		const cfg_absval *b, unsigned op, unsigned width)
{
	cfg_absval r;
	uint8_t i, j;
	uint64_t mask = cfg_mask(width);

	val_set_unknown(&r);
	r.unknown = false;
	r.count = 0;
	if (a->unknown || b->unknown) {
		val_set_unknown(dst);
		return;
	}
	for (i = 0; i < a->count && !r.unknown; ++i) {
		for (j = 0; j < b->count; ++j) {
			uint64_t x = a->vals[i], y = b->vals[j], z = 0;
			switch (op) {
			case 0: z = (x + y) & mask; break;	/* ADD */
			case 1: z = (x - y) & mask; break;	/* SUB */
			default: z = (x & y) & mask; break;	/* AND */
			}
			val_insert(&r, z);
			if (r.unknown)
				break;
		}
	}
	*dst = r;
}

/* Apply one non control instruction to the abstract state. Anything not
 * explicitly modelled falls back to cs_regs_access invalidation. */
static void cfg_transfer(cfg_engine *e, cfg_node *n, cfg_absval *state)
{
	const cfg_insn *ins = &n->insn;
	const cs_x86 *x = &ins->x;
	const cs_x86_op *op0, *op1;
	int didx;
	unsigned width;

	if (x->op_count == 0) {
		cfg_invalidate_writes(ins, state);
		return;
	}
	op0 = &x->operands[0];
	op1 = x->op_count > 1 ? &x->operands[1] : NULL;

	switch (ins->id) {
	case X86_INS_MOV:
		if (op0->type != X86_OP_REG)
			break;
		didx = cfg_reg_index(op0->reg);
		if (didx < 0)
			break;
		width = op0->size;
		if (width != 4 && width != 8) {
			val_set_unknown(&state[didx]);
			return;
		}
		if (op1->type == X86_OP_REG) {
			int sidx = cfg_reg_index(op1->reg);
			cfg_absval src;
			uint64_t mask = cfg_mask(width);
			uint8_t k;
			if (sidx < 0 || state[sidx].unknown) {
				val_set_unknown(&state[didx]);
				return;
			}
			/* snapshot first: source and destination may share an
			 * identity (mov rax, rax) */
			src = state[sidx];
			val_set_unknown(&state[didx]);
			state[didx].unknown = false;
			state[didx].count = 0;
			for (k = 0; k < src.count; ++k)
				val_insert(&state[didx], src.vals[k] & mask);
			if (state[didx].count == 0)
				val_set_unknown(&state[didx]);
			return;
		}
		if (op1->type == X86_OP_IMM) {
			val_set_singleton(&state[didx],
					(uint64_t)op1->imm & cfg_mask(width));
			return;
		}
		if (op1->type == X86_OP_MEM) {
			cfg_absval addrs, vals;
			if (width != 4 && width != 8) {
				val_set_unknown(&state[didx]);
				return;
			}
			cfg_eff_addr(&op1->mem, n->addr + ins->size, state,
					&addrs);
			cfg_mem_read(e, &addrs, width, false, &vals);
			state[didx] = vals;
			return;
		}
		break;

	case X86_INS_MOVSXD:
		if (op0->type != X86_OP_REG || !op1 || op0->size != 8)
			break;
		didx = cfg_reg_index(op0->reg);
		if (didx < 0)
			break;
		if (op1->type == X86_OP_REG) {
			int sidx = cfg_reg_index(op1->reg);
			cfg_absval r;
			uint8_t k;
			val_set_unknown(&r);
			r.unknown = false;
			r.count = 0;
			if (sidx < 0 || state[sidx].unknown) {
				val_set_unknown(&state[didx]);
				return;
			}
			for (k = 0; k < state[sidx].count; ++k) {
				uint64_t v = state[sidx].vals[k] &
					0xffffffffULL;
				if (v & 0x80000000ULL)
					v |= 0xffffffff00000000ULL;
				val_insert(&r, v);
			}
			state[didx] = r;
			return;
		}
		if (op1->type == X86_OP_MEM && op1->size == 4) {
			cfg_absval addrs, vals;
			cfg_eff_addr(&op1->mem, n->addr + ins->size, state,
					&addrs);
			cfg_mem_read(e, &addrs, 4, true, &vals);
			state[didx] = vals;
			return;
		}
		break;

	case X86_INS_LEA:
		if (op0->type != X86_OP_REG || !op1 ||
				op1->type != X86_OP_MEM)
			break;
		didx = cfg_reg_index(op0->reg);
		if (didx < 0)
			break;
		width = op0->size;
		if (width != 4 && width != 8) {
			val_set_unknown(&state[didx]);
			return;
		}
		{
			cfg_absval addrs;
			cfg_eff_addr(&op1->mem, n->addr + ins->size, state,
					&addrs);
			if (addrs.unknown) {
				val_set_unknown(&state[didx]);
			} else {
				uint64_t mask = cfg_mask(width);
				uint8_t k;
				val_set_unknown(&state[didx]);
				state[didx].unknown = false;
				state[didx].count = 0;
				for (k = 0; k < addrs.count; ++k)
					val_insert(&state[didx],
							addrs.vals[k] & mask);
				if (state[didx].count == 0)
					val_set_unknown(&state[didx]);
			}
		}
		return;

	case X86_INS_ADD:
	case X86_INS_SUB:
	case X86_INS_AND:
		if (!op1 || op0->type != X86_OP_REG)
			break;
		didx = cfg_reg_index(op0->reg);
		if (didx < 0)
			break;
		width = op0->size;
		if (width != 4 && width != 8) {
			val_set_unknown(&state[didx]);
			return;
		}
		if (op1->type == X86_OP_IMM) {
			cfg_absval imm;
			val_set_singleton(&imm,
					(uint64_t)op1->imm & cfg_mask(width));
			cfg_pointwise(&state[didx], &state[didx], &imm,
					ins->id == X86_INS_ADD ? 0 :
					ins->id == X86_INS_SUB ? 1 : 2, width);
			return;
		}
		if (op1->type == X86_OP_REG) {
			int sidx = cfg_reg_index(op1->reg);
			if (sidx < 0) {
				val_set_unknown(&state[didx]);
				return;
			}
			cfg_pointwise(&state[didx], &state[didx],
					&state[sidx],
					ins->id == X86_INS_ADD ? 0 :
					ins->id == X86_INS_SUB ? 1 : 2, width);
			return;
		}
		break;

	case X86_INS_XOR:
		if (!op1 || op0->type != X86_OP_REG ||
				op1->type != X86_OP_REG)
			break;
		didx = cfg_reg_index(op0->reg);
		if (didx < 0)
			break;
		width = op0->size;
		if (width == 4 || width == 8) {
			if (op0->reg == op1->reg) {
				val_set_singleton(&state[didx], 0);
				return;
			}
			{
				int sidx = cfg_reg_index(op1->reg);
				if (sidx < 0) {
					val_set_unknown(&state[didx]);
					return;
				}
				cfg_pointwise(&state[didx], &state[didx],
						&state[sidx], 2, width);
			}
			return;
		}
		val_set_unknown(&state[didx]);
		return;

	default:
		break;
	}

	/* conservative fallback: every written GPR identity, including
	 * 8/16 bit aliases, loses any constant knowledge */
	cfg_invalidate_writes(ins, state);
}

/* ------------------------------------------------------------------ */
/* Worklist                                                            */
/* ------------------------------------------------------------------ */

static bool cfg_enqueue(cfg_engine *e, uint64_t addr,
		const cfg_absval *state)
{
	cfg_work_item *p;

	p = cfg_grow(e->work, &e->work_cap, e->work_count + 1,
			sizeof(e->work[0]));
	if (!p) {
		e->oom = true;
		return false;
	}
	e->work = p;
	e->work[e->work_count].addr = addr;
	memcpy(e->work[e->work_count].state, state,
			sizeof(cfg_absval) * CFG_NGPR);
	++e->work_count;
	return true;
}

static void cfg_check_target(cfg_engine *e, uint64_t target)
{
	if (!cfg_in_executable(e, target))
		cfg_diag(e, target, CS_CFG_DIAG_OUTSIDE_REGION);
}

/* Resolve an indirect branch operand into finite candidates. */
static void cfg_indirect_targets(cfg_engine *e, cfg_node *n,
		const cs_x86_op *op, const cfg_absval *state,
		cfg_absval *targets)
{
	val_set_unknown(targets);
	targets->unknown = false;
	targets->count = 0;

	if (op->type == X86_OP_REG) {
		int idx = cfg_reg_index(op->reg);
		if (idx < 0) {
			val_set_unknown(targets);
			return;
		}
		*targets = state[idx];
	} else if (op->type == X86_OP_MEM) {
		cfg_absval addrs;
		if (op->size != 4 && op->size != 8) {
			val_set_unknown(targets);
			return;
		}
		cfg_eff_addr(&op->mem, n->addr + n->insn.size, state, &addrs);
		if (addrs.unknown) {
			val_set_unknown(targets);
			return;
		}
		cfg_mem_read(e, &addrs, op->size, false, targets);
	} else {
		val_set_unknown(targets);
	}
}

static void cfg_exec(cfg_engine *e, cfg_node *n, const cfg_absval *in)
{
	cfg_absval state[CFG_NGPR];
	cfg_insn *ins = &n->insn;
	uint64_t next = n->addr + ins->size;
	bool is_jump, is_call, is_ret;

	memcpy(state, in, sizeof(state));
	memset(&n->term, 0, sizeof(n->term));

	if (!ins->valid) {
		n->term.kind = CS_CFG_TERM_TRAP;
		if (!ins->no_region) {
			if (ins->truncated)
				cfg_diag(e, n->addr, CS_CFG_DIAG_TRUNCATED);
			else
				cfg_diag(e, n->addr, CS_CFG_DIAG_ILLEGAL);
		}
		memcpy(n->out, state, sizeof(state));
		return;
	}

	is_jump = cfg_has_group(ins, CS_GRP_JUMP);
	is_call = cfg_has_group(ins, CS_GRP_CALL);
	is_ret = cfg_has_group(ins, CS_GRP_RET);

	if (is_ret) {
		n->term.kind = CS_CFG_TERM_RET;
		memcpy(n->out, state, sizeof(state));
		return;
	}

	if (is_call) {
		n->term.kind = CS_CFG_TERM_CALL;
		if (ins->x.op_count >= 1 &&
				ins->x.operands[0].type == X86_OP_IMM) {
			uint64_t t = (uint64_t)ins->x.operands[0].imm;
			cfg_edge(e, n->addr, t, CS_CFG_EDGE_CALL);
			cfg_check_target(e, t);
		} else if (ins->x.op_count >= 1) {
			/* indirect call: record whatever candidates the
			 * register lattice gives, but never enter them */
			cfg_absval targets;
			uint8_t k;
			cfg_indirect_targets(e, n, &ins->x.operands[0], state,
					&targets);
			if (!targets.unknown) {
				for (k = 0; k < targets.count; ++k) {
					cfg_edge(e, n->addr, targets.vals[k],
							CS_CFG_EDGE_CALL);
					cfg_check_target(e, targets.vals[k]);
				}
			}
		}
		/* only the return successor is traced, and calls may clobber
		 * anything: all GPR knowledge is dropped */
		{
			int k;
			for (k = 0; k < CFG_NGPR; ++k)
				val_set_unknown(&state[k]);
		}
		cfg_edge(e, n->addr, next, CS_CFG_EDGE_CALL_RETURN);
		cfg_check_target(e, next);
		memcpy(n->out, state, sizeof(state));
		if (cfg_in_executable(e, next))
			cfg_enqueue(e, next, state);
		return;
	}

	if (is_jump) {
		if (ins->id == X86_INS_JMP) {
			n->term.kind = CS_CFG_TERM_JMP;
			if (ins->x.op_count >= 1 &&
					ins->x.operands[0].type ==
						X86_OP_IMM) {
				uint64_t t = (uint64_t)ins->x.operands[0].imm;
				cfg_edge(e, n->addr, t, CS_CFG_EDGE_JUMP);
				cfg_check_target(e, t);
				if (cfg_in_executable(e, t))
					cfg_enqueue(e, t, state);
			} else if (ins->x.op_count >= 1) {
				cfg_absval targets;
				uint8_t k;
				n->term.indirect_jmp = true;
				cfg_indirect_targets(e, n,
						&ins->x.operands[0], state,
						&targets);
				if (targets.unknown || targets.count == 0) {
					n->term.unresolved = true;
					cfg_diag(e, n->addr,
							CS_CFG_DIAG_UNRESOLVED_INDIRECT);
				} else {
					for (k = 0; k < targets.count; ++k) {
						uint64_t t = targets.vals[k];
						if (!cfg_in_executable(e, t)) {
							cfg_diag(e, t,
								CS_CFG_DIAG_OUTSIDE_REGION);
							continue;
						}
						cfg_indirect(e, n->addr, t);
						cfg_edge(e, n->addr, t,
								CS_CFG_EDGE_INDIRECT);
						cfg_enqueue(e, t, state);
					}
				}
			}
		} else {
			/* conditional jumps: both successors are kept */
			n->term.kind = CS_CFG_TERM_JCC;
			if (ins->x.op_count >= 1 &&
					ins->x.operands[0].type ==
						X86_OP_IMM) {
				uint64_t t = (uint64_t)ins->x.operands[0].imm;
				cfg_edge(e, n->addr, t,
						CS_CFG_EDGE_TRUE_BRANCH);
				cfg_edge(e, n->addr, next,
						CS_CFG_EDGE_FALSE_BRANCH);
				cfg_check_target(e, t);
				cfg_check_target(e, next);
				if (cfg_in_executable(e, t))
					cfg_enqueue(e, t, state);
				if (cfg_in_executable(e, next))
					cfg_enqueue(e, next, state);
			}
		}
		memcpy(n->out, state, sizeof(state));
		return;
	}

	/* ordinary instruction: model the register transfer then continue
	 * linearly; data gaps stop the chain instead of being swept in */
	cfg_transfer(e, n, state);
	n->term.kind = CS_CFG_TERM_FALLTHROUGH;
	cfg_edge(e, n->addr, next, CS_CFG_EDGE_FALLTHROUGH);
	cfg_check_target(e, next);
	memcpy(n->out, state, sizeof(state));
	if (cfg_in_executable(e, next))
		cfg_enqueue(e, next, state);
}

/* ------------------------------------------------------------------ */
/* Finalization: basic blocks and sorted output                        */
/* ------------------------------------------------------------------ */

typedef struct cfg_final_block {
	uint64_t address;
	uint64_t end;
	cs_cfg_terminator terminator;
	uint32_t instruction_count;
	uint32_t overlap_count;
	cfg_absval out[CFG_NGPR];
} cfg_final_block;

static int cfg_cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
	return x < y ? -1 : x > y ? 1 : 0;
}

static int cfg_cmp_block(const void *a, const void *b)
{
	const cs_cfg_block *x = a, *y = b;
	return cfg_cmp_u64(&x->address, &y->address);
}

static int cfg_cmp_edge(const void *a, const void *b)
{
	const cs_cfg_edge *x = a, *y = b;
	if (x->source != y->source)
		return x->source < y->source ? -1 : 1;
	if (x->kind != y->kind)
		return (int)x->kind - (int)y->kind;
	if (x->target != y->target)
		return x->target < y->target ? -1 : 1;
	return 0;
}

static int cfg_cmp_indirect(const void *a, const void *b)
{
	const cs_cfg_indirect_target *x = a, *y = b;
	if (x->site != y->site)
		return x->site < y->site ? -1 : 1;
	return cfg_cmp_u64(&x->target, &y->target);
}

static int cfg_cmp_overlap(const void *a, const void *b)
{
	const cs_cfg_overlap *x = a, *y = b;
	if (x->start != y->start)
		return x->start < y->start ? -1 : 1;
	if (x->first != y->first)
		return x->first < y->first ? -1 : 1;
	if (x->second != y->second)
		return x->second < y->second ? -1 : 1;
	if (x->end != y->end)
		return x->end < y->end ? -1 : 1;
	return 0;
}

static int cfg_cmp_diag(const void *a, const void *b)
{
	const cs_cfg_diagnostic *x = a, *y = b;
	if (x->address != y->address)
		return x->address < y->address ? -1 : 1;
	if (x->kind != y->kind)
		return (int)x->kind - (int)y->kind;
	return 0;
}

static int cfg_cmp_final_block(const void *a, const void *b)
{
	const cfg_final_block *x = a, *y = b;
	return cfg_cmp_u64(&x->address, &y->address);
}

static bool cfg_leader_contains(const uint64_t *leaders, size_t n, uint64_t a)
{
	size_t lo = 0, hi = n;
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		if (leaders[mid] == a)
			return true;
		if (leaders[mid] < a)
			lo = mid + 1;
		else
			hi = mid;
	}
	return false;
}

static cs_err cfg_finalize(cfg_engine *e, cs_cfg_report **out)
{
	cs_cfg_report *rep;
	uint64_t *leaders = NULL;
	size_t leaders_count = 0, leaders_cap = 0;
	cfg_final_block *fblocks = NULL;
	size_t fblocks_count = 0, fblocks_cap = 0;
	cs_cfg_edge *all_edges;
	cs_cfg_indirect_target *all_indirect;
	cs_cfg_overlap *all_overlaps;
	cs_cfg_diagnostic *all_diag;
	size_t i, j;
	cfg_arr kept;

	memset(&kept, 0, sizeof(kept));
	rep = calloc(1, sizeof(*rep));
	if (!rep)
		return CS_ERR_MEM;

	/* leaders: entries plus every edge target that has a decoded node */
	for (i = 0; i < e->entry_count; ++i) {
		uint64_t *p = cfg_grow(leaders, &leaders_cap,
				leaders_count + 1, sizeof(*leaders));
		if (!p) {
			free(leaders);
			free(rep);
			return CS_ERR_MEM;
		}
		leaders = p;
		leaders[leaders_count++] = e->entries[i];
	}
	all_edges = e->edges.data;
	for (i = 0; i < e->edges.count; ++i) {
		uint64_t t = all_edges[i].target;
		uint64_t *p;
		/* internal fallthrough edges are absorbed into blocks; only
		 * real control transfers create new block leaders */
		if (all_edges[i].kind == CS_CFG_EDGE_FALLTHROUGH)
			continue;
		if (!cfg_node_find(e, t))
			continue;
		p = cfg_grow(leaders, &leaders_cap, leaders_count + 1,
				sizeof(*leaders));
		if (!p) {
			free(leaders);
			free(rep);
			return CS_ERR_MEM;
		}
		leaders = p;
		leaders[leaders_count++] = t;
	}
	qsort(leaders, leaders_count, sizeof(*leaders), cfg_cmp_u64);
	{
		size_t w = 0;
		for (i = 0; i < leaders_count; ++i) {
			if (w == 0 || leaders[i] != leaders[w - 1])
				leaders[w++] = leaders[i];
		}
		leaders_count = w;
	}

	/* walk each leader into a basic block */
	for (i = 0; i < leaders_count; ++i) {
		cfg_node *cur = cfg_node_find(e, leaders[i]);
		cfg_final_block *fb;
		cfg_final_block *p;

		if (!cur || !cur->decoded || !cur->insn.valid)
			continue;
		p = cfg_grow(fblocks, &fblocks_cap, fblocks_count + 1,
				sizeof(*fblocks));
		if (!p) {
			free(fblocks);
			free(leaders);
			free(rep);
			return CS_ERR_MEM;
		}
		fblocks = p;
		fb = &fblocks[fblocks_count++];
		memset(fb, 0, sizeof(*fb));
		fb->address = cur->addr;
		fb->terminator = CS_CFG_TERM_FALLTHROUGH;
		for (;;) {
			uint64_t nxt;
			++fb->instruction_count;
			fb->end = cur->addr + cur->insn.size;
			fb->terminator = (cs_cfg_terminator)cur->term.kind;
			memcpy(fb->out, cur->out, sizeof(fb->out));
			if (cur->term.kind != CS_CFG_TERM_FALLTHROUGH)
				break;
			nxt = cur->addr + cur->insn.size;
			if (cfg_leader_contains(leaders, leaders_count, nxt))
				break;
			cur = cfg_node_find(e, nxt);
			if (!cur || !cur->decoded || !cur->insn.valid)
				break;
		}
	}

	qsort(fblocks, fblocks_count, sizeof(*fblocks), cfg_cmp_final_block);

	/* overlap starts strictly inside each block's byte range */
	for (i = 0; i < fblocks_count; ++i) {
		uint32_t cnt = 0;
		for (j = 0; j < e->nodes_cap; ++j) {
			cfg_node *nn = &e->nodes[j];
			if (!nn->used || !nn->decoded || !nn->insn.valid ||
					!nn->overlap_start)
				continue;
			if (nn->addr > fblocks[i].address &&
					nn->addr < fblocks[i].end)
				++cnt;
		}
		fblocks[i].overlap_count = cnt;
	}

	rep->block_count = fblocks_count;
	if (fblocks_count) {
		rep->blocks = malloc(fblocks_count * sizeof(*rep->blocks));
		rep->registers = malloc(fblocks_count * CFG_NGPR *
				sizeof(*rep->registers));
		if (!rep->blocks || !rep->registers)
			goto mem_fail;
		for (i = 0; i < fblocks_count; ++i) {
			rep->blocks[i].address = fblocks[i].address;
			rep->blocks[i].end = fblocks[i].end;
			rep->blocks[i].terminator = fblocks[i].terminator;
			rep->blocks[i].instruction_count =
				fblocks[i].instruction_count;
			rep->blocks[i].overlap_count =
				fblocks[i].overlap_count;
			for (j = 0; j < CFG_NGPR; ++j) {
				cs_cfg_register_value *rv =
					&rep->registers[i * CFG_NGPR + j];
				rv->reg = cfg_gp_regs[j];
				if (fblocks[i].out[j].unknown) {
					rv->kind = CS_CFG_VALUE_UNKNOWN;
					rv->count = 0;
				} else {
					rv->kind = CS_CFG_VALUE_SET;
					rv->count = fblocks[i].out[j].count;
					memcpy(rv->values,
							fblocks[i].out[j].vals,
							rv->count *
								sizeof(uint64_t));
				}
			}
		}
		rep->register_count = fblocks_count * CFG_NGPR;
		qsort(rep->blocks, rep->block_count, sizeof(*rep->blocks),
				cfg_cmp_block);
	}

	/* remap terminator edges from instruction addresses to block starts;
	 * internal fallthrough edges are absorbed into the blocks */
	{
		memset(&kept, 0, sizeof(kept));
		for (i = 0; i < fblocks_count; ++i) {
			cfg_node *cur = cfg_node_find(e, fblocks[i].address);
			uint64_t last_addr;
			while (cur && cur->decoded && cur->insn.valid &&
					cur->addr + cur->insn.size <
						fblocks[i].end) {
				cur = cfg_node_find(e,
						cur->addr + cur->insn.size);
			}
			last_addr = cur ? cur->addr : fblocks[i].address;
			for (j = 0; j < e->edges.count; ++j) {
				cs_cfg_edge g;
				if (all_edges[j].source != last_addr)
					continue;
				g = all_edges[j];
				g.source = fblocks[i].address;
				if (!cfg_arr_push(&kept, &g, sizeof(g)))
					goto mem_fail;
			}
		}
		rep->edges = kept.data;
		rep->edge_count = kept.count;
		qsort(rep->edges, rep->edge_count, sizeof(*rep->edges),
				cfg_cmp_edge);
	}

	all_indirect = e->indirect.data;
	if (e->indirect.count) {
		rep->indirect_targets = malloc(e->indirect.count *
				sizeof(*rep->indirect_targets));
		if (!rep->indirect_targets)
			goto mem_fail;
		memcpy(rep->indirect_targets, all_indirect,
				e->indirect.count *
					sizeof(*rep->indirect_targets));
		rep->indirect_target_count = e->indirect.count;
		qsort(rep->indirect_targets, rep->indirect_target_count,
				sizeof(*rep->indirect_targets),
				cfg_cmp_indirect);
	}

	all_overlaps = e->overlaps.data;
	if (e->overlaps.count) {
		rep->overlaps = malloc(e->overlaps.count *
				sizeof(*rep->overlaps));
		if (!rep->overlaps)
			goto mem_fail;
		memcpy(rep->overlaps, all_overlaps,
				e->overlaps.count * sizeof(*rep->overlaps));
		rep->overlap_count = e->overlaps.count;
		qsort(rep->overlaps, rep->overlap_count,
				sizeof(*rep->overlaps), cfg_cmp_overlap);
	}

	all_diag = e->diagnostics.data;
	if (e->diagnostics.count) {
		rep->diagnostics = malloc(e->diagnostics.count *
				sizeof(*rep->diagnostics));
		if (!rep->diagnostics)
			goto mem_fail;
		memcpy(rep->diagnostics, all_diag,
				e->diagnostics.count *
					sizeof(*rep->diagnostics));
		rep->diagnostic_count = e->diagnostics.count;
		qsort(rep->diagnostics, rep->diagnostic_count,
				sizeof(*rep->diagnostics), cfg_cmp_diag);
	}

	if (e->entry_count) {
		rep->entry_points = malloc(e->entry_count *
				sizeof(*rep->entry_points));
		if (!rep->entry_points)
			goto mem_fail;
		memcpy(rep->entry_points, e->entries,
				e->entry_count * sizeof(*rep->entry_points));
		rep->entry_count = e->entry_count;
		qsort(rep->entry_points, rep->entry_count,
				sizeof(*rep->entry_points), cfg_cmp_u64);
		{
			size_t w = 0;
			for (i = 0; i < rep->entry_count; ++i) {
				if (w == 0 ||
						rep->entry_points[i] !=
							rep->entry_points[w - 1])
					rep->entry_points[w++] =
						rep->entry_points[i];
			}
			rep->entry_count = w;
		}
	}

	rep->incomplete = rep->diagnostic_count != 0;
	free(fblocks);
	free(leaders);
	*out = rep;
	return CS_ERR_OK;

mem_fail:
	if (rep->edges != kept.data)
		free(kept.data);
	free(rep->blocks);
	free(rep->edges);
	free(rep->indirect_targets);
	free(rep->overlaps);
	free(rep->diagnostics);
	free(rep->entry_points);
	free(rep->registers);
	free(rep);
	free(fblocks);
	free(leaders);
	return CS_ERR_MEM;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

static bool cfg_entry_executable(const cfg_engine *e, uint64_t addr)
{
	const cs_cfg_region *r = cfg_region_at(e, addr);
	return r != NULL && r->executable;
}

cs_err CAPSTONE_API cs_cfg_recover_x86_64(const cs_cfg_region *regions,
		size_t region_count,
		const uint64_t *entries, size_t entry_count,
		cs_cfg_report **report)
{
	cfg_engine e;
	cs_err cerr;
	size_t i, j;
	cfg_absval initial[CFG_NGPR];

	if (!report)
		return CS_ERR_OPTION;
	*report = NULL;

	if (region_count > 0 && !regions)
		return CS_ERR_OPTION;
	if (entry_count > 0 && !entries)
		return CS_ERR_OPTION;
	for (i = 0; i < region_count; ++i) {
		const cs_cfg_region *r = &regions[i];
		if (!r->data || r->size == 0)
			return CS_ERR_OPTION;
		/* wrapping interval */
		if (r->address + r->size - 1 < r->address)
			return CS_ERR_OPTION;
	}
	/* overlap check (region lists are small) */
	for (i = 0; i < region_count; ++i) {
		for (j = i + 1; j < region_count; ++j) {
			const cs_cfg_region *a = &regions[i];
			const cs_cfg_region *b = &regions[j];
			if (a->address < b->address + b->size &&
					b->address < a->address + a->size)
				return CS_ERR_OPTION;
		}
	}

	memset(&e, 0, sizeof(e));
	e.regions = regions;
	e.region_count = region_count;
	e.entries = entries;
	e.entry_count = entry_count;

	for (i = 0; i < entry_count; ++i) {
		if (!cfg_entry_executable(&e, entries[i]))
			return CS_ERR_OPTION;
	}

	cerr = cs_open(CS_ARCH_X86, CS_MODE_64, &e.handle);
	if (cerr != CS_ERR_OK)
		return cerr;
	cerr = cs_option(e.handle, CS_OPT_DETAIL, CS_OPT_ON);
	if (cerr != CS_ERR_OK) {
		cs_close(&e.handle);
		return cerr;
	}
	e.insn_cache = cs_malloc(e.handle);
	if (!e.insn_cache) {
		cs_close(&e.handle);
		return CS_ERR_MEM;
	}

	/* empty entry list: empty graph */
	if (entry_count == 0) {
		cs_free(e.insn_cache, 1);
		cs_close(&e.handle);
		*report = calloc(1, sizeof(cs_cfg_report));
		if (!*report)
			return CS_ERR_MEM;
		return CS_ERR_OK;
	}

	for (i = 0; i < CFG_NGPR; ++i)
		val_set_unknown(&initial[i]);

	for (i = 0; i < entry_count; ++i) {
		if (!cfg_enqueue(&e, entries[i], initial))
			goto oom;
	}

	while (e.work_head < e.work_count) {
		cfg_work_item item = e.work[e.work_head++];
		cfg_node *n = cfg_node_find(&e, item.addr);
		bool changed = false;

		if (!n) {
			n = cfg_node_get(&e, item.addr);
			if (!n)
				goto oom;
		}
		if (!n->decoded) {
			n = cfg_decode(&e, item.addr);
			if (!n)
				goto oom;
			memcpy(n->in, item.state, sizeof(n->in));
			changed = true;
		} else {
			for (j = 0; j < CFG_NGPR; ++j) {
				if (val_merge(&n->in[j], &item.state[j]))
					changed = true;
			}
		}
		if (!changed)
			continue;
		cfg_exec(&e, n, n->in);
		if (e.oom)
			goto oom;
	}

	cerr = cfg_finalize(&e, report);
	cs_free(e.insn_cache, 1);
	cs_close(&e.handle);
	free(e.nodes);
	free(e.work);
	free(e.edges.data);
	free(e.indirect.data);
	free(e.overlaps.data);
	free(e.diagnostics.data);
	return cerr;

oom:
	cs_free(e.insn_cache, 1);
	cs_close(&e.handle);
	free(e.nodes);
	free(e.work);
	free(e.edges.data);
	free(e.indirect.data);
	free(e.overlaps.data);
	free(e.diagnostics.data);
	return CS_ERR_MEM;
}

void CAPSTONE_API cs_cfg_report_free(cs_cfg_report *report)
{
	if (!report)
		return;
	free(report->entry_points);
	free(report->blocks);
	free(report->edges);
	free(report->indirect_targets);
	free(report->overlaps);
	free(report->diagnostics);
	free(report->registers);
	free(report);
}
