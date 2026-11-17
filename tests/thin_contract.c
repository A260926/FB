/*-
 * Regression contract for GNU thin archive support in libelf.
 *
 * The cases below drive the real libelf API (elf_begin/elf_memory,
 * elf_getarsym, elf_rand, elf_next, elf_getarhdr, gelf_getsym and
 * friends) against the fixed, read-only samples under
 * task_support/samples and against small thin archive images
 * synthesized in memory.  No object code supplied by the samples is
 * ever executed, and no second ELF parser is implemented: the ELF
 * objects are treated purely as byte images and decoded through
 * libelf itself.
 *
 * The file also contains the firmware build provenance audit
 * example: an explicit caller-supplied manifest maps permitted
 * archive member names to the complete trusted object bytes and their
 * expected symbols.  The resolver does exact name lookups only, never
 * touches the file system or joins paths, and rejects absolute names,
 * parent references, unlisted names and same-named replacement
 * objects (recognized by a full byte comparison, without hashing).
 */

#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <gelf.h>
#include <libelf.h>

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../task_support/check.h"

#define	FIRST_OBJECT	"task_support/samples/objects/first-object.o"
#define	SECOND_OBJECT	"task_support/samples/objects/second-object.o"
#define	REGULAR_ARCHIVE	"task_support/samples/regular.a"
#define	THIN_ARCHIVE	"task_support/samples/thin.a"
#define	OBJECT_SIZE	512

/* ------------------------------------------------------------------ */
/* Small helpers.                                                      */
/* ------------------------------------------------------------------ */

static unsigned char *
read_file(const char *path, size_t *size)
{
	FILE *f;
	long len;
	unsigned char *buf;

	f = fopen(path, "rb");
	if (f == NULL)
		return (NULL);
	if (fseek(f, 0, SEEK_END) != 0 || (len = ftell(f)) < 0 ||
	    fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		return (NULL);
	}
	*size = (size_t) len;
	buf = malloc(*size);
	if (buf == NULL || fread(buf, 1, *size, f) != *size) {
		free(buf);
		fclose(f);
		return (NULL);
	}
	fclose(f);
	return (buf);
}

static void
put_be32(unsigned char *p, uint32_t v)
{
	p[0] = (unsigned char) (v >> 24);
	p[1] = (unsigned char) (v >> 16);
	p[2] = (unsigned char) (v >> 8);
	p[3] = (unsigned char) v;
}

/*
 * Format one 60 byte ar(1) header into 'dst'.
 */
static void
ar_header(unsigned char *dst, const char *name, size_t member_size)
{
	char tmp[64];
	int n;

	n = snprintf(tmp, sizeof(tmp),
	    "%-16.16s%-12zu%-6d%-6d%-8o%-10zu`\n",
	    name, member_size, 0, 0, 0100644, member_size);
	if (n != 60) {
		fprintf(stderr, "ar header formatting error\n");
		abort();
	}
	(void) memcpy(dst, tmp, 60);
}

struct synth_member {
	const char *name;	/* full member path                 */
	const char *symbol;	/* symbol table name, or NULL       */
	const unsigned char *bytes;
	size_t size;
	off_t header_offset;	/* filled in by the builder         */
};

/*
 * Build an in-memory GNU thin archive with an inline SVR4 symbol
 * table and GNU long name table.  Regular members carry no payload.
 * Returns a malloc'd image; the caller frees it.
 */
static unsigned char *
build_thin_archive(struct synth_member *members, size_t count,
    size_t *out_size)
{
	size_t i, name_table_size, symtab_size, symtab_padded,
	    names_padded, total;
	unsigned char *img, *p;

	name_table_size = 0;
	symtab_size = 4;			/* entry count */
	for (i = 0; i < count; i++) {
		name_table_size += strlen(members[i].name) + 2; /* name + /\n */
		if (members[i].symbol != NULL) {
			symtab_size += 4;			/* offset */
			symtab_size += strlen(members[i].symbol) + 1;
		}
	}

	symtab_padded = (symtab_size + 1) & ~(size_t) 1;
	names_padded = (name_table_size + 1) & ~(size_t) 1;
	total = 8 + 60 + symtab_padded + 60 + names_padded + 60 * count;

	img = malloc(total);
	if (img == NULL)
		return (NULL);
	p = img;

	(void) memcpy(p, "!<thin>\n", 8);
	p += 8;

	/* Inline symbol table member "/". */
	ar_header(p, "/", symtab_size);
	p += 60;
	put_be32(p, 0);			/* count, patched below */
	p += 4;
	/* Reserve room for the offset words first. */
	for (i = 0; i < count; i++)
		if (members[i].symbol != NULL)
			p += 4;
	for (i = 0; i < count; i++)
		if (members[i].symbol != NULL) {
			size_t slen = strlen(members[i].symbol) + 1;
			(void) memcpy(p, members[i].symbol, slen);
			p += slen;
		}
	if ((p - img) & 1)
		*p++ = '\n';

	/* Inline long name table member "//". */
	ar_header(p, "//", name_table_size);
	p += 60;
	for (i = 0; i < count; i++) {
		size_t entry = strlen(members[i].name) + 2;
		(void) memcpy(p, members[i].name, entry - 2);
		p[entry - 2] = '/';
		p[entry - 1] = '\n';
		p += entry;
	}
	if ((p - img) & 1)
		*p++ = '\n';

	/* Regular member headers, no payload. */
	{
		size_t name_offsets[16];
		size_t no = 0;

		if (count > 16) {
			free(img);
			return (NULL);
		}
		for (i = 0; i < count; i++) {
			name_offsets[i] = no;
			no += strlen(members[i].name) + 2;
		}

		for (i = 0; i < count; i++) {
			char ref[24];

			members[i].header_offset = (off_t) (p - img);
			(void) snprintf(ref, sizeof(ref), "/%zu",
			    name_offsets[i]);
			ar_header(p, ref, members[i].size);
			p += 60;
		}
	}

	/* Patch the symbol table: count and member offsets. */
	{
		unsigned char *sp = img + 8 + 60;
		unsigned n = 0;

		for (i = 0; i < count; i++)
			if (members[i].symbol != NULL)
				n++;
		put_be32(sp, n);
		sp += 4;
		for (i = 0; i < count; i++)
			if (members[i].symbol != NULL) {
				put_be32(sp,
				    (uint32_t) members[i].header_offset);
				sp += 4;
			}
	}

	*out_size = (size_t) (p - img);
	if (*out_size != total) {
		fprintf(stderr, "thin archive size mismatch %zu != %zu\n",
		    *out_size, total);
		abort();
	}
	return (img);
}

/*
 * Walk an ELF descriptor's SHT_SYMTAB section and return the name of
 * the first defined symbol (index 1 in these fixtures), decoded
 * through libelf.
 */
static char *
defined_symbol(Elf *e, char *buf, size_t bufsize)
{
	Elf_Scn *scn = NULL;

	while ((scn = elf_nextscn(e, scn)) != NULL) {
		GElf_Shdr sh;
		Elf_Data *data;
		GElf_Sym sym;
		char *name;

		if (gelf_getshdr(scn, &sh) == NULL)
			return (NULL);
		if (sh.sh_type != SHT_SYMTAB)
			continue;
		data = elf_getdata(scn, NULL);
		if (data == NULL || gelf_getsym(data, 1, &sym) == NULL)
			return (NULL);
		name = elf_strptr(e, sh.sh_link, sym.st_name);
		if (name == NULL)
			return (NULL);
		(void) snprintf(buf, bufsize, "%s", name);
		return (buf);
	}
	return (NULL);
}

/* ------------------------------------------------------------------ */
/* Firmware build provenance audit example.                            */
/* ------------------------------------------------------------------ */

#define	AUDIT_NAME_MAX	128
#define	AUDIT_SYM_MAX	 32
#define	AUDIT_REASON_MAX	 32

