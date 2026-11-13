/*
 * Independent DNS model, wire generator and oracle.
 *
 * This file never calls c-ares to produce an expected value and never
 * derives the model from a c-ares result.  The logical model (label
 * arrays, CNAME chain, terminal A/AAAA set) is generated from a fixed
 * seed, the three wire layouts are encoded from that model alone, and
 * the expected identities/enclens come from the bytes this encoder
 * wrote.  c-ares is used only on the checking side, through its public
 * ares_parse_a_reply / ares_parse_aaaa_reply / ares_expand_name API.
 */

#include "model.h"
#include "ares.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <netdb.h>

/* ------------------------------------------------------------------ */
/* Deterministic RNG (xorshift32): no rand(), fully seed-replayable.  */
/* ------------------------------------------------------------------ */

static unsigned rng_state;

static void rng_seed(unsigned s)
{
  rng_state = s ? s : 0x9e3779b9u;
}

static unsigned rng_next(void)
{
  unsigned x = rng_state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  rng_state = x;
  return x;
}

static int rng_below(int n)
{
  return (int)(rng_next() % (unsigned)n);
}

/* ------------------------------------------------------------------ */
/* Names: label arrays over [A-Za-z0-9-], root allowed.               */
/* ------------------------------------------------------------------ */

static const char ALPHABET[] =
  "abcdefghijklmnopqrstuvwxyz0123456789-";

int em_name_is_root(const em_name *n)
{
  return n->root || n->nlabels == 0;
}

/* Presentation form: labels joined with '.', root rendered as ".". */
void em_render(const em_name *n, char *buf, size_t bufsz)
{
  size_t pos = 0;
  int i, j;
  if (bufsz == 0) return;
  if (em_name_is_root(n))
    {
      if (bufsz > 1) { buf[0] = '.'; buf[1] = '\0'; }
      else buf[0] = '\0';
      return;
    }
  for (i = 0; i < n->nlabels; i++)
    {
      if (i && pos + 1 < bufsz) buf[pos++] = '.';
      for (j = 0; j < n->nlens[i] && pos + 1 < bufsz; j++)
        buf[pos++] = (char)n->labels[i][j];
    }
  if (pos < bufsz) buf[pos] = '\0';
}

/* Form ares_expand_name produces: joined labels, root rendered "". */
static void render_expanded(const em_name *n, char *buf, size_t bufsz)
{
  if (em_name_is_root(n))
    {
      if (bufsz) buf[0] = '\0';
      return;
    }
  em_render(n, buf, bufsz);
}

int em_name_wirelen(const em_name *n)
{
  int i, total = 1; /* terminating zero label */
  if (em_name_is_root(n)) return 1;
  for (i = 0; i < n->nlabels; i++) total += 1 + n->nlens[i];
  return total;
}

static int label_eq_ci(const unsigned char *a, int alen,
                       const unsigned char *b, int blen)
{
  int i;
  if (alen != blen) return 0;
  for (i = 0; i < alen; i++)
    if (tolower((int)(unsigned char)a[i]) !=
        tolower((int)(unsigned char)b[i]))
      return 0;
  return 1;
}

static int name_eq_ci(const em_name *a, const em_name *b)
{
  int i;
  if (em_name_is_root(a) || em_name_is_root(b))
    return em_name_is_root(a) && em_name_is_root(b);
  if (a->nlabels != b->nlabels) return 0;
  for (i = 0; i < a->nlabels; i++)
    if (!label_eq_ci(a->labels[i], a->nlens[i], b->labels[i], b->nlens[i]))
      return 0;
  return 1;
}

static void name_set_root(em_name *n)
{
  n->nlabels = 0;
  n->root = 1;
}

static void gen_label(unsigned char *dst, unsigned char *len_out)
{
  int len = 1 + rng_below(EM_MAX_LABLEN - 2);
  int i;
  for (i = 0; i < len; i++)
    {
      char c = ALPHABET[rng_below((int)sizeof(ALPHABET) - 1)];
      /* keep hyphens internal so labels stay plain host labels */
      if ((i == 0 || i == len - 1) && c == '-') c = 'a';
      dst[i] = (unsigned char)c;
    }
  *len_out = (unsigned char)len;
}

