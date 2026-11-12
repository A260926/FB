/*
 * Name Constraints contract tests for the "minimal" X.509 engine.
 * CC0-1.0.
 *
 * The test builds a complete synthetic PKI in memory: deterministic
 * P-256 keys are generated with the library's HMAC-DRBG, certificates
 * are DER-encoded by the small writer below, and signatures are produced
 * with br_ecdsa_i31_sign_asn1(). No OpenSSL, network, file-based
 * private key, or external generator is used at runtime. The only files
 * read are the public DER fixtures in checks/fixtures.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bearssl.h"
#include "contract.h"

/* ------------------------------------------------------------------ */
/* DER writer                                                          */
/* ------------------------------------------------------------------ */

#define NC_W_CAP   8192

typedef struct {
	unsigned char b[NC_W_CAP];
	size_t len;
} wbuf;

static void
w_raw(wbuf *w, const void *data, size_t len)
{
	if (w->len + len > sizeof w->b) {
		fprintf(stderr, "der buffer overflow at w_raw len=%zu add=%zu\n",
			w->len, len);
		exit(2);
	}
	memcpy(w->b + w->len, data, len);
	w->len += len;
}

static void
w_u8(wbuf *w, unsigned char x)
{
	w_raw(w, &x, 1);
}

static size_t
w_mark(wbuf *w)
{
	return w->len;
}

/* Back-patch a TLV header around the content written since 'mark'. */
static void
w_tlv_end(wbuf *w, unsigned tag, size_t mark)
{
	size_t content_len, hdr_len, shift;
	unsigned char hdr[8];

	content_len = w->len - mark;
	hdr[0] = (unsigned char)tag;
	if (content_len < 0x80) {
		hdr_len = 2;
		hdr[1] = (unsigned char)content_len;
	} else {
		size_t n, x;

		n = 0;
		x = content_len;
		while (x != 0) {
			n ++;
			x >>= 8;
		}
		hdr_len = 1 + 1 + n;
		hdr[1] = (unsigned char)(0x80 | n);
		for (x = 0; x < n; x ++) {
			hdr[2 + n - 1 - x] =
				(unsigned char)(content_len >> (8 * x));
		}
	}
	shift = hdr_len;
	if (w->len + shift > sizeof w->b) {
		fprintf(stderr,
			"der buffer overflow at tlv_end mark=%zu len=%zu cl=%zu hdr=%zu tag=%02x\n",
			mark, w->len, content_len, hdr_len, tag);
		exit(2);
	}
	memmove(w->b + mark + shift, w->b + mark, content_len);
	memcpy(w->b + mark, hdr, hdr_len);
	w->len += shift;
}

static void
w_oid(wbuf *w, const unsigned char *oid, size_t len)
{
	size_t mark = w_mark(w);

	w_raw(w, oid, len);
	w_tlv_end(w, 0x06, mark);
}

static void
w_utf8(wbuf *w, const char *s)
{
	size_t mark = w_mark(w);

	w_raw(w, s, strlen(s));
	w_tlv_end(w, 0x0C, mark);
}

static void
w_ia5_tag(wbuf *w, unsigned tag, const void *s, size_t len)
{
	size_t mark = w_mark(w);

	w_raw(w, s, len);
	w_tlv_end(w, tag, mark);
}

static const unsigned char OID_ECPUB[] = {
	0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x02, 0x01
};
static const unsigned char OID_P256[] = {
	0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x03, 0x01, 0x07
};
static const unsigned char OID_ECDSA_SHA256[] = {
	0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03, 0x02
};
static const unsigned char OID_CN[] = {
	0x55, 0x04, 0x03
};
static const unsigned char OID_BC[] = {
	0x55, 0x1D, 0x13
};
static const unsigned char OID_KU[] = {
	0x55, 0x1D, 0x0F
};
static const unsigned char OID_SAN[] = {
	0x55, 0x1D, 0x11
};
static const unsigned char OID_NC[] = {
	0x55, 0x1D, 0x1E
};

static void
w_dn(wbuf *w, const char *cn)
{
	size_t s1 = w_mark(w), s2 = w_mark(w), s3 = w_mark(w);

	w_oid(w, OID_CN, sizeof OID_CN);
	w_utf8(w, cn);
	w_tlv_end(w, 0x30, s3);
	w_tlv_end(w, 0x31, s2);
	w_tlv_end(w, 0x30, s1);
}

static void
w_algid(wbuf *w)
{
	size_t s = w_mark(w);

	w_oid(w, OID_ECDSA_SHA256, sizeof OID_ECDSA_SHA256);
	w_tlv_end(w, 0x30, s);
}

/* ------------------------------------------------------------------ */
/* Synthetic key ring                                                  */
/* ------------------------------------------------------------------ */

typedef struct {
	br_ec_private_key sk;
	br_ec_public_key pk;
	unsigned char x[32];
	unsigned char q[65];
} nc_key;

#define NC_NUM_KEYS   6

static nc_key keys[NC_NUM_KEYS];
static int keys_ready;

static void
init_keys(void)
{
	int i;

	if (keys_ready) {
		return;
	}
	for (i = 0; i < NC_NUM_KEYS; i ++) {
		br_hmac_drbg_context rng;
		static const unsigned char seed_prefix[16] = {
			0x4E, 0x43, 0x2D, 0x74, 0x65, 0x73, 0x74, 0x2D,
			0x6B, 0x65, 0x79, 0x2D, 0x73, 0x65, 0x65, 0x64
		};
		unsigned char seed[32];
		size_t qlen;

		memcpy(seed, seed_prefix, sizeof seed_prefix);
		seed[20] = (unsigned char)i;
		br_hmac_drbg_init(&rng, &br_sha256_vtable,
			seed, sizeof seed);
		do {
			br_hmac_drbg_generate(&rng, keys[i].x, 32);
			keys[i].x[0] &= 0x7F;
		} while (keys[i].x[0] == 0);
		keys[i].sk.curve = BR_EC_secp256r1;
		keys[i].sk.x = keys[i].x;
		keys[i].sk.xlen = 32;
		qlen = br_ec_prime_i31.mulgen(keys[i].q,
			keys[i].x, 32, BR_EC_secp256r1);
		if (qlen != 65) {
			fprintf(stderr, "key generation failed\n");
			exit(2);
		}
		keys[i].pk.curve = BR_EC_secp256r1;
		keys[i].pk.q = keys[i].q;
		keys[i].pk.qlen = 65;
	}
	keys_ready = 1;
}

/* ------------------------------------------------------------------ */
/* Certificate specifications and builder                              */
/* ------------------------------------------------------------------ */

#define NC_GN_DNS   2
#define NC_GN_IP    7

typedef struct {
	int type;                 /* NC_GN_DNS / NC_GN_IP */
	const unsigned char *d;
	size_t dlen;
} nc_gn;

typedef struct {
	const nc_gn *permit;
	size_t num_permit;
	const nc_gn *exclude;
	size_t num_exclude;
	int minimum;              /* 0 = omit; 1 = minimum:1; 2 = minimum:0 */
	int maximum_present;
	int empty_value;          /* emit a zero-length NC value */
	const unsigned char *raw; /* raw NC extension content override */
	size_t raw_len;
} nc_spec;

typedef struct {
	int type;
	const unsigned char *d;
	size_t dlen;
} san_entry;

typedef struct {
	const char *issuer_cn;
	const char *subject_cn;
	const nc_key *issuer_key;
	const nc_key *subject_key;
	unsigned serial;
	int is_ca;
	int pathlen;
	int key_usage;             /* 0 = omit, otherwise critical KU byte */
	const san_entry *sans;
	size_t num_sans;
	const unsigned char *san_raw;
	size_t san_raw_len;
	const nc_spec *nc;
	int nc_critical;
	int nc_before_san;
	int nc_duplicate;
} cert_spec;

static void
w_bitstring(wbuf *w, const unsigned char *d, size_t len)
{
	size_t mark = w_mark(w);

	w_u8(w, 0x00);
	w_raw(w, d, len);
	w_tlv_end(w, 0x03, mark);
}

static void
w_spki(wbuf *w, const nc_key *k)
{
	size_t outer = w_mark(w), alg = w_mark(w);

	w_oid(w, OID_ECPUB, sizeof OID_ECPUB);
	w_oid(w, OID_P256, sizeof OID_P256);
	w_tlv_end(w, 0x30, alg);
	w_bitstring(w, k->q, k->pk.qlen);
	w_tlv_end(w, 0x30, outer);
}

static void
w_general_name(wbuf *w, const nc_gn *g)
{
	if (g->type == NC_GN_DNS) {
		w_ia5_tag(w, 0x82, g->d, g->dlen);
	} else {
		/* iPAddress uses an OCTET STRING under context tag 7. */
		w_ia5_tag(w, 0x87, g->d, g->dlen);
	}
}

static void
w_subtrees(wbuf *w, unsigned tag, const nc_gn *list, size_t n,
	const nc_spec *nc)
{
	size_t outer = w_mark(w), u;

	for (u = 0; u < n; u ++) {
		size_t st = w_mark(w);

		w_general_name(w, &list[u]);
		if (nc != NULL && nc->minimum) {
			/* [0] IMPLICIT INTEGER, primitive. */
			unsigned char minv[] = {
				0x80, 0x03, 0x02, 0x01,
				(unsigned char)(nc->minimum == 1 ? 1 : 0)
			};

			w_raw(w, minv, sizeof minv);
		}
		if (nc != NULL && nc->maximum_present) {
			/* [1] IMPLICIT INTEGER 1 (primitive). */
			unsigned char maxv[] = {
				0x81, 0x03, 0x02, 0x01, 0x01
			};

			w_raw(w, maxv, sizeof maxv);
		}
		w_tlv_end(w, 0x30, st);
	}
	w_tlv_end(w, tag, outer);
}

static void
w_nc_value(wbuf *w, const nc_spec *nc)
{
	size_t outer;

	if (nc->empty_value) {
		/* Emit no content: the extension value stays empty. */
		return;
	}
	if (nc->raw != NULL) {
		/* Raw bytes are the complete Name Constraints value,
		   i.e. a DER SEQUENCE (possibly empty). */
		w_raw(w, nc->raw, nc->raw_len);
		return;
	}
	outer = w_mark(w);
	if (nc->num_permit > 0) {
		w_subtrees(w, 0xA0, nc->permit, nc->num_permit, nc);
	}
	if (nc->num_exclude > 0) {
		w_subtrees(w, 0xA1, nc->exclude, nc->num_exclude, nc);
	}
	w_tlv_end(w, 0x30, outer);
}

