/*
 * Replayable property / metamorphic tests for the c-ares DNS parser.
 *
 * The whole chain exercises real library interfaces:
 *   ares_parse_a_reply(), ares_parse_aaaa_reply(), ares_expand_name()
 * against DNS messages synthesised by an *independent* model/encoder in
 * this file.  The encoder never calls c-ares to produce an expected value
 * and the oracle never derives the model from a library result.
 *
 * Three groups are exposed through task_cases():
 *   model       - independent logical model vs. parsed hostent and enclen
 *   metamorphic - legal representation changes that must preserve identity
 *   replay      - determinism, protocol limits and fault-injection of the
 *                 oracle itself (pre-determined logic-difference simulator)
 *
 * C99 only.  Fixed seed, no network, no resolver, no external packages.
 */

#include "check.h"

#include "ares.h"

#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* DNS wire constants (RFC 1035), mirrored so the model is standalone. */
/* ------------------------------------------------------------------ */
#define HDR_LEN            12
#define MAX_MSG           512   /* classic UDP message limit           */
#define MAX_NAME_WIRE     255   /* maximum on-wire name length         */
#define MAX_LABEL_WIRE     63   /* maximum label length                */
#define MAX_PTR_DEPTH       8   /* kept far under c-ares MAX_INDIRS=50 */
#define C_IN                1
#define T_A                 1
#define T_CNAME             5
#define T_AAAA             28
#define T_TXT              16
#define FLAG_QR_RD_RA   0x8180u

#define N_CASES           500
#define MAX_K               4   /* CNAME hops: 1..4                    */
#define MAX_LABELS          4   /* labels per non-root name            */
#define MAX_ADDRS           3   /* final address records per case      */
#define MAX_OCC            24   /* name occurrences recorded per msg   */
#define MAX_BND           128   /* compression boundaries per message  */
#define FIXED_SEED  0xC4E5A11ULL

/* ------------------------------------------------------------------ */
/* Deterministic PRNG: splitmix64.  Seed replay reproduces every byte. */
/* ------------------------------------------------------------------ */
static uint64_t rng_state;

static uint64_t rng_next(void)
{
  uint64_t z;
  rng_state += 0x9e3779b97f4a7c15ULL;
  z = rng_state;
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  return z ^ (z >> 31);
}

static unsigned rng_below(unsigned n)
{
  return (unsigned)(rng_next() % n);
}

/* ------------------------------------------------------------------ */
/* Logical model.                                                      */
/*                                                                     */
/* A domain name is an array of ASCII label strings ([A-Za-z0-9-]);   */
/* the root is the zero-label name.  A case holds k (1..4) CNAME hops  */
/* N0 -> N1 -> ... -> Nk followed by 1..3 final A or AAAA records.     */
/* All non-root names in a case share a generated zone so that legal   */
/* suffix compression is always available across records.              */
/* ------------------------------------------------------------------ */
struct dname {
  char     label[MAX_LABELS][MAX_LABEL_WIRE + 1]; /* wire-case bytes  */
  unsigned nlabels;
  int      is_root;
};

struct model {
  uint64_t       seed;
  unsigned       idx;
  struct dname   name[MAX_K + 1];   /* N0 query ... Nk final owner     */
  struct dname   zone;              /* shared trailing labels          */
  unsigned       k;                 /* CNAME hops 1..4                 */
  int            family;            /* T_A or T_AAAA                   */
  unsigned       naddr;
  unsigned char  addr[MAX_ADDRS][16];
  int            has_root;
  unsigned       root_pos;
};

/* Per-occurrence record of how a name actually occupied the wire. */
struct occurrence {
  int          off;                 /* offset of name in message       */
  long         wire_len;            /* bytes the name occupies         */
  int          depth;               /* pointer hops reached from here  */
  char         expect[256];         /* expected expanded (wire-case)   */
};

/* A compression boundary: a label start written earlier in the msg. */
struct boundary {
  int            off;
  int            depth;   /* pointer hops decodable after this point  */
  struct dname  *owner;
  unsigned       lidx;    /* label index inside owner                 */
};

struct encoder {
  unsigned char     buf[MAX_MSG];
  int               len;
  struct boundary   bnd[MAX_BND];
  unsigned          nbnd;
  struct occurrence occ[MAX_OCC];
  unsigned          nocc;
  struct dname      ladder_owners[4]; /* owner storage for pointer ladder */
  int               limit_violation;
};

/* Aggregate coverage / result counters. */
struct stats {
  unsigned messages;
  unsigned parse_ok;
  unsigned oracle_fail;
  unsigned name_exact_ok;
  unsigned name_fold_ok;
  unsigned enclen_checks;
  unsigned enclen_fail;
  unsigned khist[MAX_K + 1];
  unsigned depthhist[MAX_PTR_DEPTH + 2];
  unsigned n_a;
  unsigned n_aaaa;
  unsigned root_cases;
  unsigned root_one_byte;
  unsigned root_two_byte;
  unsigned meta_runs;
  unsigned meta_fail;
  unsigned meta_addr_invariant;
  unsigned trunc_rejected;
  unsigned ptr_illegal;
  unsigned size_violation;
  uint64_t checksum;
  int      first_idx;
  char     first_dev[160];
};

static struct stats g_s;

/* ------------------------------------------------------------------ */
/* Small model helpers (no c-ares calls in the model/encoder section). */
/* ------------------------------------------------------------------ */
static const char CHARSET[] =
  "abcdefghijklmnopqrstuvwxyz0123456789-ABCDEFGHIJKLMNOPQRSTUVWXYZ";
static const char *const ZONE_WORDS[] = { "com", "net", "org", "io", "dev" };

static void ascii_fold(char *dst, const char *src)
{
  while (*src) {
    unsigned char c = (unsigned char)*src++;
    if (c >= 'A' && c <= 'Z')
      c = (unsigned char)(c + ('a' - 'A'));
    *dst++ = (char)c;
  }
  *dst = 0;
}

static int dname_fold_equal(const struct dname *a, const struct dname *b)
{
  char la[256], lb[256];
  unsigned i;
  if (a->is_root != b->is_root || a->nlabels != b->nlabels)
    return 0;
  for (i = 0; i < a->nlabels; ++i) {
    ascii_fold(la, a->label[i]);
    ascii_fold(lb, b->label[i]);
    if (strcmp(la, lb) != 0)
      return 0;
  }
  return 1;
}

/* Render a name the way ares_expand_name presents it (no trailing dot,
 * root becomes the empty string). */
static void dname_render(const struct dname *d, char *out, size_t outsz)
{
  size_t pos = 0;
  unsigned i;
  out[0] = 0;
  if (d->is_root)
    return;
  for (i = 0; i < d->nlabels; ++i) {
    size_t ll = strlen(d->label[i]);
    if (i) {
      if (pos + 1 >= outsz) break;
      out[pos++] = '.';
    }
    if (pos + ll >= outsz) ll = outsz - pos - 1;
    memcpy(out + pos, d->label[i], ll);
    pos += ll;
    out[pos] = 0;
  }
}