/*
 * Names in one case all hang off a shared two-label base domain so
 * legal suffix compression has frequent matches.  Each CNAME target
 * prepends one fresh label to its predecessor, giving the sequence
 *   r0.base, r1.r0.base, r2.r1.r0.base, ...
 * so the longest suffix of hop i is exactly the whole previous name,
 * which was itself emitted through a pointer: pointer depth then grows
 * hop by hop under both suffix layouts, and multi-level cross-record
 * pointers arise naturally in layout 2.
 */
static void gen_base(em_name *base)
{
  base->root = 0;
  base->nlabels = 2;
  gen_label(base->labels[0], &base->nlens[0]);
  gen_label(base->labels[1], &base->nlens[1]);
}

/* out = single fresh label prepended to tail (tail must not be root). */
static void prepend_label(em_name *out, const em_name *tail)
{
  int i;
  out->root = 0;
  gen_label(out->labels[0], &out->nlens[0]);
  for (i = 0; i < tail->nlabels; i++)
    {
      memcpy(out->labels[i + 1], tail->labels[i], EM_MAX_LABLEN);
      out->nlens[i + 1] = tail->nlens[i];
    }
  out->nlabels = tail->nlabels + 1;
}

/* out = one fresh label over the shared base. */
static void make_one_over_base(em_name *out, const em_name *base)
{
  int i;
  out->root = 0;
  gen_label(out->labels[0], &out->nlens[0]);
  for (i = 0; i < base->nlabels; i++)
    {
      memcpy(out->labels[i + 1], base->labels[i], EM_MAX_LABLEN);
      out->nlens[i + 1] = base->nlens[i];
    }
  out->nlabels = base->nlabels + 1;
}

/* ------------------------------------------------------------------ */
/* Model generation                                                    */
/* ------------------------------------------------------------------ */

em_model em_generate(unsigned seed, int n)
{
  em_model m;
  em_name base;
  int i, j;

  memset(&m, 0, sizeof(m));
  rng_seed(seed + (unsigned)n * 2654435761u + 1u);
  m.seed = seed;
  m.nhops = 1 + rng_below(EM_MAX_CHAIN);            /* 1..4 hops */
  m.qtype = rng_below(2) ? 28 : 1;                  /* A or AAAA */

  gen_base(&base);

  if (rng_below(12) == 0)
    name_set_root(&m.qname);                        /* explicit root */
  else
    make_one_over_base(&m.qname, &base);

  /* Nested CNAME chain over the same base. */
  for (i = 0; i < m.nhops; i++)
    {
      if (i == m.nhops - 1 && !em_name_is_root(&m.qname) &&
          rng_below(15) == 0)
        {
          name_set_root(&m.cname[i]);               /* terminal root */
          continue;
        }
      if (i == 0)
        make_one_over_base(&m.cname[i], &base);
      else
        prepend_label(&m.cname[i], &m.cname[i - 1]);
      /* keep every hop a distinct identity */
      if (i > 0 && name_eq_ci(&m.cname[i], &m.cname[i - 1]))
        prepend_label(&m.cname[i], &m.cname[i - 1]);
    }
  /* a self-CNAME on the question name would be a degenerate chain */
  if (!em_name_is_root(&m.qname) &&
      name_eq_ci(&m.cname[0], &m.qname))
    prepend_label(&m.cname[0], &m.qname);

  m.naddrs = 1 + rng_below(EM_MAX_ADDRS);
  for (i = 0; i < m.naddrs; i++)
    {
      m.addrs[i].len = (m.qtype == 1) ? 4 : 16;
      for (j = 0; j < m.addrs[i].len; j++)
        m.addrs[i].v[j] = (unsigned char)rng_next();
      m.addrs[i].v[0] |= 1; /* never an all-zero identifier */
    }

  m.has_extra = rng_below(2);
  if (m.has_extra)
    {
      int tries = 0;
      /* a distinct identity that still shares the base suffix */
      do
        {
          make_one_over_base(&m.extra, &base);
          tries++;
        }
      while (tries < 8 &&
             (name_eq_ci(&m.extra, &m.qname) ||
              name_eq_ci(&m.extra, &m.cname[m.nhops - 1])));
    }
  return m;
}

