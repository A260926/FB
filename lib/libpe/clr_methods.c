/*
    libpe - the PE library

    Copyright (C) 2010 - 2024 libpe authors

    This file is part of libpe.

    libpe is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    libpe is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with libpe.  If not, see <http://www.gnu.org/licenses/>.
*/

/*
 * Static CLR method index (ECMA-335 Partition II, sections 22, 24 and 25).
 *
 * Read-only analysis: the CLR is never loaded, assemblies are never resolved
 * and CIL is never interpreted. Every byte the index exposes is copied into
 * owned storage, so pe_clr_methods_free() is the only lifetime dependency a
 * caller keeps after pe_clr_methods_build() returns.
 */

#include <libpe/clr_methods.h>

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

#define LOC_NONE UINT64_MAX

/* Metadata table numbers (ECMA-335 II.22). */
#define T_MODULE       0
#define T_TYPEREF      1
#define T_TYPEDEF      2
#define T_FIELDPTR     3
#define T_FIELD        4
#define T_METHODPTR    5
#define T_METHODDEF    6
#define T_PARAMPTR     7
#define T_PARAM        8
#define T_ASSEMBLY     32
#define T_ASSEMBLYREF  35
#define T_NESTEDCLASS  41
#define T_TABLE_COUNT  64

enum col_kind {
	COL_U2,
	COL_U4,
	COL_STRING,    /* #Strings heap index */
	COL_GUID,      /* #GUID heap index */
	COL_BLOB,      /* #Blob heap index */
	COL_FIELD,     /* simple Field index */
	COL_METHOD,    /* simple MethodDef index */
	COL_PARAM,     /* simple Param index */
	COL_TYPEDEF,   /* simple TypeDef index */
	COL_TDOR,      /* TypeDefOrRef coded index (2/1/27, 2 tag bits) */
	COL_RSCOPE     /* ResolutionScope coded index (0/26/35/1, 2 bits) */
};

typedef struct {
	const unsigned char *base;
	size_t size;
} map_view_t;

typedef struct {
	uint32_t offset;     /* relative to the metadata root */
	uint32_t size;
	uint64_t file_base;  /* physical file offset of the stream's first byte */
	const unsigned char *p;
} heap_stream_t;

typedef struct {
	uint32_t rows;
	const unsigned char *ptr; /* row 1, inside the retained root copy */
	uint64_t base;            /* physical file offset of row 1 */
	size_t row_size;
	unsigned ncols;
	const enum col_kind *cols;
	size_t col_off[10];
} meta_table_t;

typedef struct {
	uint32_t va;
	uint32_t virtual_size;
	uint32_t raw_ptr;
	uint32_t raw_size;
} raw_section_t;

typedef struct {
	const pe_ctx_t *ctx;
	map_view_t map;
	raw_section_t *sections;
	uint32_t section_count;
	uint32_t headers_size;
	int is_pe32_plus;
} build_env_t;

static uint16_t rd16(const unsigned char *p) {
	return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const unsigned char *p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
		| ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const unsigned char *p) {
	uint64_t v = 0;
	for (unsigned i = 0; i < 8; i++)
		v |= (uint64_t)p[i] << (8 * i);
	return v;
}

static int map_in(const map_view_t *m, uint64_t off, size_t len) {
	return off <= m->size && len <= m->size - (size_t)off;
}

/* The legacy libpe directory accessor predates const-correct callers. */
static IMAGE_DATA_DIRECTORY *clr_directory(const pe_ctx_t *ctx) {
	return pe_directory_by_entry((pe_ctx_t *)(uintptr_t)ctx,
		IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR);
}

static pe_clr_methods_err_e fail(pe_clr_methods_error_t *e,
	pe_clr_methods_err_e code, uint64_t location, const char *message) {
	if (e != NULL) {
		e->code = code;
		e->location = location;
		e->message = message;
	}
	return code;
}

const char *pe_clr_methods_error_msg(pe_clr_methods_err_e code) {
	switch (code) {
	case PE_CLR_METHODS_E_OK:
		return "ok";
	case PE_CLR_METHODS_E_ALLOCATION:
		return "memory allocation failed";
	case PE_CLR_METHODS_E_MALFORMED:
		return "malformed CLR metadata or method body";
	case PE_CLR_METHODS_E_UNSUPPORTED:
		return "unsupported CLR structure";
	}
	return "unknown CLR method index error";
}

static void *xdup(const void *src, size_t len) {
	void *dst = malloc(len ? len : 1);
	if (dst == NULL)
		return NULL;
	if (len)
		memcpy(dst, src, len);
	return dst;
}

static char *xstr(const char *s) {
	size_t n = strlen(s) + 1;
	char *p = malloc(n);
	if (p != NULL)
		memcpy(p, s, n);
	return p;
}

/* ------------------------------------------------------------------ */
/* Strict virtual-to-raw mapping with explicit span splitting          */
/* ------------------------------------------------------------------ */

/*
 * Every byte of a virtual range must come from a section's raw data or from
 * the PE headers region. Virtual-only tails, holes and overlapping sections
 * are rejected instead of being guessed at.
 */
static int section_contains_raw(const raw_section_t *s, uint32_t rva,
	size_t len) {
	uint64_t virt_end = (uint64_t)s->va + s->virtual_size;
	if ((uint64_t)rva < s->va || (uint64_t)rva + len > virt_end)
		return 0;
	uint64_t delta = (uint64_t)rva - s->va;
	if (delta > s->raw_size)
		return 0;
	return len <= (size_t)(s->raw_size - (uint32_t)delta);
}

/* Returns 1 with *out filled, 0 when unbacked, -1 on ambiguous overlap. */
static int find_raw_section(const build_env_t *env, uint32_t rva,
	const raw_section_t **out) {
	int found = 0;
	for (uint32_t i = 0; i < env->section_count; i++) {
		if (section_contains_raw(&env->sections[i], rva, 1)) {
			if (found)
				return -1;
			out[found++] = &env->sections[i];
		}
	}
	return found;
}

/*
 * Builds the physical spans backing [rva, rva+size). Adjacent virtual section
 * ranges with non-adjacent raw offsets are followed; anything unbacked fails.
 * The returned spans reference the mapped file, no bytes are copied.
 */