static void make_root(struct dname *d)
{
  d->is_root = 1;
  d->nlabels = 0;
  d->label[0][0] = 0;
}

/* One unique random label followed by the shared zone labels. */
static void make_zoned_name(struct dname *d, const struct dname *zone)
{
  unsigned ll = 1 + rng_below(5);
  unsigned i, j;
  d->is_root = 0;
  d->nlabels = 1 + zone->nlabels;
  for (j = 0; j < ll; ++j)
    d->label[0][j] = CHARSET[rng_below(62)];
  d->label[0][ll] = 0;
  for (i = 0; i < zone->nlabels; ++i)
    strcpy(d->label[1 + i], zone->label[i]);
}

static void make_zone(struct dname *zone)
{
  zone->is_root = 0;
  zone->nlabels = 1 + rng_below(2);               /* 1 or 2 labels     */
  strcpy(zone->label[0], ZONE_WORDS[rng_below(5)]);
  if (zone->nlabels == 2)
    strcpy(zone->label[1], ZONE_WORDS[rng_below(5)]);
}

/* Unrelated owner used by metamorphic insertions; the fixed "zz"
 * prefix cannot collide with a generated chain name. */
static void make_unrelated(struct dname *d)
{
  d->is_root = 0;
  d->nlabels = 2;
  strcpy(d->label[0], "zz0-extra");
  strcpy(d->label[1], "irrelevant");
}

/* ------------------------------------------------------------------ */
/* Encoder.  Three legal layouts:                                      */
/*   0 LAYOUT_NONE   - no compression at all                           */
/*   1 LAYOUT_SUFFIX - suffix only, >=1 literal label, depth-0 target  */
/*   2 LAYOUT_MULTI  - cross-record suffix, whole-name and multi-level */
/*                    pointers allowed, always to earlier boundaries   */
/* ------------------------------------------------------------------ */
enum { LAYOUT_NONE, LAYOUT_SUFFIX, LAYOUT_MULTI };

static void enc_init(struct encoder *e)
{
  memset(e, 0, sizeof *e);
}

static int suffix_matches(const struct boundary *b, const struct dname *d,
                          unsigned s)
{
  unsigned j;
  if (b->owner->is_root || b->owner->nlabels - b->lidx != s)
    return 0;
  if (d->is_root || d->nlabels < s)
    return 0;
  for (j = 0; j < s; ++j) {
    if (strcmp(b->owner->label[b->lidx + j],
               d->label[d->nlabels - s + j]) != 0)
      return 0;
  }
  return 1;
}

/* Find the longest legal compressible suffix for d under the layout;
 * among equal suffixes prefer the deepest target so that legitimate
 * multi-level pointer chains arise in LAYOUT_MULTI. */
static const struct boundary *find_suffix(struct encoder *e,
                                          const struct dname *d,
                                          int layout, int allow_full)
{
  unsigned L = d->nlabels;
  unsigned smax = allow_full ? L : L - 1;
  const struct boundary *best = NULL;
  unsigned s, i;

  for (s = smax; s >= 1; --s) {
    for (i = 0; i < e->nbnd; ++i) {
      const struct boundary *b = &e->bnd[i];
      if (layout == LAYOUT_SUFFIX && b->depth != 0)
        continue;
      if (b->depth + 1 > MAX_PTR_DEPTH)
        continue;
      if (suffix_matches(b, d, s)) {
        if (!best || b->depth > best->depth)
          best = b;
      }
    }
    if (best)
      return best;
  }
  return NULL;
}

/* Earlier zero-label boundary, for a pointer to the root. */
static const struct boundary *find_root_boundary(struct encoder *e)
{
  unsigned i;
  for (i = 0; i < e->nbnd; ++i) {
    if (e->bnd[i].owner->is_root &&
        e->bnd[i].depth + 1 <= MAX_PTR_DEPTH)
      return &e->bnd[i];
  }
  return NULL;
}

static void enc_record_occurrence(struct encoder *e, int start, int depth,
                                  const struct dname *d)
{
  if (e->nocc < MAX_OCC) {
    struct occurrence *o = &e->occ[e->nocc++];
    o->off = start;
    o->wire_len = (long)(e->len - start);
    o->depth = depth;
    dname_render(d, o->expect, sizeof o->expect);
  }
}

/* Forward declarations: ladder helpers are defined before enc_put and
 * the boundary lookup below, but need these encoder primitives. */
static void enc_put(struct encoder *e, const void *p, unsigned n);

/* Render the decoded text reachable from an existing boundary. */
static void render_from_boundary(const struct boundary *b, char *out,
                                 size_t outsz)
{
  size_t pos = 0;
  unsigned j;
  out[0] = 0;
  for (j = b->lidx; j < b->owner->nlabels; ++j) {
    size_t ll = strlen(b->owner->label[j]);
    if (j != b->lidx) {
      if (pos + 1 < outsz) { out[pos++] = '.'; out[pos] = 0; }
    }
    if (pos + ll >= outsz) ll = outsz - pos - 1;
    memcpy(out + pos, b->owner->label[j], ll);
    pos += ll;
    out[pos] = 0;
  }
}

/* First depth-0, non-root boundary: a safe suffix target for the ladder. */
static const struct boundary *pick_depth0_name(struct encoder *e)
{
  unsigned i;
  for (i = 0; i < e->nbnd; ++i) {
    if (e->bnd[i].depth == 0 && !e->bnd[i].owner->is_root)
      return &e->bnd[i];
  }
  return NULL;
}

/* Write one ladder owner: a fresh literal label followed by a single
 * compression pointer at an earlier boundary.  Registers the new label
 * boundary at depth tgt->depth+1 and records the occurrence with the
 * exact expected expansion text (the caller supplies the full text,
 * already accounting for every pointer hop beneath this name). */
static int write_ladder_name(struct encoder *e, struct dname *owner,
                             const char *label, const struct boundary *tgt,
                             const char *full_expect,
                             struct boundary *registered)
{
  int start = e->len;
  size_t ll = strlen(label);
  unsigned char lc = (unsigned char)ll;
  unsigned char ptr[2];

  strcpy(owner->label[0], label);
  owner->nlabels = 1;
  owner->is_root = 0;

  enc_put(e, &lc, 1);
  enc_put(e, label, (unsigned)ll);

  registered->off = start;
  registered->depth = tgt->depth + 1;
  registered->owner = owner;
  registered->lidx = 0;
  if (e->nbnd < MAX_BND)
    e->bnd[e->nbnd++] = *registered;