/* ------------------------------------------------------------------ */
/* Wire encoder                                                        */
/* ------------------------------------------------------------------ */

#define EM_MAX_BOUND 192

/* A previously written name-label boundary that a pointer may target. */
typedef struct {
  int       offset;            /* message offset of the label        */
  em_wname *owner;             /* name record this boundary belongs  */
  int       labelidx;          /* first logical label at offset      */
  int       depth;             /* indirections expanding from here   */
  int       name_start;        /* 1 for a name-start boundary        */
} em_boundary;

typedef struct {
  unsigned char buf[EM_MSGMAX];
  int           pos;
  em_wname      names[EM_MAX_WNAMES];
  int           nnames;
  em_boundary   bounds[EM_MAX_BOUND];
  int           nbound;
  int           ptr_depth;
  int           failed;
} emitter;

static void put16(emitter *e, unsigned v)
{
  if (e->pos + 2 > EM_MSGMAX) { e->failed = 1; return; }
  e->buf[e->pos++] = (unsigned char)((v >> 8) & 0xff);
  e->buf[e->pos++] = (unsigned char)(v & 0xff);
}

static void put32(emitter *e, unsigned long v)
{
  int i;
  if (e->pos + 4 > EM_MSGMAX) { e->failed = 1; return; }
  for (i = 0; i < 4; i++)
    e->buf[e->pos++] = (unsigned char)((v >> (24 - 8 * i)) & 0xff);
}

static void put_bytes(emitter *e, const unsigned char *p, int n)
{
  if (e->pos + n > EM_MSGMAX) { e->failed = 1; return; }
  memcpy(e->buf + e->pos, p, (size_t)n);
  e->pos += n;
}

/* Does boundary b encode the suffix of n starting at label first? */
static int boundary_matches(const em_boundary *b, const em_name *n,
                            int first)
{
  const em_name *p = &b->owner->identity;
  int plen = p->nlabels - b->labelidx;
  int nlen = n->nlabels - first;
  int k;
  if (plen != nlen || plen <= 0) return 0;
  for (k = 0; k < plen; k++)
    if (!label_eq_ci(p->labels[b->labelidx + k], p->nlens[b->labelidx + k],
                     n->labels[first + k], n->nlens[first + k]))
      return 0;
  return 1;
}

/* Longest suffix match among earlier boundaries. */
static const em_boundary *find_suffix(emitter *e, const em_name *n,
                                      int name_starts_only)
{
  const em_boundary *best = NULL;
  int best_len = 0;
  int i, first;
  for (i = 0; i < e->nbound; i++)
    {
      const em_boundary *b = &e->bounds[i];
      int cand;
      if (name_starts_only && !b->name_start) continue;
      for (first = 0; first < n->nlabels; first++)
        {
          if (!boundary_matches(b, n, first)) continue;
          cand = n->nlabels - first;
          if (cand > best_len)
            {
              best_len = cand;
              best = b;
            }
        }
    }
  return best;
}

static void write_label_bytes(emitter *e, const em_name *n, int idx,
                              int case_variant, int is_question)
{
  const unsigned char *lab = n->labels[idx];
  int len = n->nlens[idx];
  int j;
  if (e->pos + 1 + len > EM_MSGMAX) { e->failed = 1; return; }
  e->buf[e->pos++] = (unsigned char)len;
  for (j = 0; j < len; j++)
    {
      unsigned char c = lab[j];
      if (is_question)
        {
          int upper = (case_variant == 1) ||
                      (case_variant == 2 && (idx % 2) == 0);
          if (upper && c >= 'a' && c <= 'z') c = (unsigned char)(c - 32);
        }
      e->buf[e->pos++] = c;
    }
}

/*
 * Emit one domain name.  Layout 0: no compression.  Layout 1: pointer
 * only to a whole earlier name (suffix-only).  Layout 2: pointer may
 * target any earlier label boundary, producing multi-level cross-record
 * suffix chains.  Every pointer target precedes the pointer itself.
 */
