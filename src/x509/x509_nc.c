/*
 * Copyright (c) 2026 BearSSL contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 *
 * Optional Name Constraints support for the "minimal" X.509 engine.
 *
 * The main certificate decoder is the T0 virtual machine in
 * x509_minimal.c; this file implements the name-constraint mode enabled
 * with br_x509_minimal_set_name_constraints(). The engine never
 * allocates memory: all variable-length data (subject names, constraint
 * entries, the streamed certificate bytes) is stored in the
 * caller-provided workspace; only fixed-size state lives in the
 * context.
 *
 * The certificate bytes are accumulated into the workspace while they
 * are streamed to the T0 decoder, and parsed again when the certificate
 * is complete. Therefore the outcome for a given input and workspace
 * capacity does not depend on how the bytes are split across append()
 * calls.
 */

#include "inner.h"

/*
 * Name Constraints OID (2.5.29.30) and Subject Alt Name OID (2.5.29.17),
 * encoded as tag-less OID values: first byte is the content length.
 */
static const unsigned char NC_OID[] = {
	0x03, 0x55, 0x1D, 0x1E
};
static const unsigned char SAN_OID[] = {
	0x03, 0x55, 0x1D, 0x11
};

#define NC_TAG_DNS        2
#define NC_TAG_IP         7

/*
 * Internal record name types: IPv4 and IPv6 GeneralNames share the
 * on-the-wire tag 7 but are independent constraint classes.
 */
#define NC_RTYPE_DNS      1
#define NC_RTYPE_IP4      2
#define NC_RTYPE_IP6      3

#define NC_KIND_NAME       1
#define NC_KIND_CONSTRAINT 2

/*
 * Record header packed in the workspace arena. All fields are bytes, so
 * the structure has no padding.
 */
typedef struct {
	unsigned char kind;        /* NC_KIND_NAME / NC_KIND_CONSTRAINT */
	unsigned char name_type;   /* NC_RTYPE_DNS / NC_RTYPE_IP4 / ... */
	unsigned char cert_index;  /* certificate the record belongs to */
	unsigned char flags;       /* name: wildcard; constraint: permitted */
	unsigned char data_len_hi;
	unsigned char data_len_lo;
	/* followed by data_len bytes */
} nc_record_hdr;

/*
 * Small DER cursor.
 */
typedef struct {
	const unsigned char *p;
	const unsigned char *end;
} nc_cursor;

const unsigned char br_x509_nc_sentinel[BR_X509_NC_SENTINEL_LEN] = {
	0xBE, 0x4A, 0x53, 0x4C, 0x2D, 0x4E, 0x43, 0x2D,
	0xF3, 0x91, 0x77, 0x0D, 0x5E, 0x6E, 0x17, 0x1E
};

/* see bearssl_x509.h */
void
br_x509_minimal_set_name_constraints(br_x509_minimal_context *ctx,
	void *workspace, size_t len)
{
	unsigned char *ws;

	ws = workspace;
	if (ws == NULL && len == 0) {
		ctx->nc_mode = 0;
		ctx->nc_workspace = NULL;
		ctx->nc_workspace_len = 0;
		ctx->nc.config_err = 0;
	} else if (ws != NULL && len > 0) {
		ctx->nc_mode = 1;
		ctx->nc_workspace = ws;
		ctx->nc_workspace_len = len;
		ctx->nc.config_err = 0;
	} else {
		/*
		 * Mixed NULL/non-zero arguments: the mode is left
		 * untouched but the next validation must fail.
		 */
		ctx->nc.config_err = BR_ERR_X509_INVALID_VALUE;
	}
}

static void
nc_fail(br_x509_minimal_context *cc, int err)
{
	/*
	 * BR_ERR_X509_OK is set by the main decoder once the signature
	 * chain reaches a trust anchor; name-constraint failures happen
	 * later in the validation and must override it.
	 */
	if (cc->err == 0 || cc->err == BR_ERR_X509_OK) {
		cc->err = err;
	}
}

/*
 * A non-zero, non-OK error means the main decoder has halted and the
 * name-constraint processing must not touch the certificate anymore.
 */
static int
nc_dead(const br_x509_minimal_context *cc)
{
	return cc->err != 0 && cc->err != BR_ERR_X509_OK;
}

/*
 * Allocate a chunk from the upward-growing arena. It must not collide
 * with the downward-growing certificate scratch area.
 */
static int
nc_alloc(br_x509_minimal_context *cc, size_t len, unsigned char **out)
{
	size_t base, need;

	base = BR_X509_NC_SENTINEL_LEN;
	if (len > (size_t)-1 - base - cc->nc.arena_used) {
		nc_fail(cc, BR_ERR_X509_LIMIT_EXCEEDED);
		return -1;
	}
	need = base + cc->nc.arena_used + len;
	if (cc->nc.scratch_start == 0 || need > cc->nc.scratch_start) {
		nc_fail(cc, BR_ERR_X509_LIMIT_EXCEEDED);
		return -1;
	}
	*out = cc->nc_workspace + base + cc->nc.arena_used;
	cc->nc.arena_used += len;
	return 0;
}

static int
nc_store_record(br_x509_minimal_context *cc, int kind, int name_type,
	unsigned cert_index, unsigned flags,
	const unsigned char *data, size_t data_len)
{
	nc_record_hdr hdr;
	unsigned char *dst;

	if (data_len > 0xFFFF || cert_index >= BR_X509_NC_MAX_CERTS) {
		nc_fail(cc, BR_ERR_X509_LIMIT_EXCEEDED);
		return -1;
	}
	if (nc_alloc(cc, sizeof hdr + data_len, &dst) != 0) {
		return -1;
	}
	hdr.kind = (unsigned char)kind;
	hdr.name_type = (unsigned char)name_type;
	hdr.cert_index = (unsigned char)cert_index;
	hdr.flags = (unsigned char)flags;
	hdr.data_len_hi = (unsigned char)(data_len >> 8);
	hdr.data_len_lo = (unsigned char)data_len;
	memcpy(dst, &hdr, sizeof hdr);
	memcpy(dst + sizeof hdr, data, data_len);
	return 0;
}