static void
w_san_value(wbuf *w, const cert_spec *cs)
{
	size_t outer, u;

	if (cs->san_raw != NULL) {
		/* Raw bytes are the complete SubjectAltName value,
		   i.e. a DER SEQUENCE OF GeneralName. */
		w_raw(w, cs->san_raw, cs->san_raw_len);
		return;
	}
	outer = w_mark(w);
	for (u = 0; u < cs->num_sans; u ++) {
		if (cs->sans[u].type == NC_GN_DNS) {
			w_ia5_tag(w, 0x82, cs->sans[u].d, cs->sans[u].dlen);
		} else {
			w_ia5_tag(w, 0x87, cs->sans[u].d, cs->sans[u].dlen);
		}
	}
	w_tlv_end(w, 0x30, outer);
}

static void
w_extension(wbuf *w, const unsigned char *oid, size_t oid_len,
	int critical, const void *value, size_t value_len)
{
	size_t ext = w_mark(w);

	w_oid(w, oid, oid_len);
	if (critical) {
		unsigned char bv[] = {0x01, 0x01, 0xFF};

		w_raw(w, bv, sizeof bv);
	}
	{
		size_t oct = w_mark(w);

		w_raw(w, value, value_len);
		w_tlv_end(w, 0x04, oct);
	}
	w_tlv_end(w, 0x30, ext);
}

static void
build_cert(const cert_spec *cs, wbuf *out)
{
	static wbuf tbs, val, sigs;
	size_t tbs_start, ext_start, mark;
	unsigned char hv[32], sig[72];
	size_t sig_len;

	out->len = 0;
	memset(&tbs, 0, sizeof tbs);
	memset(&val, 0, sizeof val);
	memset(&sigs, 0, sizeof sigs);
	tbs_start = w_mark(&tbs);

	/* version [0] EXPLICIT INTEGER 2 */
	{
		unsigned char ver[] = {0xA0, 0x03, 0x02, 0x01, 0x02};

		w_raw(&tbs, ver, sizeof ver);
	}
	/* serial */
	{
		unsigned char ser[4];
		size_t s = w_mark(&tbs);

		ser[0] = (unsigned char)(cs->serial >> 24);
		ser[1] = (unsigned char)(cs->serial >> 16);
		ser[2] = (unsigned char)(cs->serial >> 8);
		ser[3] = (unsigned char)cs->serial;
		w_raw(&tbs, ser, 4);
		w_tlv_end(&tbs, 0x02, s);
	}
	w_algid(&tbs);
	w_dn(&tbs, cs->issuer_cn);

	/* validity: 2020-01-01 .. 2040-01-01, UTCTime (13 chars each). */
	{
		static const unsigned char nb[] = {
			0x17, 0x0D,
			'2', '0', '0', '1', '0', '1',
			'0', '0', '0', '0', '0', '0', 'Z'
		};
		static const unsigned char na[] = {
			0x17, 0x0D,
			'4', '0', '0', '1', '0', '1',
			'0', '0', '0', '0', '0', '0', 'Z'
		};
		size_t v = w_mark(&tbs);

		w_raw(&tbs, nb, sizeof nb);
		w_raw(&tbs, na, sizeof na);
		w_tlv_end(&tbs, 0x30, v);
	}
	w_dn(&tbs, cs->subject_cn);
	w_spki(&tbs, cs->subject_key);

	/* extensions */
	ext_start = w_mark(&tbs);
	{
		size_t seq = w_mark(&tbs);

		if (cs->is_ca) {
			size_t bc = w_mark(&val);

			w_u8(&val, 0x01);
			w_u8(&val, 0x01);
			w_u8(&val, 0xFF);
			if (cs->pathlen >= 0) {
				unsigned char pl[3];

				pl[0] = 0x02;
				pl[1] = 0x01;
				pl[2] = (unsigned char)cs->pathlen;
				w_raw(&val, pl, 3);
			}
			w_tlv_end(&val, 0x30, bc);
			w_extension(&tbs, OID_BC, sizeof OID_BC, 1,
				val.b, val.len);
			val.len = 0;
		}
		if (cs->key_usage) {
			/* BIT STRING: one unused bit for digitalSignature
			   (0x80), two for keyCertSign (0x04). */
			unsigned char ku_content[2];
			size_t ku = w_mark(&val);

			ku_content[0] = (cs->is_ca ? 2 : 7);
			ku_content[1] = (unsigned char)cs->key_usage;
			w_raw(&val, ku_content, 2);
			w_tlv_end(&val, 0x03, ku);
			w_extension(&tbs, OID_KU, sizeof OID_KU, 1,
				val.b, val.len);
			val.len = 0;
		}

		if (cs->nc != NULL && cs->nc_before_san) {
			w_nc_value(&val, cs->nc);
			w_extension(&tbs, OID_NC, sizeof OID_NC,
				cs->nc_critical, val.b, val.len);
			val.len = 0;
			if (cs->nc_duplicate) {
				w_nc_value(&val, cs->nc);
				w_extension(&tbs, OID_NC, sizeof OID_NC,
					cs->nc_critical, val.b, val.len);
				val.len = 0;
			}
		}
		if (cs->num_sans > 0 || cs->san_raw != NULL) {
			w_san_value(&val, cs);
			w_extension(&tbs, OID_SAN, sizeof OID_SAN, 0,
				val.b, val.len);
			val.len = 0;
		}
		if (cs->nc != NULL && !cs->nc_before_san) {
			w_nc_value(&val, cs->nc);
			w_extension(&tbs, OID_NC, sizeof OID_NC,
				cs->nc_critical, val.b, val.len);
			val.len = 0;
			if (cs->nc_duplicate) {
				w_nc_value(&val, cs->nc);
				w_extension(&tbs, OID_NC, sizeof OID_NC,
					cs->nc_critical, val.b, val.len);
				val.len = 0;
			}
		}

		w_tlv_end(&tbs, 0x30, seq);
	}
	w_tlv_end(&tbs, 0xA3, ext_start);
	w_tlv_end(&tbs, 0x30, tbs_start);

	/* sign TBS */
	br_sha256_init((br_sha256_context *)&sigs);
	br_sha256_update((br_sha256_context *)&sigs, tbs.b, tbs.len);
	br_sha256_out((br_sha256_context *)&sigs, hv);
	sig_len = br_ecdsa_i31_sign_asn1(&br_ec_prime_i31,
		&br_sha256_vtable, hv, &cs->issuer_key->sk, sig);
	if (sig_len == 0) {
		fprintf(stderr, "signing failed\n");
		exit(2);
	}

	/* Certificate ::= SEQUENCE { tbs, algId, BIT STRING sig } */
	mark = w_mark(out);
	w_raw(out, tbs.b, tbs.len);
	w_algid(out);
	w_bitstring(out, sig, sig_len);
	w_tlv_end(out, 0x30, mark);
}

/* ------------------------------------------------------------------ */
/* Fixture loading and trust anchor extraction                         */
/* ------------------------------------------------------------------ */

static unsigned char ta_dn[1024];
static size_t ta_dn_len;

static void
ta_append_dn(void *unused, const void *p, size_t n)
{
	(void)unused;
	if (n > sizeof ta_dn - ta_dn_len) {
		exit(2);
	}
	memcpy(ta_dn + ta_dn_len, p, n);
	ta_dn_len += n;
}

/*
 * Build a trust anchor from a DER certificate. The decoder context and
 * DN buffer are provided by the caller so each anchor owns independent
 * storage: the public key returned by the decoder references the
 * decoder's internal buffers, so a shared decoder would clobber an
 * anchor built earlier.
 */
static void
anchor_from_cert(const unsigned char *der, size_t der_len,
	br_x509_trust_anchor *ta,
	br_x509_decoder_context *dc,
	unsigned char *dn_buf, size_t *dn_len)
{
	br_x509_pkey *pk;

	ta_dn_len = 0;
	br_x509_decoder_init(dc, ta_append_dn, NULL);
	br_x509_decoder_push(dc, der, der_len);
	pk = br_x509_decoder_get_pkey(dc);
	if (pk == NULL || ta_dn_len == 0) {
		fprintf(stderr, "trust anchor extraction failed\n");
		exit(2);
	}
	memcpy(dn_buf, ta_dn, ta_dn_len);
	ta->dn.data = dn_buf;
	ta->dn.len = ta_dn_len;
	ta->flags = BR_X509_TA_CA;
	ta->pkey = *pk;
	*dn_len = ta_dn_len;
}

static unsigned char *
load_file(const char *name, size_t *n)
{
	unsigned char *p;
	long z;
	FILE *f = fopen(name, "rb");

	if (!f || fseek(f, 0, SEEK_END) || (z = ftell(f)) <= 0
		|| fseek(f, 0, SEEK_SET))
	{
		fprintf(stderr, "cannot load %s\n", name);
		exit(2);
	}
	p = malloc((size_t)z);
	if (!p || fread(p, 1, (size_t)z, f) != (size_t)z || fclose(f)) {
		exit(2);
	}
	*n = (size_t)z;
	return p;
}

/* ------------------------------------------------------------------ */
/* Chain runner                                                         */
/* ------------------------------------------------------------------ */

#define NC_BIG_WS   16384

static const unsigned char *
find_sub(const unsigned char *h, size_t hlen,
	const unsigned char *n, size_t nlen)
{
	size_t i;

	if (nlen == 0 || nlen > hlen) {
		return NULL;
	}
	for (i = 0; i + nlen <= hlen; i ++) {
		if (memcmp(h + i, n, nlen) == 0) {
			return h + i;
		}
	}
	return NULL;
}

/*
 * Run a chain. 'cuts' has one offset per cert (SIZE_MAX = no cut); the
 * certificate is otherwise fed in chunks of 'chunk' bytes.
 */
static unsigned
run_chain(const br_x509_trust_anchor *ta,
	const wbuf *certs, size_t ncerts, const char *server,
	unsigned char *ws, size_t wslen, size_t chunk,
	const size_t *cuts, const br_x509_pkey **pk_out)
{
	br_x509_minimal_context c;
	size_t i;

	br_x509_minimal_init_full(&c, ta, 1);
	br_x509_minimal_set_time(&c, 739951, 0);
	if (ws != NULL || wslen != 0) {
		br_x509_minimal_set_name_constraints(&c, ws, wslen);
	}
	c.vtable->start_chain(&c.vtable, server);
	for (i = 0; i < ncerts; i ++) {
		const unsigned char *p = certs[i].b;
		size_t n = certs[i].len, pos = 0;

		c.vtable->start_cert(&c.vtable, (uint32_t)n);
		if (cuts != NULL && cuts[i] != (size_t)-1 && cuts[i] < n) {
			size_t k = cuts[i];

			c.vtable->append(&c.vtable, p, k);
			c.vtable->append(&c.vtable, p + k, n - k);
		} else {
			while (pos < n) {
				size_t z = n - pos;

				if (z > chunk) {
					z = chunk;
				}
				c.vtable->append(&c.vtable, p + pos, z);
				pos += z;
			}
		}
		c.vtable->end_cert(&c.vtable);
	}
	{
		unsigned r = c.vtable->end_chain(&c.vtable);

		if (pk_out != NULL) {
			*pk_out = c.vtable->get_pkey(&c.vtable, NULL);
		}
		return r;
	}
}

