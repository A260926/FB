/*
 * Pattern model, independent oracle and deterministic generator for the
 * hex-string correctness regression. See hex_model.h for the supported
 * surface. The oracle works solely from these nodes plus the raw input
 * bytes; it never consults YARA's parser, AST, atoms, bytecode or scanner.
 */

#include "hex_model.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// ---- small string appender ----------------------------------------------

typedef struct {
  char* buf;
  size_t size;
  size_t pos;
} hbuf;

static void hbuf_init(hbuf* h, char* buf, size_t size) {
  h->buf = buf;
  h->size = size;
  h->pos = 0;
  if (size > 0) buf[0] = '\0';
}

static void hfmt(hbuf* h, const char* fmt, ...) {
  if (h->pos + 1 >= h->size) return;
  va_list ap;
  va_start(ap, fmt);
  int w = vsnprintf(h->buf + h->pos, h->size - h->pos, fmt, ap);
  va_end(ap);
  if (w < 0) return;
  size_t u = (size_t) w;
  h->pos += (u < h->size - h->pos) ? u : (h->size - 1 - h->pos);
}

// ---- pool ---------------------------------------------------------------

void hm_pattern_init(hm_pattern* p) {
  memset(p, 0, sizeof(*p));
}

hm_node* hm_new_node(hm_pattern* p, hm_type type) {
  if (p->pool_used >= HM_POOL_SIZE) return NULL;
  hm_node* n = &p->pool[p->pool_used++];
  memset(n, 0, sizeof(*n));
  n->type = type;
  return n;
}

hm_node* hm_byte(hm_pattern* p, uint8_t value, uint8_t mask, bool neg) {
  hm_node* n = hm_new_node(p, HM_BYTE);
  if (n == NULL) return NULL;
  n->value = value;
  n->mask = mask;
  n->neg = neg;
  return n;
}

hm_node* hm_jump(hm_pattern* p, int lo, int hi) {
  hm_node* n = hm_new_node(p, HM_JUMP);
  if (n == NULL) return NULL;
  n->lo = lo;
  n->hi = hi;
  return n;
}

hm_node* hm_concat(hm_pattern* p) {
  return hm_new_node(p, HM_CONCAT);
}

hm_node* hm_alt(hm_pattern* p) {
  return hm_new_node(p, HM_ALT);
}

void hm_kid(hm_node* parent, hm_node* child) {
  if (parent->n < HM_MAX_NODES) parent->kids[parent->n++] = child;
}

// ---- fixed width / lengths ----------------------------------------------

static int node_min_len(const hm_node* n) {
  switch (n->type) {
    case HM_BYTE:
      return 1;
    case HM_JUMP:
      return n->lo;
    case HM_ALT: {
      int m = node_min_len(n->kids[0]);
      for (int i = 1; i < n->n; i++) {
        int v = node_min_len(n->kids[i]);
        if (v < m) m = v;
      }
      return m;
    }
    case HM_CONCAT: {
      int s = 0;
      for (int i = 0; i < n->n; i++) s += node_min_len(n->kids[i]);
      return s;
    }
  }
  return 0;
}

// Fixed width in bytes, or -1 when the node contains a jump.
static int fixed_width(const hm_node* n) {
  switch (n->type) {
    case HM_BYTE:
      return 1;
    case HM_JUMP:
      return -1;
    case HM_ALT:
    case HM_CONCAT: {
      int s = 0;
      for (int i = 0; i < n->n; i++) {
        int w = fixed_width(n->kids[i]);
        if (w < 0) return -1;
        s += w;
      }
      return s;
    }
  }
  return -1;
}

// ---- validation ----------------------------------------------------------

static bool vfail(char* err, size_t err_size, const char* fmt, ...) {
  if (err != NULL && err_size > 0) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, err_size, fmt, ap);
    va_end(ap);
  }
  return false;
}

static int count_nodes(const hm_node* n) {
  int c = 1;
  for (int i = 0; i < n->n; i++) c += count_nodes(n->kids[i]);
  return c;
}

static int tree_depth(const hm_node* n) {
  int d = (n->type == HM_CONCAT || n->type == HM_ALT) ? 1 : 0;
  int best = 0;
  for (int i = 0; i < n->n; i++) {
    int c = tree_depth(n->kids[i]);
    if (c > best) best = c;
  }
  return d + best;
}