  if (tgt->off >= start)
    g_s.ptr_illegal++;
  ptr[0] = (unsigned char)(0xc0 | (tgt->off >> 8));
  ptr[1] = (unsigned char)(tgt->off & 0xff);
  enc_put(e, ptr, 2);

  if (e->nocc < MAX_OCC) {
    struct occurrence *o = &e->occ[e->nocc++];
    o->off = start;
    o->wire_len = (long)(e->len - start);
    o->depth = tgt->depth + 1;
    strncpy(o->expect, full_expect, sizeof o->expect - 1);
    o->expect[sizeof o->expect - 1] = 0;
  }
  return e->len - start;
}

static void enc_put(struct encoder *e, const void *p, unsigned n)
{
  if (e->len + (int)n > MAX_MSG) {
    e->limit_violation = 1;
    return;
  }
  memcpy(e->buf + e->len, p, n);
  e->len += (int)n;
}

/* Write one domain name.  Returns the number of wire bytes used.
 * Pointers only ever reference boundaries written earlier, and the
 * model tracks the exact on-wire length of every occurrence. */
static int write_name(struct encoder *e, const struct dname *d, int layout)
{
  int start = e->len;
  unsigned L = d->nlabels;
  const struct boundary *tgt = NULL;
  int tail_depth = 0;
  unsigned s = 0;
  unsigned j;

  if (d->is_root) {
    /* Root: either a two-byte pointer at an earlier root label, or a
     * single zero byte (which itself becomes a root boundary). */
    if (layout == LAYOUT_MULTI)
      tgt = find_root_boundary(e);
    if (tgt) {
      unsigned char ptr[2];
      ptr[0] = (unsigned char)(0xc0 | (tgt->off >> 8));
      ptr[1] = (unsigned char)(tgt->off & 0xff);
      if (tgt->off >= start)
        g_s.ptr_illegal++;
      enc_put(e, ptr, 2);
      g_s.root_two_byte++;
      enc_record_occurrence(e, start, tgt->depth + 1, d);
      return e->len - start;
    }
    if (e->nbnd < MAX_BND) {
      e->bnd[e->nbnd].off = e->len;
      e->bnd[e->nbnd].depth = 0;
      e->bnd[e->nbnd].owner = (struct dname *)d;
      e->bnd[e->nbnd].lidx = 0;
      e->nbnd++;
    }
    e->buf[e->len++] = 0;
    g_s.root_one_byte++;
    enc_record_occurrence(e, start, 0, d);
    return e->len - start;
  }

  if (layout != LAYOUT_NONE)
    tgt = find_suffix(e, d, layout, layout == LAYOUT_MULTI);
  if (tgt) {
    s = tgt->owner->nlabels - tgt->lidx;
    tail_depth = tgt->depth + 1;
  }

  /* Literal leading labels; each label start is a legal boundary. */
  for (j = 0; j < L - s; ++j) {
    size_t ll = strlen(d->label[j]);
    unsigned char lc;
    struct boundary b;
    if (ll > MAX_LABEL_WIRE)
      e->limit_violation = 1;
    b.off = e->len;
    b.depth = tail_depth;
    b.owner = (struct dname *)d;
    b.lidx = j;
    if (e->nbnd < MAX_BND)
      e->bnd[e->nbnd++] = b;
    lc = (unsigned char)ll;
    enc_put(e, &lc, 1);
    enc_put(e, d->label[j], (unsigned)ll);
  }

  if (s > 0) {
    unsigned char ptr[2];
    if (tgt->off >= start)
      g_s.ptr_illegal++;
    ptr[0] = (unsigned char)(0xc0 | (tgt->off >> 8));
    ptr[1] = (unsigned char)(tgt->off & 0xff);
    enc_put(e, ptr, 2);
  } else {
    e->buf[e->len++] = 0;               /* terminating zero label */
  }

  if (e->len - start > MAX_NAME_WIRE)
    e->limit_violation = 1;

  enc_record_occurrence(e, start, tail_depth, d);
  return e->len - start;
}

static void put16(struct encoder *e, unsigned v)
{
  unsigned char b[2] = { (unsigned char)(v >> 8), (unsigned char)(v & 0xff) };
  enc_put(e, b, 2);
}

static void put32(struct encoder *e, unsigned long v)
{
  unsigned char b[4] = {
    (unsigned char)(v >> 24), (unsigned char)(v >> 16),
    (unsigned char)(v >> 8), (unsigned char)(v)
  };
  enc_put(e, b, 4);
}

static void write_rr_fixed(struct encoder *e, unsigned type,
                           unsigned rdlength, unsigned long ttl)
{
  put16(e, type);
  put16(e, C_IN);
  put32(e, ttl);
  put16(e, rdlength);
}

/* ------------------------------------------------------------------ */
/* Model generation.                                                   */
/* ------------------------------------------------------------------ */
static void gen_model(struct model *m, unsigned idx)
{
  unsigned i, j;
  int collide;

  m->idx = idx;
  m->seed = rng_state;
  m->k = 1 + rng_below(MAX_K);
  m->family = (rng_next() & 1) ? T_A : T_AAAA;
  m->naddr = 1 + rng_below(MAX_ADDRS);
  m->has_root = (idx % 16 == 0);
  m->root_pos = m->has_root ? rng_below(m->k + 1) : 0;

  make_zone(&m->zone);
  for (i = 0; i <= m->k; ++i) {
    if (m->has_root && i == m->root_pos) {
      make_root(&m->name[i]);
      continue;
    }
    do {
      make_zoned_name(&m->name[i], &m->zone);
      collide = 0;
      for (j = 0; j < i; ++j) {
        if (dname_fold_equal(&m->name[i], &m->name[j])) {
          collide = 1;
          break;
        }
      }
    } while (collide);
  }

  for (i = 0; i < m->naddr; ++i) {
    unsigned nbytes = (m->family == T_A) ? 4 : 16;
    for (j = 0; j < nbytes; ++j)
      m->addr[i][j] = (unsigned char)rng_next();
  }
}

static void chain_text(const struct model *m, char *out, size_t outsz)
{
  char name[256];
  size_t pos = 0;
  unsigned i;
  out[0] = 0;
  for (i = 0; i <= m->k; ++i) {
    if (i && pos + 4 < outsz) {
      memcpy(out + pos, " -> ", 4);
      pos += 4;
    }
    if (m->name[i].is_root)
      strcpy(name, ".");
    else
      dname_render(&m->name[i], name, sizeof name);
    if (pos + strlen(name) < outsz) {
      size_t ll = strlen(name);
      memcpy(out + pos, name, ll);
      pos += ll;
      out[pos] = 0;
    }
  }
}