static unsigned char big_ws[NC_BIG_WS];
static unsigned char small_ws[128];

/* ------------------------------------------------------------------ */
/* Reusable specs                                                       */
/* ------------------------------------------------------------------ */

static const unsigned char d_example[] = "example.test";
static const unsigned char d_sub_example[] = "sub.example.test";
static const unsigned char d_service[] = "service.example.test";
static const unsigned char d_a_sub[] = "a.sub.example.test";
static const unsigned char d_b_example[] = "b.example.test";
static const unsigned char d_outside[] = "outside.invalid";
static const unsigned char d_badexample[] = "badexample.test";
static const unsigned char d_lead_example[] = ".example.test";
static const unsigned char d_a_example[] = "a.example.test";
static const unsigned char d_wild[] = "*.example.test";
static const unsigned char d_lead_a[] = ".a.example.test";
static const unsigned char d_a_test[] = "a.test";
static const unsigned char d_b_test[] = "b.test";
static const unsigned char d_c_test[] = "c.test";

static const unsigned char ip4_name_good[] = {10, 1, 2, 3};
static const unsigned char ip4_name_bad[] = {11, 1, 2, 3};
static const unsigned char ip4_cons[] = {
	10, 0, 0, 0, 255, 0, 0, 0
};
static const unsigned char ip4_cons_24[] = {
	10, 1, 2, 0, 255, 255, 255, 0
};
static const unsigned char ip6_name_good[16] = {
	0x20, 0x01, 0x0D, 0xB8, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 1
};
static const unsigned char ip6_name_bad[16] = {
	0x20, 0x01, 0x0D, 0xB8, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 2
};
/* Exact /128 match against 2001:db8::1. */
static const unsigned char ip6_cons[32] = {
	0x20, 0x01, 0x0D, 0xB8, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 1,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};

static const nc_gn gn_permit_example[] = {
	{NC_GN_DNS, d_example, sizeof d_example - 1}
};
static const nc_gn gn_lead_example[] = {
	{NC_GN_DNS, d_lead_example, sizeof d_lead_example - 1}
};
static const nc_gn gn_permit_a_only[] = {
	{NC_GN_DNS, d_a_example, sizeof d_a_example - 1}
};
static const nc_gn gn_exclude_a[] = {
	{NC_GN_DNS, d_a_example, sizeof d_a_example - 1}
};
static const nc_gn gn_exclude_lead_a[] = {
	{NC_GN_DNS, d_lead_a, sizeof d_lead_a - 1}
};
static const nc_gn gn_exclude_example[] = {
	{NC_GN_DNS, d_example, sizeof d_example - 1}
};
static const nc_gn gn_exclude_test[] = {
	{NC_GN_DNS, (const unsigned char *)"test", 4}
};
static const nc_gn gn_ip4[] = {
	{NC_GN_IP, ip4_cons, sizeof ip4_cons}
};
static const nc_gn gn_ip6[] = {
	{NC_GN_IP, ip6_cons, sizeof ip6_cons}
};
static const nc_gn gn_ip4_24[] = {
	{NC_GN_IP, ip4_cons_24, sizeof ip4_cons_24}
};
static const nc_gn gn_permit_ab[] = {
	{NC_GN_DNS, d_a_test, sizeof d_a_test - 1},
	{NC_GN_DNS, d_b_test, sizeof d_b_test - 1}
};

static const nc_spec nc_example = {
	gn_permit_example, 1, NULL, 0, 0, 0, 0, NULL, 0
};
static const nc_spec nc_lead = {
	gn_lead_example, 1, NULL, 0, 0, 0, 0, NULL, 0
};
static const nc_spec nc_a_only = {
	gn_permit_a_only, 1, NULL, 0, 0, 0, 0, NULL, 0
};
static const nc_spec nc_permit_exclude_a = {
	gn_permit_example, 1, gn_exclude_a, 1, 0, 0, 0, NULL, 0
};
static const nc_spec nc_permit_exclude_lead_a = {
	gn_permit_example, 1, gn_exclude_lead_a, 1, 0, 0, 0, NULL, 0
};
static const nc_spec nc_permit_exclude_example = {
	gn_permit_example, 1, gn_exclude_example, 1, 0, 0, 0, NULL, 0
};
static const nc_spec nc_permit_exclude_test = {
	gn_permit_example, 1, gn_exclude_test, 1, 0, 0, 0, NULL, 0
};
static const nc_spec nc_ip4_spec = {
	gn_ip4, 1, NULL, 0, 0, 0, 0, NULL, 0
};
static const nc_spec nc_ip6_spec = {
	gn_ip6, 1, NULL, 0, 0, 0, 0, NULL, 0
};
static const nc_spec nc_ip4_24_spec = {
	gn_ip4_24, 1, NULL, 0, 0, 0, 0, NULL, 0
};
static const nc_spec nc_ab_spec = {
	gn_permit_ab, 2, NULL, 0, 0, 0, 0, NULL, 0
};

static const san_entry san_service[] = {
	{NC_GN_DNS, d_service, sizeof d_service - 1}
};
static const san_entry san_outside[] = {
	{NC_GN_DNS, d_outside, sizeof d_outside - 1}
};
static const san_entry san_badexample[] = {
	{NC_GN_DNS, d_badexample, sizeof d_badexample - 1}
};
static const san_entry san_wild[] = {
	{NC_GN_DNS, d_wild, sizeof d_wild - 1}
};
static const san_entry san_a_sub[] = {
	{NC_GN_DNS, d_a_sub, sizeof d_a_sub - 1}
};
static const san_entry san_b_example[] = {
	{NC_GN_DNS, d_b_example, sizeof d_b_example - 1}
};
static const san_entry san_b_test[] = {
	{NC_GN_DNS, d_b_test, sizeof d_b_test - 1}
};
static const san_entry san_c_test[] = {
	{NC_GN_DNS, d_c_test, sizeof d_c_test - 1}
};
static const san_entry san_ip4_good[] = {
	{NC_GN_IP, ip4_name_good, 4}
};
static const san_entry san_ip4_bad[] = {
	{NC_GN_IP, ip4_name_bad, 4}
};
static const san_entry san_ip6_good[] = {
	{NC_GN_IP, ip6_name_good, 16}
};
static const san_entry san_ip6_bad[] = {
	{NC_GN_IP, ip6_name_bad, 16}
};

/* Standard two-cert chain: root key 0, CA key 1, leaf key 2. */
static int force_duplicate_nc;

static void
build_standard_chain(const nc_spec *ca_nc, int ca_critical,
	const san_entry *sans, size_t num_sans,
	const unsigned char *san_raw, size_t san_raw_len,
	int nc_on_leaf, int swap_order,
	wbuf *leaf_out, wbuf *ca_out)
{
	cert_spec ca, leaf;

	memset(&ca, 0, sizeof ca);
	ca.issuer_cn = "NC Test Root";
	ca.subject_cn = "NC Test CA";
	ca.issuer_key = &keys[0];
	ca.subject_key = &keys[1];
	ca.serial = 1;
	ca.is_ca = 1;
	ca.pathlen = 4;
	ca.key_usage = 0x04;
	ca.nc = ca_nc;
	ca.nc_critical = ca_critical;
	ca.nc_before_san = swap_order;
	ca.nc_duplicate = force_duplicate_nc;
	build_cert(&ca, ca_out);

	memset(&leaf, 0, sizeof leaf);
	leaf.issuer_cn = "NC Test CA";
	leaf.subject_cn = "NC Test Leaf";
	leaf.issuer_key = &keys[1];
	leaf.subject_key = &keys[2];
	leaf.serial = 2;
	leaf.is_ca = 0;
	leaf.pathlen = -1;
	leaf.key_usage = 0x80;
	leaf.sans = sans;
	leaf.num_sans = num_sans;
	leaf.san_raw = san_raw;
	leaf.san_raw_len = san_raw_len;
	leaf.nc = nc_on_leaf ? ca_nc : NULL;
	leaf.nc_critical = ca_critical;
	leaf.nc_before_san = swap_order;
	build_cert(&leaf, leaf_out);
}

static br_x509_trust_anchor root_ta;

static void
build_root_anchor(void)
{
	static wbuf root;
	static br_x509_decoder_context dc;
	static unsigned char dn[1024];
	size_t dn_len;
	cert_spec rs;

	memset(&rs, 0, sizeof rs);
	rs.issuer_cn = "NC Test Root";
	rs.subject_cn = "NC Test Root";
	rs.issuer_key = &keys[0];
	rs.subject_key = &keys[0];
	rs.serial = 0;
	rs.is_ca = 1;
	rs.pathlen = 6;
	rs.key_usage = 0x04;
	build_cert(&rs, &root);
	anchor_from_cert(root.b, root.len, &root_ta, &dc, dn, &dn_len);
}

/* ------------------------------------------------------------------ */
/* DER group cases                                                     */
/* ------------------------------------------------------------------ */

static int
case_permitted_pass(void)
{
	wbuf leaf, ca;
	const br_x509_pkey *pk = NULL;
	unsigned r;

	build_standard_chain(&nc_example, 0,
		san_service, 1, NULL, 0, 0, 0, &leaf, &ca);
	{
		wbuf chain[2] = {leaf, ca};

		r = run_chain(&root_ta, chain, 2, NULL,
			big_ws, sizeof big_ws, 4096, NULL, &pk);
	}
	NC_CHECK(r == 0);
	NC_CHECK(pk != NULL);
	return 0;
}

static int
case_permitted_violation(void)
{
	wbuf leaf, ca;
	const br_x509_pkey *pk = (void *)1;
	unsigned r;

	build_standard_chain(&nc_example, 0,
		san_outside, 1, NULL, 0, 0, 0, &leaf, &ca);
	{
		wbuf chain[2] = {leaf, ca};

		r = run_chain(&root_ta, chain, 2, NULL,
			big_ws, sizeof big_ws, 4096, NULL, &pk);
	}
	NC_CHECK(r == BR_ERR_X509_NAME_CONSTRAINTS);
	NC_CHECK(pk == NULL);
	return 0;
}

static int
case_non_critical_enforced(void)
{
	wbuf leaf, ca;
	unsigned r;

	/* The seed scenario: non-critical NC must now be enforced. */
	build_standard_chain(&nc_example, 0,
		san_outside, 1, NULL, 0, 0, 0, &leaf, &ca);
	{
		wbuf chain[2] = {leaf, ca};

		r = run_chain(&root_ta, chain, 2, NULL,
			big_ws, sizeof big_ws, 1, NULL, NULL);
	}
	NC_CHECK(r == BR_ERR_X509_NAME_CONSTRAINTS);
	return 0;
}