// Depth counts container nesting (CONCAT/ALT), matching tree_depth().
static bool validate_node(const hm_node* n, int level, bool jumps_allowed,
                          int* jump_count, char* err, size_t err_size) {
  bool is_container = (n->type == HM_CONCAT || n->type == HM_ALT);
  if (is_container && level > HM_MAX_DEPTH)
    return vfail(err, err_size, "nesting depth %d exceeds %d", level,
                 HM_MAX_DEPTH);

  switch (n->type) {
    case HM_BYTE:
      // ~?? has no meaning in the hex grammar and is never generated.
      if (n->neg && n->mask == 0x00)
        return vfail(err, err_size, "negated wildcard ~?? is not supported");
      return true;

    case HM_JUMP:
      if (!jumps_allowed)
        return vfail(err, err_size,
                     "jumps are only allowed in the outer concatenation");
      if (n->lo < 0 || n->hi < 0 || n->lo > n->hi)
        return vfail(err, err_size, "invalid jump range [%d-%d]", n->lo, n->hi);
      if (n->hi > HM_MAX_JUMP_END)
        return vfail(err, err_size, "jump end %d exceeds %d", n->hi,
                     HM_MAX_JUMP_END);
      (*jump_count)++;
      if (*jump_count > HM_MAX_JUMPS)
        return vfail(err, err_size, "more than %d jumps", HM_MAX_JUMPS);
      return true;

    case HM_ALT: {
      if (n->n < 2)
        return vfail(err, err_size, "alternation needs at least two branches");
      int w = fixed_width(n->kids[0]);
      if (w <= 0)
        return vfail(err, err_size,
                     "alternation branches must be fixed-width and non-empty");
      for (int i = 1; i < n->n; i++) {
        int wi = fixed_width(n->kids[i]);
        if (wi != w)
          return vfail(err, err_size,
                       "alternation branches have unequal widths %d and %d", w,
                       wi);
      }
      for (int i = 0; i < n->n; i++)
        if (!validate_node(n->kids[i], level + 1, false, jump_count, err,
                           err_size))
          return false;
      return true;
    }
    case HM_CONCAT:
      if (n->n < 1) return vfail(err, err_size, "empty concatenation");
      // Nested concats only appear below an alternation; jumps there are
      // illegal, so children never receive jump permission.
      for (int i = 0; i < n->n; i++)
        if (!validate_node(n->kids[i], level + 1, false, jump_count, err,
                           err_size))
          return false;
      return true;
  }
  return vfail(err, err_size, "unknown node type %d", (int) n->type);
}

bool hm_validate(const hm_pattern* p, char* err, size_t err_size) {
  if (err != NULL && err_size > 0) err[0] = '\0';
  if (p->root == NULL) return vfail(err, err_size, "empty pattern");

  if (count_nodes(p->root) > HM_MAX_NODES)
    return vfail(err, err_size, "pattern has more than %d nodes", HM_MAX_NODES);
  if (tree_depth(p->root) > HM_MAX_DEPTH)
    return vfail(err, err_size, "nesting depth exceeds %d", HM_MAX_DEPTH);

  int jump_count = 0;
  bool root_concat = (p->root->type == HM_CONCAT);
  if (!root_concat) {
    if (!validate_node(p->root, 1, false, &jump_count, err, err_size))
      return false;
  } else {
    if (p->root->n < 1) return vfail(err, err_size, "empty concatenation");
    if (p->root->kids[0]->type == HM_JUMP)
      return vfail(err, err_size, "hex string cannot start with a jump");
    if (p->root->kids[p->root->n - 1]->type == HM_JUMP)
      return vfail(err, err_size, "hex string cannot end with a jump");
    for (int i = 0; i < p->root->n; i++)
      if (!validate_node(p->root->kids[i], 2, true, &jump_count, err,
                         err_size))
        return false;
  }
  return true;
}

// ---- rendering -----------------------------------------------------------