/* ------------------------------------------------------------------ */
/* Resource record writers.                                            */
/* ------------------------------------------------------------------ */
static void write_cname_rr(struct encoder *e, const struct model *m,
                           unsigned hop, int layout)
{
  int rdlen_pos;
  int rdlen;
  write_name(e, &m->name[hop], layout);
  rdlen_pos = e->len + 8;             /* rdlength field offset */
  write_rr_fixed(e, T_CNAME, 0, 60);
  rdlen = write_name(e, &m->name[hop + 1], layout);
  e->buf[rdlen_pos] = (unsigned char)(rdlen >> 8);
  e->buf[rdlen_pos + 1] = (unsigned char)(rdlen & 0xff);
}

static void write_final_rr(struct encoder *e, const struct model *m,
                           unsigned which, int layout)
{
  unsigned nbytes = (m->family == T_A) ? 4 : 16;
  write_name(e, &m->name[m->k], layout);
  write_rr_fixed(e, (unsigned)m->family, nbytes, 60);
  enc_put(e, m->addr[which], nbytes);
}

/* An A/AAAA RR at an unrelated owner: legal on the wire, must be
 * ignored by the parser because its owner never matches the hostname. */
static void write_unrelated_rr(struct encoder *e, const struct dname *u,
                               int family)
{
  static const unsigned char dummy4[4] = { 198, 51, 100, 7 };
  static const unsigned char dummy6[16] = {
    0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xfe
  };
  write_name(e, u, LAYOUT_NONE);
  if (family == T_A) {
    write_rr_fixed(e, T_A, 4, 30);
    enc_put(e, dummy4, 4);
  } else {
    write_rr_fixed(e, T_AAAA, 16, 30);
    enc_put(e, dummy6, 16);
  }
}

static void write_txt_rr(struct encoder *e, const struct dname *u)
{
  const char txt[] = "prop";
  unsigned rd = 1 + (unsigned)sizeof(txt) - 1;
  write_name(e, u, LAYOUT_NONE);
  write_rr_fixed(e, T_TXT, rd, 30);
  e->buf[e->len++] = (unsigned char)(rd - 1);
  enc_put(e, txt, rd - 1);
}

/* Append four TXT RRs whose owners form a legal backward compression
 * ladder of pointer depth 1..4.  TXT owners are never consulted by the
 * A/AAAA parser, so hostent identity is untouched, while every ladder
 * name is still validated through ares_expand_name.  The first owner
 * points at an earlier depth-0 name boundary; each later owner points
 * at the boundary registered by the previous ladder owner. */
static int append_compression_ladder(struct encoder *e)
{
  static const char *const labels[4] = { "ld0a", "ld1b", "ld2c", "ld3d" };
  const struct boundary *tgt = pick_depth0_name(e);
  struct boundary reg;
  char prev_full[256];
  char full[256];
  unsigned ar_now;
  int i;

  if (!tgt)
    return 0;                       /* e.g. root-only question: skip    */

  /* The depth-0 boundary's own labels give its complete expansion. */
  render_from_boundary(tgt, prev_full, sizeof prev_full);

  ar_now = (unsigned)((e->buf[10] << 8) | e->buf[11]);
  for (i = 0; i < 4; ++i) {
    snprintf(full, sizeof full, "%s.%s", labels[i], prev_full);
    write_ladder_name(e, &e->ladder_owners[i], labels[i], tgt, full, &reg);
    /* TXT rdata: one length octet + the two-character string "ld". */
    write_rr_fixed(e, T_TXT, 3, 30);
    e->buf[e->len++] = 2;
    enc_put(e, "ld", 2);
    strcpy(prev_full, full);
    tgt = &e->bnd[e->nbnd - 1];     /* boundary just registered */
    if (tgt->off != reg.off)
      return -2;
  }
  ar_now += 4;
  e->buf[10] = (unsigned char)(ar_now >> 8);
  e->buf[11] = (unsigned char)(ar_now & 0xff);

  if (e->len > MAX_MSG || e->limit_violation)
    return -3;
  return 0;
}

/* ------------------------------------------------------------------ */
/* Message construction.                                               */
/*                                                                     */
/* variant: V_BASE plain, V_ORDER unrelated RRs inserted at slot,      */
/* V_CASE question-name case flip, V_ADD unrelated additional records. */
/* ------------------------------------------------------------------ */
#define V_BASE 0
#define V_ORDER 1
#define V_CASE 2
#define V_ADD 3

static int build_message(const struct model *min, int layout, int variant,
                         unsigned slot, int qcase, struct encoder *e)
{
  struct model m;
  struct dname unrelated;
  unsigned canonical, want_answer, want_ar;
  unsigned ancount_pos, arcount_pos;
  unsigned pos = 0, emitted = 0, ar_emitted = 0, i;

  enc_init(e);
  m = *min;                         /* local copy for the case flip */

  if (variant == V_CASE && qcase && !m.name[0].is_root) {
    for (i = 0; i < m.name[0].nlabels; ++i) {
      char *p = m.name[0].label[i];
      while (*p) {
        if (*p >= 'a' && *p <= 'z')
          *p = (char)(*p - ('a' - 'A'));
        else if (*p >= 'A' && *p <= 'Z')
          *p = (char)(*p + ('a' - 'A'));
        ++p;
      }
    }
  }
  make_unrelated(&unrelated);

  /* Header; counts are patched once the sections are known. */
  put16(e, (unsigned)(m.seed & 0xffff));
  put16(e, FLAG_QR_RD_RA);
  put16(e, 1);                       /* QDCOUNT */
  ancount_pos = (unsigned)e->len;
  put16(e, 0);
  put16(e, 0);                       /* NSCOUNT */
  arcount_pos = (unsigned)e->len;
  put16(e, 0);

  /* Question section. */
  write_name(e, &m.name[0], layout);
  put16(e, (unsigned)m.family);
  put16(e, C_IN);

  canonical = m.k + m.naddr;
  want_answer = canonical + (variant == V_ORDER ? 2 : 0);
  want_ar = (variant == V_ADD) ? 2 : 0;

  /* Answer section; the unrelated pair can land at any legal slot. */
  while (emitted < want_answer) {
    if (variant == V_ORDER && emitted == slot) {
      write_unrelated_rr(e, &unrelated, m.family);
      write_unrelated_rr(e, &unrelated,
                        m.family == T_A ? T_AAAA : T_A);
      emitted += 2;
      continue;
    }
    if (pos < m.k)
      write_cname_rr(e, &m, pos, layout);
    else
      write_final_rr(e, &m, pos - m.k, layout);
    pos++;
    emitted++;
  }

  /* Additional section: parser never consults it for A/AAAA identity. */
  while (ar_emitted < want_ar) {
    if (ar_emitted == 0)
      write_txt_rr(e, &unrelated);
    else
      write_unrelated_rr(e, &unrelated, m.family);
    ar_emitted++;
  }

  e->buf[ancount_pos] = (unsigned char)(want_answer >> 8);
  e->buf[ancount_pos + 1] = (unsigned char)(want_answer & 0xff);
  e->buf[arcount_pos] = (unsigned char)(want_ar >> 8);
  e->buf[arcount_pos + 1] = (unsigned char)(want_ar & 0xff);

  /* Baseline multi-layout messages additionally carry a legal backward
   * pointer ladder (depth 1..4) in the additional section, so the
   * multi-level compression and enclen paths are genuinely exercised. */
  if (variant == V_BASE && layout == LAYOUT_MULTI) {
    if (append_compression_ladder(e) < 0) {
      g_s.size_violation++;
      return -1;
    }
  }

  if (e->len > MAX_MSG || e->limit_violation) {
    g_s.size_violation++;
    return -1;
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/* Oracle: compare a real parse/expand result against the model.       */
/* It never calls TASK_CHECK and never derives expectations from the   */
/* library output; it returns an error code plus a deviation string so */
/* the difference simulator can drive the same comparison logic.       */
/* ------------------------------------------------------------------ */
#define OV_OK            0
#define OV_PARSE        -1
#define OV_FAMILY       -2
#define OV_HNAME        -3
#define OV_ALIAS_N      -4
#define OV_ALIAS        -5
#define OV_ADDR_N       -6
#define OV_ADDR         -7
#define OV_EXPAND       -8
#define OV_ENCLEN       -9
#define OV_EXPAND_TEXT -10

struct override {
  int corrupt_addr;     /* simulator: flip one expected address byte   */
  int corrupt_hname;    /* simulator: append a byte to expected h_name */
  int fold_names;       /* V_CASE: compare names case-insensitively    */
};

static int fold_str_eq(const char *a, const char *b)
{
  while (*a && *b) {
    unsigned char ca = (unsigned char)*a++;
    unsigned char cb = (unsigned char)*b++;
    if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + 32);
    if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + 32);
    if (ca != cb)
      return 0;
  }
  return *a == *b;
}