/*
 * Read one DER tag+length header. Every length is checked against the
 * element outer bound. Indefinite and non-minimal length encodings and
 * multi-byte (high tag number) tags are rejected.
 * Returns 0 on success and positions the cursor on the element content;
 * the content length is written to *data_len.
 */
static int
nc_tlv(nc_cursor *c, unsigned char *tag, size_t *data_len)
{
	unsigned char t, b;
	size_t l, n, k;

	if (c->p >= c->end) {
		return BR_ERR_X509_INNER_TRUNC;
	}
	t = *c->p ++;
	if ((t & 0x1F) == 0x1F) {
		return BR_ERR_X509_BAD_TAG_VALUE;
	}
	if (c->p >= c->end) {
		return BR_ERR_X509_INNER_TRUNC;
	}
	b = *c->p ++;
	if (b == 0x80) {
		return BR_ERR_X509_INDEFINITE_LENGTH;
	}
	if (b < 0x80) {
		l = b;
	} else {
		n = b & 0x7F;
		if (n == 0 || n > 4) {
			return BR_ERR_X509_OVERFLOW;
		}
		l = 0;
		for (k = 0; k < n; k ++) {
			if (c->p >= c->end) {
				return BR_ERR_X509_INNER_TRUNC;
			}
			b = *c->p ++;
			if (k == 0 && b == 0) {
				return BR_ERR_X509_INVALID_VALUE;
			}
			if (l > (SIZE_MAX >> 8)) {
				return BR_ERR_X509_OVERFLOW;
			}
			l = (l << 8) | b;
		}
		if (n == 1 && l < 0x80) {
			return BR_ERR_X509_INVALID_VALUE;
		}
	}
	if (l > (size_t)(c->end - c->p)) {
		return BR_ERR_X509_INNER_TRUNC;
	}
	*tag = t;
	*data_len = l;
	return 0;
}

/*
 * Open a TLV: check the tag, position the cursor on the content and set
 * the element end boundary. The outer cursor skips the whole element.
 */
static int
nc_open(nc_cursor *c, unsigned char want_tag, nc_cursor *content)
{
	unsigned char tag;
	size_t l;
	int r;

	r = nc_tlv(c, &tag, &l);
	if (r != 0) {
		return r;
	}
	if (tag != want_tag) {
		return BR_ERR_X509_UNEXPECTED;
	}
	content->p = c->p;
	content->end = c->p + l;
	c->p += l;
	return 0;
}

/*
 * Skip one complete element, walking constructed contents.
 */
static int
nc_skip(nc_cursor *c, unsigned depth)
{
	unsigned char tag;
	size_t l;
	const unsigned char *elem_end;
	nc_cursor in;
	int r;

	if (depth > 8) {
		return BR_ERR_X509_BAD_TAG_VALUE;
	}
	r = nc_tlv(c, &tag, &l);
	if (r != 0) {
		return r;
	}
	elem_end = c->p + l;
	if (tag & 0x20) {
		in.p = c->p;
		in.end = elem_end;
		while (in.p < in.end) {
			r = nc_skip(&in, depth + 1);
			if (r != 0) {
				return r;
			}
		}
		if (in.p != in.end) {
			return BR_ERR_X509_EXTRA_ELEMENT;
		}
	}
	c->p = elem_end;
	return 0;
}

static int
nc_ascii_ok(int b)
{
	/* IA5String is ASCII; reject control bytes and DEL. */
	return b >= 0x20 && b <= 0x7E;
}

static int
nc_ascii_lower(int b)
{
	if (b >= 'A' && b <= 'Z') {
		return b + ('a' - 'A');
	}
	return b;
}

static int
nc_ascii_eq(const unsigned char *a, const unsigned char *b, size_t len)
{
	size_t u;

	for (u = 0; u < len; u ++) {
		if (nc_ascii_lower(a[u]) != nc_ascii_lower(b[u])) {
			return 0;
		}
	}
	return 1;
}

/*
 * Count the labels in an already-checked domain (no leading/trailing dot,
 * no empty labels).
 */
static size_t
nc_label_count(const unsigned char *n, size_t len)
{
	size_t u, count;

	count = 1;
	for (u = 0; u < len; u ++) {
		if (n[u] == '.') {
			count ++;
		}
	}
	return count;
}

/*
 * Validate a dNSName from a SAN: no leading/trailing dot, no empty
 * labels, no embedded NUL, no non-ASCII, and only a single full leftmost
 * wildcard label ("*.example.test").
 */
static int
nc_valid_san_dns(const unsigned char *n, size_t len, int *wildcard)
{
	size_t u;
	int label_start;

	*wildcard = 0;
	if (len == 0 || len > 253) {
		return 0;
	}
	for (u = 0; u < len; u ++) {
		if (!nc_ascii_ok(n[u])) {
			return 0;
		}
	}
	if (n[0] == '.' || n[len - 1] == '.') {
		return 0;
	}
	if (len >= 2 && n[0] == '*' && n[1] == '.') {
		*wildcard = 1;
		n += 2;
		len -= 2;
		if (len == 0) {
			return 0;
		}
	}
	label_start = 1;
	for (u = 0; u < len; u ++) {
		if (n[u] == '.') {
			if (label_start || u == len - 1) {
				return 0;
			}
			label_start = 1;
		} else {
			if (n[u] == '*') {
				return 0;
			}
			label_start = 0;
		}
	}
	return 1;
}

/*
 * Validate a DNS constraint: same rules as a SAN DNS name, except that
 * one leading dot is allowed and wildcards are never accepted.
 */
