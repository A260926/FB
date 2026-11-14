/*
 * examples/clr_methods.c - static CLR method index demo.
 *
 * Read-only usage of pe_clr_methods_build/query/free: the PE context is
 * closed before anything is printed, demonstrating that the index owns all
 * of its strings, signatures, IL bytes and source positions.
 *
 * Usage: clr_methods <pe-file> <decimal-rva>
 *
 * Output is TSV; the columns are documented in doc/clr-methods.md.
 */

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libpe/clr_methods.h>
#include "task_support/sample_io.h"

static void print_hex(const unsigned char *p, size_t n) {
	for (size_t i = 0; i < n; i++)
		printf("%02x", p[i]);
}

static void print_spans(const char *tag, uint32_t method_token,
	const pe_clr_span_t *spans, size_t count) {
	for (size_t i = 0; i < count; i++) {
		printf("%s\t%08" PRIx32 "\t%" PRIu64 "\t%" PRIu32 "\t%" PRIu32
			"\t%" PRIu32 "\n", tag, method_token, spans[i].file_offset,
			spans[i].rva, spans[i].size, spans[i].section_index);
	}
}

static void print_index(const pe_clr_methods_t *index) {
	for (size_t i = 0; i < index->method_count; i++) {
		const pe_clr_method_t *m = &index->methods[i];

		printf("METHOD\t%08" PRIx32 "\t%" PRIu32 "\t%s\t%04" PRIx16
			"\t%04" PRIx16 "\t%08" PRIx32 "\t%s\t%" PRIu64 "\t%" PRIu32
			"\t%" PRIu64 "\t%" PRIu32 "\n",
			m->method_token, m->rva, m->name, m->flags, m->impl_flags,
			m->typedef_token, m->type_name,
			m->method_row.file_offset, m->method_row.size,
			m->typedef_row.file_offset, m->typedef_row.size);

		printf("SIGNATURE\t%08" PRIx32 "\t%zu\t",
			m->method_token, m->signature_size);
		print_hex(m->signature, m->signature_size);
		putchar('\n');

		const pe_clr_body_t *b = &m->body;
		if (!b->present) {
			printf("BODY\t%08" PRIx32 "\tnone\t0\t0\t0\t0\t0\t00000000\t0000\n",
				m->method_token);
		} else {
			printf("BODY\t%08" PRIx32 "\t%s\t%" PRIu32 "\t%" PRIu32
				"\t%" PRIu32 "\t%" PRIu32 "\t%" PRIu16 "\t%08" PRIx32
				"\t%04" PRIx16 "\n",
				m->method_token, b->fat ? "fat" : "tiny",
				b->header_rva, b->header_size, b->il_rva, b->il_size,
				b->max_stack, b->local_var_sig_token, b->header_flags);
			print_spans("HDRSPAN", m->method_token,
				b->header_spans, b->header_span_count);
			print_spans("ILSPAN", m->method_token,
				b->il_spans, b->il_span_count);
			printf("IL\t%08" PRIx32 "\t", m->method_token);
			print_hex(b->il, b->il_size);
			putchar('\n');
		}

		for (size_t j = 0; j < m->param_count; j++) {
			const pe_clr_param_t *p = &m->params[j];
			printf("PARAM\t%08" PRIx32 "\t%08" PRIx32 "\t%" PRIu16
				"\t%04" PRIx16 "\t%s\t%" PRIu64 "\t%" PRIu32 "\n",
				m->method_token, p->token, p->sequence, p->flags, p->name,
				p->row.file_offset, p->row.size);
		}
	}
}

int main(int argc, char **argv) {
	if (argc != 3) {
		fprintf(stderr, "usage: %s <pe-file> <decimal-rva>\n",
			argc ? argv[0] : "clr_methods");
		return 1;
	}

	errno = 0;
	char *end = NULL;
	unsigned long rva = strtoul(argv[2], &end, 10);
	if (errno != 0 || end == argv[2] || *end != '\0' || rva > UINT32_MAX) {
		fprintf(stderr, "invalid decimal RVA: %s\n", argv[2]);
		return 1;
	}

	pe_ctx_t ctx;
	if (sample_open(&ctx, argv[1]) != 0) {
		fprintf(stderr, "cannot parse PE file: %s\n", argv[1]);
		return 2;
	}

	pe_clr_methods_t *index = NULL;
	pe_clr_methods_error_t error;
	pe_clr_methods_err_e rc = pe_clr_methods_build(&ctx, &index, &error);
	if (rc != PE_CLR_METHODS_E_OK) {
		fprintf(stderr,
			"clr index failed: %s (code %d) at file offset %" PRIu64 "\n",
			error.message, (int)error.code, error.location);
		sample_close(&ctx);
		return 3;
	}

	/* The index is self-contained: the mapping can go away right now. */
	sample_close(&ctx);

	print_index(index);

	pe_clr_query_hit_t hits[16];
	size_t capacity = sizeof(hits) / sizeof(hits[0]);
	size_t total = pe_clr_methods_query(index, (uint32_t)rva,
		hits, capacity);
	if (total == 0) {
		printf("UNCOVERED\t%lu\n", rva);
	} else {
		size_t shown = total < capacity ? total : capacity;
		for (size_t i = 0; i < shown; i++) {
			printf("QUERY\t%lu\t%08" PRIx32 "\t%" PRIu32 "\n",
				rva, hits[i].method_token, hits[i].il_offset);
		}
		if (total != shown)
			printf("QUERY_TRUNCATED\t%zu\n", total);
	}

	pe_clr_methods_free(index);
	return 0;
}