static em_wname *emit_name(emitter *e, const em_name *n, em_layout layout,
                           int case_variant, int is_question)
{
  em_wname *wn;
  const em_boundary *match = NULL;
  int first = 0;       /* first label not covered by the pointer */
  int tail_depth = 0;
  int start = e->pos;
  int i;

  if (e->nnames >= EM_MAX_WNAMES) { e->failed = 1; return NULL; }
  wn = &e->names[e->nnames++];
  memset(wn, 0, sizeof(*wn));
  wn->offset = start;
  wn->identity = *n;

  if (em_name_is_root(n))
    {
      if (e->pos + 1 > EM_MSGMAX) { e->failed = 1; return wn; }
      e->buf[e->pos++] = 0;
      wn->enclen = e->pos - start;
      return wn;
    }

  if (layout != EM_LAYOUT_UNCOMPRESSED)
    match = find_suffix(e, n, layout == EM_LAYOUT_SUFFIX);

  if (match)
    {
      first = n->nlabels -
              (match->owner->identity.nlabels - match->labelidx);
      tail_depth = 1 + match->depth;
    }

  /* Prefix labels, each exposing a fresh label boundary. */
  for (i = 0; i < first; i++)
    {
      if (e->nbound < EM_MAX_BOUND)
        {
          e->bounds[e->nbound].offset = e->pos;
          e->bounds[e->nbound].owner = wn;
          e->bounds[e->nbound].labelidx = i;
          e->bounds[e->nbound].depth = tail_depth;
          e->bounds[e->nbound].name_start = (i == 0);
          e->nbound++;
        }
      write_label_bytes(e, n, i, case_variant, is_question);
    }

  if (match)
    {
      if (e->pos + 2 > EM_MSGMAX) { e->failed = 1; return wn; }
      /* RFC 1035 compression pointer: 11 + 14-bit offset. */
      e->buf[e->pos++] = (unsigned char)(0xc0 | (match->offset >> 8));
      e->buf[e->pos++] = (unsigned char)(match->offset & 0xff);
      wn->ptrs = tail_depth;
      if (tail_depth > e->ptr_depth) e->ptr_depth = tail_depth;
    }
  else
    {
      for (i = first; i < n->nlabels; i++)
        {
          if (e->nbound < EM_MAX_BOUND)
            {
              e->bounds[e->nbound].offset = e->pos;
              e->bounds[e->nbound].owner = wn;
              e->bounds[e->nbound].labelidx = i;
              e->bounds[e->nbound].depth = 0;
              e->bounds[e->nbound].name_start = (i == 0 && first == 0);
              e->nbound++;
            }
          write_label_bytes(e, n, i, case_variant, is_question);
        }
      if (e->pos + 1 > EM_MSGMAX) { e->failed = 1; return wn; }
      e->buf[e->pos++] = 0;
    }

  wn->enclen = e->pos - start;
  return wn;
}

static void emit_answer_rr(emitter *e, const em_name *owner,
                           em_layout layout, unsigned short type,
                           unsigned long ttl,
                           const unsigned char *rdata, int rdlen)
{
  emit_name(e, owner, layout, 0, 0);
  put16(e, type);
  put16(e, 1);                 /* C_IN */
  put32(e, ttl);
  put16(e, (unsigned)rdlen);
  put_bytes(e, rdata, rdlen);
}

static void emit_cname_rr(emitter *e, const em_name *alias,
                          const em_name *target, em_layout layout,
                          unsigned long ttl)
{
  int rdlen_at;
  emit_name(e, alias, layout, 0, 0);
  put16(e, 5);                 /* T_CNAME */
  put16(e, 1);
  put32(e, ttl);
  rdlen_at = e->pos;
  put16(e, 0);                 /* RDLENGTH placeholder */
  {
    int rdata_start = e->pos;
    emit_name(e, target, layout, 0, 0);
    if (!e->failed)
      {
        int d = e->pos - rdata_start;
        e->buf[rdlen_at] = (unsigned char)((d >> 8) & 0xff);
        e->buf[rdlen_at + 1] = (unsigned char)(d & 0xff);
      }
  }
}

static void emit_txt_rr(emitter *e, const em_name *owner, em_layout layout)
{
  static const unsigned char txt[] = { 1, 'x' };
  emit_answer_rr(e, owner, layout, 16 /* T_TXT */, 60,
                 txt, (int)sizeof(txt));
}