static int
nc_valid_constraint_dns(const unsigned char *n, size_t len, int *lead_dot)
{
	size_t u;
	int label_start;

	*lead_dot = 0;
	if (len == 0 || len > 254) {
		return 0;
	}
	for (u = 0; u < len; u ++) {
		if (!nc_ascii_ok(n[u]) || n[u] == '*') {
			return 0;
		}
	}
	if (n[0] == '.') {
		*lead_dot = 1;
		n ++;
		len --;
		if (len == 0) {
			return 0;
		}
	}
	if (n[len - 1] == '.') {
		return 0;
	}
	label_start = 1;
	for (u = 0; u < len; u ++) {
		if (n[u] == '.') {
			if (label_start || u == len - 1) {
				return 0;
			}
			label_start = 1;
		} else {
			label_start = 0;
		}
	}
	return 1;
}

/*
 * Exact or sub-domain match of a plain (non-wildcard) name against a
 * DNS constraint. Label boundaries are always honoured.
 */
static int
nc_dns_match(const unsigned char *c, size_t clen, int c_lead_dot,
	const unsigned char *n, size_t nlen)
{
	const unsigned char *dom;
	size_t dlen;

	dom = c + (c_lead_dot ? 1 : 0);
	dlen = clen - (c_lead_dot ? 1 : 0);
	if (c_lead_dot) {
		/* Sub-domains only: name must end with ".<domain>". */
		if (nlen <= clen || n[nlen - clen] != '.') {
			return 0;
		}
		return nc_ascii_eq(n + nlen - clen, c, clen);
	}
	if (nlen == dlen) {
		return nc_ascii_eq(n, dom, dlen);
	}
	if (nlen > dlen + 1 && n[nlen - dlen - 1] == '.') {
		return nc_ascii_eq(n + nlen - dlen, dom, dlen);
	}
	return 0;
}

/*
 * Check whether domain 'a' equals domain 'p' or is one of its ancestor
 * domains, label by label, case-insensitively.
 */
static int
nc_dns_self_or_ancestor(const unsigned char *a, size_t alen,
	const unsigned char *p, size_t plen)
{
	for (;;) {
		if (alen == plen && nc_ascii_eq(a, p, alen)) {
			return 1;
		}
		if (plen <= alen || p[0] == '.') {
			return 0;
		}
		/* Drop the leftmost label of p. */
		do {
			p ++;
			plen --;
		} while (plen > 0 && *p != '.');
		if (plen == 0) {
			return 0;
		}
		p ++;
		plen --;
	}
}

/*
 * Wildcard SAN "*.parent" against a permitted DNS constraint. Every
 * single-label expansion L.parent must be covered; that holds iff the
 * constraint domain (leading dot stripped) is the parent itself or one
 * of its ancestor domains.
 */
static int
nc_dns_wildcard_permit(const unsigned char *c, size_t clen, int c_lead_dot,
	const unsigned char *parent, size_t plen)
{
	const unsigned char *dom;
	size_t dlen;

	(void)c_lead_dot;
	dom = c + (c[0] == '.');
	dlen = clen - (c[0] == '.');
	return nc_dns_self_or_ancestor(dom, dlen, parent, plen);
}

/*
 * Wildcard SAN "*.parent" against an excluded DNS constraint. Return
 * non-zero iff at least one single-label expansion can be hit.
 *
 * An excluded constraint only matches the names it literally covers:
 *   - the sub-only form (leading dot) covers strict sub-domains;
 *   - the exact form covers the domain itself and its sub-domains.
 * A wildcard expansion L.parent is therefore never equal to the parent
 * itself; it can only be reached by a sub-domain-covering constraint.
 */
static int
nc_dns_wildcard_excluded(const unsigned char *c, size_t clen,
	int c_lead_dot, const unsigned char *parent, size_t plen)
{
	const unsigned char *dom;
	size_t dlen;

	dom = c + (c_lead_dot ? 1 : 0);
	dlen = clen - (c_lead_dot ? 1 : 0);

	if (nc_dns_self_or_ancestor(dom, dlen, parent, plen)) {
		/*
		 * Constraint domain equals the parent or is an ancestor
		 * of it. The sub-only form always covers the wildcard
		 * names. The exact form matches its domain itself, which
		 * no wildcard expansion equals; it only reaches them when
		 * its domain is a strict ancestor of the parent.
		 */
		if (c_lead_dot) {
			return 1;
		}
		return dlen != plen;
	}
	if (c_lead_dot) {
		/* Sub-only constraint below the parent cannot reach the
		   wildcard set. */
		return 0;
	}

	/*
	 * Exact-domain form: one expansion is hit iff the constraint
	 * domain is exactly one label below the parent: D = L.parent.
	 */
	if (dlen <= plen + 1) {
		return 0;
	}
	if (!nc_ascii_eq(dom + dlen - plen, parent, plen)
		|| dom[dlen - plen - 1] != '.')
	{
		return 0;
	}
	return nc_label_count(dom, dlen)
		== nc_label_count(parent, plen) + 1;
}

static int
nc_ip_match(size_t alen, const unsigned char *name,
	const unsigned char *cvalue)
{
	size_t u;

	for (u = 0; u < alen; u ++) {
		if ((name[u] & cvalue[alen + u])
			!= (cvalue[u] & cvalue[alen + u]))
		{
			return 0;
		}
	}
	return 1;
}

/*
 * Parse a GeneralSubtrees value and store its constraints. The caller
 * passes the content of the implicit [0]/[1] tag, which is directly the
 * SEQUENCE OF GeneralSubtree payload: a succession of GeneralSubtree
 * SEQUENCE elements.
 */