struct audit_entry {
	const char *name;
	const unsigned char *bytes;
	size_t size;
	const char *symbol;
};

struct audit_report {
	off_t ar_offset;
	char name[AUDIT_NAME_MAX];
	char symbol[AUDIT_SYM_MAX];
	int allowed;
	char reason[AUDIT_REASON_MAX];
};

struct audit_state {
	const struct audit_entry *manifest;	/* trusted allow list */
	size_t manifest_count;
	const struct audit_entry *presented;	/* build artifacts seen */
	size_t presented_count;
	struct audit_report *reports;
	size_t report_count;
	size_t report_capacity;
	off_t current_offset;
};

struct module_relation {
	off_t ar_offset;
	char name[AUDIT_NAME_MAX];
	char symbol[AUDIT_SYM_MAX];
};

static const struct audit_entry *
audit_find(const struct audit_entry *entries, size_t count, const char *name)
{
	size_t i;

	for (i = 0; i < count; i++)
		if (strcmp(entries[i].name, name) == 0)
			return (&entries[i]);
	return (NULL);
}

static int
name_has_parent_reference(const char *name)
{
	const char *p = name;

	while (*p != '\0') {
		const char *q = p;

		while (*q != '\0' && *q != '/')
			q++;
		if ((size_t) (q - p) == 2 && p[0] == '.' && p[1] == '.')
			return (1);
		p = (*q == '/') ? q + 1 : q;
	}
	return (0);
}

static void
audit_record(struct audit_state *audit, const char *name, int allowed,
    const char *reason, const char *symbol)
{
	struct audit_report *r;

	if (audit->report_count == audit->report_capacity) {
		fprintf(stderr, "audit report capacity exhausted\n");
		abort();
	}
	r = &audit->reports[audit->report_count++];
	r->ar_offset = audit->current_offset;
	(void) snprintf(r->name, sizeof(r->name), "%s", name);
	(void) snprintf(r->symbol, sizeof(r->symbol), "%s",
	    symbol != NULL ? symbol : "");
	r->allowed = allowed;
	(void) snprintf(r->reason, sizeof(r->reason), "%s",
	    reason != NULL ? reason : "");
}

static const struct audit_report *
audit_find_report(const struct audit_state *audit, const char *name)
{
	const struct audit_report *found = NULL;
	size_t i;

	/* Return the most recent report for the name. */
	for (i = 0; i < audit->report_count; i++)
		if (strcmp(audit->reports[i].name, name) == 0)
			found = &audit->reports[i];
	return (found);
}

/*
 * Resolver implementing the source boundary.  It performs exact name
 * lookups against the explicit manifest and never opens paths itself.
 *
 * The check is deliberately scoped to the local manifest: a presented
 * object is trusted only because its complete bytes equal the bytes
 * recorded in the manifest for that exact member name.  This is a
 * provenance/integrity boundary, not signature authentication; nothing
 * here establishes who produced the bytes or proves their origin
 * beyond the manifest itself.
 */
static int
audit_resolver(const char *name, void *ctx, const unsigned char **bytes,
    size_t *size)
{
	struct audit_state *audit = ctx;
	const struct audit_entry *trusted, *candidate;

	if (name == NULL || name[0] == '\0') {
		audit_record(audit, name != NULL ? name : "", 0,
		    "empty-name", NULL);
		return (-1);
	}

	if (name[0] == '/') {
		audit_record(audit, name, 0, "absolute-path", NULL);
		return (-1);
	}

	if (name_has_parent_reference(name)) {
		audit_record(audit, name, 0, "parent-reference", NULL);
		return (-1);
	}

	trusted = audit_find(audit->manifest, audit->manifest_count, name);
	if (trusted == NULL) {
		audit_record(audit, name, 0, "not-in-manifest", NULL);
		return (-1);
	}

	/*
	 * When no separate build-output list is configured the
	 * manifest's own bytes are the presented artifact.  Otherwise
	 * the artifact presented for the build is compared against
	 * the manifest entry.
	 */
	if (audit->presented_count == 0)
		candidate = trusted;
	else
		candidate = audit_find(audit->presented,
		    audit->presented_count, name);
	if (candidate == NULL) {
		audit_record(audit, name, 0, "object-unavailable", NULL);
		return (-1);
	}

	if (candidate->size != trusted->size ||
	    memcmp(candidate->bytes, trusted->bytes, trusted->size) != 0) {
		audit_record(audit, name, 0, "object-replaced", NULL);
		return (-1);
	}

	audit_record(audit, name, 1, NULL, trusted->symbol);
	*bytes = trusted->bytes;
	*size = trusted->size;
	return (0);
}

/*
 * A resolver that hands every result back through one shared scratch
 * buffer: its output is only valid until the next call, exactly the
 * lifetime guarantee the libelf contract describes.
 */
static unsigned char scratch_buffer[OBJECT_SIZE];

static int
scratch_resolver(const char *name, void *ctx, const unsigned char **bytes,
    size_t *size)
{
	const struct audit_entry *e;
	struct audit_state *audit = ctx;

	e = audit_find(audit->manifest, audit->manifest_count, name);
	if (e == NULL)
		return (-1);
	(void) memcpy(scratch_buffer, e->bytes, e->size);
	*bytes = scratch_buffer;
	*size = e->size;
	return (0);
}

/* A resolver that always fails, for failure-path testing. */
static int
failing_resolver(const char *name, void *ctx, const unsigned char **bytes,
    size_t *size)
{
	(void) name;
	(void) ctx;
	(void) bytes;
	(void) size;
	return (-1);
}

/* ------------------------------------------------------------------ */
/* Shared fixtures.                                                    */
/* ------------------------------------------------------------------ */

static unsigned char *alpha_bytes;
static unsigned char *beta_bytes;
static unsigned char *thin_bytes;
static size_t thin_size;
static unsigned char *regular_bytes;
static size_t regular_size;

static int
fixture_setup(void)
{
	size_t a = 0, b = 0;

	if (elf_version(EV_CURRENT) == EV_NONE)
		return (1);
	alpha_bytes = read_file(FIRST_OBJECT, &a);
	beta_bytes = read_file(SECOND_OBJECT, &b);
	thin_bytes = read_file(THIN_ARCHIVE, &thin_size);
	regular_bytes = read_file(REGULAR_ARCHIVE, &regular_size);
	if (alpha_bytes == NULL || beta_bytes == NULL ||
	    thin_bytes == NULL || regular_bytes == NULL ||
	    a != OBJECT_SIZE || b != OBJECT_SIZE)
		return (1);
	return (0);
}

static int
ensure_fixtures(void)
{

	if (thin_bytes != NULL)
		return (0);
	return (fixture_setup());
}

/* Open the sample thin archive through both supported entry points. */
static int
open_thin_pair(Elf **mem_elf, Elf **fd_elf, int *fd)
{
	*fd = open(THIN_ARCHIVE, O_RDONLY);
	if (*fd < 0)
		return (1);
	*mem_elf = elf_memory((char *) thin_bytes, thin_size);
	*fd_elf = elf_begin(*fd, ELF_C_READ, NULL);
	if (*mem_elf == NULL || *fd_elf == NULL ||
	    elf_kind(*mem_elf) != ELF_K_AR || elf_kind(*fd_elf) != ELF_K_AR)
		return (1);
	return (0);
}

/* The two manifest names used by the sample thin archive. */
#define	ALPHA_NAME	"objects/first-object.o"
#define	BETA_NAME	"objects/second-object.o"

/* ------------------------------------------------------------------ */
/* Resolution group.                                                   */
/* ------------------------------------------------------------------ */