/* Name identity check: exact wire case normally, folded for V_CASE. */
static int name_matches(const char *got, const char *want, int fold,
                        unsigned *exact, unsigned *folded)
{
  int exact_ok = (strcmp(got, want) == 0);
  int fold_ok = fold_str_eq(got, want);
  if (exact_ok)
    (*exact)++;
  if (fold_ok)
    (*folded)++;
  return fold ? fold_ok : (exact_ok && fold_ok);
}

static int verify_message(const struct model *m, const struct encoder *e,
                          const struct override *ov, char *dev, size_t devsz)
{
  struct hostent *h = NULL;
  int st;
  unsigned i;
  int expected_len = (m->family == T_A) ? 4 : 16;
  int fold = ov ? ov->fold_names : 0;
  char want[256];

  if (m->family == T_A)
    st = ares_parse_a_reply(e->buf, e->len, &h, NULL, NULL);
  else
    st = ares_parse_aaaa_reply(e->buf, e->len, &h, NULL, NULL);

  if (st != ARES_SUCCESS || !h) {
    snprintf(dev, devsz, "parse status=%d", st);
    return OV_PARSE;
  }

  if (h->h_addrtype != (m->family == T_A ? AF_INET : AF_INET6) ||
      h->h_length != expected_len) {
    snprintf(dev, devsz, "addrtype/length mismatch");
    ares_free_hostent(h);
    return OV_FAMILY;
  }

  /* Observed c-ares semantics: h_name is the first CNAME target. */
  dname_render(&m->name[1], want, sizeof want);
  if (ov && ov->corrupt_hname)
    strcat(want, "X");
  if (!h->h_name ||
      !name_matches(h->h_name, want, fold,
                    &g_s.name_exact_ok, &g_s.name_fold_ok)) {
    snprintf(dev, devsz, "h_name got='%s' want='%s'",
             h->h_name ? h->h_name : "(null)", want);
    ares_free_hostent(h);
    return OV_HNAME;
  }

  /* One alias per CNAME hop, owners in wire order. */
  {
    unsigned nali = 0;
    while (h->h_aliases && h->h_aliases[nali])
      ++nali;
    if (nali != m->k) {
      snprintf(dev, devsz, "alias count got=%u want=%u", nali, m->k);
      ares_free_hostent(h);
      return OV_ALIAS_N;
    }
    for (i = 0; i < m->k; ++i) {
      dname_render(&m->name[i], want, sizeof want);
      if (!name_matches(h->h_aliases[i], want, fold,
                        &g_s.name_exact_ok, &g_s.name_fold_ok)) {
        snprintf(dev, devsz, "alias[%u] got='%s' want='%s'", i,
                 h->h_aliases[i], want);
        ares_free_hostent(h);
        return OV_ALIAS;
      }
    }
  }

  /* Addresses: logical identity and wire order both preserved. */
  {
    unsigned ngot = 0;
    while (h->h_addr_list && h->h_addr_list[ngot])
      ++ngot;
    if (ngot != m->naddr) {
      snprintf(dev, devsz, "addr count got=%u want=%u", ngot, m->naddr);
      ares_free_hostent(h);
      return OV_ADDR_N;
    }
    for (i = 0; i < m->naddr; ++i) {
      unsigned char expected[16];
      memcpy(expected, m->addr[i], expected_len);
      if (ov && ov->corrupt_addr)
        expected[0] = (unsigned char)(expected[0] ^ 0xff);
      if (memcmp(h->h_addr_list[i], expected, expected_len) != 0) {
        snprintf(dev, devsz, "addr[%u] bytes differ", i);
        ares_free_hostent(h);
        return OV_ADDR;
      }
    }
  }
  ares_free_hostent(h);

  /* Every recorded name occurrence: model wire length and expanded
   * text vs. ares_expand_name's enclen and returned string. */
  for (i = 0; i < e->nocc; ++i) {
    const struct occurrence *o = &e->occ[i];
    char *s = NULL;
    long enclen = 0;
    int r = ares_expand_name(e->buf + o->off, e->buf, e->len, &s, &enclen);
    g_s.enclen_checks++;
    if (r != ARES_SUCCESS || !s) {
      snprintf(dev, devsz, "expand_name r=%d at off=%d", r, o->off);
      if (s) ares_free_string(s);
      return OV_EXPAND;
    }
    if (enclen != o->wire_len) {
      snprintf(dev, devsz, "enclen got=%ld model=%ld at off=%d",
               enclen, o->wire_len, o->off);
      ares_free_string(s);
      g_s.enclen_fail++;
      return OV_ENCLEN;
    }
    if (strcmp(s, o->expect) != 0) {
      snprintf(dev, devsz, "expand text got='%s' want='%s' at off=%d",
               s, o->expect, o->off);
      ares_free_string(s);
      return OV_EXPAND_TEXT;
    }
    ares_free_string(s);
  }
  return OV_OK;
}