static int
nc_parse_subtrees(br_x509_minimal_context *cc, nc_cursor *outer,
	int permit, unsigned cert_index)
{
	nc_cursor seq;
	int saw_one;
	int r;

	seq = *outer;
	saw_one = 0;
	while (seq.p < seq.end) {
		nc_cursor st, base_content;
		unsigned char tag;
		size_t l;
		int base_type;
		int minimum_seen;

		r = nc_open(&seq, 0x30, &st);
		if (r != 0) {
			return r;
		}
		if (st.p == st.end) {
			/* A GeneralSubtree must contain a GeneralName. */
			return BR_ERR_X509_INVALID_VALUE;
		}
		saw_one = 1;

		/* GeneralName base. */
		r = nc_tlv(&st, &tag, &l);
		if (r != 0) {
			return r;
		}
		if ((tag & 0xC0) != 0x80) {
			return BR_ERR_X509_BAD_TAG_CLASS;
		}
		base_type = tag & 0x1F;
		if (tag & 0x20) {
			/* Constructed GeneralName forms (directoryName,
			   ediPartyName) are not supported. */
			return BR_ERR_X509_UNSUPPORTED;
		}
		base_content.p = st.p;
		base_content.end = st.p + l;
		st.p += l;

		if (base_type == NC_TAG_DNS) {
			int lead_dot;
			size_t nlen;

			nlen = (size_t)(base_content.end - base_content.p);
			if (!nc_valid_constraint_dns(base_content.p, nlen,
				&lead_dot))
			{
				return BR_ERR_X509_INVALID_VALUE;
			}
			if (nc_store_record(cc, NC_KIND_CONSTRAINT,
				NC_RTYPE_DNS, cert_index, permit ? 1 : 0,
				base_content.p, nlen) != 0)
			{
				return cc->err;
			}
		} else if (base_type == NC_TAG_IP) {
			size_t ilen;
			int rtype;

			ilen = (size_t)(base_content.end - base_content.p);
			if (ilen != 8 && ilen != 32) {
				return BR_ERR_X509_INVALID_VALUE;
			}
			rtype = (ilen == 8) ? NC_RTYPE_IP4 : NC_RTYPE_IP6;
			if (nc_store_record(cc, NC_KIND_CONSTRAINT,
				rtype, cert_index, permit ? 1 : 0,
				base_content.p, ilen) != 0)
			{
				return cc->err;
			}
		} else {
			/* rfc822Name, uniformResourceIdentifier,
			   x400Address, directoryName, otherName, ... */
			return BR_ERR_X509_UNSUPPORTED;
		}

		/* Optional minimum [0] and maximum [1]. */
		minimum_seen = 0;
		while (st.p < st.end) {
			nc_cursor iv;
			unsigned char vtag;
			size_t vl;

			r = nc_tlv(&st, &vtag, &vl);
			if (r != 0) {
				return r;
			}
			if ((vtag & 0xC0) != 0x80 || (vtag & 0x20) != 0) {
				return BR_ERR_X509_BAD_TAG_CLASS;
			}
			iv.p = st.p;
			iv.end = st.p + vl;
			st.p += vl;
			if ((vtag & 0x1F) == 0) {
				unsigned char itag;
				size_t il;

				if (minimum_seen) {
					return BR_ERR_X509_UNEXPECTED;
				}
				minimum_seen = 1;
				/* The [0] content is an INTEGER. */
				r = nc_tlv(&iv, &itag, &il);
				if (r != 0 || itag != 0x02) {
					return r != 0
						? r : BR_ERR_X509_UNEXPECTED;
				}
				/* Only the default value zero is accepted. */
				if (il != 1 || *iv.p != 0) {
					return BR_ERR_X509_UNSUPPORTED;
				}
			} else if ((vtag & 0x1F) == 1) {
				return BR_ERR_X509_UNSUPPORTED;
			} else {
				return BR_ERR_X509_BAD_TAG_VALUE;
			}
		}
		if (st.p != st.end) {
			return BR_ERR_X509_EXTRA_ELEMENT;
		}
	}
	if (!saw_one) {
		return BR_ERR_X509_INVALID_VALUE;
	}
	if (seq.p != seq.end) {
		return BR_ERR_X509_EXTRA_ELEMENT;
	}
	return 0;
}

static int
nc_parse_name_constraints(br_x509_minimal_context *cc,
	const unsigned char *value, size_t value_len,
	unsigned cert_index)
{
	nc_cursor outer, seq, content;
	unsigned char tag;
	size_t l;
	int any;
	int r;

	if (value_len == 0) {
		return BR_ERR_X509_INVALID_VALUE;
	}
	outer.p = value;
	outer.end = value + value_len;
	r = nc_open(&outer, 0x30, &seq);
	if (r != 0) {
		return r;
	}
	any = 0;
	while (seq.p < seq.end) {
		int permit;

		r = nc_tlv(&seq, &tag, &l);
		if (r != 0) {
			return r;
		}
		if ((tag & 0xC0) != 0x80 || !(tag & 0x20)) {
			return BR_ERR_X509_BAD_TAG_CLASS;
		}
		if ((tag & 0x1F) == 0) {
			permit = 1;
		} else if ((tag & 0x1F) == 1) {
			permit = 0;
		} else {
			return BR_ERR_X509_BAD_TAG_VALUE;
		}
		content.p = seq.p;
		content.end = seq.p + l;
		seq.p += l;
		r = nc_parse_subtrees(cc, &content, permit, cert_index);
		if (r != 0) {
			return r;
		}
		any = 1;
	}
	if (!any) {
		return BR_ERR_X509_INVALID_VALUE;
	}
	if (seq.p != seq.end || outer.p != outer.end) {
		return BR_ERR_X509_EXTRA_ELEMENT;
	}
	return 0;
}