static int
case_kind_and_resolver_api(void)
{
	Elf *mem_elf, *fd_elf, *regular;
	int fd;

	TASK_CHECK(ensure_fixtures() == 0);
	TASK_CHECK(open_thin_pair(&mem_elf, &fd_elf, &fd) == 0);
	TASK_CHECK(elf_kind(mem_elf) == ELF_K_AR);
	TASK_CHECK(elf_kind(fd_elf) == ELF_K_AR);

	/* Setter accepts a thin archive, replacement and clearing. */
	TASK_CHECK(elf_set_archive_resolver(mem_elf, failing_resolver, NULL)
	    == 0);
	TASK_CHECK(elf_set_archive_resolver(mem_elf, NULL, NULL) == 0);

	/* Both supported entry points produce thin parents. */
	TASK_CHECK(elf_set_archive_resolver(fd_elf, failing_resolver, NULL)
	    == 0);

	/* NULL and ordinary (non-thin) archives are rejected. */
	TASK_CHECK(elf_set_archive_resolver(NULL, failing_resolver, NULL)
	    == -1);
	regular = elf_memory((char *) regular_bytes, regular_size);
	TASK_CHECK(regular != NULL && elf_kind(regular) == ELF_K_AR);
	TASK_CHECK(elf_set_archive_resolver(regular, failing_resolver, NULL)
	    == -1);

	(void) elf_end(mem_elf);
	(void) elf_end(fd_elf);
	(void) elf_end(regular);
	(void) close(fd);
	return (0);
}

static int
case_long_names_and_interleaved_members(void)
{
	static struct audit_entry manifest[2];
	struct audit_state audit;
	struct audit_report reports[8];
	Elf *ar, *e1, *e2;
	Elf_Arhdr *h1, *h2;
	char sym1[AUDIT_SYM_MAX], sym2[AUDIT_SYM_MAX];

	manifest[0] = (struct audit_entry) { ALPHA_NAME, alpha_bytes,
	    OBJECT_SIZE, "alpha" };
	manifest[1] = (struct audit_entry) { BETA_NAME, beta_bytes,
	    OBJECT_SIZE, "beta" };
	(void) memset(&audit, 0, sizeof(audit));
	audit.manifest = manifest;
	audit.manifest_count = 2;
	audit.reports = reports;
	audit.report_capacity = 8;

	TASK_CHECK(ensure_fixtures() == 0);
	ar = elf_memory((char *) thin_bytes, thin_size);
	TASK_CHECK(ar != NULL && elf_kind(ar) == ELF_K_AR);
	TASK_CHECK(elf_set_archive_resolver(ar, scratch_resolver, &audit)
	    == 0);

	{
		Elf_Arsym *syms;
		size_t count;

		syms = elf_getarsym(ar, &count);
		TASK_CHECK(syms != NULL && count == 3);

		/* Open both members before reading either one. */
		TASK_CHECK(elf_rand(ar, syms[0].as_off) == syms[0].as_off);
		e1 = elf_begin(-1, ELF_C_READ, ar);
		TASK_CHECK(e1 != NULL && elf_kind(e1) == ELF_K_ELF);

		TASK_CHECK(elf_rand(ar, syms[1].as_off) == syms[1].as_off);
		e2 = elf_begin(-1, ELF_C_READ, ar);
		TASK_CHECK(e2 != NULL && elf_kind(e2) == ELF_K_ELF);
	}

	h1 = elf_getarhdr(e1);
	h2 = elf_getarhdr(e2);
	TASK_CHECK(h1 != NULL && strcmp(h1->ar_name, ALPHA_NAME) == 0);
	TASK_CHECK(h2 != NULL && strcmp(h2->ar_name, BETA_NAME) == 0);
	TASK_CHECK(h1->ar_size == OBJECT_SIZE && h2->ar_size ==
	    OBJECT_SIZE);
	TASK_CHECK(h1->ar_rawname != NULL &&
	    strncmp(h1->ar_rawname, "/0", 2) == 0);
	/* Metadata from the physical header is preserved. */
	TASK_CHECK(h1->ar_mode == (mode_t) 0100644 &&
	    h2->ar_mode == (mode_t) 0100644);
	TASK_CHECK(h1->ar_uid == 0 && h1->ar_gid == 0 &&
	    h1->ar_date == 0);
	TASK_CHECK(h2->ar_uid == 0 && h2->ar_gid == 0 &&
	    h2->ar_date == 0);

	/* Interleaved reads: each descriptor owns its own object copy. */
	TASK_CHECK(defined_symbol(e1, sym1, sizeof(sym1)) != NULL);
	TASK_CHECK(defined_symbol(e2, sym2, sizeof(sym2)) != NULL);
	TASK_CHECK(strcmp(sym1, "alpha") == 0);
	TASK_CHECK(strcmp(sym2, "beta") == 0);
	/* Read the first one again after the second resolver call. */
	TASK_CHECK(defined_symbol(e1, sym1, sizeof(sym1)) != NULL &&
	    strcmp(sym1, "alpha") == 0);

	TASK_CHECK(elf_getbase(e1) == 0 && elf_getbase(e2) == 0);

	(void) elf_end(e1);
	(void) elf_end(e2);
	(void) elf_end(ar);
	return (0);
}

static int
case_failure_retry_does_not_consume_cursor(void)
{
	Elf *ar, *e;
	Elf_Arsym *syms;
	size_t count;
	off_t alpha_off;
	static struct audit_entry manifest[1];
	struct audit_state audit;
	struct audit_report reports[8];

	TASK_CHECK(ensure_fixtures() == 0);
	ar = elf_memory((char *) thin_bytes, thin_size);
	TASK_CHECK(ar != NULL);
	syms = elf_getarsym(ar, &count);
	TASK_CHECK(syms != NULL && count == 3);
	alpha_off = syms[0].as_off;

	/* No resolver: opening the member fails but keeps the cursor. */
	TASK_CHECK(elf_rand(ar, alpha_off) == alpha_off);
	TASK_CHECK(elf_begin(-1, ELF_C_READ, ar) == NULL);
	TASK_CHECK(elf_rand(ar, alpha_off) == alpha_off);
	TASK_CHECK(elf_begin(-1, ELF_C_READ, ar) == NULL);

	/* A failing resolver behaves the same way. */
	TASK_CHECK(elf_set_archive_resolver(ar, failing_resolver, NULL)
	    == 0);
	TASK_CHECK(elf_begin(-1, ELF_C_READ, ar) == NULL);
	TASK_CHECK(elf_rand(ar, alpha_off) == alpha_off);

	/* A corrected resolver opens the same member on retry. */
	manifest[0] = (struct audit_entry) { ALPHA_NAME, alpha_bytes,
	    OBJECT_SIZE, "alpha" };
	(void) memset(&audit, 0, sizeof(audit));
	audit.manifest = manifest;
	audit.manifest_count = 1;
	audit.reports = reports;
	audit.report_capacity = 8;
	TASK_CHECK(elf_set_archive_resolver(ar, scratch_resolver, &audit)
	    == 0);
	e = elf_begin(-1, ELF_C_READ, ar);
	TASK_CHECK(e != NULL && elf_kind(e) == ELF_K_ELF);
	(void) elf_end(e);

	/* Clearing the resolver makes the next member fail again. */
	TASK_CHECK(elf_set_archive_resolver(ar, NULL, NULL) == 0);
	TASK_CHECK(elf_rand(ar, syms[1].as_off) == syms[1].as_off);
	TASK_CHECK(elf_begin(-1, ELF_C_READ, ar) == NULL);
	/* Already opened children are unaffected by resolver changes. */

	(void) elf_end(ar);
	return (0);
}