static pe_clr_methods_err_e build_spans(build_env_t *env, uint32_t rva,
	uint32_t size, pe_clr_span_t **out_spans, size_t *out_count,
	pe_clr_methods_error_t *e) {
	pe_clr_span_t *spans = calloc((size_t)env->section_count + 2,
		sizeof(*spans));
	if (spans == NULL)
		return fail(e, PE_CLR_METHODS_E_ALLOCATION, LOC_NONE,
			"span array allocation failed");

	size_t n = 0;
	uint32_t left = size;
	uint64_t cur = rva;

	while (left > 0) {
		const raw_section_t *sec = NULL;
		int rc = find_raw_section(env, (uint32_t)cur, &sec);
		if (rc < 0) {
			free(spans);
			return fail(e, PE_CLR_METHODS_E_MALFORMED, cur,
				"ambiguous RVA, multiple sections claim it");
		}
		if (rc == 0) {
			/* PE headers may hold metadata, but only inside SizeOfHeaders. */
			if (env->headers_size > 0 && cur < env->headers_size
				&& (uint64_t)cur + left <= env->headers_size) {
				spans[n].file_offset = cur;
				spans[n].rva = (uint32_t)cur;
				spans[n].size = left;
				spans[n].section_index = PE_CLR_NO_SECTION;
				n++;
				break;
			}
			free(spans);
			return fail(e, PE_CLR_METHODS_E_MALFORMED, cur,
				"RVA has no raw backing (hole or virtual-only tail)");
		}

		uint64_t delta = cur - sec->va;
		uint32_t raw_avail = sec->raw_size - (uint32_t)delta;
		/* Never read past the virtual extent into raw padding. */
		uint32_t virt_avail = sec->virtual_size - (uint32_t)delta;
		uint32_t take = left < raw_avail ? left : raw_avail;
		if (virt_avail < take)
			take = virt_avail;
		spans[n].file_offset = (uint64_t)sec->raw_ptr + delta;
		spans[n].rva = (uint32_t)cur;
		spans[n].size = take;
		spans[n].section_index = (uint32_t)(sec - env->sections);
		n++;
		left -= take;
		cur += take;

		if (left > 0) {
			/* The next virtual byte must open the very next section. */
			uint32_t expect_va = sec->va + sec->virtual_size;
			if ((uint32_t)cur != expect_va) {
				free(spans);
				return fail(e, PE_CLR_METHODS_E_MALFORMED, cur,
					"section raw data ends before the requested extent");
			}
			int next = 0;
			for (uint32_t i = 0; i < env->section_count; i++) {
				if (env->sections[i].va == expect_va) {
					next = 1;
					break;
				}
			}
			if (!next) {
				free(spans);
				return fail(e, PE_CLR_METHODS_E_MALFORMED, cur,
					"no adjacent section continues the virtual range");
			}
		}
	}

	*out_spans = spans;
	*out_count = n;
	return PE_CLR_METHODS_E_OK;
}

/* Copies a validated span list into a single owned buffer. */
static pe_clr_methods_err_e copy_spans(build_env_t *env,
	const pe_clr_span_t *spans, size_t count, uint32_t total,
	unsigned char **out, pe_clr_methods_error_t *e) {
	unsigned char *buf = malloc(total ? total : 1);
	if (buf == NULL)
		return fail(e, PE_CLR_METHODS_E_ALLOCATION, LOC_NONE,
			"byte copy allocation failed");
	uint32_t done = 0;
	for (size_t i = 0; i < count; i++) {
		if (spans[i].size == 0)
			continue;
		if (!map_in(&env->map, spans[i].file_offset, spans[i].size)
			|| done > total - spans[i].size) {
			free(buf);
			return fail(e, PE_CLR_METHODS_E_MALFORMED,
				spans[i].file_offset, "span leaves the mapped file");
		}
		memcpy(buf + done, env->map.base + spans[i].file_offset,
			spans[i].size);
		done += spans[i].size;
	}
	if (done != total) {
		free(buf);
		return fail(e, PE_CLR_METHODS_E_MALFORMED, LOC_NONE,
			"span list does not cover the requested extent");
	}
	*out = buf;
	return PE_CLR_METHODS_E_OK;
}

/* Span for a metadata row, identified by its physical file offset. */
static pe_clr_span_t file_span(build_env_t *env, uint64_t off, size_t len) {
	pe_clr_span_t s;
	s.file_offset = off;
	s.rva = 0;
	s.size = (uint32_t)len;
	s.section_index = PE_CLR_NO_SECTION;
	for (uint32_t i = 0; i < env->section_count; i++) {
		const raw_section_t *sec = &env->sections[i];
		if (off >= sec->raw_ptr
			&& off + len <= (uint64_t)sec->raw_ptr + sec->raw_size) {
			s.rva = (uint32_t)(sec->va + (off - sec->raw_ptr));
			s.section_index = i;
			break;
		}
	}
	if (s.section_index == PE_CLR_NO_SECTION
		&& off + len <= env->headers_size)
		s.rva = (uint32_t)off;
	return s;
}

/* ------------------------------------------------------------------ */
/* Table schemas and index widths                                      */
/* ------------------------------------------------------------------ */

static const enum col_kind schema_module[] = {
	COL_U2, COL_STRING, COL_GUID, COL_GUID, COL_GUID
};
static const enum col_kind schema_typeref[] = {
	COL_RSCOPE, COL_STRING, COL_STRING
};
static const enum col_kind schema_typedef[] = {
	COL_U4, COL_STRING, COL_STRING, COL_TDOR, COL_FIELD, COL_METHOD
};
static const enum col_kind schema_fieldptr[] = { COL_FIELD };
static const enum col_kind schema_field[] = {
	COL_U4, COL_STRING, COL_BLOB
};
static const enum col_kind schema_methodptr[] = { COL_METHOD };
static const enum col_kind schema_methoddef[] = {
	COL_U4, COL_U2, COL_U2, COL_STRING, COL_BLOB, COL_PARAM
};
static const enum col_kind schema_paramptr[] = { COL_PARAM };
static const enum col_kind schema_param[] = { COL_U2, COL_U2, COL_STRING };
static const enum col_kind schema_assembly[] = {
	COL_U4, COL_U2, COL_U2, COL_U2, COL_U2, COL_U4, COL_BLOB,
	COL_STRING, COL_STRING
};
static const enum col_kind schema_assemblyref[] = {
	COL_U2, COL_U2, COL_U2, COL_U2, COL_U4, COL_BLOB, COL_STRING,
	COL_STRING, COL_BLOB
};
static const enum col_kind schema_nestedclass[] = {
	COL_TYPEDEF, COL_TYPEDEF
};

typedef struct {
	unsigned table;
	const enum col_kind *cols;
	unsigned ncols;
} schema_entry_t;

static const schema_entry_t schemas[] = {
	{ T_MODULE,      schema_module,      5 },
	{ T_TYPEREF,     schema_typeref,     3 },
	{ T_TYPEDEF,     schema_typedef,     6 },
	{ T_FIELDPTR,    schema_fieldptr,    1 },
	{ T_FIELD,       schema_field,       3 },
	{ T_METHODPTR,   schema_methodptr,   1 },
	{ T_METHODDEF,   schema_methoddef,   6 },
	{ T_PARAMPTR,    schema_paramptr,    1 },
	{ T_PARAM,       schema_param,       3 },
	{ T_ASSEMBLY,    schema_assembly,    9 },
	{ T_ASSEMBLYREF, schema_assemblyref, 9 },
	{ T_NESTEDCLASS, schema_nestedclass, 2 }
};

static const enum col_kind *schema_for(unsigned table, unsigned *ncols) {
	for (size_t i = 0; i < sizeof(schemas) / sizeof(schemas[0]); i++) {
		if (schemas[i].table == table) {
			*ncols = schemas[i].ncols;
			return schemas[i].cols;
		}
	}
	*ncols = 0;
	return NULL;
}

static unsigned simple_width(uint32_t target_rows) {
	return target_rows < 0x10000u ? 2 : 4;
}