/*
 * Logical name sequence in emit order:
 *   slot 0                 -> qname
 *   slot 1+2i, 2+2i        -> cname[i] (alias owner, rdata target)
 *   terminal RR owners     -> cname[nhops-1]
 */
static const em_name *chain_name(const em_model *m, int slot)
{
  if (slot == 0) return &m->qname;
  if (slot <= 2 * m->nhops) return &m->cname[(slot - 1) / 2];
  return &m->cname[m->nhops - 1];
}

/* Legal permutation of the terminal address RRs (order adjustment). */
int em_addr_perm(int order_variant, int idx, int n)
{
  if (order_variant == 0 || n < 2) return idx;
  if (order_variant == 1)           /* swap the first two */
    {
      if (idx == 0) return 1;
      if (idx == 1) return 0;
      return idx;
    }
  return n - 1 - idx;               /* reverse */
}

int em_build(const em_model *m, em_layout layout, int order_variant,
             int case_variant, int extra_where, em_wire *out)
{
  emitter e;
  unsigned short ancount, arcount = 0;
  int i, total_rrs;
  unsigned short qtype = (unsigned short)m->qtype;
  unsigned short atype = (qtype == 28) ? 28 : 1;

  memset(&e, 0, sizeof(e));
  memset(out, 0, sizeof(*out));

  /* Header (12 bytes), counts patched after emission. */
  e.pos = 12;
  e.buf[0] = (unsigned char)((m->seed >> 8) & 0xff);
  e.buf[1] = (unsigned char)(m->seed & 0xff);
  e.buf[2] = 0x80;             /* QR, opcode 0 */
  e.buf[3] = 0x00;             /* RCODE 0 */

  /* Question section: one question, casing per case_variant. */
  emit_name(&e, &m->qname, layout, case_variant, 1);
  put16(&e, qtype);
  put16(&e, 1);

  /* Answer RRs: one CNAME per hop plus the terminal address RRs.
   * (Each CNAME carries two names, but it is a single RR.) */
  total_rrs = m->nhops + m->naddrs;
  ancount = (unsigned short)total_rrs;

  /* Irrelevant TXT: answer-front, answer-back or additional. */
  if (m->has_extra && extra_where == 1)
    {
      emit_txt_rr(&e, &m->extra, layout);
      ancount++;
    }

  for (i = 0; i < m->nhops; i++)
    emit_cname_rr(&e, chain_name(m, 2 * i),
                  chain_name(m, 2 * i + 1), layout,
                  300UL + (unsigned long)i * 10UL);

  for (i = 0; i < m->naddrs; i++)
    {
      int p = em_addr_perm(order_variant, i, m->naddrs);
      out->addr_order[i] = p;
      emit_answer_rr(&e, chain_name(m, 2 * m->nhops + 1 + i), layout,
                     atype, 120, m->addrs[p].v, m->addrs[p].len);
    }
  out->naddrs = m->naddrs;

  if (m->has_extra && extra_where == 2)
    {
      emit_txt_rr(&e, &m->extra, layout);
      ancount++;
    }
  if (m->has_extra && extra_where == 0)
    arcount = 1;
  if (m->has_extra && extra_where == 0)
    emit_txt_rr(&e, &m->extra, layout);

  if (e.failed)
    return -1;

  e.buf[4] = 0; e.buf[5] = 1;                 /* QDCOUNT = 1 */
  e.buf[6] = (unsigned char)((ancount >> 8) & 0xff);
  e.buf[7] = (unsigned char)(ancount & 0xff);
  e.buf[8] = 0; e.buf[9] = 0;                 /* NSCOUNT = 0 */
  e.buf[10] = (unsigned char)((arcount >> 8) & 0xff);
  e.buf[11] = (unsigned char)(arcount & 0xff);

  if (e.pos > EM_MSGMAX) return -1;
  memcpy(out->msg, e.buf, (size_t)e.pos);
  out->msglen = e.pos;
  memcpy(out->names, e.names, sizeof(e.names));
  out->nnames = e.nnames;
  out->ptr_depth = e.ptr_depth;
  out->oversized = 0;

  /* Protocol invariants: every pointer targets an earlier, written
   * name-label boundary; both the encoded form and the full logical
   * name stay within the RFC 1035 255-byte name limit. */
  for (i = 0; i < out->nnames; i++)
    {
      const unsigned char *p = out->msg + out->names[i].offset;
      const unsigned char *end = p + out->names[i].enclen;
      int k, found;
      if (out->names[i].enclen > EM_NAMEMAX) return -1;
      if (em_name_wirelen(&out->names[i].identity) > EM_NAMEMAX) return -1;
      while (p < end)
        {
          if ((*p & 0xc0) == 0xc0)
            {
              int off = ((p[0] & 0x3f) << 8) | p[1];
              if (off >= out->names[i].offset) return -1; /* forward */
              found = 0;
              for (k = 0; k < e.nbound; k++)
                if (e.bounds[k].offset == off) { found = 1; break; }
              if (!found) return -1;
              break; /* pointer terminates the name */
            }
          if (*p == 0) break;
          p += 1 + *p;
        }
    }
  return 0;
}