static int
case_length_mismatch_rejected(void)
{
	static struct audit_entry manifest[1];
	struct audit_state audit;
	struct audit_report reports[4];
	Elf *ar, *e;
	Elf_Arsym *syms;
	size_t count;

	/* Manifest advertises a truncated object; the header says 512. */
	manifest[0] = (struct audit_entry) { ALPHA_NAME, alpha_bytes,
	    OBJECT_SIZE / 2, "alpha" };
	(void) memset(&audit, 0, sizeof(audit));
	audit.manifest = manifest;
	audit.manifest_count = 1;
	audit.reports = reports;
	audit.report_capacity = 4;

	TASK_CHECK(ensure_fixtures() == 0);
	ar = elf_memory((char *) thin_bytes, thin_size);
	TASK_CHECK(ar != NULL);
	syms = elf_getarsym(ar, &count);
	TASK_CHECK(syms != NULL);
	TASK_CHECK(elf_rand(ar, syms[0].as_off) == syms[0].as_off);
	TASK_CHECK(elf_set_archive_resolver(ar, audit_resolver, &audit)
	    == 0);
	TASK_CHECK(elf_begin(-1, ELF_C_READ, ar) == NULL);

	/* Fix the manifest length and retry the same position. */
	manifest[0].size = OBJECT_SIZE;
	audit.presented = manifest;
	audit.presented_count = 1;
	TASK_CHECK(elf_rand(ar, syms[0].as_off) == syms[0].as_off);
	e = elf_begin(-1, ELF_C_READ, ar);
	TASK_CHECK(e != NULL && elf_kind(e) == ELF_K_ELF);
	(void) elf_end(e);
	(void) elf_end(ar);
	return (0);
}

static int
case_audit_path_policy_and_nested_names(void)
{
	static struct audit_entry manifest[3];
	struct audit_state audit;
	struct audit_report reports[16];
	const unsigned char *out_bytes;
	size_t out_size;
	const struct audit_report *r;

	TASK_CHECK(ensure_fixtures() == 0);

	manifest[0] = (struct audit_entry) { ALPHA_NAME, alpha_bytes,
	    OBJECT_SIZE, "alpha" };
	manifest[1] = (struct audit_entry) {
		"objects/nested/first-object.o", alpha_bytes,
		OBJECT_SIZE, "alpha" };
	/* A parent-reference entry present even in the manifest must
	 * still lose to the path policy. */
	manifest[2] = (struct audit_entry) { "objects/../secret.o",
		beta_bytes, OBJECT_SIZE, "beta" };

	(void) memset(&audit, 0, sizeof(audit));
	audit.manifest = manifest;
	audit.manifest_count = 3;
	audit.reports = reports;
	audit.report_capacity = 16;

	out_bytes = NULL;
	out_size = 0;
	TASK_CHECK(audit_resolver("/etc/firmware.elf", &audit, &out_bytes,
	    &out_size) == -1);
	r = audit_find_report(&audit, "/etc/firmware.elf");
	TASK_CHECK(r != NULL && r->allowed == 0 &&
	    strcmp(r->reason, "absolute-path") == 0);

	out_bytes = NULL;
	TASK_CHECK(audit_resolver("../escape.o", &audit, &out_bytes,
	    &out_size) == -1);
	r = audit_find_report(&audit, "../escape.o");
	TASK_CHECK(r != NULL && r->allowed == 0 &&
	    strcmp(r->reason, "parent-reference") == 0);

	/* Manifest-listed ".." name is rejected anyway. */
	TASK_CHECK(audit_resolver("objects/../secret.o", &audit,
	    &out_bytes, &out_size) == -1);
	r = audit_find_report(&audit, "objects/../secret.o");
	TASK_CHECK(r != NULL && r->allowed == 0 &&
	    strcmp(r->reason, "parent-reference") == 0);

	/* Unlisted but well-formed relative name. */
	TASK_CHECK(audit_resolver("objects/unknown.o", &audit, &out_bytes,
	    &out_size) == -1);
	r = audit_find_report(&audit, "objects/unknown.o");
	TASK_CHECK(r != NULL && r->allowed == 0 &&
	    strcmp(r->reason, "not-in-manifest") == 0);

	/* Legal nested relative name resolves with interior slashes. */
	out_bytes = NULL;
	out_size = 0;
	TASK_CHECK(audit_resolver("objects/nested/first-object.o", &audit,
	    &out_bytes, &out_size) == 0);
	TASK_CHECK(out_size == OBJECT_SIZE && out_bytes == alpha_bytes);
	r = audit_find_report(&audit, "objects/nested/first-object.o");
	TASK_CHECK(r != NULL && r->allowed == 1 &&
	    strcmp(r->symbol, "alpha") == 0 && r->reason[0] == '\0');

	return (0);
}

static int
case_object_replacement_then_manifest_fix(void)
{
	static struct audit_entry manifest[1], presented[1];
	struct audit_state audit;
	struct audit_report reports[8];
	unsigned char *tampered;
	Elf *ar, *e;
	Elf_Arsym *syms;
	size_t count;
	const struct audit_report *r;
	char sym[AUDIT_SYM_MAX];

	TASK_CHECK(ensure_fixtures() == 0);

	tampered = malloc(OBJECT_SIZE);
	TASK_CHECK(tampered != NULL);
	(void) memcpy(tampered, alpha_bytes, OBJECT_SIZE);
	tampered[140] ^= 0xFF;		/* same length, altered payload */

	manifest[0] = (struct audit_entry) { ALPHA_NAME, alpha_bytes,
	    OBJECT_SIZE, "alpha" };
	presented[0] = (struct audit_entry) { ALPHA_NAME, tampered,
	    OBJECT_SIZE, "alpha" };

	(void) memset(&audit, 0, sizeof(audit));
	audit.manifest = manifest;
	audit.manifest_count = 1;
	audit.presented = presented;
	audit.presented_count = 1;
	audit.reports = reports;
	audit.report_capacity = 8;

	TASK_CHECK(ensure_fixtures() == 0);
	ar = elf_memory((char *) thin_bytes, thin_size);
	TASK_CHECK(ar != NULL);
	syms = elf_getarsym(ar, &count);
	TASK_CHECK(syms != NULL);
	TASK_CHECK(elf_set_archive_resolver(ar, audit_resolver, &audit)
	    == 0);
	audit.current_offset = syms[0].as_off;
	TASK_CHECK(elf_rand(ar, syms[0].as_off) == syms[0].as_off);

	/* The same-named replacement object must not open. */
	TASK_CHECK(elf_begin(-1, ELF_C_READ, ar) == NULL);
	r = audit_find_report(&audit, ALPHA_NAME);
	TASK_CHECK(r != NULL && r->allowed == 0 &&
	    strcmp(r->reason, "object-replaced") == 0);
	TASK_CHECK(r->ar_offset == syms[0].as_off);

	/* Correct the build artifact, then retry the same member. */
	presented[0].bytes = alpha_bytes;
	e = elf_begin(-1, ELF_C_READ, ar);
	TASK_CHECK(e != NULL && elf_kind(e) == ELF_K_ELF);
	TASK_CHECK(defined_symbol(e, sym, sizeof(sym)) != NULL &&
	    strcmp(sym, "alpha") == 0);
	r = audit_find_report(&audit, ALPHA_NAME);
	TASK_CHECK(r != NULL && r->allowed == 1 &&
	    strcmp(r->symbol, "alpha") == 0);

	(void) elf_end(e);
	(void) elf_end(ar);
	free(tampered);
	return (0);
}