/* ------------------------------------------------------------------ */
/* FNV-1a checksum over generated messages, for replay stability.      */
/* ------------------------------------------------------------------ */
#define FNV_OFFSET_BASIS 1469598103934665603ULL

static uint64_t fnv_update(uint64_t h, const unsigned char *p, unsigned n)
{
  unsigned i;
  for (i = 0; i < n; ++i) {
    h ^= p[i];
    h *= 1099511628211ULL;
  }
  return h;
}

/* ------------------------------------------------------------------ */
/* Hostent snapshot for metamorphic cross-variant comparison.         */
/* ------------------------------------------------------------------ */
struct snap {
  int          family;
  int          alen;
  char         hname[256];
  char         alias[MAX_K + 1][256];
  unsigned     nalias;
  unsigned     naddr;
  unsigned char addr[MAX_ADDRS][16];
};

static int snapshot_parse(const struct encoder *e, int family, struct snap *s)
{
  struct hostent *h = NULL;
  int st;
  memset(s, 0, sizeof *s);
  if (family == T_A)
    st = ares_parse_a_reply(e->buf, e->len, &h, NULL, NULL);
  else
    st = ares_parse_aaaa_reply(e->buf, e->len, &h, NULL, NULL);
  if (st != ARES_SUCCESS || !h)
    return -1;
  s->family = h->h_addrtype;
  s->alen = h->h_length;
  if (h->h_name)
    strncpy(s->hname, h->h_name, sizeof s->hname - 1);
  while (h->h_aliases && h->h_aliases[s->nalias] &&
         s->nalias < MAX_K + 1) {
    strncpy(s->alias[s->nalias], h->h_aliases[s->nalias],
            sizeof s->alias[0] - 1);
    s->nalias++;
  }
  while (h->h_addr_list && h->h_addr_list[s->naddr] &&
         s->naddr < MAX_ADDRS) {
    memcpy(s->addr[s->naddr], h->h_addr_list[s->naddr],
           (size_t)h->h_length);
    s->naddr++;
  }
  ares_free_hostent(h);
  return 0;
}

static int snap_equal(const struct snap *a, const struct snap *b, int fold)
{
  unsigned i, j;
  if (a->family != b->family || a->alen != b->alen ||
      a->nalias != b->nalias || a->naddr != b->naddr)
    return 0;
  if (fold ? !fold_str_eq(a->hname, b->hname)
           : strcmp(a->hname, b->hname) != 0)
    return 0;
  for (i = 0; i < a->nalias; ++i) {
    if (fold ? !fold_str_eq(a->alias[i], b->alias[i])
             : strcmp(a->alias[i], b->alias[i]) != 0)
      return 0;
  }
  for (i = 0; i < a->naddr; ++i)
    for (j = 0; j < (unsigned)a->alen; ++j)
      if (a->addr[i][j] != b->addr[i][j])
        return 0;
  return 1;
}

/* ------------------------------------------------------------------ */
/* One deterministic pass over all cases and all three layouts.       */
/* ------------------------------------------------------------------ */
static const char *layout_name(int layout)
{
  static const char *names[3] = { "none", "suffix", "multi" };
  return names[layout];
}

static void record_first(int idx, const char *dev)
{
  if (g_s.first_idx < 0 || idx < g_s.first_idx) {
    g_s.first_idx = idx;
    strncpy(g_s.first_dev, dev, sizeof g_s.first_dev - 1);
    g_s.first_dev[sizeof g_s.first_dev - 1] = 0;
  }
}

static int g_built = 0;
static struct encoder g_base[3];   /* baseline layouts of current case */
static struct model g_models[N_CASES];