static int
case_critical_supported(void)
{
	wbuf leaf, ca;
	unsigned r;

	/* Critical NC must not produce CRITICAL_EXTENSION anymore. */
	build_standard_chain(&nc_example, 1,
		san_service, 1, NULL, 0, 0, 0, &leaf, &ca);
	{
		wbuf chain[2] = {leaf, ca};

		r = run_chain(&root_ta, chain, 2, NULL,
			big_ws, sizeof big_ws, 1, NULL, NULL);
	}
	NC_CHECK(r == 0);
	return 0;
}

static int
case_leading_dot_boundaries(void)
{
	static const san_entry san_example[] = {
		{NC_GN_DNS, d_example, sizeof d_example - 1}
	};
	wbuf leaf, ca;
	unsigned r1, r2;

	build_standard_chain(&nc_lead, 0,
		san_service, 1, NULL, 0, 0, 0, &leaf, &ca);
	{
		wbuf chain[2] = {leaf, ca};

		/* Sub-domain matches ".example.test". */
		r1 = run_chain(&root_ta, chain, 2, NULL,
			big_ws, sizeof big_ws, 4096, NULL, NULL);
	}
	build_standard_chain(&nc_lead, 0,
		san_example, 1, NULL, 0, 0, 0, &leaf, &ca);
	{
		wbuf chain[2] = {leaf, ca};

		/* The exact domain does not match the sub-only form. */
		r2 = run_chain(&root_ta, chain, 2, NULL,
			big_ws, sizeof big_ws, 4096, NULL, NULL);
	}
	NC_CHECK(r1 == 0);
	NC_CHECK(r2 == BR_ERR_X509_NAME_CONSTRAINTS);
	return 0;
}

static int
case_label_boundary(void)
{
	wbuf leaf, ca;
	unsigned r;

	build_standard_chain(&nc_example, 0,
		san_badexample, 1, NULL, 0, 0, 0, &leaf, &ca);
	{
		wbuf chain[2] = {leaf, ca};

		r = run_chain(&root_ta, chain, 2, NULL,
			big_ws, sizeof big_ws, 4096, NULL, NULL);
	}
	NC_CHECK(r == BR_ERR_X509_NAME_CONSTRAINTS);
	return 0;
}

static int
case_wildcard_fully_permitted(void)
{
	wbuf leaf, ca;
	unsigned r;

	build_standard_chain(&nc_example, 0,
		san_wild, 1, NULL, 0, 0, 0, &leaf, &ca);
	{
		wbuf chain[2] = {leaf, ca};

		r = run_chain(&root_ta, chain, 2, NULL,
			big_ws, sizeof big_ws, 4096, NULL, NULL);
	}
	NC_CHECK(r == 0);
	return 0;
}

static int
case_wildcard_partial_permit(void)
{
	wbuf leaf, ca;
	unsigned r;

	/* Permitting only a.example.test cannot cover *.example.test. */
	build_standard_chain(&nc_a_only, 0,
		san_wild, 1, NULL, 0, 0, 0, &leaf, &ca);
	{
		wbuf chain[2] = {leaf, ca};

		r = run_chain(&root_ta, chain, 2, NULL,
			big_ws, sizeof big_ws, 4096, NULL, NULL);
	}
	NC_CHECK(r == BR_ERR_X509_NAME_CONSTRAINTS);
	return 0;
}

static int
case_wildcard_excluded_intersection(void)
{
	wbuf leaf, ca;
	unsigned r1, r2, r3, r4;

	build_standard_chain(&nc_permit_exclude_a, 0,
		san_wild, 1, NULL, 0, 0, 0, &leaf, &ca);
	{
		wbuf chain[2] = {leaf, ca};

		/* Excluded a.example.test intersects the wildcard set. */
		r1 = run_chain(&root_ta, chain, 2, NULL,
			big_ws, sizeof big_ws, 4096, NULL, NULL);
	}
	build_standard_chain(&nc_permit_exclude_lead_a, 0,
		san_wild, 1, NULL, 0, 0, 0, &leaf, &ca);
	{
		wbuf chain[2] = {leaf, ca};

		/* Excluded .a.example.test is disjoint from the set. */
		r2 = run_chain(&root_ta, chain, 2, NULL,
			big_ws, sizeof big_ws, 4096, NULL, NULL);
	}

	/* Exact-domain exclusion of the parent itself does not hit any
	   single-label wildcard expansion. */
	build_standard_chain(&nc_permit_exclude_example, 0,
		san_wild, 1, NULL, 0, 0, 0, &leaf, &ca);
	{
		wbuf chain[2] = {leaf, ca};

		r3 = run_chain(&root_ta, chain, 2, NULL,
			big_ws, sizeof big_ws, 4096, NULL, NULL);
	}
	/* An exact-domain exclusion on a strict ancestor does hit. */
	build_standard_chain(&nc_permit_exclude_test, 0,
		san_wild, 1, NULL, 0, 0, 0, &leaf, &ca);
	{
		wbuf chain[2] = {leaf, ca};

		r4 = run_chain(&root_ta, chain, 2, NULL,
			big_ws, sizeof big_ws, 4096, NULL, NULL);
	}
	NC_CHECK(r1 == BR_ERR_X509_NAME_CONSTRAINTS);
	NC_CHECK(r2 == 0);
	NC_CHECK(r3 == 0);
	NC_CHECK(r4 == BR_ERR_X509_NAME_CONSTRAINTS);
	return 0;
}

static int
case_ipv4_constraints(void)
{
	wbuf leaf, ca;
	unsigned r1, r2, r3;

	build_standard_chain(&nc_ip4_spec, 0,
		san_ip4_good, 1, NULL, 0, 0, 0, &leaf, &ca);
	{
		wbuf chain[2] = {leaf, ca};

		r1 = run_chain(&root_ta, chain, 2, NULL,
			big_ws, sizeof big_ws, 4096, NULL, NULL);
	}
	build_standard_chain(&nc_ip4_spec, 0,
		san_ip4_bad, 1, NULL, 0, 0, 0, &leaf, &ca);
	{
		wbuf chain[2] = {leaf, ca};

		r2 = run_chain(&root_ta, chain, 2, NULL,
			big_ws, sizeof big_ws, 4096, NULL, NULL);
	}
	/* A /24 constraint: .3 matches, and the v4 name is not folded
	   into the v6 space. */
	build_standard_chain(&nc_ip4_24_spec, 0,
		san_ip4_good, 1, NULL, 0, 0, 0, &leaf, &ca);
	{
		wbuf chain[2] = {leaf, ca};

		r3 = run_chain(&root_ta, chain, 2, NULL,
			big_ws, sizeof big_ws, 4096, NULL, NULL);
	}
	NC_CHECK(r1 == 0);
	NC_CHECK(r2 == BR_ERR_X509_NAME_CONSTRAINTS);
	NC_CHECK(r3 == 0);
	return 0;
}

static int
case_ipv6_constraints(void)
{
	wbuf leaf, ca;
	unsigned r1, r2;

	build_standard_chain(&nc_ip6_spec, 0,
		san_ip6_good, 1, NULL, 0, 0, 0, &leaf, &ca);
	{
		wbuf chain[2] = {leaf, ca};

		r1 = run_chain(&root_ta, chain, 2, NULL,
			big_ws, sizeof big_ws, 4096, NULL, NULL);
	}
	build_standard_chain(&nc_ip6_spec, 0,
		san_ip6_bad, 1, NULL, 0, 0, 0, &leaf, &ca);
	{
		wbuf chain[2] = {leaf, ca};

		r2 = run_chain(&root_ta, chain, 2, NULL,
			big_ws, sizeof big_ws, 4096, NULL, NULL);
	}
	NC_CHECK(r1 == 0);
	NC_CHECK(r2 == BR_ERR_X509_NAME_CONSTRAINTS);
	return 0;
}

static int
case_ip_families_separate(void)
{
	wbuf leaf, ca;
	unsigned r;

	/* A v4 name under a v6-only permitted list is unrestricted. */
	build_standard_chain(&nc_ip6_spec, 0,
		san_ip4_good, 1, NULL, 0, 0, 0, &leaf, &ca);
	{
		wbuf chain[2] = {leaf, ca};

		r = run_chain(&root_ta, chain, 2, NULL,
			big_ws, sizeof big_ws, 4096, NULL, NULL);
	}
	NC_CHECK(r == 0);
	return 0;
}

static int
case_invalid_san_dns(void)
{
	static const unsigned char non_ascii[] = {
		'h', 0xC3, 0xA9, '.', 'e', 'x', 'a', 'm',
		'p', 'l', 'e', '.', 't', 'e', 's', 't'
	};
	static const struct {
		const unsigned char *s;
		size_t len;
	} bad[] = {
		{(const unsigned char *)"a\0b.example", 11},
		{(const unsigned char *)"trail.example.", 14},
		{(const unsigned char *)".lead.example", 13},
		{(const unsigned char *)"em*bed.example", 13},
		{(const unsigned char *)"empty..example", 14},
		{non_ascii, sizeof non_ascii},
		{(const unsigned char *)"*only.example", 12}
	};
	size_t i;

	for (i = 0; i < sizeof bad / sizeof bad[0]; i ++) {
		san_entry se;
		wbuf leaf, ca;
		unsigned r;

		memset(&se, 0, sizeof se);
		se.type = NC_GN_DNS;
		se.d = bad[i].s;
		se.dlen = bad[i].len;
		build_standard_chain(&nc_example, 0,
			&se, 1, NULL, 0, 0, 0, &leaf, &ca);
		{
			wbuf chain[2] = {leaf, ca};

			r = run_chain(&root_ta, chain, 2, NULL,
				big_ws, sizeof big_ws, 4096, NULL, NULL);
		}
		NC_CHECK(r != 0);
	}
	return 0;
}

static int
case_invalid_constraint_dns(void)
{
	static const unsigned char c_wild[] = "*.example.test";
	static const unsigned char c_empty[] = "";
	static const unsigned char c_trail[] = "example.test.";
	nc_gn g;
	nc_spec spec;
	wbuf leaf, ca;
	unsigned r;

	memset(&spec, 0, sizeof spec);
	spec.permit = &g;
	spec.num_permit = 1;

	g.type = NC_GN_DNS;
	g.d = c_wild;
	g.dlen = sizeof c_wild - 1;
	build_standard_chain(&spec, 0,
		san_service, 1, NULL, 0, 0, 0, &leaf, &ca);
	{
		wbuf chain[2] = {leaf, ca};

		r = run_chain(&root_ta, chain, 2, NULL,
			big_ws, sizeof big_ws, 4096, NULL, NULL);
	}
	NC_CHECK(r == BR_ERR_X509_INVALID_VALUE);

	g.d = c_empty;
	g.dlen = 0;
	build_standard_chain(&spec, 0,
		san_service, 1, NULL, 0, 0, 0, &leaf, &ca);
	{
		wbuf chain[2] = {leaf, ca};

		r = run_chain(&root_ta, chain, 2, NULL,
			big_ws, sizeof big_ws, 4096, NULL, NULL);
	}
	NC_CHECK(r == BR_ERR_X509_INVALID_VALUE);

	g.d = c_trail;
	g.dlen = sizeof c_trail - 1;
	build_standard_chain(&spec, 0,
		san_service, 1, NULL, 0, 0, 0, &leaf, &ca);
	{
		wbuf chain[2] = {leaf, ca};

		r = run_chain(&root_ta, chain, 2, NULL,
			big_ws, sizeof big_ws, 4096, NULL, NULL);
	}
	NC_CHECK(r == BR_ERR_X509_INVALID_VALUE);
	return 0;
}