static int
case_foreign_archive_reference_unsupported(void)
{
	static struct synth_member members[2];
	unsigned char *img;
	size_t img_size;
	Elf *ar, *e;

	TASK_CHECK(ensure_fixtures() == 0);

	members[0] = (struct synth_member) { "otherlib.a/64", "alpha",
	    alpha_bytes, OBJECT_SIZE, 0 };
	members[1] = (struct synth_member) { BETA_NAME, "beta",
	    beta_bytes, OBJECT_SIZE, 0 };
	/* Force the second header's raw name to carry a colon. */
	img = build_thin_archive(members, 2, &img_size);
	TASK_CHECK(img != NULL);

	ar = elf_memory((char *) img, img_size);
	TASK_CHECK(ar != NULL && elf_kind(ar) == ELF_K_AR);
	TASK_CHECK(elf_set_archive_resolver(ar, failing_resolver, NULL)
	    == 0);

	/* "/offset:memberoffset" style references are explicitly
	 * unsupported; emulate one by patching the raw member name. */
	{
		unsigned char *hdr;
		hdr = img + members[1].header_offset;
		(void) memset(hdr, ' ', 16);
		(void) memcpy(hdr, "/0:64", 5);
	}
	TASK_CHECK(elf_rand(ar, members[1].header_offset) ==
	    members[1].header_offset);
	TASK_CHECK(elf_begin(-1, ELF_C_READ, ar) == NULL);
	TASK_CHECK(elf_errno() == ELF_E_UNIMPL);

	/* The cursor is retained and a supported member still opens. */
	TASK_CHECK(elf_rand(ar, members[0].header_offset) ==
	    members[0].header_offset);
	TASK_CHECK(elf_set_archive_resolver(ar, scratch_resolver, NULL)
	    == 0);
	{
		static struct audit_entry manifest[2];
		struct audit_state audit;
		struct audit_report reports[4];

		manifest[0] = (struct audit_entry) { "otherlib.a/64",
		    alpha_bytes, OBJECT_SIZE, "alpha" };
		manifest[1] = (struct audit_entry) { BETA_NAME, beta_bytes,
		    OBJECT_SIZE, "beta" };
		(void) memset(&audit, 0, sizeof(audit));
		audit.manifest = manifest;
		audit.manifest_count = 2;
		audit.reports = reports;
		audit.report_capacity = 4;
		TASK_CHECK(elf_set_archive_resolver(ar, scratch_resolver,
		    &audit) == 0);
		e = elf_begin(-1, ELF_C_READ, ar);
		TASK_CHECK(e != NULL && elf_kind(e) == ELF_K_ELF);
		(void) elf_end(e);
	}

	(void) elf_end(ar);
	free(img);
	return (0);
}

/* ------------------------------------------------------------------ */
/* Cursor group.                                                       */
/* ------------------------------------------------------------------ */

static int
case_physical_cursor_traversal(void)
{
	static struct audit_entry manifest[2];
	struct audit_state audit;
	struct audit_report reports[8];
	Elf *ar, *e;
	Elf_Cmd cmd;
	int seen;

	manifest[0] = (struct audit_entry) { ALPHA_NAME, alpha_bytes,
	    OBJECT_SIZE, "alpha" };
	manifest[1] = (struct audit_entry) { BETA_NAME, beta_bytes,
	    OBJECT_SIZE, "beta" };
	(void) memset(&audit, 0, sizeof(audit));
	audit.manifest = manifest;
	audit.manifest_count = 2;
	audit.reports = reports;
	audit.report_capacity = 8;

	TASK_CHECK(ensure_fixtures() == 0);
	ar = elf_memory((char *) thin_bytes, thin_size);
	TASK_CHECK(ar != NULL);
	TASK_CHECK(elf_set_archive_resolver(ar, scratch_resolver, &audit)
	    == 0);

	seen = 0;
	cmd = ELF_C_READ;
	while ((e = elf_begin(-1, cmd, ar)) != NULL) {
		Elf_Arhdr *h = elf_getarhdr(e);
		TASK_CHECK(h != NULL);
		if (seen == 0)
			TASK_CHECK(strcmp(h->ar_name, ALPHA_NAME) == 0);
		else if (seen == 1)
			TASK_CHECK(strcmp(h->ar_name, BETA_NAME) == 0);
		else
			TASK_CHECK(0);
		/* ar_size describes the external object, not archive
		 * bytes; the cursor must not use it to skip. */
		TASK_CHECK(h->ar_size == OBJECT_SIZE);
		seen++;
		cmd = elf_next(e);
		TASK_CHECK(cmd == ELF_C_READ);
		(void) elf_end(e);
	}
	TASK_CHECK(seen == 2);
	TASK_CHECK(elf_errno() == ELF_E_NONE);

	(void) elf_end(ar);
	return (0);
}

static int
case_rand_roundtrip_and_bad_offsets(void)
{
	Elf *ar, *e;
	Elf_Arsym *syms;
	size_t count, i;
	Elf_Arhdr *h;
	static struct audit_entry manifest[2];
	struct audit_state audit;
	struct audit_report reports[8];

	manifest[0] = (struct audit_entry) { ALPHA_NAME, alpha_bytes,
	    OBJECT_SIZE, "alpha" };
	manifest[1] = (struct audit_entry) { BETA_NAME, beta_bytes,
	    OBJECT_SIZE, "beta" };
	(void) memset(&audit, 0, sizeof(audit));
	audit.manifest = manifest;
	audit.manifest_count = 2;
	audit.reports = reports;
	audit.report_capacity = 8;

	TASK_CHECK(ensure_fixtures() == 0);
	ar = elf_memory((char *) thin_bytes, thin_size);
	TASK_CHECK(ar != NULL);
	TASK_CHECK(elf_set_archive_resolver(ar, scratch_resolver, &audit)
	    == 0);
	syms = elf_getarsym(ar, &count);
	TASK_CHECK(syms != NULL && count == 3);

	for (i = 0; syms[i].as_name != NULL; i++)
		TASK_CHECK(elf_rand(ar, syms[i].as_off) == syms[i].as_off);

	/* Invalid arguments are refused without moving the cursor. */
	TASK_CHECK(elf_rand(ar, 7) == 0);		/* odd offset */
	TASK_CHECK(elf_rand(ar, (off_t) thin_size) == 0); /* past EOF */
	TASK_CHECK(elf_rand(ar, (off_t) -1) == 0);
	TASK_CHECK(elf_rand(NULL, syms[0].as_off) == 0);

	/* The cursor still points at the last requested member. */
	e = elf_begin(-1, ELF_C_READ, ar);
	TASK_CHECK(e != NULL && elf_kind(e) == ELF_K_ELF);
	h = elf_getarhdr(e);
	TASK_CHECK(h != NULL && strcmp(h->ar_name, BETA_NAME) == 0);
	(void) elf_end(e);

	(void) elf_end(ar);
	return (0);
}

static int
case_last_header_ends_at_eof(void)
{
	Elf *ar, *e;
	Elf_Arsym *syms;
	size_t count;
	off_t last_off;
	Elf_Arhdr *h;
	static struct audit_entry manifest[1];
	struct audit_state audit;
	struct audit_report reports[4];

	TASK_CHECK(ensure_fixtures() == 0);
	ar = elf_memory((char *) thin_bytes, thin_size);
	TASK_CHECK(ar != NULL);
	syms = elf_getarsym(ar, &count);
	TASK_CHECK(syms != NULL && count == 3);
	last_off = syms[1].as_off;

	/* The final member header ends exactly at the end of file. */
	TASK_CHECK(last_off + 60 == (off_t) thin_size);
	TASK_CHECK(elf_rand(ar, last_off) == last_off);

	manifest[0] = (struct audit_entry) { BETA_NAME, beta_bytes,
	    OBJECT_SIZE, "beta" };
	(void) memset(&audit, 0, sizeof(audit));
	audit.manifest = manifest;
	audit.manifest_count = 1;
	audit.reports = reports;
	audit.report_capacity = 4;
	TASK_CHECK(elf_set_archive_resolver(ar, scratch_resolver, &audit)
	    == 0);

	e = elf_begin(-1, ELF_C_READ, ar);
	TASK_CHECK(e != NULL && elf_kind(e) == ELF_K_ELF);
	h = elf_getarhdr(e);
	TASK_CHECK(h != NULL && strcmp(h->ar_name, BETA_NAME) == 0);
	(void) elf_end(e);
	(void) elf_end(ar);
	return (0);
}