static int
nc_parse_san(br_x509_minimal_context *cc, const unsigned char *value,
	size_t value_len, unsigned cert_index)
{
	nc_cursor outer, seq, gn;
	unsigned char tag;
	size_t l;
	int r;

	if (value_len == 0) {
		return BR_ERR_X509_INVALID_VALUE;
	}
	outer.p = value;
	outer.end = value + value_len;
	r = nc_open(&outer, 0x30, &seq);
	if (r != 0) {
		return r;
	}
	while (seq.p < seq.end) {
		int gtype;

		r = nc_tlv(&seq, &tag, &l);
		if (r != 0) {
			return r;
		}
		if ((tag & 0xC0) != 0x80) {
			return BR_ERR_X509_BAD_TAG_CLASS;
		}
		gtype = tag & 0x1F;
		if (gtype > 8) {
			return BR_ERR_X509_BAD_TAG_VALUE;
		}
		gn.p = seq.p;
		gn.end = seq.p + l;
		seq.p += l;

		if (gtype == NC_TAG_DNS) {
			int wildcard;
			size_t nlen;

			if (tag & 0x20) {
				return BR_ERR_X509_NOT_PRIMITIVE;
			}
			nlen = (size_t)(gn.end - gn.p);
			if (!nc_valid_san_dns(gn.p, nlen, &wildcard)) {
				return BR_ERR_X509_INVALID_VALUE;
			}
			if (nc_store_record(cc, NC_KIND_NAME, NC_RTYPE_DNS,
				cert_index, wildcard ? 1 : 0,
				gn.p, nlen) != 0)
			{
				return cc->err;
			}
		} else if (gtype == NC_TAG_IP) {
			size_t nlen;
			int rtype;

			if (tag & 0x20) {
				return BR_ERR_X509_NOT_PRIMITIVE;
			}
			nlen = (size_t)(gn.end - gn.p);
			if (nlen != 4 && nlen != 16) {
				return BR_ERR_X509_INVALID_VALUE;
			}
			rtype = (nlen == 4) ? NC_RTYPE_IP4 : NC_RTYPE_IP6;
			if (nc_store_record(cc, NC_KIND_NAME, rtype,
				cert_index, 0, gn.p, nlen) != 0)
			{
				return cc->err;
			}
		}
		/*
		 * Other GeneralName forms keep their historical
		 * behaviour: they are not constraint-checked.
		 */
	}
	if (seq.p != seq.end || outer.p != outer.end) {
		return BR_ERR_X509_EXTRA_ELEMENT;
	}
	return 0;
}

static int
nc_oid_eq(const unsigned char *oid, const unsigned char *p, size_t len)
{
	return oid[0] == len && memcmp(oid + 1, p, len) == 0;
}

/*
 * Full parse of one accumulated certificate.
 */
static int
nc_parse_cert(br_x509_minimal_context *cc, const unsigned char *buf,
	size_t len, unsigned cert_index)
{
	nc_cursor c, in, tbs, exts, ext;
	unsigned char tag;
	size_t l;
	const unsigned char *issuer_start, *issuer_end;
	const unsigned char *subject_start, *subject_end;
	unsigned dn_len, nc_ext_count;
	int r;

	c.p = buf;
	c.end = buf + len;
	r = nc_open(&c, 0x30, &in);
	if (r != 0) {
		return r;
	}
	r = nc_open(&in, 0x30, &tbs);
	if (r != 0) {
		return r;
	}

	/* Optional explicit version [0]. */
	if (tbs.p < tbs.end && *tbs.p == 0xA0) {
		r = nc_skip(&tbs, 0);
		if (r != 0) {
			return r;
		}
	}
	/* serialNumber, signature AlgorithmIdentifier, issuer Name,
	   validity, subject Name, SubjectPublicKeyInfo. */
	if (nc_skip(&tbs, 0) != 0
		|| nc_skip(&tbs, 0) != 0)
	{
		return BR_ERR_X509_INNER_TRUNC;
	}
	r = nc_tlv(&tbs, &tag, &l);
	if (r != 0) {
		return r;
	}
	if (tag != 0x30) {
		return BR_ERR_X509_BAD_DN;
	}
	issuer_start = tbs.p;
	issuer_end = tbs.p + l;
	tbs.p += l;
	if (nc_skip(&tbs, 0) != 0) {
		return BR_ERR_X509_INNER_TRUNC;
	}
	r = nc_tlv(&tbs, &tag, &l);
	if (r != 0) {
		return r;
	}
	if (tag != 0x30) {
		return BR_ERR_X509_BAD_DN;
	}
	subject_start = tbs.p;
	subject_end = tbs.p + l;
	tbs.p += l;
	if (nc_skip(&tbs, 0) != 0) {
		return BR_ERR_X509_INNER_TRUNC;
	}

	/*
	 * Compute the DN hashes with the same hasher the engine uses for
	 * issuer/subject comparison, so the self-issued decision follows
	 * the engine's encoding comparison rules.
	 */
	dn_len = (unsigned)((cc->dn_hash_impl->desc >> BR_HASHDESC_OUT_OFF)
		& BR_HASHDESC_OUT_MASK);
	cc->dn_hash_impl->init(&cc->nc.dn_hash.vtable);
	cc->dn_hash_impl->update(&cc->nc.dn_hash.vtable,
		issuer_start, (size_t)(issuer_end - issuer_start));
	cc->dn_hash_impl->out(&cc->nc.dn_hash.vtable, cc->nc.issuer_dn);
	cc->dn_hash_impl->init(&cc->nc.dn_hash.vtable);
	cc->dn_hash_impl->update(&cc->nc.dn_hash.vtable,
		subject_start, (size_t)(subject_end - subject_start));
	cc->dn_hash_impl->out(&cc->nc.dn_hash.vtable, cc->nc.subject_dn);
	cc->nc.self_issued[cert_index] =
		memcmp(cc->nc.issuer_dn, cc->nc.subject_dn, dn_len) == 0;

	nc_ext_count = 0;
	while (tbs.p < tbs.end) {
		r = nc_tlv(&tbs, &tag, &l);
		if (r != 0) {
			return r;
		}
		if (tag == 0xA1 || tag == 0xA2) {
			/*
			 * nc_tlv already skipped the element content;
			 * continue with the next field.
			 */
			continue;
		}
		if (tag != 0xA3) {
			return BR_ERR_X509_UNEXPECTED;
		}
		/* Re-open the [3] header just consumed by nc_tlv. */
		{
			nc_cursor wrap;

			wrap.p = tbs.p;
			wrap.end = tbs.p + l;
			r = nc_open(&wrap, 0x30, &exts);
			if (r != 0) {
				return r;
			}
			tbs.p = wrap.end;
		}
		while (exts.p < exts.end) {
			nc_cursor octet;
			unsigned char etag, crit;
			const unsigned char *oid_p;
			size_t oid_len;

			r = nc_open(&exts, 0x30, &ext);
			if (r != 0) {
				return r;
			}
			r = nc_tlv(&ext, &etag, &l);
			if (r != 0 || etag != 0x06) {
	return r != 0 ? r : BR_ERR_X509_UNEXPECTED;
			}
			if (l > 64) {
				return BR_ERR_X509_LIMIT_EXCEEDED;
			}
			oid_p = ext.p;
			oid_len = l;
			ext.p += l;
			crit = 0;
			if (ext.p < ext.end && *ext.p == 0x01) {
				nc_cursor bv;

				r = nc_open(&ext, 0x01, &bv);
				if (r != 0) {
					return r;
				}
				if (bv.end - bv.p != 1
					|| (*bv.p != 0xFF && *bv.p != 0x00))
				{
					return BR_ERR_X509_BAD_BOOLEAN;
				}
				crit = (*bv.p == 0xFF);
			}
			r = nc_tlv(&ext, &etag, &l);
			if (r != 0 || etag != 0x04) {
	return r != 0 ? r : BR_ERR_X509_UNEXPECTED;
			}
			octet.p = ext.p;
			octet.end = ext.p + l;
			ext.p += l;

			if (nc_oid_eq(NC_OID, oid_p, oid_len)) {
				nc_ext_count ++;
				if (nc_ext_count > 1) {
					return BR_ERR_X509_INVALID_VALUE;
				}
				if (cert_index == 0) {
					/* Name Constraints on the
					   end-entity certificate. */
					return BR_ERR_X509_INVALID_VALUE;
				}
				r = nc_parse_name_constraints(cc,
					octet.p,
					(size_t)(octet.end - octet.p),
					cert_index);
				if (r != 0) {
					return r;
				}
			} else if (nc_oid_eq(SAN_OID, oid_p, oid_len)) {
				r = nc_parse_san(cc, octet.p,
					(size_t)(octet.end - octet.p),
					cert_index);
				if (r != 0) {
					return r;
				}
			}
			(void)crit;
			if (ext.p != ext.end) {
				return BR_ERR_X509_EXTRA_ELEMENT;
			}
		}
		/*
		 * All extensions consumed, and the inner SEQUENCE bound
		 * (derived from the [3] length) is respected.
		 */
		if (exts.p != exts.end) {
			return BR_ERR_X509_EXTRA_ELEMENT;
		}
	}
	/*
	 * The TBS content must be fully consumed. The certificate
	 * SEQUENCE still contains the signature algorithm and signature
	 * value, which the main engine validates.
	 */
	if (tbs.p != tbs.end) {
		return BR_ERR_X509_EXTRA_ELEMENT;
	}
	return 0;
}