static void render_node(const hm_node* n, hbuf* h) {
  switch (n->type) {
    case HM_BYTE: {
      if (n->neg) hfmt(h, "~");
      if (n->mask == 0xFF) {
        hfmt(h, "%02X", n->value);
      } else if (n->mask == 0x00) {
        hfmt(h, "??");
      } else if (n->mask == 0xF0) {
        hfmt(h, "%X?", (n->value >> 4) & 0x0F);
      } else if (n->mask == 0x0F) {
        hfmt(h, "?%X", n->value & 0x0F);
      }
      break;
    }
    case HM_JUMP:
      hfmt(h, "[%d-%d]", n->lo, n->hi);
      break;
    case HM_ALT:
      hfmt(h, "( ");
      for (int i = 0; i < n->n; i++) {
        if (i > 0) hfmt(h, " | ");
        render_node(n->kids[i], h);
      }
      hfmt(h, " )");
      break;
    case HM_CONCAT:
      for (int i = 0; i < n->n; i++) {
        if (i > 0) hfmt(h, " ");
        render_node(n->kids[i], h);
      }
      break;
  }
}

void hm_render_hex(const hm_pattern* p, char* out, size_t out_size) {
  hbuf h;
  hbuf_init(&h, out, out_size);
  hfmt(&h, "{ ");
  render_node(p->root, &h);
  hfmt(&h, " }");
}

void hm_render_rule(const hm_pattern* p, char* out, size_t out_size) {
  char hex[HM_HEX_MAX];
  hm_render_hex(p, hex, sizeof(hex));
  snprintf(out, out_size,
           "rule hex_model_test {\n strings:\n $h = %s\n"
           " condition:\n #h >= 0\n}\n",
           hex);
}

static void dump_node(const hm_node* n, int indent, hbuf* h) {
  for (int i = 0; i < indent; i++) hfmt(h, "  ");
  switch (n->type) {
    case HM_BYTE:
      hfmt(h, "BYTE value=0x%02X mask=0x%02X neg=%d\n", n->value, n->mask,
           (int) n->neg);
      break;
    case HM_JUMP:
      hfmt(h, "JUMP [%d-%d]\n", n->lo, n->hi);
      break;
    case HM_ALT:
      hfmt(h, "ALT branches=%d width=%d\n", n->n, fixed_width(n));
      for (int i = 0; i < n->n; i++) dump_node(n->kids[i], indent + 1, h);
      break;
    case HM_CONCAT:
      hfmt(h, "CONCAT kids=%d\n", n->n);
      for (int i = 0; i < n->n; i++) dump_node(n->kids[i], indent + 1, h);
      break;
  }
}

void hm_dump(const hm_pattern* p, char* out, size_t out_size) {
  hbuf h;
  hbuf_init(&h, out, out_size);
  hfmt(&h, "pattern nodes=%d depth=%d:\n", count_nodes(p->root),
       tree_depth(p->root));
  dump_node(p->root, 0, &h);
}

// ---- RNG (splitmix64) ----------------------------------------------------

void hm_rng_seed(hm_rng* r, uint64_t seed) {
  r->state = seed;
}