static int
case_inline_tables_and_regular_archive(void)
{
	Elf *thin, *regular;
	Elf_Arsym *thin_syms, *regular_syms;
	size_t thin_count, regular_count;
	Elf *member;
	Elf_Arhdr *h;

	TASK_CHECK(ensure_fixtures() == 0);
	thin = elf_memory((char *) thin_bytes, thin_size);
	regular = elf_memory((char *) regular_bytes, regular_size);
	TASK_CHECK(thin != NULL && regular != NULL);
	TASK_CHECK(elf_kind(thin) == ELF_K_AR &&
	    elf_kind(regular) == ELF_K_AR);

	/* The "/" symbol index and "//" name table stay embedded. */
	thin_syms = elf_getarsym(thin, &thin_count);
	regular_syms = elf_getarsym(regular, &regular_count);
	TASK_CHECK(thin_syms != NULL && regular_syms != NULL);
	TASK_CHECK(thin_count == regular_count && thin_count == 3);
	TASK_CHECK(strcmp(thin_syms[0].as_name,
	    regular_syms[0].as_name) == 0);
	TASK_CHECK(thin_syms[0].as_off == regular_syms[0].as_off);

	/* A regular archive member still lives inside the archive and
	 * therefore keeps its non-zero base. */
	TASK_CHECK(elf_rand(regular, regular_syms[0].as_off) ==
	    regular_syms[0].as_off);
	member = elf_begin(-1, ELF_C_READ, regular);
	TASK_CHECK(member != NULL && elf_kind(member) == ELF_K_ELF);
	TASK_CHECK(elf_getbase(member) == regular_syms[0].as_off + 60);
	h = elf_getarhdr(member);
	TASK_CHECK(h != NULL && h->ar_size == OBJECT_SIZE);
	(void) elf_end(member);

	(void) elf_end(thin);
	(void) elf_end(regular);
	return (0);
}

static int
case_inline_special_members_bypass_resolver(void)
{
	Elf *ar, *e;
	Elf_Arsym *syms;
	size_t count;

	/*
	 * In the fixed fixture the symbol table header sits at
	 * offset 8 (right after the magic) and the long name table
	 * header at offset 92; both payloads are inline.
	 */
	TASK_CHECK(ensure_fixtures() == 0);
	ar = elf_memory((char *) thin_bytes, thin_size);
	TASK_CHECK(ar != NULL);
	syms = elf_getarsym(ar, &count);
	TASK_CHECK(syms != NULL && count == 3);

	/* A resolver that always fails must not be consulted for the
	 * inline tables. */
	TASK_CHECK(elf_set_archive_resolver(ar, failing_resolver, NULL)
	    == 0);

	TASK_CHECK(elf_rand(ar, (off_t) 8) == (off_t) 8);
	e = elf_begin(-1, ELF_C_READ, ar);
	TASK_CHECK(e != NULL);
	TASK_CHECK(elf_getarhdr(e) != NULL &&
	    strcmp(elf_getarhdr(e)->ar_name, "/") == 0);
	(void) elf_end(e);

	TASK_CHECK(elf_rand(ar, (off_t) 92) == (off_t) 92);
	e = elf_begin(-1, ELF_C_READ, ar);
	TASK_CHECK(e != NULL);
	TASK_CHECK(elf_getarhdr(e) != NULL &&
	    strcmp(elf_getarhdr(e)->ar_name, "//") == 0);
	(void) elf_end(e);

	/* A regular member at the same archive still needs resolving. */
	TASK_CHECK(elf_rand(ar, syms[0].as_off) == syms[0].as_off);
	TASK_CHECK(elf_begin(-1, ELF_C_READ, ar) == NULL);

	(void) elf_end(ar);
	return (0);
}

static int
case_parent_closed_before_children(void)
{
	static struct audit_entry manifest[2];
	struct audit_state audit;
	struct audit_report reports[8];
	Elf *ar, *e1, *e2;
	Elf_Arsym *syms;
	size_t count;
	char s1[AUDIT_SYM_MAX], s2[AUDIT_SYM_MAX];

	manifest[0] = (struct audit_entry) { ALPHA_NAME, alpha_bytes,
	    OBJECT_SIZE, "alpha" };
	manifest[1] = (struct audit_entry) { BETA_NAME, beta_bytes,
	    OBJECT_SIZE, "beta" };
	(void) memset(&audit, 0, sizeof(audit));
	audit.manifest = manifest;
	audit.manifest_count = 2;
	audit.reports = reports;
	audit.report_capacity = 8;

	TASK_CHECK(ensure_fixtures() == 0);
	ar = elf_memory((char *) thin_bytes, thin_size);
	TASK_CHECK(ar != NULL);
	TASK_CHECK(elf_set_archive_resolver(ar, scratch_resolver, &audit)
	    == 0);
	syms = elf_getarsym(ar, &count);
	TASK_CHECK(syms != NULL);

	TASK_CHECK(elf_rand(ar, syms[0].as_off) == syms[0].as_off);
	e1 = elf_begin(-1, ELF_C_READ, ar);
	TASK_CHECK(e1 != NULL);
	TASK_CHECK(elf_rand(ar, syms[1].as_off) == syms[1].as_off);
	e2 = elf_begin(-1, ELF_C_READ, ar);
	TASK_CHECK(e2 != NULL);

	/* Close the parent first; the children stay readable. */
	TASK_CHECK(elf_end(ar) == 0);
	TASK_CHECK(defined_symbol(e1, s1, sizeof(s1)) != NULL &&
	    strcmp(s1, "alpha") == 0);
	TASK_CHECK(defined_symbol(e2, s2, sizeof(s2)) != NULL &&
	    strcmp(s2, "beta") == 0);
	TASK_CHECK(elf_getbase(e1) == 0 && elf_getbase(e2) == 0);

	/* The member header is still translatable without the parent. */
	{
		Elf_Arhdr *h1 = elf_getarhdr(e1);
		Elf_Arhdr *h2 = elf_getarhdr(e2);

		TASK_CHECK(h1 != NULL &&
		    strcmp(h1->ar_name, ALPHA_NAME) == 0 &&
		    h1->ar_size == OBJECT_SIZE);
		TASK_CHECK(h2 != NULL &&
		    strcmp(h2->ar_name, BETA_NAME) == 0 &&
		    h2->ar_size == OBJECT_SIZE);
		/* A repeated call returns the cached translated header. */
		TASK_CHECK(elf_getarhdr(e1) == h1);
	}

	TASK_CHECK(elf_end(e1) == 0);
	TASK_CHECK(elf_end(e2) == 0);
	return (0);
}

/*
 * Same lifecycle through the file descriptor entry point: close the
 * parent descriptor and the underlying fd while children are still
 * open.  The parent's mapped image is released, so each child must be
 * fully self-contained.
 */