static unsigned coded_width(const uint32_t rows[T_TABLE_COUNT],
	const unsigned tables[], unsigned ntables, unsigned tag_bits) {
	uint32_t max_rows = 0;
	for (unsigned i = 0; i < ntables; i++) {
		if (rows[tables[i]] > max_rows)
			max_rows = rows[tables[i]];
	}
	uint64_t limit = (uint64_t)1 << (16 - tag_bits);
	return (uint64_t)max_rows < limit ? 2 : 4;
}

static unsigned column_width(enum col_kind kind,
	const uint32_t rows[T_TABLE_COUNT], uint8_t heap_sizes) {
	static const unsigned tdor_tables[] = { T_TYPEDEF, T_TYPEREF, 27 };
	static const unsigned rscope_tables[] = {
		T_MODULE, 26, T_ASSEMBLYREF, T_TYPEREF
	};
	switch (kind) {
	case COL_U2:
		return 2;
	case COL_U4:
		return 4;
	case COL_STRING:
		return (heap_sizes & 0x01) ? 4 : 2;
	case COL_GUID:
		return (heap_sizes & 0x02) ? 4 : 2;
	case COL_BLOB:
		return (heap_sizes & 0x04) ? 4 : 2;
	case COL_FIELD:
		return simple_width(rows[T_FIELD]);
	case COL_METHOD:
		return simple_width(rows[T_METHODDEF]);
	case COL_PARAM:
		return simple_width(rows[T_PARAM]);
	case COL_TYPEDEF:
		return simple_width(rows[T_TYPEDEF]);
	case COL_TDOR:
		return coded_width(rows, tdor_tables, 3, 2);
	case COL_RSCOPE:
		return coded_width(rows, rscope_tables, 4, 2);
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Heap reads                                                          */
/* ------------------------------------------------------------------ */

static char *read_string(const map_view_t *map, const heap_stream_t *hs,
	uint32_t idx, pe_clr_methods_error_t *e) {
	(void)map;
	if (idx >= hs->size) {
		fail(e, PE_CLR_METHODS_E_MALFORMED, hs->file_base + idx,
			"#Strings index leaves the heap");
		return NULL;
	}
	const unsigned char *p = hs->p + idx;
	size_t max = hs->size - idx;
	size_t n = 0;
	while (n < max && p[n] != 0)
		n++;
	if (n == max) {
		fail(e, PE_CLR_METHODS_E_MALFORMED, hs->file_base + idx,
			"unterminated #Strings entry");
		return NULL;
	}
	char *out = malloc(n + 1);
	if (out == NULL) {
		fail(e, PE_CLR_METHODS_E_ALLOCATION, LOC_NONE,
			"string allocation failed");
		return NULL;
	}
	memcpy(out, p, n);
	out[n] = '\0';
	return out;
}

/* Decodes an ECMA-335 II.23.2 compressed unsigned length. */
static int decode_compressed(const unsigned char *p, size_t avail,
	uint32_t *value, unsigned *prefix) {
	if (avail == 0)
		return -1;
	uint8_t b0 = p[0];
	if ((b0 & 0x80) == 0) {
		*value = b0;
		*prefix = 1;
		return 0;
	}
	if ((b0 & 0xC0) == 0x80) {
		if (avail < 2)
			return -1;
		*value = ((uint32_t)(b0 & 0x3f) << 8) | p[1];
		*prefix = 2;
		return 0;
	}
	if ((b0 & 0xE0) == 0xC0) {
		if (avail < 4)
			return -1;
		*value = ((uint32_t)(b0 & 0x1f) << 24)
			| ((uint32_t)p[1] << 16)
			| ((uint32_t)p[2] << 8) | p[3];
		*prefix = 4;
		return 0;
	}
	return -1;
}

static unsigned char *read_blob(const heap_stream_t *hs, uint32_t idx,
	size_t *out_len, pe_clr_methods_error_t *e) {
	*out_len = 0;
	if (idx == 0)
		return NULL; /* NULL blob */
	if (idx >= hs->size) {
		fail(e, PE_CLR_METHODS_E_MALFORMED, hs->file_base + idx,
			"#Blob index leaves the heap");
		return NULL;
	}
	const unsigned char *p = hs->p + idx;
	size_t avail = hs->size - idx;
	uint32_t len;
	unsigned prefix;
	if (decode_compressed(p, avail, &len, &prefix) != 0) {
		fail(e, PE_CLR_METHODS_E_MALFORMED, hs->file_base + idx,
			"bad compressed blob length");
		return NULL;
	}
	if ((uint64_t)prefix + len > avail) {
		fail(e, PE_CLR_METHODS_E_MALFORMED, hs->file_base + idx,
			"blob payload leaves the heap");
		return NULL;
	}
	unsigned char *out = xdup(p + prefix, len);
	if (out == NULL) {
		fail(e, PE_CLR_METHODS_E_ALLOCATION, LOC_NONE,
			"blob allocation failed");
		return NULL;
	}
	*out_len = len;
	return out;
}

/* ------------------------------------------------------------------ */
/* Raw parsed rows                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
	uint32_t flags;
	uint32_t name_idx;
	uint32_t namespace_idx;
	uint32_t method_start;
	uint64_t row_off;
	size_t row_size;
} typedef_rec_t;

typedef struct {
	uint32_t rva;
	uint16_t impl_flags;
	uint16_t flags;
	uint32_t name_idx;
	uint32_t sig_idx;
	uint32_t param_start;
	uint64_t row_off;
	size_t row_size;
} method_rec_t;

typedef struct {
	uint16_t flags;
	uint16_t sequence;
	uint32_t name_idx;
	uint64_t row_off;
	size_t row_size;
} param_rec_t;

static const unsigned char *cell_ptr(const meta_table_t *t, uint32_t rid,
	unsigned col) {
	return t->ptr + (size_t)(rid - 1) * t->row_size + t->col_off[col];
}

static uint32_t cell_u32(const meta_table_t *t, uint32_t rid,
	unsigned col) {
	const unsigned char *p = cell_ptr(t, rid, col);
	return t->col_off[col + 1] - t->col_off[col] == 2 ? rd16(p) : rd32(p);
}

/* ------------------------------------------------------------------ */
/* Full names through NestedClass, independent of row order            */
/* ------------------------------------------------------------------ */

/* rid is a 1-based TypeDef RID; all arrays are 0-based. */
static char *build_full_name(char **names, char **namespaces,
	uint32_t *parent, uint32_t rid, char **memo,
	pe_clr_methods_error_t *e) {
	size_t i = (size_t)rid - 1;
	if (memo[i] != NULL)
		return memo[i];

	char *own = names[i];
	if (parent[i] == 0) {
		const char *ns = namespaces[i];
		size_t nsl = strlen(ns);
		size_t nl = strlen(own);
		char *full = malloc(nsl + (nsl ? 1 : 0) + nl + 1);
		if (full == NULL) {
			fail(e, PE_CLR_METHODS_E_ALLOCATION, LOC_NONE,
				"type name allocation failed");
			return NULL;
		}
		char *w = full;
		if (nsl) {
			memcpy(w, ns, nsl);
			w += nsl;
			*w++ = '.';
		}
		memcpy(w, own, nl + 1);
		memo[i] = full;
		return full;
	}

	char *outer = build_full_name(names, namespaces, parent,
		parent[i], memo, e);
	if (outer == NULL)
		return NULL;
	size_t ol = strlen(outer);
	size_t nl = strlen(own);
	char *full = malloc(ol + 1 + nl + 1);
	if (full == NULL) {
		fail(e, PE_CLR_METHODS_E_ALLOCATION, LOC_NONE,
			"nested type name allocation failed");
		return NULL;
	}
	memcpy(full, outer, ol);
	full[ol] = '+';
	memcpy(full + ol + 1, own, nl + 1);
	memo[i] = full;
	return full;
}

/* ------------------------------------------------------------------ */
/* Method bodies                                                       */
/* ------------------------------------------------------------------ */

static pe_clr_methods_err_e parse_body(build_env_t *env, method_rec_t *rec,
	pe_clr_body_t *body, pe_clr_methods_error_t *e) {
	memset(body, 0, sizeof(*body));
	if (rec->rva == 0)
		return PE_CLR_METHODS_E_OK; /* abstract/interface/PInvoke: no body */

	/* Read the first byte to learn the header kind. */
	pe_clr_span_t *one = NULL;
	size_t one_n = 0;
	pe_clr_methods_err_e rc = build_spans(env, rec->rva, 1, &one, &one_n, e);
	if (rc != PE_CLR_METHODS_E_OK)
		return rc;
	if (!map_in(&env->map, one[0].file_offset, 1)) {
		uint64_t loc = one[0].file_offset;
		free(one);
		return fail(e, PE_CLR_METHODS_E_MALFORMED, loc,
			"method header leaves the mapped file");
	}
	uint8_t first = env->map.base[one[0].file_offset];
	free(one);

	uint32_t header_size;
	uint32_t code_size;
	body->header_rva = rec->rva;

	if ((first & 0x03) == 0x02) {
		/* Tiny header (II.25.2). */
		body->fat = 0;
		body->header_flags = first;
		body->max_stack = 8;
		body->local_var_sig_token = 0;
		header_size = 1;
		code_size = first >> 2;
	} else if ((first & 0x03) == 0x03) {
		/* Fat header: read the fixed 12-byte core first. */
		pe_clr_span_t *probe = NULL;
		size_t probe_n = 0;
		rc = build_spans(env, rec->rva, 12, &probe, &probe_n, e);
		if (rc != PE_CLR_METHODS_E_OK)
			return rc;
		unsigned char hdr[12];
		uint32_t done = 0;
		for (size_t i = 0; i < probe_n; i++) {
			if (!map_in(&env->map, probe[i].file_offset, probe[i].size)) {
				uint64_t loc = probe[i].file_offset;
				free(probe);
				return fail(e, PE_CLR_METHODS_E_MALFORMED, loc,
					"fat header leaves the mapped file");
			}
			memcpy(hdr + done,
				env->map.base + probe[i].file_offset, probe[i].size);
			done += probe[i].size;
		}
		free(probe);
		if (done != 12)
			return fail(e, PE_CLR_METHODS_E_MALFORMED, rec->rva,
				"truncated fat header");

		uint16_t flags = rd16(hdr);
		uint16_t dwords = flags >> 12;
		if (dwords < 3)
			return fail(e, PE_CLR_METHODS_E_MALFORMED, rec->rva,
				"fat header smaller than three DWORDs");
		body->fat = 1;
		body->header_flags = flags;
		body->max_stack = rd16(hdr + 2);
		code_size = rd32(hdr + 4);
		body->local_var_sig_token = rd32(hdr + 8);
		header_size = (uint32_t)dwords * 4u;
	} else {
		/* Native (0) and runtime (1) method code types are unsupported. */
		return fail(e, PE_CLR_METHODS_E_UNSUPPORTED, rec->rva,
			"native or runtime method code type");
	}

	/* Header extent follows the actual DWORD count, not a fixed size. */
	rc = build_spans(env, rec->rva, header_size,
		&body->header_spans, &body->header_span_count, e);
	if (rc != PE_CLR_METHODS_E_OK)
		return rc;
	body->header_size = header_size;

	uint64_t il_rva64 = (uint64_t)rec->rva + header_size;
	uint64_t end64 = il_rva64 + code_size;
	if (il_rva64 > UINT32_MAX || end64 > UINT32_MAX)
		return fail(e, PE_CLR_METHODS_E_MALFORMED, rec->rva,
			"method body overflows the RVA space");
	body->il_rva = (uint32_t)il_rva64;
	body->il_size = code_size;
	body->present = 1;

	if (code_size > 0) {
		rc = build_spans(env, body->il_rva, code_size,
			&body->il_spans, &body->il_span_count, e);
		if (rc != PE_CLR_METHODS_E_OK)
			return rc;
		rc = copy_spans(env, body->il_spans, body->il_span_count,
			code_size, &body->il, e);
		if (rc != PE_CLR_METHODS_E_OK)
			return rc;
	}
	return PE_CLR_METHODS_E_OK;
}

/* ------------------------------------------------------------------ */
/* Metadata layout                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
	uint32_t rows[T_TABLE_COUNT];
	meta_table_t tables[T_TABLE_COUNT];
	heap_stream_t tables_stream;
	heap_stream_t strings;
	heap_stream_t blob;
	heap_stream_t guid;
	int has_strings;
	int has_blob;
	int has_guid;
	int minus_mode; /* 1 for #-, 0 for #~ */
} metadata_t;

static void free_body(pe_clr_body_t *b) {
	free(b->header_spans);
	free(b->il_spans);
	free(b->il);
}

static void free_index_contents(pe_clr_methods_t *idx) {
	if (idx == NULL)
		return;
	for (size_t i = 0; i < idx->method_count; i++) {
		pe_clr_method_t *m = &idx->methods[i];
		free(m->type_name);
		free(m->name);
		free(m->signature);
		for (size_t j = 0; j < m->param_count; j++)
			free(m->params[j].name);
		free(m->params);
		free_body(&m->body);
	}
	free(idx->methods);
	idx->methods = NULL;
	idx->method_count = 0;
}

void pe_clr_methods_free(pe_clr_methods_t *index) {
	if (index == NULL)
		return;
	free_index_contents(index);
	free(index);
}

/*
 * Copies the metadata root (it may itself cross section boundaries) and
 * enumerates its streams. The retained copy stays alive until the whole index
 * has been assembled because every heap and table pointer references it.
 */
static pe_clr_methods_err_e locate_streams(build_env_t *env,
	uint32_t meta_rva, uint32_t meta_size, metadata_t *md,
	uint64_t *root_off_out, unsigned char **root_copy_out,
	pe_clr_methods_error_t *e) {
	pe_clr_span_t *spans = NULL;
	size_t span_count = 0;
	pe_clr_methods_err_e rc = build_spans(env, meta_rva, meta_size,
		&spans, &span_count, e);
	if (rc != PE_CLR_METHODS_E_OK)
		return rc;

	unsigned char *root = NULL;
	rc = copy_spans(env, spans, span_count, meta_size, &root, e);
	free(spans);
	if (rc != PE_CLR_METHODS_E_OK)
		return rc;

	pe_clr_span_t *anchor = NULL;
	size_t anchor_n = 0;
	rc = build_spans(env, meta_rva, 1, &anchor, &anchor_n, e);
	if (rc != PE_CLR_METHODS_E_OK) {
		free(root);
		return rc;
	}
	uint64_t root_off = anchor[0].file_offset;
	free(anchor);
	*root_off_out = root_off;
	*root_copy_out = root;

	if (meta_size < 16 || memcmp(root, "BSJB", 4) != 0)
		return fail(e, PE_CLR_METHODS_E_MALFORMED, root_off,
			"bad metadata root signature");
	uint32_t ver_len = rd32(root + 12);
	if ((uint64_t)16 + ver_len + 4 > meta_size)
		return fail(e, PE_CLR_METHODS_E_MALFORMED, root_off + 12,
			"version string leaves the metadata root");
	/* 2 bytes of flags, then the 2-byte stream count. */
	uint16_t stream_count = rd16(root + 16 + ver_len + 2);
	/* Version string, then 2 bytes of flags and the 2-byte stream count. */
	size_t pos = 16 + ver_len + 4;

	md->has_strings = md->has_blob = md->has_guid = 0;
	md->minus_mode = 0;
	int has_tables = 0;

	for (uint16_t i = 0; i < stream_count; i++) {
		if (pos + 8 > meta_size)
			return fail(e, PE_CLR_METHODS_E_MALFORMED,
				root_off + pos, "truncated stream header");
		uint32_t off = rd32(root + pos);
		uint32_t size = rd32(root + pos + 4);
		pos += 8;
		size_t name_start = pos;
		const char *name = (const char *)(root + pos);
		while (pos < meta_size && root[pos] != 0)
			pos++;
		if (pos >= meta_size)
			return fail(e, PE_CLR_METHODS_E_MALFORMED,
				root_off + name_start, "unterminated stream name");
		pos++; /* NUL */
		pos = (pos + 3) & ~(size_t)3;

		if ((uint64_t)off + size > meta_size)
			return fail(e, PE_CLR_METHODS_E_MALFORMED,
				root_off + name_start, "stream extent leaves the root");

		heap_stream_t hs;
		hs.offset = off;
		hs.size = size;
		hs.file_base = root_off + off;
		hs.p = root + off;

		if (strcmp(name, "#~") == 0 || strcmp(name, "#-") == 0) {
			if (has_tables)
				return fail(e, PE_CLR_METHODS_E_MALFORMED,
					root_off + name_start, "duplicate tables stream");
			has_tables = 1;
			md->minus_mode = (name[1] == '-');
			md->tables_stream = hs;
		} else if (strcmp(name, "#Strings") == 0) {
			md->strings = hs;
			md->has_strings = 1;
		} else if (strcmp(name, "#Blob") == 0) {
			md->blob = hs;
			md->has_blob = 1;
		} else if (strcmp(name, "#GUID") == 0) {
			md->guid = hs;
			md->has_guid = 1;
		}
	}

	if (!has_tables)
		return fail(e, PE_CLR_METHODS_E_MALFORMED, LOC_NONE,
			"no #~ or #- tables stream");
	if (!md->has_strings)
		return fail(e, PE_CLR_METHODS_E_MALFORMED, LOC_NONE,
			"missing #Strings heap");
	if (!md->has_blob)
		return fail(e, PE_CLR_METHODS_E_MALFORMED, LOC_NONE,
			"missing #Blob heap");
	return PE_CLR_METHODS_E_OK;
}

/* ------------------------------------------------------------------ */
/* Tables stream parsing and index assembly                            */
/* ------------------------------------------------------------------ */

static pe_clr_methods_err_e build_index(build_env_t *env,
	pe_clr_methods_t **out, pe_clr_methods_error_t *e) {
	pe_clr_methods_err_e rc;
	metadata_t md;
	memset(&md, 0, sizeof(md));
	unsigned char *root_copy = NULL;

	IMAGE_DATA_DIRECTORY *dir = clr_directory(env->ctx);
	uint32_t clr_rva = dir->VirtualAddress;
	uint32_t clr_size = dir->Size;

	/* CLI header (II.25.3.3), fixed 72 bytes. */
	pe_clr_span_t *clr_spans = NULL;
	size_t clr_span_n = 0;
	rc = build_spans(env, clr_rva, clr_size < 72 ? 72 : clr_size,
		&clr_spans, &clr_span_n, e);
	if (rc != PE_CLR_METHODS_E_OK)
		return rc;
	unsigned char clr_hdr[72];
	uint32_t got = 0;
	for (size_t i = 0; i < clr_span_n && got < 72; i++) {
		uint32_t take = clr_spans[i].size;
		if (got + take > 72)
			take = 72 - got;
		if (!map_in(&env->map, clr_spans[i].file_offset, take)) {
			uint64_t loc = clr_spans[i].file_offset;
			free(clr_spans);
			rc = fail(e, PE_CLR_METHODS_E_MALFORMED, loc,
				"CLI header leaves the mapped file");
			goto out;
		}
		memcpy(clr_hdr + got,
			env->map.base + clr_spans[i].file_offset, take);
		got += take;
	}
	free(clr_spans);
	if (got < 72) {
		rc = fail(e, PE_CLR_METHODS_E_MALFORMED, clr_rva,
			"truncated CLI header");
		goto out;
	}
	if (rd32(clr_hdr) != 72) {
		rc = fail(e, PE_CLR_METHODS_E_MALFORMED, clr_rva,
			"CLI header cb must be 72");
		goto out;
	}

	uint32_t meta_rva = rd32(clr_hdr + 8);
	uint32_t meta_size = rd32(clr_hdr + 12);
	if (meta_rva == 0 || meta_size < 16) {
		rc = fail(e, PE_CLR_METHODS_E_MALFORMED, clr_rva + 8,
			"bad CLI metadata directory");
		goto out;
	}

	uint64_t root_off = 0;
	rc = locate_streams(env, meta_rva, meta_size, &md, &root_off,
		&root_copy, e);
	if (rc != PE_CLR_METHODS_E_OK)
		goto out;

	const heap_stream_t *ts = &md.tables_stream;
	if (ts->size < 24) {
		rc = fail(e, PE_CLR_METHODS_E_MALFORMED, ts->file_base,
			"truncated tables stream header");
		goto out;
	}
	const unsigned char *tp = ts->p;
	size_t tlen = ts->size;
	if (rd32(tp) != 0) {
		rc = fail(e, PE_CLR_METHODS_E_MALFORMED, ts->file_base,
			"tables stream Reserved must be zero");
		goto out;
	}
	uint8_t heap_sizes = tp[6];
	uint64_t valid = rd64(tp + 8);
	/* sorted mask at +16; intentionally not needed */
	size_t hp = 24;

	uint32_t valid_count = 0;
	for (unsigned i = 0; i < T_TABLE_COUNT; i++)
		if (valid & ((uint64_t)1 << i))
			valid_count++;
	if ((uint64_t)hp + (uint64_t)valid_count * 4 > tlen) {
		rc = fail(e, PE_CLR_METHODS_E_MALFORMED,
			ts->file_base + hp, "row count array leaves the tables stream");
		goto out;
	}

	memset(md.rows, 0, sizeof(md.rows));
	for (unsigned i = 0; i < T_TABLE_COUNT; i++) {
		if (!(valid & ((uint64_t)1 << i)))
			continue;
		md.rows[i] = rd32(tp + hp);
		hp += 4;
	}

	/* #~ never carries pointer tables; #- is the only pointer layout. */
	if (!md.minus_mode
		&& (md.rows[T_FIELDPTR] || md.rows[T_METHODPTR]
			|| md.rows[T_PARAMPTR])) {
		rc = fail(e, PE_CLR_METHODS_E_UNSUPPORTED, ts->file_base,
			"pointer tables are only valid in a #- stream");
		goto out;
	}

	/* Compute row sizes and table bases in numeric table order. */
	size_t cursor = hp;
	for (unsigned t = 0; t < T_TABLE_COUNT; t++) {
		meta_table_t *mt = &md.tables[t];
		mt->rows = md.rows[t];
		mt->ptr = NULL;
		mt->base = 0;
		mt->row_size = 0;
		if (mt->rows == 0)
			continue;
		unsigned ncols;
		const enum col_kind *cols = schema_for(t, &ncols);
		if (cols == NULL) {
			rc = fail(e, PE_CLR_METHODS_E_UNSUPPORTED,
				ts->file_base + cursor,
				"nonempty unsupported metadata table");
			goto out;
		}
		mt->cols = cols;
		mt->ncols = ncols;
		size_t off = 0;
		for (unsigned c = 0; c < ncols; c++) {
			mt->col_off[c] = off;
			off += column_width(cols[c], md.rows, heap_sizes);
		}
		mt->col_off[ncols] = off;
		mt->row_size = off;
		uint64_t extent = (uint64_t)cursor + (uint64_t)mt->rows * off;
		if (extent > tlen) {
			rc = fail(e, PE_CLR_METHODS_E_MALFORMED,
				ts->file_base + cursor,
				"table rows leave the tables stream");
			goto out;
		}
		mt->ptr = tp + cursor;
		mt->base = ts->file_base + cursor;
		cursor = (size_t)extent;
	}

	const meta_table_t *td_t = &md.tables[T_TYPEDEF];
	const meta_table_t *m_t = &md.tables[T_METHODDEF];
	const meta_table_t *p_t = &md.tables[T_PARAM];
	const meta_table_t *mp_t = &md.tables[T_METHODPTR];
	const meta_table_t *pp_t = &md.tables[T_PARAMPTR];
	const meta_table_t *nc_t = &md.tables[T_NESTEDCLASS];

	if (m_t->rows == 0) {
		/* Managed image without methods: legal, produce an empty index. */
		pe_clr_methods_t *idx = calloc(1, sizeof(*idx));
		if (idx == NULL) {
			rc = fail(e, PE_CLR_METHODS_E_ALLOCATION, LOC_NONE,
				"index allocation failed");
			goto out;
		}
		*out = idx;
		rc = PE_CLR_METHODS_E_OK;
		goto out;
	}
	uint32_t n_td = td_t->rows;
	uint32_t n_m = m_t->rows;
	uint32_t n_p = p_t->rows;

	/* ---- Raw rows -------------------------------------------------- */
	typedef_rec_t *tds = calloc(n_td ? n_td : 1, sizeof(*tds));
	method_rec_t *mds = calloc(n_m, sizeof(*mds));
	param_rec_t *pms = calloc(n_p ? n_p : 1, sizeof(*pms));
	if (tds == NULL || mds == NULL || pms == NULL) {
		free(tds);
		free(mds);
		free(pms);
		rc = fail(e, PE_CLR_METHODS_E_ALLOCATION, LOC_NONE,
			"row storage allocation failed");
		goto out;
	}

	for (uint32_t r = 1; r <= n_td; r++) {
		tds[r - 1].flags = cell_u32(td_t, r, 0);
		tds[r - 1].name_idx = cell_u32(td_t, r, 1);
		tds[r - 1].namespace_idx = cell_u32(td_t, r, 2);
		/* col 3 Extends and col 4 FieldList are skipped with their widths */
		tds[r - 1].method_start = cell_u32(td_t, r, 5);
		tds[r - 1].row_off = td_t->base + (size_t)(r - 1) * td_t->row_size;
		tds[r - 1].row_size = td_t->row_size;
	}
	for (uint32_t r = 1; r <= n_m; r++) {
		mds[r - 1].rva = rd32(cell_ptr(m_t, r, 0));
		mds[r - 1].impl_flags = rd16(cell_ptr(m_t, r, 1));
		mds[r - 1].flags = rd16(cell_ptr(m_t, r, 2));
		mds[r - 1].name_idx = cell_u32(m_t, r, 3);
		mds[r - 1].sig_idx = cell_u32(m_t, r, 4);
		mds[r - 1].param_start = cell_u32(m_t, r, 5);
		mds[r - 1].row_off = m_t->base + (size_t)(r - 1) * m_t->row_size;
		mds[r - 1].row_size = m_t->row_size;
	}
	for (uint32_t r = 1; r <= n_p; r++) {
		pms[r - 1].flags = rd16(cell_ptr(p_t, r, 0));
		pms[r - 1].sequence = rd16(cell_ptr(p_t, r, 1));
		pms[r - 1].name_idx = cell_u32(p_t, r, 2);
		pms[r - 1].row_off = p_t->base + (size_t)(r - 1) * p_t->row_size;
		pms[r - 1].row_size = p_t->row_size;
	}

	/* ---- Pointer tables: complete permutations --------------------- */
	uint32_t *method_map = NULL; /* logical row -> physical MethodDef RID */
	if (mp_t->rows) {
		if (mp_t->rows != n_m) {
			rc = fail(e, PE_CLR_METHODS_E_MALFORMED, mp_t->base,
				"MethodPtr must cover every MethodDef row");
			goto fail_rows;
		}
		method_map = malloc((size_t)n_m * sizeof(*method_map));
		unsigned char *seen = calloc(n_m, 1);
		if (method_map == NULL || seen == NULL) {
			free(method_map);
			free(seen);
			rc = fail(e, PE_CLR_METHODS_E_ALLOCATION, LOC_NONE,
				"MethodPtr allocation failed");
			goto fail_rows;
		}
		for (uint32_t r = 1; r <= n_m; r++) {
			uint32_t target = cell_u32(mp_t, r, 0);
			if (target < 1 || target > n_m || seen[target - 1]) {
				free(seen);
				free(method_map);
				rc = fail(e, PE_CLR_METHODS_E_MALFORMED,
					mp_t->base + (size_t)(r - 1) * mp_t->row_size,
					"MethodPtr is not a complete permutation");
				goto fail_rows;
			}
			seen[target - 1] = 1;
			method_map[r - 1] = target;
		}
		free(seen);
	}

	uint32_t *param_map = NULL; /* logical row -> physical Param RID */
	if (pp_t->rows) {
		if (pp_t->rows != n_p) {
			rc = fail(e, PE_CLR_METHODS_E_MALFORMED, pp_t->base,
				"ParamPtr must cover every Param row");
			goto fail_mptr;
		}
		param_map = malloc((size_t)n_p * sizeof(*param_map));
		unsigned char *seen = calloc(n_p ? n_p : 1, 1);
		if (param_map == NULL || seen == NULL) {
			free(param_map);
			free(seen);
			rc = fail(e, PE_CLR_METHODS_E_ALLOCATION, LOC_NONE,
				"ParamPtr allocation failed");
			goto fail_mptr;
		}
		for (uint32_t r = 1; r <= n_p; r++) {
			uint32_t target = cell_u32(pp_t, r, 0);
			if (target < 1 || target > n_p || seen[target - 1]) {
				free(seen);
				free(param_map);
				rc = fail(e, PE_CLR_METHODS_E_MALFORMED,
					pp_t->base + (size_t)(r - 1) * pp_t->row_size,
					"ParamPtr is not a complete permutation");
				goto fail_mptr;
			}
			seen[target - 1] = 1;
			param_map[r - 1] = target;
		}
		free(seen);
	}

	/* ---- TypeDef -> logical method slices -------------------------- */
	uint32_t effective_methods = n_m;
	uint32_t *owner_of_method = calloc(n_m ? n_m : 1,
		sizeof(*owner_of_method));
	if (owner_of_method == NULL) {
		rc = fail(e, PE_CLR_METHODS_E_ALLOCATION, LOC_NONE,
			"owner table allocation failed");
		goto fail_pptr;
	}
	{
		uint32_t prev_start = 1;
		for (uint32_t t = 0; t < n_td; t++) {
			uint32_t start = tds[t].method_start;
			uint32_t end = (t + 1 < n_td)
				? tds[t + 1].method_start : effective_methods + 1;
			if (start < prev_start || start > effective_methods + 1
				|| end < start || end > effective_methods + 1) {
				rc = fail(e, PE_CLR_METHODS_E_MALFORMED, tds[t].row_off,
					"TypeDef.MethodList is not monotone");
				goto fail_owner;
			}
			for (uint32_t k = start; k < end; k++) {
				uint32_t phys = method_map ? method_map[k - 1] : k;
				if (owner_of_method[phys - 1] != 0) {
					rc = fail(e, PE_CLR_METHODS_E_MALFORMED,
						tds[t].row_off,
						"a MethodDef row is owned by two TypeDef rows");
					goto fail_owner;
				}
				owner_of_method[phys - 1] = t + 1;
			}
			prev_start = start;
		}
		for (uint32_t k = 0; k < n_m; k++) {
			if (owner_of_method[k] == 0) {
				rc = fail(e, PE_CLR_METHODS_E_MALFORMED, mds[k].row_off,
					"a MethodDef row is outside every TypeDef method list");
				goto fail_owner;
			}
		}
	}

	/* ---- NestedClass parentage ------------------------------------- */
	uint32_t *parent = calloc(n_td ? n_td : 1, sizeof(*parent));
	if (parent == NULL) {
		rc = fail(e, PE_CLR_METHODS_E_ALLOCATION, LOC_NONE,
			"nesting allocation failed");
		goto fail_owner;
	}
	if (nc_t->rows) {
		for (uint32_t r = 1; r <= nc_t->rows; r++) {
			uint32_t nested = cell_u32(nc_t, r, 0);
			uint32_t enclosing = cell_u32(nc_t, r, 1);
			if (nested < 1 || nested > n_td || enclosing < 1
				|| enclosing > n_td) {
				rc = fail(e, PE_CLR_METHODS_E_MALFORMED,
					nc_t->base + (size_t)(r - 1) * nc_t->row_size,
					"NestedClass row leaves the TypeDef table");
				goto fail_parent;
			}
			if (parent[nested - 1] != 0) {
				rc = fail(e, PE_CLR_METHODS_E_MALFORMED,
					nc_t->base + (size_t)(r - 1) * nc_t->row_size,
					"duplicate NestedClass parentage");
				goto fail_parent;
			}
			parent[nested - 1] = enclosing;
		}
		/* Cycle detection, independent of row appearance order. */
		for (uint32_t t = 1; t <= n_td; t++) {
			uint32_t cur = t;
			size_t steps = 0;
			while (cur != 0) {
				if (++steps > n_td) {
					rc = fail(e, PE_CLR_METHODS_E_MALFORMED, nc_t->base,
						"NestedClass parent cycle");
					goto fail_parent;
				}
				cur = parent[cur - 1];
			}
		}
	}

	/* ---- Assemble the owned index ---------------------------------- */
	pe_clr_methods_t *idx = calloc(1, sizeof(*idx));
	if (idx == NULL) {
		rc = fail(e, PE_CLR_METHODS_E_ALLOCATION, LOC_NONE,
			"index allocation failed");
		goto fail_parent;
	}
	idx->method_count = n_m;
	idx->methods = calloc(n_m, sizeof(*idx->methods));
	if (idx->methods == NULL) {
		free(idx);
		rc = fail(e, PE_CLR_METHODS_E_ALLOCATION, LOC_NONE,
			"method array allocation failed");
		goto fail_parent;
	}

	char **td_names = calloc(n_td ? n_td : 1, sizeof(char *));
	char **td_ns = calloc(n_td ? n_td : 1, sizeof(char *));
	char **full_names = calloc(n_td ? n_td : 1, sizeof(char *));
	if (td_names == NULL || td_ns == NULL || full_names == NULL) {
		free(td_names);
		free(td_ns);
		free(full_names);
		free_index_contents(idx);
		free(idx);
		rc = fail(e, PE_CLR_METHODS_E_ALLOCATION, LOC_NONE,
			"type name storage allocation failed");
		goto fail_parent;
	}
	for (uint32_t t = 1; t <= n_td; t++) {
		td_names[t - 1] = read_string(&env->map, &md.strings,
			tds[t - 1].name_idx, e);
		td_ns[t - 1] = read_string(&env->map, &md.strings,
			tds[t - 1].namespace_idx, e);
		if (td_names[t - 1] == NULL || td_ns[t - 1] == NULL) {
			rc = e->code;
			goto fail_index;
		}
	}
	for (uint32_t t = 1; t <= n_td; t++) {
		full_names[t - 1] = build_full_name(td_names, td_ns, parent,
			t, full_names, e);
		if (full_names[t - 1] == NULL) {
			rc = e->code;
			goto fail_index;
		}
	}

	for (uint32_t r = 1; r <= n_m; r++) {
		pe_clr_method_t *m = &idx->methods[r - 1];
		method_rec_t *rec = &mds[r - 1];

		m->method_token = PE_CLR_TOKEN_METHODDEF | r;
		m->rva = rec->rva;
		m->impl_flags = rec->impl_flags;
		m->flags = rec->flags;
		m->method_row = file_span(env, rec->row_off, rec->row_size);

		uint32_t owner = owner_of_method[r - 1];
		m->typedef_token = PE_CLR_TOKEN_TYPEDEF | owner;
		m->typedef_row = file_span(env, tds[owner - 1].row_off,
			tds[owner - 1].row_size);
		m->type_name = xstr(full_names[owner - 1]);
		m->name = read_string(&env->map, &md.strings, rec->name_idx, e);
		m->signature = read_blob(&md.blob, rec->sig_idx,
			&m->signature_size, e);
		if (m->type_name == NULL || m->name == NULL
			|| (rec->sig_idx != 0 && m->signature == NULL)) {
			rc = e->code;
			goto fail_index;
		}

		/*
		 * ParamList endpoints are sliced in physical MethodDef order.
		 * MethodPtr reordering never touches these endpoints: after the
		 * slice, each logical Param is resolved through ParamPtr.
		 */
		uint32_t p_start = rec->param_start;
		uint32_t p_end = (r < n_m)
			? mds[r].param_start : n_p + 1;
		if (p_start < 1 || p_end < p_start || p_end > n_p + 1) {
			rc = fail(e, PE_CLR_METHODS_E_MALFORMED, rec->row_off,
				"MethodDef.ParamList out of range");
			goto fail_index;
		}
		uint32_t count = p_end - p_start;
		m->param_count = count;
		m->params = count ? calloc(count, sizeof(*m->params)) : NULL;
		if (count && m->params == NULL) {
			rc = fail(e, PE_CLR_METHODS_E_ALLOCATION, LOC_NONE,
				"parameter array allocation failed");
			goto fail_index;
		}
		for (uint32_t k = 0; k < count; k++) {
			uint32_t logical = p_start + k;
			uint32_t phys = param_map ? param_map[logical - 1] : logical;
			param_rec_t *pr = &pms[phys - 1];
			pe_clr_param_t *dst = &m->params[k];
			dst->token = PE_CLR_TOKEN_PARAM | phys;
			dst->flags = pr->flags;
			dst->sequence = pr->sequence;
			dst->row = file_span(env, pr->row_off, pr->row_size);
			dst->name = read_string(&env->map, &md.strings,
				pr->name_idx, e);
			if (dst->name == NULL) {
				rc = e->code;
				goto fail_index;
			}
		}

		rc = parse_body(env, rec, &m->body, e);
		if (rc != PE_CLR_METHODS_E_OK)
			goto fail_index;
	}

	for (uint32_t t = 0; t < n_td; t++) {
		free(td_names[t]);
		free(td_ns[t]);
		free(full_names[t]);
	}
	free(td_names);
	free(td_ns);
	free(full_names);
	free(parent);
	free(owner_of_method);
	free(param_map);
	free(method_map);
	free(pms);
	free(mds);
	free(tds);

	*out = idx;
	rc = PE_CLR_METHODS_E_OK;
	goto out;

fail_index:
	for (uint32_t t = 0; t < n_td; t++) {
		free(td_names[t]);
		free(td_ns[t]);
		free(full_names[t]);
	}
	free(td_names);
	free(td_ns);
	free(full_names);
	free_index_contents(idx);
fail_parent:
	free(parent);
fail_owner:
	free(owner_of_method);
fail_pptr:
	free(param_map);
fail_mptr:
	free(method_map);
fail_rows:
	free(pms);
	free(mds);
	free(tds);
out:
	free(root_copy);
	return rc;
}

/* ------------------------------------------------------------------ */
/* Public entry points                                                 */
/* ------------------------------------------------------------------ */

pe_clr_methods_err_e pe_clr_methods_build(const pe_ctx_t *ctx,
	pe_clr_methods_t **out, pe_clr_methods_error_t *error) {
	pe_clr_methods_error_t local;
	if (error == NULL)
		error = &local;
	error->code = PE_CLR_METHODS_E_OK;
	error->location = LOC_NONE;
	error->message = pe_clr_methods_error_msg(PE_CLR_METHODS_E_OK);
	*out = NULL;

	if (ctx == NULL)
		return fail(error, PE_CLR_METHODS_E_MALFORMED, LOC_NONE,
			"null PE context");

	build_env_t env;
	memset(&env, 0, sizeof(env));
	env.ctx = ctx;
	env.map.base = ctx->map_addr;
	env.map.size = (size_t)ctx->map_size;

	/* Not managed: empty, successful index. */
	IMAGE_DATA_DIRECTORY *dir = clr_directory(ctx);
	if (dir == NULL || dir->VirtualAddress == 0) {
		pe_clr_methods_t *idx = calloc(1, sizeof(*idx));
		if (idx == NULL)
			return fail(error, PE_CLR_METHODS_E_ALLOCATION, LOC_NONE,
				"index allocation failed");
		*out = idx;
		return PE_CLR_METHODS_E_OK;
	}

	if (ctx->pe.optional_hdr_ptr == NULL
		|| (ctx->pe.optional_hdr.type != MAGIC_PE32
			&& ctx->pe.optional_hdr.type != MAGIC_PE64))
		return fail(error, PE_CLR_METHODS_E_UNSUPPORTED, LOC_NONE,
			"CLR directory requires a PE32 or PE32+ image");
	env.is_pe32_plus = ctx->pe.optional_hdr.type == MAGIC_PE64;
	env.headers_size = env.is_pe32_plus
		? ctx->pe.optional_hdr._64->SizeOfHeaders
		: ctx->pe.optional_hdr._32->SizeOfHeaders;

	env.section_count = ctx->pe.num_sections;
	env.sections = calloc(env.section_count ? env.section_count : 1,
		sizeof(*env.sections));
	if (env.sections == NULL)
		return fail(error, PE_CLR_METHODS_E_ALLOCATION, LOC_NONE,
			"section array allocation failed");
	for (uint32_t i = 0; i < env.section_count; i++) {
		IMAGE_SECTION_HEADER *s = ctx->pe.sections[i];
		env.sections[i].va = s->VirtualAddress;
		env.sections[i].virtual_size = s->Misc.VirtualSize
			? s->Misc.VirtualSize : s->SizeOfRawData;
		env.sections[i].raw_ptr = s->PointerToRawData;
		env.sections[i].raw_size = s->SizeOfRawData;
	}

	pe_clr_methods_err_e rc = build_index(&env, out, error);
	free(env.sections);
	return rc;
}

size_t pe_clr_methods_query(const pe_clr_methods_t *index, uint32_t rva,
	pe_clr_query_hit_t *hits, size_t capacity) {
	if (index == NULL)
		return 0;
	size_t total = 0;
	for (size_t i = 0; i < index->method_count; i++) {
		const pe_clr_method_t *m = &index->methods[i];
		const pe_clr_body_t *b = &m->body;
		if (!b->present || b->il_size == 0)
			continue;
		uint64_t end = (uint64_t)b->il_rva + b->il_size;
		if ((uint64_t)rva >= b->il_rva && (uint64_t)rva < end) {
			if (hits != NULL && total < capacity) {
				hits[total].method_token = m->method_token;
				hits[total].il_offset = rva - b->il_rva;
			}
			total++;
		}
	}
	return total;
}