static const nc_record_hdr *
nc_next_record(const unsigned char *base, size_t *off, size_t limit)
{
	const nc_record_hdr *h;
	size_t data_len, total;

	if (*off >= limit) {
		return NULL;
	}
	h = (const nc_record_hdr *)(base + *off);
	data_len = ((size_t)h->data_len_hi << 8) | h->data_len_lo;
	total = sizeof *h + data_len;
	if (*off + total > limit) {
		return NULL;
	}
	*off += total;
	return h;
}

static const unsigned char *
nc_record_data(const nc_record_hdr *h)
{
	return (const unsigned char *)h + sizeof *h;
}

static size_t
nc_record_len(const nc_record_hdr *h)
{
	return ((size_t)h->data_len_hi << 8) | h->data_len_lo;
}

static int
nc_name_matches_constraint(const nc_record_hdr *name,
	const nc_record_hdr *cons)
{
	const unsigned char *nd, *cd;
	size_t nlen, clen;

	nd = nc_record_data(name);
	cd = nc_record_data(cons);
	nlen = nc_record_len(name);
	clen = nc_record_len(cons);

	if (name->name_type == NC_RTYPE_DNS) {
		if (name->flags) {
			/* Wildcard SAN: parent domain follows "*.". */
			if (cons->flags) {
				return nc_dns_wildcard_permit(cd, clen,
					cd[0] == '.', nd + 2, nlen - 2);
			}
			return nc_dns_wildcard_excluded(cd, clen,
				cd[0] == '.', nd + 2, nlen - 2);
		}
		return nc_dns_match(cd, clen, cd[0] == '.', nd, nlen);
	} else {
		/* IPv4 and IPv6 are handled separately. */
		if (clen != nlen * 2) {
			return 0;
		}
		return nc_ip_match(nlen, nd, cd);
	}
}

/*
 * Enforce every accumulated constraint against every accumulated name.
 * Constraints apply from CAs above the named certificate; permits of a
 * single CA and name type form a union, while each CA's restriction
 * must independently hold. Any excluded match rejects the chain.
 */
static int
nc_evaluate(br_x509_minimal_context *cc)
{
	const unsigned char *base;
	size_t arena_limit, noff;

	base = cc->nc_workspace + BR_X509_NC_SENTINEL_LEN;
	arena_limit = cc->nc.arena_used;

	noff = 0;
	for (;;) {
		const nc_record_hdr *name;
		unsigned ca;

		name = nc_next_record(base, &noff, arena_limit);
		if (name == NULL) {
			break;
		}
		if (name->kind != NC_KIND_NAME) {
			continue;
		}
		if (cc->nc.self_issued[name->cert_index]) {
			/* Self-issued certificate: the constraints of
			   upper CAs do not apply to its own names. */
			continue;
		}

		for (ca = name->cert_index + 1;
			ca < BR_X509_NC_MAX_CERTS; ca ++)
		{
			size_t coff;
			int has_permit, permitted;

			has_permit = 0;
			permitted = 0;
			coff = 0;
			for (;;) {
				const nc_record_hdr *cons;

				cons = nc_next_record(base, &coff,
					arena_limit);
				if (cons == NULL) {
					break;
				}
				if (cons->kind != NC_KIND_CONSTRAINT
					|| cons->name_type != name->name_type
					|| cons->cert_index != ca)
				{
					continue;
				}
				if (cons->flags) {
					has_permit = 1;
					if (nc_name_matches_constraint(
						name, cons))
					{
						permitted = 1;
					}
				} else if (nc_name_matches_constraint(
					name, cons))
				{
					return BR_ERR_X509_NAME_CONSTRAINTS;
				}
			}
			if (has_permit && !permitted) {
				return BR_ERR_X509_NAME_CONSTRAINTS;
			}
		}
	}
	return 0;
}