static void run_pass(void)
{
  unsigned ci, layout;
  char dev[160];
  char chain[400];

  if (g_built)
    return;
  g_built = 1;
  g_s.first_idx = -1;
  g_s.checksum = FNV_OFFSET_BASIS;

  rng_state = FIXED_SEED;
  for (ci = 0; ci < N_CASES; ++ci)
    gen_model(&g_models[ci], ci);

  for (ci = 0; ci < N_CASES; ++ci) {
    struct model *m = &g_models[ci];
    int max_depth_seen = -1;

    g_s.khist[m->k]++;
    if (m->family == T_A)
      g_s.n_a++;
    else
      g_s.n_aaaa++;
    if (m->has_root)
      g_s.root_cases++;
    chain_text(m, chain, sizeof chain);

    for (layout = 0; layout < 3; ++layout) {
      struct override ov;
      int rc;
      memset(&ov, 0, sizeof ov);
      rc = build_message(m, layout, V_BASE, 0, 0, &g_base[layout]);
      if (rc != 0) {
        snprintf(dev, sizeof dev, "build failed layout=%s",
                 layout_name(layout));
        record_first((int)ci, dev);
        g_s.oracle_fail++;
        continue;
      }
      g_s.messages++;
      g_s.checksum = fnv_update(g_s.checksum, g_base[layout].buf,
                                (unsigned)g_base[layout].len);
      {
        unsigned o;
        for (o = 0; o < g_base[layout].nocc; ++o) {
          int d = g_base[layout].occ[o].depth;
          if (d > max_depth_seen)
            max_depth_seen = d;
          g_s.depthhist[d <= MAX_PTR_DEPTH ? d : MAX_PTR_DEPTH + 1]++;
        }
      }
      rc = verify_message(m, &g_base[layout], &ov, dev, sizeof dev);
      if (rc != OV_OK) {
        char full[220];
        snprintf(full, sizeof full, "case=%u layout=%s %s",
                 ci, layout_name(layout), dev);
        record_first((int)ci, full);
        g_s.oracle_fail++;
      } else {
        g_s.parse_ok++;
      }
    }

    if (max_depth_seen < 0)
      max_depth_seen = 0;
    printf("CASE %03u seed=0x%016llx k=%u fam=%s root=%s depthmax=%d "
           "chain=%s layouts=none/suffix/multi\n",
           ci, (unsigned long long)m->seed, m->k,
           m->family == T_A ? "A" : "AAAA",
           m->has_root ? "yes" : "no", max_depth_seen, chain);

    /* Metamorphic variants, compared against each layout's baseline. */
    for (layout = 0; layout < 3; ++layout) {
      struct encoder v;
      struct snap s0, s1;
      struct override ovf;

      if (snapshot_parse(&g_base[layout], m->family, &s0) != 0) {
        record_first((int)ci, "baseline snapshot failed");
        g_s.meta_fail++;
      }

      /* V_ORDER: unrelated RRs at three insertion points. */
      {
        unsigned slots[3] = { 0, m->k, m->k + m->naddr };
        unsigned si;
        for (si = 0; si < 3; ++si) {
          if (build_message(m, layout, V_ORDER, slots[si], 0, &v) != 0) {
            g_s.meta_fail++;
            continue;
          }
          g_s.meta_runs++;
          memset(&ovf, 0, sizeof ovf);
          if (verify_message(m, &v, &ovf, dev, sizeof dev) != OV_OK) {
            record_first((int)ci, "order self-verify failed");
            g_s.meta_fail++;
          }
          if (snapshot_parse(&v, m->family, &s1) != 0 ||
              !snap_equal(&s0, &s1, 0)) {
            record_first((int)ci, "order changed hostent identity");
            g_s.meta_fail++;
          } else {
            g_s.meta_addr_invariant++;
          }
        }
      }

      /* V_CASE: question case flip; identity must fold-equal. */
      if (build_message(m, layout, V_CASE, 0, 1, &v) != 0) {
        g_s.meta_fail++;
      } else {
        g_s.meta_runs++;
        memset(&ovf, 0, sizeof ovf);
        ovf.fold_names = 1;
        if (verify_message(m, &v, &ovf, dev, sizeof dev) != OV_OK) {
          record_first((int)ci, "case self-verify failed");
          g_s.meta_fail++;
        }
        if (snapshot_parse(&v, m->family, &s1) != 0 ||
            !snap_equal(&s0, &s1, 1)) {
          record_first((int)ci, "case change altered identity");
          g_s.meta_fail++;
        } else {
          g_s.meta_addr_invariant++;
        }
      }

      /* V_ADD: unrelated additional-section records; exact equality. */
      if (build_message(m, layout, V_ADD, 0, 0, &v) != 0) {
        g_s.meta_fail++;
      } else {
        g_s.meta_runs++;
        memset(&ovf, 0, sizeof ovf);
        if (verify_message(m, &v, &ovf, dev, sizeof dev) != OV_OK) {
          record_first((int)ci, "additional self-verify failed");
          g_s.meta_fail++;
        }
        if (snapshot_parse(&v, m->family, &s1) != 0 ||
            !snap_equal(&s0, &s1, 0)) {
          record_first((int)ci, "additional records changed hostent");
          g_s.meta_fail++;
        } else {
          g_s.meta_addr_invariant++;
        }
      }
    }
  }

  printf("COVERAGE cases=%u messages=%u parses_ok=%u "
         "chain_k={1:%u,2:%u,3:%u,4:%u} A=%u AAAA=%u root_cases=%u "
         "root_1byte=%u root_2byte=%u depth={0:%u,1:%u,2:%u,3:%u,4:%u,"
         ">=5:%u}\n",
         N_CASES, g_s.messages, g_s.parse_ok,
         g_s.khist[1], g_s.khist[2], g_s.khist[3], g_s.khist[4],
         g_s.n_a, g_s.n_aaaa, g_s.root_cases,
         g_s.root_one_byte, g_s.root_two_byte,
         g_s.depthhist[0], g_s.depthhist[1], g_s.depthhist[2],
         g_s.depthhist[3], g_s.depthhist[4],
         g_s.depthhist[5] + g_s.depthhist[6] + g_s.depthhist[7] +
         g_s.depthhist[8] + g_s.depthhist[9]);
  printf("ENCLEN checks=%u failures=%u names_exact=%u names_fold=%u "
         "meta_runs=%u meta_invariant=%u meta_fail=%u "
         "size_violations=%u ptr_illegal=%u\n",
         g_s.enclen_checks, g_s.enclen_fail, g_s.name_exact_ok,
         g_s.name_fold_ok, g_s.meta_runs, g_s.meta_addr_invariant,
         g_s.meta_fail, g_s.size_violation, g_s.ptr_illegal);
  if (g_s.first_idx < 0)
    printf("FIRST_DIVERGENCE none\n");
  else
    printf("FIRST_DIVERGENCE case=%d %s\n",
           g_s.first_idx, g_s.first_dev);
}

/* ------------------------------------------------------------------ */
/* Replay: byte-identical regeneration and checksum stability.        */
/* ------------------------------------------------------------------ */
static int model_equal(const struct model *a, const struct model *b)
{
  unsigned i, j;
  if (a->k != b->k || a->family != b->family ||
      a->naddr != b->naddr || a->has_root != b->has_root ||
      a->root_pos != b->root_pos || a->seed != b->seed)
    return 0;
  for (i = 0; i <= a->k; ++i) {
    if (a->name[i].is_root != b->name[i].is_root ||
        a->name[i].nlabels != b->name[i].nlabels)
      return 0;
    for (j = 0; j < a->name[i].nlabels; ++j)
      if (strcmp(a->name[i].label[j], b->name[i].label[j]) != 0)
        return 0;
  }
  for (i = 0; i < a->naddr; ++i) {
    unsigned nbytes = (a->family == T_A) ? 4u : 16u;
    if (memcmp(a->addr[i], b->addr[i], nbytes) != 0)
      return 0;
  }
  return 1;
}

static int replay_byte_identical(void)
{
  struct model m;
  struct encoder a, b;
  unsigned ci, layout;
  rng_state = FIXED_SEED;
  for (ci = 0; ci < N_CASES; ++ci) {
    gen_model(&m, ci);
    /* The regenerated logical model must match the stored one first. */
    if (!model_equal(&m, &g_models[ci]))
      return -1;
    for (layout = 0; layout < 3; ++layout) {
      if (build_message(&g_models[ci], layout, V_BASE, 0, 0, &a) != 0)
        return -2;
      if (build_message(&m, layout, V_BASE, 0, 0, &b) != 0)
        return -2;
      if (a.len != b.len || memcmp(a.buf, b.buf, (size_t)a.len) != 0)
        return -3;
    }
  }
  return 0;
}

static uint64_t replay_checksum(void)
{
  struct model m;
  struct encoder e;
  uint64_t h = FNV_OFFSET_BASIS;
  unsigned ci, layout;
  rng_state = FIXED_SEED;
  for (ci = 0; ci < N_CASES; ++ci) {
    gen_model(&m, ci);
    for (layout = 0; layout < 3; ++layout) {
      if (build_message(&m, layout, V_BASE, 0, 0, &e) != 0)
        return 0;
      h = fnv_update(h, e.buf, (unsigned)e.len);
    }
  }
  return h;
}

