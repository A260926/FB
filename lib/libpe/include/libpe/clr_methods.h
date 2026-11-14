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

#ifndef LIBPE_CLR_METHODS_H
#define LIBPE_CLR_METHODS_H

#include <stddef.h>
#include <stdint.h>

#include "pe.h"

#ifdef __cplusplus
extern "C" {
#endif

// Metadata table tokens (ECMA-335 Partition II, section 21.1.1).
#define PE_CLR_TOKEN_TYPEDEF 0x02000000u
#define PE_CLR_TOKEN_METHODDEF 0x06000000u
#define PE_CLR_TOKEN_PARAM 0x08000000u

// Sentinel meaning "not backed by a PE section" (e.g. bytes inside the
// PE headers region).
#define PE_CLR_NO_SECTION UINT32_MAX

// A contiguous run of bytes in the mapped file. A virtual range whose raw
// backing is split across section boundaries produces several spans.
typedef struct pe_clr_span {
	uint64_t file_offset;   // physical file offset of the first byte
	uint32_t rva;           // RVA of the first byte; 0 when not virtualized
	uint32_t size;          // number of bytes in this run
	uint32_t section_index; // 0-based PE section, or PE_CLR_NO_SECTION
} pe_clr_span_t;

typedef enum {
	PE_CLR_METHODS_E_OK = 0,
	PE_CLR_METHODS_E_ALLOCATION,
	PE_CLR_METHODS_E_MALFORMED,    // bad metadata/body, location is meaningful
	PE_CLR_METHODS_E_UNSUPPORTED   // legal PE but a structure this index rejects
} pe_clr_methods_err_e;

// Always initialized by pe_clr_methods_build(), even on success. The message
// points at static storage. location is a physical file offset for metadata,
// heap and span errors, the offending RVA for method-body mapping errors that
// have no raw offset (e.g. an unbacked RVA), or UINT64_MAX when no precise
// position exists.
typedef struct pe_clr_methods_error {
	pe_clr_methods_err_e code;
	uint64_t location;
	const char *message;
} pe_clr_methods_error_t;

typedef struct pe_clr_param {
	uint32_t token;        // physical Param token, never a pointer row
	uint16_t flags;        // Param.Flags
	uint16_t sequence;     // 0 marks the return value metadata
	char *name;            // NUL-terminated, owned by the index
	pe_clr_span_t row;     // physical Param table row span
} pe_clr_param_t;

typedef struct pe_clr_body {
	int present;                  // 0 for RVA=0 methods: nothing below is valid
	int fat;                      // 1 fat header, 0 tiny header
	uint16_t header_flags;        // raw CorILMethod flags
	uint16_t max_stack;           // fat: MaxStack; tiny: the spec default 8
	uint32_t local_var_sig_token; // fat only, 0 when absent
	uint32_t header_rva;
	uint32_t header_size;
	pe_clr_span_t *header_spans;  // actual DWORD-count header, sections split
	size_t header_span_count;
	uint32_t il_rva;              // first IL byte (half-open range end = +il_size)
	uint32_t il_size;
	unsigned char *il;            // owned copy of the IL bytes only
	pe_clr_span_t *il_spans;      // physical backing of the IL range
	size_t il_span_count;
} pe_clr_body_t;

typedef struct pe_clr_method {
	uint32_t typedef_token;       // owning TypeDef token (physical RID)
	char *type_name;              // full nested name, '+' joins inner names
	uint32_t method_token;        // physical MethodDef token
	char *name;                   // MethodDef.Name, never used as identity
	uint32_t rva;                 // raw MethodDef.RVA; 0 is preserved
	uint16_t impl_flags;
	uint16_t flags;               // MethodDef.Flags
	unsigned char *signature;     // Blob payload after the compressed prefix
	size_t signature_size;
	pe_clr_span_t method_row;     // physical MethodDef table row span
	pe_clr_span_t typedef_row;    // owning TypeDef table row span
	pe_clr_param_t *params;       // logical list order, Sequence 0 retained
	size_t param_count;
	pe_clr_body_t body;
} pe_clr_method_t;

typedef struct pe_clr_methods {
	pe_clr_method_t *methods;     // physical MethodDef RID order
	size_t method_count;
} pe_clr_methods_t;

typedef struct pe_clr_query_hit {
	uint32_t method_token;
	uint32_t il_offset;           // offset of the queried RVA inside the IL
} pe_clr_query_hit_t;

// Builds a self-contained method index from a parsed, byte-backed context.
//
// A PE without a CLR data directory yields an empty successful index. A PE
// identified as managed whose metadata or bodies are invalid/unsupported
// fails with *out set to NULL and a location-bearing error; no partial index
// is returned. The result owns every byte it exposes and stays valid after
// the context and its mapping are released.
pe_clr_methods_err_e pe_clr_methods_build(const pe_ctx_t *ctx,
	pe_clr_methods_t **out, pe_clr_methods_error_t *error);

// Returns the number of methods whose IL half-open range contains rva.
// Up to capacity hits are written (NULL buffer is allowed for a count-only
// call); hits are emitted in physical MethodDef order. Headers, empty bodies
// and trailing method sections never match; the same RVA may hit several
// methods.
size_t pe_clr_methods_query(const pe_clr_methods_t *index, uint32_t rva,
	pe_clr_query_hit_t *hits, size_t capacity);

void pe_clr_methods_free(pe_clr_methods_t *index);

const char *pe_clr_methods_error_msg(pe_clr_methods_err_e code);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // LIBPE_CLR_METHODS_H