/*
 * Soft TLV header read for partially received certificates. Returns 1
 * when the full tag+length header is available (the content itself may
 * be incomplete), 0 when more bytes are needed, and a negative error
 * code on malformed headers.
 */
static int
nc_soft_tlv(nc_cursor *c, unsigned char *tag, size_t *data_len)
{
	unsigned char t, b;
	size_t l, n, k;

	if (c->p >= c->end) {
		return 0;
	}
	t = *c->p ++;
	if ((t & 0x1F) == 0x1F) {
		return -BR_ERR_X509_BAD_TAG_VALUE;
	}
	if (c->p >= c->end) {
		return 0;
	}
	b = *c->p ++;
	if (b == 0x80) {
		return -BR_ERR_X509_INDEFINITE_LENGTH;
	}
	if (b < 0x80) {
		l = b;
	} else {
		n = b & 0x7F;
		if (n == 0 || n > 4) {
			return -BR_ERR_X509_OVERFLOW;
		}
		l = 0;
		for (k = 0; k < n; k ++) {
			if (c->p >= c->end) {
				return 0;
			}
			b = *c->p ++;
			if (k == 0 && b == 0) {
				return -BR_ERR_X509_INVALID_VALUE;
			}
			if (l > (SIZE_MAX >> 8)) {
				return -BR_ERR_X509_OVERFLOW;
			}
			l = (l << 8) | b;
		}
		if (n == 1 && l < 0x80) {
			return -BR_ERR_X509_INVALID_VALUE;
		}
	}
	*tag = t;
	*data_len = l;
	return 1;
}

/*
 * When the T0 decoder halts on a critical unknown extension, check
 * whether the element whose OCTET STRING header ends exactly at the
 * current byte is a critical Name Constraints extension. All earlier
 * TBS elements are complete at that point; only the current extension's
 * content and the trailing signature may not have arrived yet, so the
 * outer wrappers are read with the soft header parser and every cursor
 * is bounded by the received byte count.
 */
static int
nc_halt_is_critical_nc(const unsigned char *buf, size_t len)
{
	nc_cursor tbs, ext_wrap, exts, ext;
	unsigned char tag;
	size_t l;
	int sr;

	tbs.p = buf;
	tbs.end = buf + len;

	/* Outer Certificate SEQUENCE. */
	sr = nc_soft_tlv(&tbs, &tag, &l);
	if (sr <= 0 || tag != 0x30) {
		return 0;
	}
	/* TBSCertificate SEQUENCE. */
	sr = nc_soft_tlv(&tbs, &tag, &l);
	if (sr <= 0 || tag != 0x30) {
		return 0;
	}
	tbs.end = buf + len;
	if (tbs.p < tbs.end && *tbs.p == 0xA0) {
		if (nc_skip(&tbs, 0) != 0) {
			return 0;
		}
	}
	if (nc_skip(&tbs, 0) != 0       /* serial */
		|| nc_skip(&tbs, 0) != 0    /* sigAlg */
		|| nc_skip(&tbs, 0) != 0    /* issuer */
		|| nc_skip(&tbs, 0) != 0    /* validity */
		|| nc_skip(&tbs, 0) != 0    /* subject */
		|| nc_skip(&tbs, 0) != 0)   /* SPKI */
	{
		return 0;
	}
	/* The [3] extensions wrapper and its SEQUENCE headers must be
	   fully present. */
	if (nc_soft_tlv(&tbs, &tag, &l) <= 0 || tag != 0xA3) {
		return 0;
	}
	ext_wrap.p = tbs.p;
	ext_wrap.end = buf + len;
	if (nc_soft_tlv(&ext_wrap, &tag, &l) <= 0 || tag != 0x30) {
		return 0;
	}
	exts.p = ext_wrap.p;
	exts.end = buf + len;

	while (exts.p < exts.end) {
		nc_cursor after_ext;
		unsigned char etag, crit;
		const unsigned char *op;
		size_t ol, oct_hdr_end;
		int sr;

		sr = nc_soft_tlv(&exts, &etag, &l);
		if (sr <= 0 || etag != 0x30) {
			return 0;
		}
		ext.p = exts.p;
		ext.end = exts.p + l;
		after_ext = exts;
		after_ext.p = exts.p + l;

		sr = nc_soft_tlv(&ext, &etag, &ol);
		if (sr <= 0 || etag != 0x06) {
			return 0;
		}
		op = ext.p;
		ext.p += ol;
		crit = 0;
		if (ext.p < ext.end && *ext.p == 0x01) {
			nc_cursor bv;

			if (nc_open(&ext, 0x01, &bv) != 0) {
				return 0;
			}
			if (bv.end - bv.p != 1 || *bv.p != 0xFF) {
				return 0;
			}
			crit = 1;
		}
		sr = nc_soft_tlv(&ext, &etag, &ol);
		if (sr <= 0 || etag != 0x04) {
			return 0;
		}
		oct_hdr_end = (size_t)(ext.p - buf);
		if (oct_hdr_end == len) {
			/* Exact T0 halt point. */
			return crit && nc_oid_eq(NC_OID, op,
				(size_t)NC_OID[0]);
		}
		/* Not the halt point: the whole extension must be
		   present so we can move to the next one. */
		exts = after_ext;
	}
	return 0;
}

/*
 * Reset all chain-scoped state. Called from start_chain().
 */