static int
case_fd_parent_and_fd_closed_first(void)
{
	static struct audit_entry manifest[2];
	struct audit_state audit;
	struct audit_report reports[8];
	Elf *ar, *e1, *e2;
	Elf_Arsym *syms;
	size_t count;
	int fd;
	char s1[AUDIT_SYM_MAX], s2[AUDIT_SYM_MAX];

	TASK_CHECK(ensure_fixtures() == 0);
	manifest[0] = (struct audit_entry) { ALPHA_NAME, alpha_bytes,
	    OBJECT_SIZE, "alpha" };
	manifest[1] = (struct audit_entry) { BETA_NAME, beta_bytes,
	    OBJECT_SIZE, "beta" };
	(void) memset(&audit, 0, sizeof(audit));
	audit.manifest = manifest;
	audit.manifest_count = 2;
	audit.reports = reports;
	audit.report_capacity = 8;

	fd = open(THIN_ARCHIVE, O_RDONLY);
	TASK_CHECK(fd >= 0);
	ar = elf_begin(fd, ELF_C_READ, NULL);
	TASK_CHECK(ar != NULL && elf_kind(ar) == ELF_K_AR);
	TASK_CHECK(elf_set_archive_resolver(ar, scratch_resolver, &audit)
	    == 0);
	syms = elf_getarsym(ar, &count);
	TASK_CHECK(syms != NULL && count == 3);

	TASK_CHECK(elf_rand(ar, syms[0].as_off) == syms[0].as_off);
	e1 = elf_begin(fd, ELF_C_READ, ar);
	TASK_CHECK(e1 != NULL);
	TASK_CHECK(elf_rand(ar, syms[1].as_off) == syms[1].as_off);
	e2 = elf_begin(fd, ELF_C_READ, ar);
	TASK_CHECK(e2 != NULL);

	/* Release the parent descriptor and the file descriptor. */
	TASK_CHECK(elf_end(ar) == 0);
	TASK_CHECK(close(fd) == 0);

	/* Children keep their own bytes and cached member headers. */
	TASK_CHECK(defined_symbol(e1, s1, sizeof(s1)) != NULL &&
	    strcmp(s1, "alpha") == 0);
	TASK_CHECK(defined_symbol(e2, s2, sizeof(s2)) != NULL &&
	    strcmp(s2, "beta") == 0);
	TASK_CHECK(elf_getarhdr(e1) != NULL &&
	    strcmp(elf_getarhdr(e1)->ar_name, ALPHA_NAME) == 0);
	TASK_CHECK(elf_getarhdr(e2) != NULL &&
	    strcmp(elf_getarhdr(e2)->ar_name, BETA_NAME) == 0);
	TASK_CHECK(elf_getbase(e1) == 0 && elf_getbase(e2) == 0);

	TASK_CHECK(elf_end(e1) == 0);
	TASK_CHECK(elf_end(e2) == 0);
	return (0);
}

/* ------------------------------------------------------------------ */
/* Symbols group.                                                      */
/* ------------------------------------------------------------------ */

static int
case_symbol_index_to_real_definition(void)
{
	static struct audit_entry manifest[2];
	struct audit_state audit;
	struct audit_report reports[8];
	Elf *ar;
	Elf_Arsym *syms;
	size_t count, i;

	manifest[0] = (struct audit_entry) { ALPHA_NAME, alpha_bytes,
	    OBJECT_SIZE, "alpha" };
	manifest[1] = (struct audit_entry) { BETA_NAME, beta_bytes,
	    OBJECT_SIZE, "beta" };
	(void) memset(&audit, 0, sizeof(audit));
	audit.manifest = manifest;
	audit.manifest_count = 2;
	audit.reports = reports;
	audit.report_capacity = 8;

	TASK_CHECK(ensure_fixtures() == 0);
	ar = elf_memory((char *) thin_bytes, thin_size);
	TASK_CHECK(ar != NULL);
	TASK_CHECK(elf_set_archive_resolver(ar, scratch_resolver, &audit)
	    == 0);
	syms = elf_getarsym(ar, &count);
	TASK_CHECK(syms != NULL && count == 3);

	for (i = 0; syms[i].as_name != NULL; i++) {
		Elf *e;
		Elf_Arhdr *h;
		char sym[AUDIT_SYM_MAX];

		TASK_CHECK(elf_rand(ar, syms[i].as_off) == syms[i].as_off);
		e = elf_begin(-1, ELF_C_READ, ar);
		TASK_CHECK(e != NULL && elf_kind(e) == ELF_K_ELF);
		h = elf_getarhdr(e);
		TASK_CHECK(h != NULL && h->ar_size == OBJECT_SIZE);
		TASK_CHECK(defined_symbol(e, sym, sizeof(sym)) != NULL);
		TASK_CHECK(strcmp(sym, syms[i].as_name) == 0);
		if (i == 0)
			TASK_CHECK(strcmp(h->ar_name, ALPHA_NAME) == 0);
		else
			TASK_CHECK(strcmp(h->ar_name, BETA_NAME) == 0);
		TASK_CHECK(elf_getbase(e) == 0);
		(void) elf_end(e);
	}
	(void) elf_end(ar);
	return (0);
}

/*
 * Drive the audit end to end through the archive symbol index and
 * verify that unauthorized objects never enter the relation results.
 */
static int
case_audit_relations_exclude_unauthorized(void)
{
	static struct synth_member members[4];
	static struct audit_entry manifest[2], presented[4];
	struct audit_state audit;
	struct audit_report reports[16];
	struct module_relation relations[8];
	unsigned char *img;
	size_t img_size, nrel = 0, i;
	Elf *ar;
	Elf_Arsym *syms;
	size_t count;

	TASK_CHECK(ensure_fixtures() == 0);

	members[0] = (struct synth_member) { ALPHA_NAME, "alpha",
	    alpha_bytes, OBJECT_SIZE, 0 };
	members[1] = (struct synth_member) { BETA_NAME, "beta",
	    beta_bytes, OBJECT_SIZE, 0 };
	members[2] = (struct synth_member) { "/abs/evil.o", NULL,
	    beta_bytes, OBJECT_SIZE, 0 };
	members[3] = (struct synth_member) { "../escape.o", NULL,
	    beta_bytes, OBJECT_SIZE, 0 };
	img = build_thin_archive(members, 4, &img_size);
	TASK_CHECK(img != NULL);

	manifest[0] = (struct audit_entry) { ALPHA_NAME, alpha_bytes,
	    OBJECT_SIZE, "alpha" };
	manifest[1] = (struct audit_entry) { BETA_NAME, beta_bytes,
	    OBJECT_SIZE, "beta" };
	presented[0] = manifest[0];
	presented[1] = manifest[1];
	presented[2] = (struct audit_entry) { "/abs/evil.o", beta_bytes,
	    OBJECT_SIZE, "beta" };
	presented[3] = (struct audit_entry) { "../escape.o", beta_bytes,
	    OBJECT_SIZE, "beta" };

	(void) memset(&audit, 0, sizeof(audit));
	audit.manifest = manifest;
	audit.manifest_count = 2;
	audit.presented = presented;
	audit.presented_count = 4;
	audit.reports = reports;
	audit.report_capacity = 16;

	ar = elf_memory((char *) img, img_size);
	TASK_CHECK(ar != NULL && elf_kind(ar) == ELF_K_AR);
	TASK_CHECK(elf_set_archive_resolver(ar, audit_resolver, &audit)
	    == 0);
	syms = elf_getarsym(ar, &count);
	TASK_CHECK(syms != NULL);

	/*
	 * Attempt every member, including the two that no symbol
	 * references: rejected members must produce no descriptor and
	 * therefore never enter the module relation results.
	 */
	for (i = 0; i < 4; i++) {
		Elf *e;
		Elf_Arhdr *h;
		char sym[AUDIT_SYM_MAX];

		audit.current_offset = members[i].header_offset;
		TASK_CHECK(elf_rand(ar, members[i].header_offset) ==
		    members[i].header_offset);
		e = elf_begin(-1, ELF_C_READ, ar);
		if (e == NULL)
			continue;			/* rejected member */
		h = elf_getarhdr(e);
		TASK_CHECK(h != NULL);
		TASK_CHECK(defined_symbol(e, sym, sizeof(sym)) != NULL);
		TASK_CHECK(nrel < 8);
		relations[nrel].ar_offset = members[i].header_offset;
		(void) snprintf(relations[nrel].name,
		    sizeof(relations[nrel].name), "%s", h->ar_name);
		(void) snprintf(relations[nrel].symbol,
		    sizeof(relations[nrel].symbol), "%s", sym);
		nrel++;
		(void) elf_end(e);
	}

	/* Only the two manifest members enter the module relations. */
	TASK_CHECK(nrel == 2);
	TASK_CHECK(strcmp(relations[0].name, ALPHA_NAME) == 0 &&
	    strcmp(relations[0].symbol, "alpha") == 0);
	TASK_CHECK(relations[0].ar_offset == members[0].header_offset);
	TASK_CHECK(strcmp(relations[1].name, BETA_NAME) == 0 &&
	    strcmp(relations[1].symbol, "beta") == 0);
	TASK_CHECK(relations[1].ar_offset == members[1].header_offset);

	for (i = 0; i < nrel; i++) {
		TASK_CHECK(strcmp(relations[i].name, "/abs/evil.o") != 0);
		TASK_CHECK(strcmp(relations[i].name, "../escape.o") != 0);
	}

	{
		const struct audit_report *r;

		r = audit_find_report(&audit, "/abs/evil.o");
		TASK_CHECK(r != NULL && r->allowed == 0 &&
		    strcmp(r->reason, "absolute-path") == 0);
		r = audit_find_report(&audit, "../escape.o");
		TASK_CHECK(r != NULL && r->allowed == 0 &&
		    strcmp(r->reason, "parent-reference") == 0);
		r = audit_find_report(&audit, ALPHA_NAME);
		TASK_CHECK(r != NULL && r->allowed == 1 &&
		    r->ar_offset == members[0].header_offset &&
		    strcmp(r->symbol, "alpha") == 0);
	}

	(void) elf_end(ar);
	free(img);
	return (0);
}