static int
case_unsupported_generalname(void)
{
	/* NC value: SEQUENCE { permittedSubtrees { rfc822Name } } */
	static const unsigned char raw_rfc822[] = {
		0x30, 0x0E,
		0xA0, 0x0C,
		0x30, 0x0A,
		0x81, 0x08, 'a', '@', 'b', '.', 't', 'e', 's', 't'
	};
	/* NC value: SEQUENCE { permittedSubtrees { directoryName [4] } } */
	static const unsigned char raw_dir[] = {
		0x30, 0x08,
		0xA0, 0x06,
		0x30, 0x04,
		0xA4, 0x02,
		0x30, 0x00
	};
	nc_spec spec;
	wbuf leaf, ca;
	unsigned r1, r2;

	memset(&spec, 0, sizeof spec);
	spec.raw = raw_rfc822;
	spec.raw_len = sizeof raw_rfc822;
	build_standard_chain(&spec, 0,
		san_service, 1, NULL, 0, 0, 0, &leaf, &ca);
	{
		wbuf chain[2] = {leaf, ca};

		r1 = run_chain(&root_ta, chain, 2, NULL,
			big_ws, sizeof big_ws, 4096, NULL, NULL);
	}
	spec.raw = raw_dir;
	spec.raw_len = sizeof raw_dir;
	build_standard_chain(&spec, 0,
		san_service, 1, NULL, 0, 0, 0, &leaf, &ca);
	{
		wbuf chain[2] = {leaf, ca};

		r2 = run_chain(&root_ta, chain, 2, NULL,
			big_ws, sizeof big_ws, 4096, NULL, NULL);
	}
	NC_CHECK(r1 == BR_ERR_X509_UNSUPPORTED);
	NC_CHECK(r2 == BR_ERR_X509_UNSUPPORTED);
	return 0;
}

static int
case_min_max_unsupported(void)
{
	nc_spec spec;
	wbuf leaf, ca;
	unsigned r1, r2;

	memset(&spec, 0, sizeof spec);
	spec.permit = gn_permit_example;
	spec.num_permit = 1;
	spec.minimum = 1;
	build_standard_chain(&spec, 0,
		san_service, 1, NULL, 0, 0, 0, &leaf, &ca);
	{
		wbuf chain[2] = {leaf, ca};

		r1 = run_chain(&root_ta, chain, 2, NULL,
			big_ws, sizeof big_ws, 4096, NULL, NULL);
	}
	spec.minimum = 0;
	spec.maximum_present = 1;
	build_standard_chain(&spec, 0,
		san_service, 1, NULL, 0, 0, 0, &leaf, &ca);
	{
		wbuf chain[2] = {leaf, ca};

		r2 = run_chain(&root_ta, chain, 2, NULL,
			big_ws, sizeof big_ws, 4096, NULL, NULL);
	}
	NC_CHECK(r1 == BR_ERR_X509_UNSUPPORTED);
	NC_CHECK(r2 == BR_ERR_X509_UNSUPPORTED);

	/* minimum=0 explicitly present is the default and must pass. */
	{
		nc_spec spec0;
		unsigned r0;

		memset(&spec0, 0, sizeof spec0);
		spec0.permit = gn_permit_example;
		spec0.num_permit = 1;
		spec0.minimum = 2;
		build_standard_chain(&spec0, 0,
			san_service, 1, NULL, 0, 0, 0, &leaf, &ca);
		{
			wbuf chain[2] = {leaf, ca};

			r0 = run_chain(&root_ta, chain, 2, NULL,
				big_ws, sizeof big_ws, 4096, NULL, NULL);
		}
		NC_CHECK(r0 == 0);
	}
	return 0;
}

static int
case_empty_and_duplicate(void)
{
	static const unsigned char raw_empty_seq[] = {0x30, 0x00};
	/* NC value with a permitted field containing an empty
	   GeneralSubtree: SEQUENCE { [0] { SEQUENCE {} } }. */
	static const unsigned char raw_empty_subtree[] = {
		0x30, 0x04, 0xA0, 0x02, 0x30, 0x00
	};
	/* NC value with a zero-length permitted list. */
	static const unsigned char raw_empty_list[] = {
		0x30, 0x02, 0xA0, 0x00
	};
	nc_spec spec;
	wbuf leaf, ca;
	unsigned r1, r2, r3;

	memset(&spec, 0, sizeof spec);

	/* Empty extension value (no content at all). */
	spec.empty_value = 1;
	build_standard_chain(&spec, 0,
		san_service, 1, NULL, 0, 0, 0, &leaf, &ca);
	{
		wbuf chain[2] = {leaf, ca};

		r1 = run_chain(&root_ta, chain, 2, NULL,
			big_ws, sizeof big_ws, 4096, NULL, NULL);
	}
	spec.empty_value = 0;

	/* An empty GeneralSubtree SEQUENCE within the permitted list. */
	spec.raw = raw_empty_subtree;
	spec.raw_len = sizeof raw_empty_subtree;
	build_standard_chain(&spec, 0,
		san_service, 1, NULL, 0, 0, 0, &leaf, &ca);
	{
		wbuf chain[2] = {leaf, ca};

		r2 = run_chain(&root_ta, chain, 2, NULL,
			big_ws, sizeof big_ws, 4096, NULL, NULL);
	}
	spec.raw = NULL;
	spec.raw_len = 0;

	/* A zero-length permitted list is rejected too. */
	spec.raw = raw_empty_list;
	spec.raw_len = sizeof raw_empty_list;
	build_standard_chain(&spec, 0,
		san_service, 1, NULL, 0, 0, 0, &leaf, &ca);
	{
		unsigned r5;
		wbuf chain[2] = {leaf, ca};

		r5 = run_chain(&root_ta, chain, 2, NULL,
			big_ws, sizeof big_ws, 4096, NULL, NULL);
		NC_CHECK(r5 == BR_ERR_X509_INVALID_VALUE);
	}
	spec.raw = NULL;
	spec.raw_len = 0;

	/* Duplicate Name Constraints extension. */
	spec.permit = gn_permit_example;
	spec.num_permit = 1;
	force_duplicate_nc = 1;
	build_standard_chain(&spec, 0,
		san_service, 1, NULL, 0, 0, 0, &leaf, &ca);
	force_duplicate_nc = 0;
	{
		wbuf chain[2] = {leaf, ca};

		r3 = run_chain(&root_ta, chain, 2, NULL,
			big_ws, sizeof big_ws, 4096, NULL, NULL);
	}

	/* An empty SEQUENCE content is itself malformed. */
	spec.raw = raw_empty_seq;
	spec.raw_len = sizeof raw_empty_seq;
	{
		unsigned r4;

		build_standard_chain(&spec, 0,
			san_service, 1, NULL, 0, 0, 0, &leaf, &ca);
		spec.raw = NULL;
		{
			wbuf chain[2] = {leaf, ca};

			r4 = run_chain(&root_ta, chain, 2, NULL,
				big_ws, sizeof big_ws, 4096, NULL, NULL);
		}
		NC_CHECK(r4 == BR_ERR_X509_INVALID_VALUE);
	}
	NC_CHECK(r1 == BR_ERR_X509_INVALID_VALUE);
	NC_CHECK(r2 == BR_ERR_X509_INVALID_VALUE);
	NC_CHECK(r3 == BR_ERR_X509_INVALID_VALUE);
	return 0;
}

static int
case_nc_on_leaf(void)
{
	wbuf leaf, ca;
	unsigned r;

	build_standard_chain(&nc_example, 0,
		san_service, 1, NULL, 0, 1, 0, &leaf, &ca);
	{
		wbuf chain[2] = {leaf, ca};

		r = run_chain(&root_ta, chain, 2, NULL,
			big_ws, sizeof big_ws, 4096, NULL, NULL);
	}
	NC_CHECK(r == BR_ERR_X509_INVALID_VALUE);
	return 0;
}

static int
case_truncated_tlv(void)
{
	wbuf leaf, ca;
	br_x509_minimal_context c;
	unsigned r;

	build_standard_chain(&nc_example, 1,
		san_service, 1, NULL, 0, 0, 0, &leaf, &ca);

	/* Feed the CA certificate truncated inside the NC extension. */
	br_x509_minimal_init_full(&c, &root_ta, 1);
	br_x509_minimal_set_time(&c, 739951, 0);
	br_x509_minimal_set_name_constraints(&c,
		big_ws, sizeof big_ws);
	c.vtable->start_chain(&c.vtable, NULL);
	c.vtable->start_cert(&c.vtable, (uint32_t)leaf.len);
	c.vtable->append(&c.vtable, leaf.b, leaf.len);
	c.vtable->end_cert(&c.vtable);
	{
		const unsigned char *p = find_sub(ca.b, ca.len,
			OID_NC, sizeof OID_NC);
		size_t cut;

		NC_CHECK(p != NULL);
		cut = (size_t)(p - ca.b) + 2; /* inside the NC TLV */
		c.vtable->start_cert(&c.vtable, (uint32_t)ca.len);
		c.vtable->append(&c.vtable, ca.b, cut);
		c.vtable->end_cert(&c.vtable);
	}
	r = c.vtable->end_chain(&c.vtable);
	NC_CHECK(r != 0);
	NC_CHECK(c.vtable->get_pkey(&c.vtable, NULL) == NULL);
	return 0;
}

static int
case_bad_ip_lengths(void)
{
	/* SAN value: SEQUENCE { iPAddress of 3 bytes }. */
	static const unsigned char san_block[] = {
		0x30, 0x05, 0x87, 0x03, 10, 0, 1
	};
	static const unsigned char bad_cons_ip[4] = {10, 0, 0, 0};
	nc_gn g;
	nc_spec spec;
	wbuf leaf, ca;
	unsigned r1, r2;

	/* SAN iPAddress of 3 bytes. */
	build_standard_chain(&nc_example, 0,
		NULL, 0, san_block, sizeof san_block, 0, 0,
		&leaf, &ca);
	{
		wbuf chain[2] = {leaf, ca};

		r1 = run_chain(&root_ta, chain, 2, NULL,
			big_ws, sizeof big_ws, 4096, NULL, NULL);
	}

	/* IP constraint of 4 bytes (must be 8 or 32). */
	memset(&spec, 0, sizeof spec);
	g.type = NC_GN_IP;
	g.d = bad_cons_ip;
	g.dlen = sizeof bad_cons_ip;
	spec.permit = &g;
	spec.num_permit = 1;
	build_standard_chain(&spec, 0,
		san_service, 1, NULL, 0, 0, 0, &leaf, &ca);
	{
		wbuf chain[2] = {leaf, ca};

		r2 = run_chain(&root_ta, chain, 2, NULL,
			big_ws, sizeof big_ws, 4096, NULL, NULL);
	}
	NC_CHECK(r1 == BR_ERR_X509_INVALID_VALUE);
	NC_CHECK(r2 == BR_ERR_X509_INVALID_VALUE);
	return 0;
}