void
br_nc_start_chain(br_x509_minimal_context *cc)
{
	int config_err;
	size_t wlen;

	config_err = cc->nc.config_err;
	memset(&cc->nc, 0, sizeof cc->nc);
	cc->nc.config_err = config_err;
	if (config_err != 0) {
		cc->err = config_err;
		return;
	}
	wlen = cc->nc_workspace_len;
	if (wlen < 2 * BR_X509_NC_SENTINEL_LEN) {
		cc->err = BR_ERR_X509_LIMIT_EXCEEDED;
		return;
	}
	memcpy(cc->nc_workspace, br_x509_nc_sentinel,
		BR_X509_NC_SENTINEL_LEN);
	memcpy(cc->nc_workspace + wlen - BR_X509_NC_SENTINEL_LEN,
		br_x509_nc_sentinel, BR_X509_NC_SENTINEL_LEN);
	cc->nc.arena_used = 0;
	cc->nc.scratch_start = 0;
	cc->nc.cert_len = 0;
	cc->nc.cert_recv = 0;
	cc->nc.cert_index = 0;
}

void
br_nc_start_cert(br_x509_minimal_context *cc, uint32_t length)
{
	size_t wlen, scratch;

	if (nc_dead(cc)) {
		return;
	}
	if (cc->nc.cert_index >= BR_X509_NC_MAX_CERTS || length == 0) {
		nc_fail(cc, BR_ERR_X509_LIMIT_EXCEEDED);
		return;
	}
	wlen = cc->nc_workspace_len;
	if ((size_t)length
		> wlen - 2 * BR_X509_NC_SENTINEL_LEN - cc->nc.arena_used)
	{
		nc_fail(cc, BR_ERR_X509_LIMIT_EXCEEDED);
		return;
	}
	scratch = wlen - BR_X509_NC_SENTINEL_LEN - length;
	if (scratch < BR_X509_NC_SENTINEL_LEN + cc->nc.arena_used) {
		nc_fail(cc, BR_ERR_X509_LIMIT_EXCEEDED);
		return;
	}
	cc->nc.scratch_start = scratch;
	cc->nc.cert_len = length;
	cc->nc.cert_recv = 0;
}

void
br_nc_append(br_x509_minimal_context *cc, const unsigned char *buf,
	size_t len)
{
	size_t u;

	if (nc_dead(cc)) {
		return;
	}
	if (cc->err == BR_ERR_X509_OK) {
		/*
		 * The main decoder reached a trust anchor and halted its
		 * virtual machine. Still record the remaining bytes (they
		 * are needed by the certificate parser) but do not drive
		 * the halted VM again.
		 */
		if (cc->nc.scratch_start == 0 || cc->nc.cert_len == 0) {
			return;
		}
		for (u = 0; u < len; u ++) {
			if (cc->nc.cert_recv >= cc->nc.cert_len) {
				nc_fail(cc, BR_ERR_X509_INNER_TRUNC);
				return;
			}
			cc->nc_workspace[cc->nc.scratch_start
				+ cc->nc.cert_recv] = buf[u];
			cc->nc.cert_recv ++;
		}
		return;
	}
	if (cc->nc.scratch_start == 0 || cc->nc.cert_len == 0) {
		nc_fail(cc, BR_ERR_X509_LIMIT_EXCEEDED);
		return;
	}
	if (cc->nc.cert_recv + len > cc->nc.cert_len) {
		nc_fail(cc, BR_ERR_X509_INNER_TRUNC);
		return;
	}
	for (u = 0; u < len; u ++) {
		cc->nc_workspace[cc->nc.scratch_start + cc->nc.cert_recv] =
			buf[u];
		cc->nc.cert_recv ++;
	}
}

/*
 * After a single T0 run, handle a critical Name Constraints halt:
 * clear the T0 error so the main decoder resumes (its VM instruction
 * pointer is saved across runs), while this file validates the
 * extension contents at end_cert().
 */
void
br_nc_t0_post_byte(br_x509_minimal_context *cc)
{
	if (cc->err == BR_ERR_X509_CRITICAL_EXTENSION
		&& cc->nc.scratch_start != 0
		&& nc_halt_is_critical_nc(
			cc->nc_workspace + cc->nc.scratch_start,
			cc->nc.cert_recv))
	{
		cc->err = 0;
	}
}

void
br_nc_end_cert(br_x509_minimal_context *cc)
{
	int r;

	if (nc_dead(cc)) {
		cc->nc.cert_index ++;
		return;
	}
	if (cc->nc.cert_recv != cc->nc.cert_len || cc->nc.cert_len == 0) {
		nc_fail(cc, BR_ERR_X509_INNER_TRUNC);
		cc->nc.cert_index ++;
		return;
	}
	r = nc_parse_cert(cc,
		cc->nc_workspace + cc->nc.scratch_start,
		cc->nc.cert_len, cc->nc.cert_index);
	if (r != 0) {
		nc_fail(cc, r);
	}
	cc->nc.cert_index ++;
}

void
br_nc_end_chain(br_x509_minimal_context *cc)
{
	int r;

	if (cc->nc.config_err != 0) {
		if (cc->err == 0) {
			cc->err = cc->nc.config_err;
		}
		return;
	}
	if (cc->nc_mode == 0) {
		return;
	}
	/* The guard patterns must be intact. */
	if (memcmp(cc->nc_workspace, br_x509_nc_sentinel,
		BR_X509_NC_SENTINEL_LEN) != 0
		|| memcmp(cc->nc_workspace + cc->nc_workspace_len
			- BR_X509_NC_SENTINEL_LEN,
			br_x509_nc_sentinel,
			BR_X509_NC_SENTINEL_LEN) != 0)
	{
		nc_fail(cc, BR_ERR_X509_LIMIT_EXCEEDED);
		return;
	}
	if (cc->err != 0 && cc->err != BR_ERR_X509_OK) {
		return;
	}
	r = nc_evaluate(cc);
	if (r != 0) {
		cc->err = r;
	}
}