static int
case_audit_replacement_retry_symbol(void)
{
	static struct audit_entry manifest[1], presented[1];
	struct audit_state audit;
	struct audit_report reports[8];
	unsigned char *tampered;
	Elf *ar, *e;
	Elf_Arsym *syms;
	size_t count;
	char sym[AUDIT_SYM_MAX];
	struct module_relation relation;
	int have_relation;

	TASK_CHECK(ensure_fixtures() == 0);

	tampered = malloc(OBJECT_SIZE);
	TASK_CHECK(tampered != NULL);
	(void) memcpy(tampered, alpha_bytes, OBJECT_SIZE);
	tampered[200] ^= 0x01;

	manifest[0] = (struct audit_entry) { ALPHA_NAME, alpha_bytes,
	    OBJECT_SIZE, "alpha" };
	presented[0] = (struct audit_entry) { ALPHA_NAME, tampered,
	    OBJECT_SIZE, "alpha" };

	(void) memset(&audit, 0, sizeof(audit));
	audit.manifest = manifest;
	audit.manifest_count = 1;
	audit.presented = presented;
	audit.presented_count = 1;
	audit.reports = reports;
	audit.report_capacity = 8;

	TASK_CHECK(ensure_fixtures() == 0);
	ar = elf_memory((char *) thin_bytes, thin_size);
	TASK_CHECK(ar != NULL);
	syms = elf_getarsym(ar, &count);
	TASK_CHECK(syms != NULL);
	TASK_CHECK(elf_set_archive_resolver(ar, audit_resolver, &audit)
	    == 0);
	audit.current_offset = syms[0].as_off;
	TASK_CHECK(elf_rand(ar, syms[0].as_off) == syms[0].as_off);

	have_relation = 0;
	TASK_CHECK(elf_begin(-1, ELF_C_READ, ar) == NULL);
	TASK_CHECK(have_relation == 0);	/* nothing was recorded */

	/* Repair the artifact and retry through the real ELF APIs. */
	presented[0].bytes = alpha_bytes;
	e = elf_begin(-1, ELF_C_READ, ar);
	TASK_CHECK(e != NULL && elf_kind(e) == ELF_K_ELF);
	TASK_CHECK(defined_symbol(e, sym, sizeof(sym)) != NULL &&
	    strcmp(sym, "alpha") == 0);
	relation.ar_offset = syms[0].as_off;
	(void) snprintf(relation.name, sizeof(relation.name), "%s",
	    ALPHA_NAME);
	(void) snprintf(relation.symbol, sizeof(relation.symbol), "%s",
	    sym);
	have_relation = 1;
	TASK_CHECK(have_relation == 1 &&
	    strcmp(relation.symbol, syms[0].as_name) == 0 &&
	    relation.ar_offset == syms[0].as_off);

	(void) elf_end(e);
	(void) elf_end(ar);
	free(tampered);
	return (0);
}

static int
case_regular_archive_symbol_walk_unchanged(void)
{
	Elf *ar;
	Elf_Arsym *syms;
	size_t count, i, found;

	TASK_CHECK(ensure_fixtures() == 0);
	ar = elf_memory((char *) regular_bytes, regular_size);
	TASK_CHECK(ar != NULL && elf_kind(ar) == ELF_K_AR);
	syms = elf_getarsym(ar, &count);
	TASK_CHECK(syms != NULL && count == 3);

	found = 0;
	for (i = 0; syms[i].as_name != NULL; i++) {
		Elf *e;
		Elf_Arhdr *h;
		char sym[AUDIT_SYM_MAX];

		TASK_CHECK(elf_rand(ar, syms[i].as_off) == syms[i].as_off);
		e = elf_begin(-1, ELF_C_READ, ar);
		TASK_CHECK(e != NULL && elf_kind(e) == ELF_K_ELF);
		h = elf_getarhdr(e);
		TASK_CHECK(h != NULL && h->ar_size == OBJECT_SIZE);
		TASK_CHECK(elf_getbase(e) == syms[i].as_off + 60);
		TASK_CHECK(defined_symbol(e, sym, sizeof(sym)) != NULL);
		TASK_CHECK(strcmp(sym, syms[i].as_name) == 0);
		found++;
		(void) elf_end(e);
	}
	TASK_CHECK(found == 2);
	(void) elf_end(ar);
	return (0);
}

/* ------------------------------------------------------------------ */
/* Registration.                                                       */
/* ------------------------------------------------------------------ */

const struct task_case task_cases[] = {
	/* resolution */
	{ "resolution", "kind-and-resolver-api",
	    case_kind_and_resolver_api },
	{ "resolution", "long-names-and-interleaved-members",
	    case_long_names_and_interleaved_members },
	{ "resolution", "failure-retry-keeps-cursor",
	    case_failure_retry_does_not_consume_cursor },
	{ "resolution", "length-mismatch-rejected",
	    case_length_mismatch_rejected },
	{ "resolution", "audit-path-policy-and-nested-names",
	    case_audit_path_policy_and_nested_names },
	{ "resolution", "object-replacement-then-manifest-fix",
	    case_object_replacement_then_manifest_fix },
	{ "resolution", "foreign-archive-reference-unsupported",
	    case_foreign_archive_reference_unsupported },

	/* cursor */
	{ "cursor", "physical-cursor-traversal",
	    case_physical_cursor_traversal },
	{ "cursor", "rand-roundtrip-and-bad-offsets",
	    case_rand_roundtrip_and_bad_offsets },
	{ "cursor", "last-header-ends-at-eof",
	    case_last_header_ends_at_eof },
	{ "cursor", "inline-tables-and-regular-archive",
	    case_inline_tables_and_regular_archive },
	{ "cursor", "inline-special-members-bypass-resolver",
	    case_inline_special_members_bypass_resolver },
	{ "cursor", "parent-closed-before-children",
	    case_parent_closed_before_children },
	{ "cursor", "fd-parent-and-fd-closed-first",
	    case_fd_parent_and_fd_closed_first },

	/* symbols */
	{ "symbols", "symbol-index-to-real-definition",
	    case_symbol_index_to_real_definition },
	{ "symbols", "audit-relations-exclude-unauthorized",
	    case_audit_relations_exclude_unauthorized },
	{ "symbols", "audit-replacement-retry-symbol",
	    case_audit_replacement_retry_symbol },
	{ "symbols", "regular-archive-symbol-walk-unchanged",
	    case_regular_archive_symbol_walk_unchanged },
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