static int
case_extension_reorder(void)
{
	wbuf leaf1, ca1, leaf2, ca2;
	unsigned r1, r2;

	build_standard_chain(&nc_example, 0,
		san_outside, 1, NULL, 0, 0, 0, &leaf1, &ca1);
	build_standard_chain(&nc_example, 0,
		san_outside, 1, NULL, 0, 0, 1, &leaf2, &ca2);
	{
		wbuf c1[2] = {leaf1, ca1};
		wbuf c2[2] = {leaf2, ca2};

		r1 = run_chain(&root_ta, c1, 2, NULL,
			big_ws, sizeof big_ws, 4096, NULL, NULL);
		r2 = run_chain(&root_ta, c2, 2, NULL,
			big_ws, sizeof big_ws, 4096, NULL, NULL);
	}
	NC_CHECK(r1 == BR_ERR_X509_NAME_CONSTRAINTS);
	NC_CHECK(r2 == BR_ERR_X509_NAME_CONSTRAINTS);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Chain group cases                                                   */
/* ------------------------------------------------------------------ */

static int
case_fixture_enforcement(void)
{
	static unsigned char root_der[1024], ca_der[1024],
		good_der[1024], outside_der[1024], crit_der[1024];
	static size_t root_n, ca_n, good_n, outside_n, crit_n;
	static br_x509_decoder_context fixture_dc;
	static unsigned char fixture_dn[1024];
	static size_t fixture_dn_len;
	br_x509_trust_anchor ta;
	wbuf good, outside, ca, crit;
	unsigned r1, r2, r3;

	root_n = 0;
	{
		unsigned char *p = load_file("checks/fixtures/root.der",
			&root_n);

		memcpy(root_der, p, root_n);
		free(p);
		p = load_file("checks/fixtures/ca.der", &ca_n);
		memcpy(ca_der, p, ca_n);
		free(p);
		p = load_file("checks/fixtures/good.der", &good_n);
		memcpy(good_der, p, good_n);
		free(p);
		p = load_file("checks/fixtures/outside.der", &outside_n);
		memcpy(outside_der, p, outside_n);
		free(p);
		p = load_file("checks/fixtures/ca-critical.der", &crit_n);
		memcpy(crit_der, p, crit_n);
		free(p);
	}
	anchor_from_cert(root_der, root_n, &ta,
		&fixture_dc, fixture_dn, &fixture_dn_len);
	memset(&good, 0, sizeof good);
	memcpy(good.b, good_der, good_n);
	good.len = good_n;
	memset(&outside, 0, sizeof outside);
	memcpy(outside.b, outside_der, outside_n);
	outside.len = outside_n;
	memset(&ca, 0, sizeof ca);
	memcpy(ca.b, ca_der, ca_n);
	ca.len = ca_n;
	memset(&crit, 0, sizeof crit);
	memcpy(crit.b, crit_der, crit_n);
	crit.len = crit_n;
	{
		wbuf c1[2] = {good, ca};
		wbuf c2[2] = {outside, ca};
		wbuf c3[2] = {good, crit};

		r1 = run_chain(&ta, c1, 2, NULL,
			big_ws, sizeof big_ws, 1, NULL, NULL);
		r2 = run_chain(&ta, c2, 2, NULL,
			big_ws, sizeof big_ws, 1, NULL, NULL);
		r3 = run_chain(&ta, c3, 2, NULL,
			big_ws, sizeof big_ws, 1, NULL, NULL);
	}
	NC_CHECK(r1 == 0);
	NC_CHECK(r2 == BR_ERR_X509_NAME_CONSTRAINTS);
	NC_CHECK(r3 == 0);
	return 0;
}

/* Three-level chain: root(key0) -> CA1(key1) -> CA2(key3) -> leaf(key2). */
static void
build_three_level(const nc_spec *nc1, const nc_spec *nc2,
	const san_entry *sans, size_t num_sans,
	wbuf *leaf_out, wbuf *ca2_out, wbuf *ca1_out)
{
	cert_spec ca1, ca2, leaf;

	memset(&ca1, 0, sizeof ca1);
	ca1.issuer_cn = "NC Test Root";
	ca1.subject_cn = "NC Level1 CA";
	ca1.issuer_key = &keys[0];
	ca1.subject_key = &keys[1];
	ca1.serial = 1;
	ca1.is_ca = 1;
	ca1.pathlen = 4;
	ca1.key_usage = 0x04;
	ca1.nc = nc1;
	build_cert(&ca1, ca1_out);

	memset(&ca2, 0, sizeof ca2);
	ca2.issuer_cn = "NC Level1 CA";
	ca2.subject_cn = "NC Level2 CA";
	ca2.issuer_key = &keys[1];
	ca2.subject_key = &keys[3];
	ca2.serial = 2;
	ca2.is_ca = 1;
	ca2.pathlen = 3;
	ca2.key_usage = 0x04;
	ca2.nc = nc2;
	build_cert(&ca2, ca2_out);

	memset(&leaf, 0, sizeof leaf);
	leaf.issuer_cn = "NC Level2 CA";
	leaf.subject_cn = "NC Test Leaf";
	leaf.issuer_key = &keys[3];
	leaf.subject_key = &keys[2];
	leaf.serial = 3;
	leaf.is_ca = 0;
	leaf.pathlen = -1;
	leaf.key_usage = 0x80;
	leaf.sans = sans;
	leaf.num_sans = num_sans;
	build_cert(&leaf, leaf_out);
}

static int
case_multi_level_narrow(void)
{
	static const nc_gn permit_sub[] = {
		{NC_GN_DNS, d_sub_example, sizeof d_sub_example - 1}
	};
	nc_spec nc2;
	wbuf leaf, ca2, ca1;
	unsigned r1, r2;

	memset(&nc2, 0, sizeof nc2);
	nc2.permit = permit_sub;
	nc2.num_permit = 1;

	build_three_level(&nc_example, &nc2, san_a_sub, 1,
		&leaf, &ca2, &ca1);
	{
		wbuf chain[3] = {leaf, ca2, ca1};

		r1 = run_chain(&root_ta, chain, 3, NULL,
			big_ws, sizeof big_ws, 4096, NULL, NULL);
	}
	build_three_level(&nc_example, &nc2, san_b_example, 1,
		&leaf, &ca2, &ca1);
	{
		wbuf chain[3] = {leaf, ca2, ca1};

		r2 = run_chain(&root_ta, chain, 3, NULL,
			big_ws, sizeof big_ws, 4096, NULL, NULL);
	}
	NC_CHECK(r1 == 0);
	NC_CHECK(r2 == BR_ERR_X509_NAME_CONSTRAINTS);
	return 0;
}

static int
case_multi_level_exclude(void)
{
	static const nc_gn exclude_bad[] = {
		{NC_GN_DNS, d_b_example, sizeof d_b_example - 1}
	};
	nc_spec nc2;
	wbuf leaf, ca2, ca1;
	unsigned r1, r2;

	memset(&nc2, 0, sizeof nc2);
	nc2.exclude = exclude_bad;
	nc2.num_exclude = 1;

	build_three_level(&nc_example, &nc2, san_service, 1,
		&leaf, &ca2, &ca1);
	{
		wbuf chain[3] = {leaf, ca2, ca1};

		r1 = run_chain(&root_ta, chain, 3, NULL,
			big_ws, sizeof big_ws, 4096, NULL, NULL);
	}
	build_three_level(&nc_example, &nc2, san_b_example, 1,
		&leaf, &ca2, &ca1);
	{
		wbuf chain[3] = {leaf, ca2, ca1};

		r2 = run_chain(&root_ta, chain, 3, NULL,
			big_ws, sizeof big_ws, 4096, NULL, NULL);
	}
	NC_CHECK(r1 == 0);
	NC_CHECK(r2 == BR_ERR_X509_NAME_CONSTRAINTS);
	return 0;
}

static int
case_permit_union(void)
{
	wbuf leaf, ca;
	unsigned r1, r2;

	build_standard_chain(&nc_ab_spec, 0,
		san_b_test, 1, NULL, 0, 0, 0, &leaf, &ca);
	{
		wbuf chain[2] = {leaf, ca};

		r1 = run_chain(&root_ta, chain, 2, NULL,
			big_ws, sizeof big_ws, 4096, NULL, NULL);
	}
	build_standard_chain(&nc_ab_spec, 0,
		san_c_test, 1, NULL, 0, 0, 0, &leaf, &ca);
	{
		wbuf chain[2] = {leaf, ca};

		r2 = run_chain(&root_ta, chain, 2, NULL,
			big_ws, sizeof big_ws, 4096, NULL, NULL);
	}
	NC_CHECK(r1 == 0);
	NC_CHECK(r2 == BR_ERR_X509_NAME_CONSTRAINTS);
	return 0;
}

static int
case_mixed_san_one_bad(void)
{
	static const san_entry mixed[] = {
		{NC_GN_DNS, d_service, sizeof d_service - 1},
		{NC_GN_DNS, d_outside, sizeof d_outside - 1}
	};
	wbuf leaf, ca;
	unsigned r;

	build_standard_chain(&nc_example, 0,
		mixed, 2, NULL, 0, 0, 0, &leaf, &ca);
	{
		wbuf chain[2] = {leaf, ca};

		r = run_chain(&root_ta, chain, 2, NULL,
			big_ws, sizeof big_ws, 4096, NULL, NULL);
	}
	NC_CHECK(r == BR_ERR_X509_NAME_CONSTRAINTS);
	return 0;
}

static int
case_no_san_not_checked(void)
{
	wbuf leaf, ca;
	const br_x509_pkey *pk = NULL;
	unsigned r;

	/* No SAN at all: nothing to constrain, even with a strict CA. */
	build_standard_chain(&nc_example, 0,
		NULL, 0, NULL, 0, 0, 0, &leaf, &ca);
	{
		wbuf chain[2] = {leaf, ca};

		r = run_chain(&root_ta, chain, 2, NULL,
			big_ws, sizeof big_ws, 4096, NULL, &pk);
	}
	NC_CHECK(r == 0);
	NC_CHECK(pk != NULL);
	return 0;
}

static int
case_cn_not_used(void)
{
	wbuf leaf, ca;
	cert_spec leaf_spec, ca_spec;
	unsigned r;

	/* CN claims outside.invalid but the SAN is within the permit. */
	memset(&ca_spec, 0, sizeof ca_spec);
	ca_spec.issuer_cn = "NC Test Root";
	ca_spec.subject_cn = "NC Test CA";
	ca_spec.issuer_key = &keys[0];
	ca_spec.subject_key = &keys[1];
	ca_spec.serial = 1;
	ca_spec.is_ca = 1;
	ca_spec.pathlen = 4;
	ca_spec.key_usage = 0x04;
	ca_spec.nc = &nc_example;
	build_cert(&ca_spec, &ca);

	memset(&leaf_spec, 0, sizeof leaf_spec);
	leaf_spec.issuer_cn = "NC Test CA";
	leaf_spec.subject_cn = "outside.invalid";
	leaf_spec.issuer_key = &keys[1];
	leaf_spec.subject_key = &keys[2];
	leaf_spec.serial = 2;
	leaf_spec.is_ca = 0;
	leaf_spec.pathlen = -1;
	leaf_spec.key_usage = 0x80;
	leaf_spec.sans = san_service;
	leaf_spec.num_sans = 1;
	build_cert(&leaf_spec, &leaf);
	{
		wbuf chain[2] = {leaf, ca};

		r = run_chain(&root_ta, chain, 2, NULL,
			big_ws, sizeof big_ws, 4096, NULL, NULL);
	}
	NC_CHECK(r == 0);
	return 0;
}

static int
case_intermediate_san_checked(void)
{
	/* The intermediate itself carries a disallowed SAN. */
	wbuf leaf, ca2, ca1;
	cert_spec c1, c2, lf;
	unsigned r;

	memset(&c1, 0, sizeof c1);
	c1.issuer_cn = "NC Test Root";
	c1.subject_cn = "NC Level1 CA";
	c1.issuer_key = &keys[0];
	c1.subject_key = &keys[1];
	c1.serial = 1;
	c1.is_ca = 1;
	c1.pathlen = 4;
	c1.key_usage = 0x04;
	c1.nc = &nc_example;
	build_cert(&c1, &ca1);

	memset(&c2, 0, sizeof c2);
	c2.issuer_cn = "NC Level1 CA";
	c2.subject_cn = "NC Level2 CA";
	c2.issuer_key = &keys[1];
	c2.subject_key = &keys[3];
	c2.serial = 2;
	c2.is_ca = 1;
	c2.pathlen = 3;
	c2.key_usage = 0x04;
	c2.sans = san_outside;
	c2.num_sans = 1;
	build_cert(&c2, &ca2);

	memset(&lf, 0, sizeof lf);
	lf.issuer_cn = "NC Level2 CA";
	lf.subject_cn = "NC Test Leaf";
	lf.issuer_key = &keys[3];
	lf.subject_key = &keys[2];
	lf.serial = 3;
	lf.is_ca = 0;
	lf.pathlen = -1;
	lf.key_usage = 0x80;
	lf.sans = san_service;
	lf.num_sans = 1;
	build_cert(&lf, &leaf);
	{
		wbuf chain[3] = {leaf, ca2, ca1};

		r = run_chain(&root_ta, chain, 3, NULL,
			big_ws, sizeof big_ws, 4096, NULL, NULL);
	}
	NC_CHECK(r == BR_ERR_X509_NAME_CONSTRAINTS);
	return 0;
}

/*
 * Self-issued chain:
 *   root anchor "SI CA" (key 0);
 *   self-issued intermediate with the SAME issuer/subject DN "SI CA",
 *   a new key (key 4), signed by key 0, carrying NC;
 *   leaf signed by key 4.
 */
static void
build_self_issued_chain(const nc_spec *si_nc,
	const san_entry *sans, size_t num_sans,
	const san_entry *si_sans, size_t num_si_sans,
	wbuf *leaf_out, wbuf *si_out)
{
	cert_spec si, leaf;

	memset(&si, 0, sizeof si);
	si.issuer_cn = "SI CA";
	si.subject_cn = "SI CA";
	si.issuer_key = &keys[0];
	si.subject_key = &keys[4];
	si.serial = 5;
	si.is_ca = 1;
	si.pathlen = 3;
	si.key_usage = 0x04;
	si.nc = si_nc;
	si.sans = si_sans;
	si.num_sans = num_si_sans;
	build_cert(&si, si_out);

	memset(&leaf, 0, sizeof leaf);
	leaf.issuer_cn = "SI CA";
	leaf.subject_cn = "SI Leaf";
	leaf.issuer_key = &keys[4];
	leaf.subject_key = &keys[2];
	leaf.serial = 6;
	leaf.is_ca = 0;
	leaf.pathlen = -1;
	leaf.key_usage = 0x80;
	leaf.sans = sans;
	leaf.num_sans = num_sans;
	build_cert(&leaf, leaf_out);
}

static int
case_self_issued_exception(void)
{
	wbuf leaf, si, root;
	cert_spec rs;
	static br_x509_decoder_context si_dc;
	static unsigned char si_dn[1024];
	size_t si_dn_len;
	br_x509_trust_anchor ta;
	unsigned r;

	memset(&rs, 0, sizeof rs);
	rs.issuer_cn = "SI CA";
	rs.subject_cn = "SI CA";
	rs.issuer_key = &keys[0];
	rs.subject_key = &keys[0];
	rs.serial = 0;
	rs.is_ca = 1;
	rs.pathlen = 4;
	rs.key_usage = 0x04;
	build_cert(&rs, &root);
	anchor_from_cert(root.b, root.len, &ta,
		&si_dc, si_dn, &si_dn_len);

	/* The intermediate's own SAN is outside.invalid, which the
	   permit on the (self-issued) intermediate would forbid for a
	   lower certificate, but its own names are exempt. */
	build_self_issued_chain(&nc_example,
		san_service, 1, san_outside, 1, &leaf, &si);
	{
		wbuf chain[2] = {leaf, si};

		r = run_chain(&ta, chain, 2, NULL,
			big_ws, sizeof big_ws, 4096, NULL, NULL);
	}
	NC_CHECK(r == 0);
	return 0;
}

static int
case_self_issued_still_constrains(void)
{
	wbuf leaf, si, root;
	cert_spec rs;
	static br_x509_decoder_context si2_dc;
	static unsigned char si2_dn[1024];
	size_t si2_dn_len;
	br_x509_trust_anchor ta;
	unsigned r;

	memset(&rs, 0, sizeof rs);
	rs.issuer_cn = "SI CA";
	rs.subject_cn = "SI CA";
	rs.issuer_key = &keys[0];
	rs.subject_key = &keys[0];
	rs.serial = 0;
	rs.is_ca = 1;
	rs.pathlen = 4;
	rs.key_usage = 0x04;
	build_cert(&rs, &root);
	anchor_from_cert(root.b, root.len, &ta,
		&si2_dc, si2_dn, &si2_dn_len);

	/* The self-issued intermediate still constrains the leaf. */
	build_self_issued_chain(&nc_example,
		san_outside, 1, NULL, 0, &leaf, &si);
	{
		wbuf chain[2] = {leaf, si};

		r = run_chain(&ta, chain, 2, NULL,
			big_ws, sizeof big_ws, 4096, NULL, NULL);
	}
	NC_CHECK(r == BR_ERR_X509_NAME_CONSTRAINTS);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Workspace group cases                                               */
/* ------------------------------------------------------------------ */

static int
case_small_then_large(void)
{
	wbuf leaf, ca;
	const br_x509_pkey *pk = NULL;
	unsigned r_small, r_big;

	build_standard_chain(&nc_example, 0,
		san_service, 1, NULL, 0, 0, 0, &leaf, &ca);
	{
		wbuf chain[2] = {leaf, ca};

		r_small = run_chain(&root_ta, chain, 2, NULL,
			small_ws, sizeof small_ws, 4096, NULL, NULL);
		r_big = run_chain(&root_ta, chain, 2, NULL,
			big_ws, sizeof big_ws, 4096, NULL, &pk);
	}
	NC_CHECK(r_small == BR_ERR_X509_LIMIT_EXCEEDED);
	NC_CHECK(r_big == 0);
	NC_CHECK(pk != NULL);
	return 0;
}

static int
case_sentinels_intact(void)
{
	wbuf leaf, ca;
	unsigned r;

	memset(big_ws, 0xAB, sizeof big_ws);
	build_standard_chain(&nc_example, 0,
		san_service, 1, NULL, 0, 0, 0, &leaf, &ca);
	{
		wbuf chain[2] = {leaf, ca};

		r = run_chain(&root_ta, chain, 2, NULL,
			big_ws, sizeof big_ws, 1, NULL, NULL);
	}
	NC_CHECK(r == 0);
	NC_CHECK(memcmp(big_ws, br_x509_nc_sentinel,
		BR_X509_NC_SENTINEL_LEN) == 0);
	NC_CHECK(memcmp(big_ws + sizeof big_ws
		- BR_X509_NC_SENTINEL_LEN,
		br_x509_nc_sentinel, BR_X509_NC_SENTINEL_LEN) == 0);
	return 0;
}

static int
case_interleaved_contexts(void)
{
	wbuf good_leaf, good_ca, bad_leaf, bad_ca;
	br_x509_minimal_context cg, cb;
	static unsigned char ws_g[4096], ws_b[4096];
	unsigned rg, rb;
	const br_x509_pkey *pg, *pb;
	size_t pos;

	build_standard_chain(&nc_example, 0,
		san_service, 1, NULL, 0, 0, 0, &good_leaf, &good_ca);
	build_standard_chain(&nc_example, 0,
		san_outside, 1, NULL, 0, 0, 0, &bad_leaf, &bad_ca);

	br_x509_minimal_init_full(&cg, &root_ta, 1);
	br_x509_minimal_set_time(&cg, 739951, 0);
	br_x509_minimal_set_name_constraints(&cg,
		ws_g, sizeof ws_g);
	br_x509_minimal_init_full(&cb, &root_ta, 1);
	br_x509_minimal_set_time(&cb, 739951, 0);
	br_x509_minimal_set_name_constraints(&cb,
		ws_b, sizeof ws_b);

	cg.vtable->start_chain(&cg.vtable, NULL);
	cb.vtable->start_chain(&cb.vtable, NULL);

	cg.vtable->start_cert(&cg.vtable, (uint32_t)good_leaf.len);
	cb.vtable->start_cert(&cb.vtable, (uint32_t)bad_leaf.len);
	for (pos = 0; pos < good_leaf.len; pos ++) {
		cg.vtable->append(&cg.vtable, good_leaf.b + pos, 1);
	}
	for (pos = 0; pos < bad_leaf.len; pos ++) {
		cb.vtable->append(&cb.vtable, bad_leaf.b + pos, 1);
	}
	cg.vtable->end_cert(&cg.vtable);
	cb.vtable->end_cert(&cb.vtable);

	cg.vtable->start_cert(&cg.vtable, (uint32_t)good_ca.len);
	cb.vtable->start_cert(&cb.vtable, (uint32_t)bad_ca.len);
	for (pos = 0; pos < good_ca.len; pos ++) {
		cg.vtable->append(&cg.vtable, good_ca.b + pos, 1);
	}
	for (pos = 0; pos < bad_ca.len; pos ++) {
		cb.vtable->append(&cb.vtable, bad_ca.b + pos, 1);
	}
	cg.vtable->end_cert(&cg.vtable);
	cb.vtable->end_cert(&cb.vtable);

	rg = cg.vtable->end_chain(&cg.vtable);
	rb = cb.vtable->end_chain(&cb.vtable);
	pg = cg.vtable->get_pkey(&cg.vtable, NULL);
	pb = cb.vtable->get_pkey(&cb.vtable, NULL);
	NC_CHECK(rg == 0);
	NC_CHECK(rb == BR_ERR_X509_NAME_CONSTRAINTS);
	NC_CHECK(pg != NULL);
	NC_CHECK(pb == NULL);
	return 0;
}

static int
case_invalid_configuration(void)
{
	wbuf leaf, ca;
	br_x509_minimal_context c;
	wbuf chain[2];
	unsigned r;

	build_standard_chain(&nc_example, 0,
		san_service, 1, NULL, 0, 0, 0, &leaf, &ca);
	chain[0] = leaf;
	chain[1] = ca;

	/* NULL workspace with non-zero length. */
	r = run_chain(&root_ta, chain, 2, NULL,
		NULL, 64, 4096, NULL, NULL);
	NC_CHECK(r == BR_ERR_X509_INVALID_VALUE);

	/* Non-NULL workspace with zero length. */
	br_x509_minimal_init_full(&c, &root_ta, 1);
	br_x509_minimal_set_time(&c, 739951, 0);
	br_x509_minimal_set_name_constraints(&c, big_ws, 0);
	c.vtable->start_chain(&c.vtable, NULL);
	c.vtable->start_cert(&c.vtable, (uint32_t)leaf.len);
	c.vtable->append(&c.vtable, leaf.b, leaf.len);
	c.vtable->end_cert(&c.vtable);
	c.vtable->start_cert(&c.vtable, (uint32_t)ca.len);
	c.vtable->append(&c.vtable, ca.b, ca.len);
	c.vtable->end_cert(&c.vtable);
	r = c.vtable->end_chain(&c.vtable);
	NC_CHECK(r == BR_ERR_X509_INVALID_VALUE);

	/* Disabling restores the historical behaviour: the
	   non-critical NC is ignored and the outside name passes. */
	build_standard_chain(&nc_example, 0,
		san_outside, 1, NULL, 0, 0, 0, &leaf, &ca);
	chain[0] = leaf;
	chain[1] = ca;
	{
		br_x509_minimal_context c2;

		br_x509_minimal_init_full(&c2, &root_ta, 1);
		br_x509_minimal_set_time(&c2, 739951, 0);
		br_x509_minimal_set_name_constraints(&c2, NULL, 0);
		c2.vtable->start_chain(&c2.vtable, NULL);
		c2.vtable->start_cert(&c2.vtable, (uint32_t)leaf.len);
		c2.vtable->append(&c2.vtable, leaf.b, leaf.len);
		c2.vtable->end_cert(&c2.vtable);
		c2.vtable->start_cert(&c2.vtable, (uint32_t)ca.len);
		c2.vtable->append(&c2.vtable, ca.b, ca.len);
		c2.vtable->end_cert(&c2.vtable);
		r = c2.vtable->end_chain(&c2.vtable);
	}
	NC_CHECK(r == 0);
	return 0;
}

static int
case_reuse_clears_state(void)
{
	wbuf leaf, ca;
	wbuf chain[2];
	unsigned r1, r2;

	build_standard_chain(&nc_example, 0,
		san_outside, 1, NULL, 0, 0, 0, &leaf, &ca);
	chain[0] = leaf;
	chain[1] = ca;
	r1 = run_chain(&root_ta, chain, 2, NULL,
		big_ws, sizeof big_ws, 4096, NULL, NULL);

	build_standard_chain(&nc_example, 0,
		san_service, 1, NULL, 0, 0, 0, &leaf, &ca);
	chain[0] = leaf;
	chain[1] = ca;
	r2 = run_chain(&root_ta, chain, 2, NULL,
		big_ws, sizeof big_ws, 4096, NULL, NULL);
	NC_CHECK(r1 == BR_ERR_X509_NAME_CONSTRAINTS);
	NC_CHECK(r2 == 0);
	return 0;
}

static int
case_chunk_independence(void)
{
	wbuf leaf, ca;
	wbuf chain[2];
	static const size_t chunks[] = {4096, 7, 1};
	unsigned ref;
	size_t i;
	const unsigned char *p;
	size_t cuts[2];

	build_standard_chain(&nc_ip4_24_spec, 1,
		san_ip4_good, 1, NULL, 0, 0, 0, &leaf, &ca);
	chain[0] = leaf;
	chain[1] = ca;
	ref = run_chain(&root_ta, chain, 2, NULL,
		big_ws, sizeof big_ws, 4096, NULL, NULL);
	NC_CHECK(ref == 0);

	for (i = 0; i < sizeof chunks / sizeof chunks[0]; i ++) {
		unsigned r = run_chain(&root_ta, chain, 2, NULL,
			big_ws, sizeof big_ws, chunks[i], NULL, NULL);

		NC_CHECK(r == ref);
	}

	/* Cut inside the CA's DNS... this spec uses IP, so locate the
	   IP mask bytes and cut inside them. */
	p = find_sub(ca.b, ca.len, ip4_cons_24, sizeof ip4_cons_24);
	NC_CHECK(p != NULL);
	cuts[0] = (size_t)-1;
	cuts[1] = (size_t)(p - ca.b) + 3;
	{
		unsigned r = run_chain(&root_ta, chain, 2, NULL,
			big_ws, sizeof big_ws, 4096, cuts, NULL);

		NC_CHECK(r == ref);
	}

	/* Cut inside a DNS name (leaf SAN) and inside a DER length
	   (just before the NC OID in the CA). */
	build_standard_chain(&nc_example, 1,
		san_service, 1, NULL, 0, 0, 0, &leaf, &ca);
	chain[0] = leaf;
	chain[1] = ca;
	ref = run_chain(&root_ta, chain, 2, NULL,
		big_ws, sizeof big_ws, 4096, NULL, NULL);
	p = find_sub(leaf.b, leaf.len, d_service,
		sizeof d_service - 1);
	NC_CHECK(p != NULL);
	cuts[0] = (size_t)(p - leaf.b) + 4;
	p = find_sub(ca.b, ca.len, OID_NC, sizeof OID_NC);
	NC_CHECK(p != NULL);
	cuts[1] = (size_t)(p - ca.b) - 1;
	{
		unsigned r = run_chain(&root_ta, chain, 2, NULL,
			big_ws, sizeof big_ws, 4096, cuts, NULL);

		NC_CHECK(r == ref);
	}
	return 0;
}

static int
case_capacity_boundary(void)
{
	wbuf leaf, ca;
	wbuf chain[2];
	static unsigned char probe_ws[2048];
	size_t cap;
	unsigned got_limit = 0, got_ok = 0;
	int last_was_limit = 0;

	build_standard_chain(&nc_example, 1,
		san_service, 1, NULL, 0, 0, 0, &leaf, &ca);
	chain[0] = leaf;
	chain[1] = ca;

	/* Sweep capacities; once a size succeeds, every larger size
	   must succeed. Find the first success and check the size just
	   below reports LIMIT_EXCEEDED. */
	for (cap = 64; cap <= sizeof probe_ws; cap += 16) {
		unsigned r = run_chain(&root_ta, chain, 2, NULL,
			probe_ws, cap, 1, NULL, NULL);

		NC_CHECK(r == 0 || r == BR_ERR_X509_LIMIT_EXCEEDED);
		if (r == BR_ERR_X509_LIMIT_EXCEEDED) {
			got_limit = 1;
			last_was_limit = 1;
		} else {
			got_ok = 1;
			if (last_was_limit) {
				/* First success after failures: the
				   previous capacity was the boundary. */
				break;
			}
		}
	}
	NC_CHECK(got_limit);
	NC_CHECK(got_ok);

	/* The boundary result must not depend on chunking. */
	{
		unsigned r1, r2;

		r1 = run_chain(&root_ta, chain, 2, NULL,
			probe_ws, cap, 1, NULL, NULL);
		r2 = run_chain(&root_ta, chain, 2, NULL,
			probe_ws, cap, 4096, NULL, NULL);
		NC_CHECK(r1 == r2);
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */

#define NC_CASE(grp, fn) { #fn, grp, fn }

static const nc_case all_cases[] = {
	/* der */
	NC_CASE("der", case_permitted_pass),
	NC_CASE("der", case_permitted_violation),
	NC_CASE("der", case_non_critical_enforced),
	NC_CASE("der", case_critical_supported),
	NC_CASE("der", case_leading_dot_boundaries),
	NC_CASE("der", case_label_boundary),
	NC_CASE("der", case_wildcard_fully_permitted),
	NC_CASE("der", case_wildcard_partial_permit),
	NC_CASE("der", case_wildcard_excluded_intersection),
	NC_CASE("der", case_ipv4_constraints),
	NC_CASE("der", case_ipv6_constraints),
	NC_CASE("der", case_ip_families_separate),
	NC_CASE("der", case_invalid_san_dns),
	NC_CASE("der", case_invalid_constraint_dns),
	NC_CASE("der", case_unsupported_generalname),
	NC_CASE("der", case_min_max_unsupported),
	NC_CASE("der", case_empty_and_duplicate),
	NC_CASE("der", case_nc_on_leaf),
	NC_CASE("der", case_truncated_tlv),
	NC_CASE("der", case_bad_ip_lengths),
	NC_CASE("der", case_extension_reorder),
	/* chain */
	NC_CASE("chain", case_fixture_enforcement),
	NC_CASE("chain", case_multi_level_narrow),
	NC_CASE("chain", case_multi_level_exclude),
	NC_CASE("chain", case_permit_union),
	NC_CASE("chain", case_mixed_san_one_bad),
	NC_CASE("chain", case_no_san_not_checked),
	NC_CASE("chain", case_cn_not_used),
	NC_CASE("chain", case_intermediate_san_checked),
	NC_CASE("chain", case_self_issued_exception),
	NC_CASE("chain", case_self_issued_still_constrains),
	/* workspace */
	NC_CASE("workspace", case_small_then_large),
	NC_CASE("workspace", case_sentinels_intact),
	NC_CASE("workspace", case_interleaved_contexts),
	NC_CASE("workspace", case_invalid_configuration),
	NC_CASE("workspace", case_reuse_clears_state),
	NC_CASE("workspace", case_chunk_independence),
	NC_CASE("workspace", case_capacity_boundary)
};

const nc_case *
nc_contract_cases(size_t *count)
{
	init_keys();
	build_root_anchor();
	if (count != NULL) {
		*count = sizeof all_cases / sizeof all_cases[0];
	}
	return all_cases;
}