/* Truncated messages must be rejected: no crash, no false success. */
static int truncation_check(void)
{
  struct model m;
  struct encoder e;
  unsigned tested = 0;
  int cuts[4];
  unsigned i;

  rng_state = FIXED_SEED;
  gen_model(&m, 0);
  /* Use the flat layout: it carries no additional-section ladder, so
   * every cut lands inside data the A/AAAA parser actually reads. */
  if (build_message(&m, LAYOUT_NONE, V_BASE, 0, 0, &e) != 0)
    return -1;
  cuts[0] = HDR_LEN - 1;
  cuts[1] = HDR_LEN;
  cuts[2] = e.len / 2;
  cuts[3] = e.len - 1;

  for (i = 0; i < 4; ++i) {
    struct hostent *h = NULL;
    int st;
    if (cuts[i] >= e.len)
      continue;
    if (m.family == T_A)
      st = ares_parse_a_reply(e.buf, cuts[i], &h, NULL, NULL);
    else
      st = ares_parse_aaaa_reply(e.buf, cuts[i], &h, NULL, NULL);
    if (h)
      ares_free_hostent(h);
    if (st == ARES_SUCCESS)
      return -1;
    tested++;
    g_s.trunc_rejected++;
  }

  /* expand_name past the buffer end must report ARES_EBADNAME. */
  {
    char *s = NULL;
    long enclen = 0;
    int r = ares_expand_name(e.buf + e.len, e.buf, e.len, &s, &enclen);
    if (s)
      ares_free_string(s);
    if (r != ARES_EBADNAME)
      return -1;
    tested++;
  }
  return tested >= 5 ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* Pre-determined logic-difference simulator.  Only the *expectation*  */
/* held by the oracle is deliberately wrong; library code and messages */
/* are untouched.  Proves the oracle fails loudly and retains the      */
/* first failing case index.                                           */
/* ------------------------------------------------------------------ */
static int difference_simulator(void)
{
  struct model m;
  struct encoder e;
  struct override ov;
  char dev[160];
  int first_seen = -1;
  unsigned ci;
  int rc;

  rng_state = FIXED_SEED;
  for (ci = 0; ci < 10; ++ci) {
    gen_model(&m, ci);
    if (build_message(&m, LAYOUT_SUFFIX, V_BASE, 0, 0, &e) != 0)
      return -1;
    memset(&ov, 0, sizeof ov);
    /* Deterministic injected differences at cases 3 and 9. */
    if (ci == 3 || ci == 9)
      ov.corrupt_addr = 1;
    rc = verify_message(&m, &e, &ov, dev, sizeof dev);
    if (ci == 3 || ci == 9) {
      if (rc != OV_ADDR)
        return -1;                       /* oracle missed the difference */
      if (first_seen < 0)
        first_seen = (int)ci;
    } else {
      if (rc != OV_OK)
        return -2;                       /* false positive                */
    }
  }
  if (first_seen != 3)
    return -3;                           /* first case not retained       */

  /* Second fault class: corrupted canonical-name expectation. */
  rng_state = FIXED_SEED;
  gen_model(&m, 0);
  if (build_message(&m, LAYOUT_NONE, V_BASE, 0, 0, &e) != 0)
    return -1;
  memset(&ov, 0, sizeof ov);
  ov.corrupt_hname = 1;
  rc = verify_message(&m, &e, &ov, dev, sizeof dev);
  if (rc != OV_HNAME)
    return -4;

  printf("DIFF_SIMULATOR injected_addr_faults=2 first_caught=%d "
         "hname_fault=caught oracle_detects=yes\n", first_seen);
  return 0;
}

/* ------------------------------------------------------------------ */
/* Group entry points: each carries at least five live assertions.     */
/* ------------------------------------------------------------------ */
int task_cases(const char *group)
{
  run_pass();

  if (strcmp(group, "model") == 0) {
    /* The independent model explains every parse on all three layouts. */
    TASK_CHECK(g_s.oracle_fail == 0);
    TASK_CHECK(g_s.parse_ok == N_CASES * 3);
    TASK_CHECK(g_s.messages == N_CASES * 3);
    /* Model wire lengths agree with ares_expand_name enclen. */
    TASK_CHECK(g_s.enclen_fail == 0);
    TASK_CHECK(g_s.enclen_checks >= N_CASES * 3 * 3);
    /* Name identity holds both exact and DNS case-folded. */
    TASK_CHECK(g_s.name_exact_ok >= N_CASES * 3 * 2);
    TASK_CHECK(g_s.name_fold_ok >= g_s.name_exact_ok);
    /* Every chain length 1..4 is exercised, no skips. */
    TASK_CHECK(g_s.khist[1] >= 50 && g_s.khist[2] >= 50 &&
               g_s.khist[3] >= 50 && g_s.khist[4] >= 50);
    /* Both families and the root name (both wire forms) are covered. */
    TASK_CHECK(g_s.n_a >= 100 && g_s.n_aaaa >= 100);
    TASK_CHECK(g_s.root_cases >= 20);
    TASK_CHECK(g_s.root_one_byte > 0 && g_s.root_two_byte > 0);
    /* Flat, single-hop and genuine multi-level (depth 3 and 4)
     * compression all occur; the ladder is not allowed to be skipped. */
    TASK_CHECK(g_s.depthhist[0] > 0 && g_s.depthhist[1] > 0 &&
               g_s.depthhist[2] > 0);
    TASK_CHECK(g_s.depthhist[3] >= 100 && g_s.depthhist[4] >= 100);
    return 0;
  }

  if (strcmp(group, "metamorphic") == 0) {
    /* No legal representation change broke model-vs-library agreement. */
    TASK_CHECK(g_s.meta_fail == 0);
    /* 3 order slots + 1 case flip + 1 addition, per case per layout. */
    TASK_CHECK(g_s.meta_runs == N_CASES * 3 * 5);
    /* Every transform preserved the address set and ordering. */
    TASK_CHECK(g_s.meta_addr_invariant == g_s.meta_runs);
    /* The case-fold identity path is genuinely exercised. */
    TASK_CHECK(g_s.name_fold_ok > g_s.name_exact_ok);
    /* Unrelated records never leaked into question/answer identity. */
    TASK_CHECK(g_s.parse_ok == N_CASES * 3);
    /* Root names and both record families survive the transforms. */
    TASK_CHECK(g_s.root_cases >= 20);
    TASK_CHECK(g_s.n_a >= 100 && g_s.n_aaaa >= 100);
    return 0;
  }

  if (strcmp(group, "replay") == 0) {
    /* Fixed seed regenerates byte-identical messages. */
    TASK_CHECK(replay_byte_identical() == 0);
    /* Whole-pass FNV checksum is stable across replay. */
    TASK_CHECK(replay_checksum() == g_s.checksum);
    /* Message and name length limits held for every generated message. */
    TASK_CHECK(g_s.size_violation == 0);
    /* Every pointer targeted an earlier label boundary. */
    TASK_CHECK(g_s.ptr_illegal == 0);
    TASK_CHECK(g_s.messages == N_CASES * 3);
    /* Truncated / out-of-range inputs are rejected, not mis-parsed. */
    TASK_CHECK(truncation_check() == 0);
    TASK_CHECK(g_s.trunc_rejected >= 4);
    /* Oracle catches injected logic differences and keeps first case. */
    TASK_CHECK(difference_simulator() == 0);
    /* Coverage is real: nothing was skipped past. */
    TASK_CHECK(g_s.oracle_fail == 0 && g_s.meta_fail == 0);
    return 0;
  }

  return 1;
}