/* ------------------------------------------------------------------ */
/* Oracle side: only here does c-ares get called.                     */
/* ------------------------------------------------------------------ */

static void set_diff(char *diff, size_t n, const char *fmt, ...)
{
  va_list ap;
  if (!diff || !n) return;
  va_start(ap, fmt);
  vsnprintf(diff, n, fmt, ap);
  va_end(ap);
}

static int count_aliases(const struct hostent *h)
{
  int n = 0;
  if (h->h_aliases)
    while (h->h_aliases[n]) n++;
  return n;
}

static int count_addrs(const struct hostent *h)
{
  int n = 0;
  if (h->h_addr_list)
    while (h->h_addr_list[n]) n++;
  return n;
}

int em_check_reply(const em_model *m, const em_wire *w, char *diff,
                   size_t diffsz)
{
  struct hostent *h = NULL;
  int rc;
  char want[EM_NAMEMAX];
  int i;

  if (m->qtype == 1)
    rc = ares_parse_a_reply(w->msg, w->msglen, &h, NULL, NULL);
  else
    rc = ares_parse_aaaa_reply(w->msg, w->msglen, &h, NULL, NULL);

  if (rc != ARES_SUCCESS)
    {
      set_diff(diff, diffsz, "seed=%u parse failed rc=%d msglen=%d",
               m->seed, rc, w->msglen);
      return -1;
    }
  if (!h)
    {
      set_diff(diff, diffsz, "seed=%u null hostent", m->seed);
      return -1;
    }

  /* Canonical name: c-ares keeps the head CNAME node's target, i.e.
   * the first hop target (verified against ares_parse_a_reply.c). */
  render_expanded(&m->cname[0], want, sizeof(want));
  if (!h->h_name || strcasecmp(h->h_name, want) != 0)
    {
      set_diff(diff, diffsz,
               "seed=%u h_name mismatch want='%s' got='%s'",
               m->seed, want, h->h_name ? h->h_name : "(null)");
      goto fail;
    }

  /* Aliases in chain order: qname then each hop source, compared by
   * DNS ASCII case-insensitive rules. */
  if (count_aliases(h) != m->nhops)
    {
      set_diff(diff, diffsz,
               "seed=%u alias count want=%d got=%d",
               m->seed, m->nhops, count_aliases(h));
      goto fail;
    }
  for (i = 0; i < m->nhops; i++)
    {
      const em_name *expected = (i == 0) ? &m->qname : &m->cname[i - 1];
      render_expanded(expected, want, sizeof(want));
      if (strcasecmp(h->h_aliases[i], want) != 0)
        {
          set_diff(diff, diffsz,
                   "seed=%u alias[%d] mismatch want='%s' got='%s'",
                   m->seed, i, want, h->h_aliases[i]);
          goto fail;
        }
    }

  /* Addresses: identity, width and the order preserved from the wire.
   * The order variant permutes the emitted RRs, so the expected model
   * index at hostent slot i is w->addr_order[i]. */
  if (count_addrs(h) != m->naddrs)
    {
      set_diff(diff, diffsz, "seed=%u addr count want=%d got=%d",
               m->seed, m->naddrs, count_addrs(h));
      goto fail;
    }
  if (h->h_length != m->addrs[0].len)
    {
      set_diff(diff, diffsz, "seed=%u h_length mismatch want=%d got=%d",
               m->seed, m->addrs[0].len, (int)h->h_length);
      goto fail;
    }
  for (i = 0; i < m->naddrs; i++)
    {
      int midx = w->addr_order[i];
      if (memcmp(h->h_addr_list[i], m->addrs[midx].v,
                 (size_t)m->addrs[midx].len) != 0)
        {
          set_diff(diff, diffsz,
                   "seed=%u addr[%d] byte mismatch (model idx %d)",
                   m->seed, i, midx);
          goto fail;
        }
    }

  /* Address family tag. */
  if ((m->qtype == 1 && h->h_addrtype != 2 /* AF_INET */) ||
      (m->qtype == 28 && h->h_addrtype != 30 /* AF_INET6 */))
    {
      set_diff(diff, diffsz, "seed=%u h_addrtype=%d unexpected",
               m->seed, (int)h->h_addrtype);
      goto fail;
    }

  ares_free_hostent(h);
  return 0;

fail:
  ares_free_hostent(h);
  return -1;
}