uint64_t hm_rand64(hm_rng* r) {
  uint64_t z = (r->state += 0x9E3779B97F4A7C15ULL);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

int hm_rand_range(hm_rng* r, int lo, int hi) {
  if (hi <= lo) return lo;
  return lo + (int) (hm_rand64(r) % (uint64_t) (hi - lo + 1));
}

// ---- reference oracle ----------------------------------------------------

static bool byte_matches(const hm_node* n, uint8_t b) {
  bool hit = (b & n->mask) == n->value;
  return n->neg ? !hit : hit;
}

// Union of end positions reachable when consuming node starting from any
// position in starts[0..sn-1]. Writes sorted unique positions into out[].
static int reach_set(const hm_node* node, const uint8_t* data, int data_len,
                     const int* starts, int sn, int* out, int out_cap) {
  bool mark[HM_MAX_DATA + 1];
  memset(mark, 0, sizeof(mark));
  int count = 0;

  for (int k = 0; k < sn; k++) {
    int s = starts[k];
    switch (node->type) {
      case HM_BYTE:
        if (s < data_len && byte_matches(node, data[s]) && !mark[s + 1]) {
          mark[s + 1] = true;
        }
        break;
      case HM_JUMP:
        for (int e = s + node->lo; e <= s + node->hi && e <= data_len; e++) {
          if (!mark[e]) mark[e] = true;
        }
        break;
      case HM_ALT:
        for (int i = 0; i < node->n; i++) {
          int tmp[HM_MAX_DATA + 1];
          int tn = reach_set(node->kids[i], data, data_len, starts, sn, tmp,
                             HM_MAX_DATA + 1);
          for (int j = 0; j < tn; j++)
            if (!mark[tmp[j]]) mark[tmp[j]] = true;
        }
        break;
      case HM_CONCAT: {
        int cur[HM_MAX_DATA + 1];
        int cn = sn;
        memcpy(cur, starts, sizeof(int) * (size_t) sn);
        for (int i = 0; i < node->n; i++) {
          int next[HM_MAX_DATA + 1];
          int nn = reach_set(node->kids[i], data, data_len, cur, cn, next,
                             HM_MAX_DATA + 1);
          cn = nn;
          memcpy(cur, next, sizeof(int) * (size_t) cn);
          if (cn == 0) break;
        }
        for (int j = 0; j < cn; j++)
          if (!mark[cur[j]]) mark[cur[j]] = true;
        break;
      }
    }
  }

  for (int e = 0; e <= data_len && count < out_cap; e++) {
    if (mark[e]) out[count++] = e;
  }
  return count;
}

int hm_reachable(const hm_node* node, const uint8_t* data, int data_len,
                 int start, int* ends, int ends_cap) {
  int s = start;
  return reach_set(node, data, data_len, &s, 1, ends, ends_cap);
}

int hm_expected_matches(const hm_pattern* p, const uint8_t* data, int data_len,
                        hm_match* out, int out_cap) {
  int n = 0;
  for (int s = 0; s < data_len && n < out_cap; s++) {
    int ends[HM_MAX_DATA + 1];
    int en = hm_reachable(p->root, data, data_len, s, ends,
                          HM_MAX_DATA + 1);
    if (en == 0) continue;
    // ends[] is sorted; the first entry is the shortest complete derivation.
    out[n].start = s;
    out[n].length = ends[0] - s;
    n++;
  }
  return n;
}

void hm_case_prepare(hm_case* c, const char* name) {
  c->name = name;
  hm_render_hex(&c->pattern, c->hex, sizeof(c->hex));
  hm_render_rule(&c->pattern, c->rule, sizeof(c->rule));
  c->expected_count =
      hm_expected_matches(&c->pattern, c->data, c->data_len, c->expected,
                          HM_MAX_DATA);
}

// ---- pattern generator ---------------------------------------------------

static hm_node* gen_byte(hm_rng* r, hm_pattern* p) {
  int kind = hm_rand_range(r, 0, 5);
  switch (kind) {
    case 0:
      return hm_byte(p, (uint8_t) hm_rand_range(r, 0, 255), 0xFF, false);
    case 1:
      return hm_byte(p, 0x00, 0x00, false);
    case 2:
      return hm_byte(p, (uint8_t) (hm_rand_range(r, 0, 15) << 4), 0xF0, false);
    case 3:
      return hm_byte(p, (uint8_t) hm_rand_range(r, 0, 15), 0x0F, false);
    case 4:
      return hm_byte(p, (uint8_t) hm_rand_range(r, 0, 255), 0xFF, true);
    default: {
      bool high = hm_rand_range(r, 0, 1) == 0;
      if (high)
        return hm_byte(p, (uint8_t) (hm_rand_range(r, 0, 15) << 4), 0xF0, true);
      return hm_byte(p, (uint8_t) hm_rand_range(r, 0, 15), 0x0F, true);
    }
  }
}

static hm_node* gen_jump(hm_rng* r, hm_pattern* p) {
  int lo, hi;
  if (hm_rand_range(r, 0, 2) == 0) {
    // Bias towards the chaining threshold boundary.
    static const int spots[] = {198, 199, 200, 201, 202};
    hi = spots[hm_rand_range(r, 0, 4)];
    lo = spots[hm_rand_range(r, 0, 4)];
    if (lo > hi) {
      int t = lo;
      lo = hi;
      hi = t;
    }
  } else {
    lo = hm_rand_range(r, 0, 5);
    hi = lo + hm_rand_range(r, 0, 5);
    if (hi > HM_MAX_JUMP_END) hi = HM_MAX_JUMP_END;
  }
  return hm_jump(p, lo, hi);
}

static hm_node* gen_alt(hm_rng* r, hm_pattern* p) {
  hm_node* alt = hm_alt(p);
  int width = hm_rand_range(r, 1, 3);
  int branches = hm_rand_range(r, 2, HM_MAX_ALT);
  for (int b = 0; b < branches; b++) {
    hm_node* branch = hm_concat(p);
    for (int w = 0; w < width; w++) hm_kid(branch, gen_byte(r, p));
    hm_kid(alt, branch);
  }
  return alt;
}

void hm_generate_pattern(hm_rng* r, hm_pattern* p) {
  for (int attempt = 0; attempt < 100; attempt++) {
    hm_pattern_init(p);
    hm_node* root = hm_concat(p);
    p->root = root;

    int segments = hm_rand_range(r, 3, 6);
    // Generated patterns carry at most one jump. A jump that crosses the
    // threshold splits the chain into two fixed-width fragments, and a
    // non-splitting jump stays inside a single RE-engine string; either way
    // every chain fragment has fixed width. Two-jump patterns exercise the
    // variable-length-fragment surface and are kept in the pinned/fixed set.
    int slots = segments - 2;
    int njumps = slots > 0 ? hm_rand_range(r, 0, 1) : 0;
    bool is_jump[HM_MAX_NODES] = {false};
    int placed = 0;
    int guard = 0;
    while (placed < njumps && guard++ < 100) {
      int pos = hm_rand_range(r, 1, segments - 2);  // never first or last
      if (!is_jump[pos]) {
        is_jump[pos] = true;
        placed++;
      }
    }

    for (int i = 0; i < segments; i++) {
      if (is_jump[i]) {
        hm_kid(root, gen_jump(r, p));
      } else if (hm_rand_range(r, 0, 2) == 0) {
        hm_kid(root, gen_alt(r, p));
      } else {
        hm_kid(root, gen_byte(r, p));
      }
    }

    char err[HM_ERR_MAX];
    if (hm_validate(p, err, sizeof(err))) return;
  }

  // Deterministic fallback: a simple valid pattern.
  hm_pattern_init(p);
  hm_node* root = hm_concat(p);
  p->root = root;
  hm_kid(root, hm_byte(p, 0x41, 0xFF, false));
  hm_kid(root, hm_jump(p, 1, 3));
  hm_kid(root, hm_byte(p, 0x42, 0xFF, false));
}

// ---- data planting -------------------------------------------------------

static uint8_t satisfying_byte(hm_rng* r, const hm_node* n) {
  uint8_t b;
  if (n->mask == 0x00) return (uint8_t) hm_rand_range(r, 0, 255);
  if (!n->neg) {
    b = (uint8_t) hm_rand_range(r, 0, 255);
    b = (b & (uint8_t) ~n->mask) | n->value;
    return b;
  }
  // Negated: retry, then force a masked bit to flip so the predicate fails.
  for (int t = 0; t < 64; t++) {
    b = (uint8_t) hm_rand_range(r, 0, 255);
    if (byte_matches(n, b)) return b;
  }
  b = (uint8_t) hm_rand_range(r, 0, 255);
  if (!byte_matches(n, b)) b ^= n->mask;
  return b;
}

static int plant_node(hm_rng* r, const hm_node* n, uint8_t* data, int pos) {
  switch (n->type) {
    case HM_BYTE:
      data[pos] = satisfying_byte(r, n);
      return pos + 1;
    case HM_JUMP: {
      int k = hm_rand_range(r, n->lo, n->hi);
      for (int i = 0; i < k; i++)
        data[pos + i] = (uint8_t) hm_rand_range(r, 0, 255);
      return pos + k;
    }
    case HM_ALT:
      return plant_node(r, n->kids[hm_rand_range(r, 0, n->n - 1)], data, pos);
    case HM_CONCAT:
      for (int i = 0; i < n->n; i++)
        pos = plant_node(r, n->kids[i], data, pos);
      return pos;
  }
  return pos;
}

int hm_plant_data(hm_rng* r, const hm_pattern* p, uint8_t* data, int max_len) {
  int min_len = node_min_len(p->root);
  if (min_len <= 0 || min_len > max_len) return -1;

  int room = max_len - min_len;
  int total = min_len + (room > 0 ? hm_rand_range(r, 0, room) : 0);
  int prefix = (total > min_len) ? hm_rand_range(r, 0, total - min_len) : 0;

  for (int i = 0; i < total; i++)
    data[i] = (uint8_t) hm_rand_range(r, 0, 255);

  int end = plant_node(r, p->root, data, prefix);
  if (end > total) return -1;
  (void) end;
  return total;
}

int hm_random_data(hm_rng* r, uint8_t* data, int max_len) {
  int len = hm_rand_range(r, 1, max_len);
  for (int i = 0; i < len; i++)
    data[i] = (uint8_t) hm_rand_range(r, 0, 255);
  return len;
}

// ---- explicit rejection probes ------------------------------------------

int hm_invalid_count(void) {
  return 8;
}

// Builds one deliberately unsupported pattern per index. Every one of them
// must be refused by hm_validate and must never reach the scanner.
int hm_build_invalid(hm_pattern* p, int index) {
  hm_pattern_init(p);
  hm_node* root = hm_concat(p);
  p->root = root;

  switch (index) {
    case 0:  // jump at the start
      hm_kid(root, hm_jump(p, 2, 3));
      hm_kid(root, hm_byte(p, 0x41, 0xFF, false));
      break;
    case 1:  // jump at the end
      hm_kid(root, hm_byte(p, 0x41, 0xFF, false));
      hm_kid(root, hm_jump(p, 2, 3));
      break;
    case 2:  // ~??
      hm_kid(root, hm_byte(p, 0x00, 0x00, true));
      hm_kid(root, hm_byte(p, 0x42, 0xFF, false));
      break;
    case 3: {  // jump nested inside an alternation
      hm_node* alt = hm_alt(p);
      hm_node* b1 = hm_concat(p);
      hm_kid(b1, hm_byte(p, 0x41, 0xFF, false));
      hm_kid(b1, hm_jump(p, 2, 3));
      hm_kid(b1, hm_byte(p, 0x42, 0xFF, false));
      hm_node* b2 = hm_concat(p);
      hm_kid(b2, hm_byte(p, 0x43, 0xFF, false));
      hm_kid(b2, hm_byte(p, 0x44, 0xFF, false));
      hm_kid(b2, hm_byte(p, 0x45, 0xFF, false));
      hm_kid(alt, b1);
      hm_kid(alt, b2);
      hm_kid(root, alt);
      hm_kid(root, hm_byte(p, 0x46, 0xFF, false));
      break;
    }
    case 4: {  // unequal alternation widths
      hm_node* alt = hm_alt(p);
      hm_node* b1 = hm_concat(p);
      hm_kid(b1, hm_byte(p, 0x41, 0xFF, false));
      hm_node* b2 = hm_concat(p);
      hm_kid(b2, hm_byte(p, 0x42, 0xFF, false));
      hm_kid(b2, hm_byte(p, 0x43, 0xFF, false));
      hm_kid(alt, b1);
      hm_kid(alt, b2);
      hm_kid(root, alt);
      hm_kid(root, hm_byte(p, 0x44, 0xFF, false));
      break;
    }
    case 5:  // jump endpoint past 202
      hm_kid(root, hm_byte(p, 0x41, 0xFF, false));
      hm_kid(root, hm_jump(p, 203, 203));
      hm_kid(root, hm_byte(p, 0x42, 0xFF, false));
      break;
    case 6:  // three jumps
      hm_kid(root, hm_byte(p, 0x41, 0xFF, false));
      hm_kid(root, hm_jump(p, 1, 2));
      hm_kid(root, hm_byte(p, 0x42, 0xFF, false));
      hm_kid(root, hm_jump(p, 1, 2));
      hm_kid(root, hm_byte(p, 0x43, 0xFF, false));
      hm_kid(root, hm_jump(p, 1, 2));
      hm_kid(root, hm_byte(p, 0x44, 0xFF, false));
      break;
    case 7: {  // too many nodes: 25 bytes
      for (int i = 0; i < 25; i++)
        hm_kid(root, hm_byte(p, (uint8_t) (0x40 + i), 0xFF, false));
      break;
    }
    default:
      return 0;
  }
  return 1;
}