int em_check_expand(const em_wire *w, char *diff, size_t diffsz)
{
  int i;
  for (i = 0; i < w->nnames; i++)
    {
      const em_wname *wn = &w->names[i];
      char *s = NULL;
      long enclen = 0;
      char want[EM_NAMEMAX];
      int rc;

      if (wn->offset < 0 || wn->offset + wn->enclen > w->msglen)
        {
          set_diff(diff, diffsz,
                   "name[%d] offset=%d enclen=%d outside msglen=%d",
                   i, wn->offset, wn->enclen, w->msglen);
          return -1;
        }

      rc = ares_expand_name(w->msg + wn->offset, w->msg, w->msglen,
                            &s, &enclen);
      if (rc != ARES_SUCCESS)
        {
          set_diff(diff, diffsz,
                   "name[%d] offset=%d expand rc=%d", i, wn->offset, rc);
          return -1;
        }
      if (enclen != wn->enclen)
        {
          set_diff(diff, diffsz,
                   "name[%d] offset=%d enclen mismatch want=%d got=%ld",
                   i, wn->offset, wn->enclen, enclen);
          ares_free_string(s);
          return -1;
        }
      render_expanded(&wn->identity, want, sizeof(want));
      if (strcasecmp(s, want) != 0)
        {
          set_diff(diff, diffsz,
                   "name[%d] offset=%d expanded mismatch want='%s' got='%s'",
                   i, wn->offset, want, s);
          ares_free_string(s);
          return -1;
        }
      ares_free_string(s);
    }
  return 0;
}

/* ------------------------------------------------------------------ */
/* Predetermined logic-difference simulator.                          */
/* ------------------------------------------------------------------ */

int em_diff_simulator(void)
{
  const unsigned fixed_seed = 0xC4E5u;
  em_model m0 = em_generate(fixed_seed, 0);
  em_model m1 = em_generate(fixed_seed, 1);
  em_wire w0, w1;
  char diff0[256], diff1[256];
  int rc0, rc1;

  if (em_build(&m0, EM_LAYOUT_MULTI, 0, 0, 0, &w0) ||
      em_build(&m1, EM_LAYOUT_SUFFIX, 0, 0, 0, &w1))
    return -1;

  /* Simulated upstream logic divergence: flip one expectation byte.
   * The wire message is untouched, only the model's prediction changes. */
  m0.addrs[0].v[0] ^= 0x80;

  memset(diff0, 0, sizeof(diff0));
  rc0 = em_check_reply(&m0, &w0, diff0, sizeof(diff0));
  memset(diff1, 0, sizeof(diff1));
  rc1 = em_check_reply(&m1, &w1, diff1, sizeof(diff1));

  /* Oracle must flag the first case (n=0) by seed and keep it; the
   * next case must still pass. */
  if (rc0 == 0) return -2;
  if (rc1 != 0) return -3;
  if (strstr(diff0, "seed=") == NULL) return -4;
  if (strstr(diff0, "mismatch") == NULL) return -5;
  return 0;
}
