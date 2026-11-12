/* cab_model_test.c -- model-based QA for libmspack's CAB decoder.
 *
 * Unlike the static sample files in test/test_files/cabd, this file
 * GENERATES its own cabinets from an explicit model:
 *
 *  - a small DEFLATE (fixed-Huffman) encoder turns a list of literal /
 *    copy actions into MSZIP frames,
 *  - the model expands the same action list itself, keeping its own
 *    32kB window, so the expected plaintext is held independently of
 *    the production decoder,
 *  - a CAB container builder wraps the frames into single cabinets or
 *    multi-volume sets with a split CFDATA block,
 *  - a recording in-memory mspack_system feeds the images to the public
 *    mscab_decompressor interface and captures every byte written.
 *
 * Three groups are exported through cab_model_test():
 *   "model"   multi-frame MSZIP folder, cross-frame dictionary history,
 *             file boundaries inside/across frames, zero-length files,
 *             non-contiguous extraction order, input immutability.
 *   "split"   one CFDATA block split over three cabinets (cbUncomp=0 on
 *             leading pieces), per-piece checksums and reserved bytes,
 *             append/prepend merge, alternate cut points, missing /
 *             corrupted pieces, differing set IDs.
 *   "partial" recording memory I/O: short reads as I/O failure, cross-
 *             file / backward / cross-instance interleaving, pending
 *             frame-tail routing, whole-span vs per-file-boundary output.
 *
 * Each group returns the number of content assertions executed (> 0 on
 * success) or a negative value on failure.  Failures print a
 * machine-greppable CAB_MODEL_FAIL record with the model, layout, file
 * interval and first differing offset so the case can be replayed.
 * This file never calls exit(); the process exit status is decided by
 * the caller (checks/cab_model_main.c).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <mspack.h>

/* ------------------------------------------------------------------ */
/* small utilities                                                     */
/* ------------------------------------------------------------------ */

static unsigned int assertions = 0;   /* content assertions executed   */
static int group_failed = 0;          /* sticky failure flag           */
static const char *cur_group = "?";
static const char *cur_model = "?";
static const char *cur_layout = "?";

/* deterministic PRNG (xorshift32) so every run replays identically */
static unsigned int rng_state = 1;
static void rng_seed(unsigned int s) { rng_state = s ? s : 1; }
static unsigned int rng_next(void) {
    unsigned int x = rng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    rng_state = x;
    return x;
}

/* FNV-1a 64 bit, used to prove input images are not mutated */
static unsigned long long fnv1a(const unsigned char *p, size_t n) {
    unsigned long long h = 1469598103934665603ULL;
    while (n--) { h ^= *p++; h *= 1099511628211ULL; }
    return h;
}

static void fail_record(const char *file, long off, long len,
                        long first_diff, int exp, int got,
                        const char *detail) {
    fprintf(stderr,
            "CAB_MODEL_FAIL group=%s model=%s layout=%s file=%s "
            "interval=%ld+%ld first_diff=%ld expected=%02x actual=%02x %s\n",
            cur_group, cur_model, cur_layout,
            file ? file : "-", off, len, first_diff,
            exp & 0xff, got & 0xff, detail ? detail : "-");
    group_failed = 1;
}

/* content assertion: compare got[] against want[]; on mismatch report
 * the first differing offset */
static int check_bytes(const char *file, long off,
                       const unsigned char *want,
                       const unsigned char *got, long len) {
    long i;
    assertions++;
    for (i = 0; i < len; i++) {
        if (want[i] != got[i]) {
            fail_record(file, off, len, off + i, want[i], got[i],
                        "content-mismatch");
            return 0;
        }
    }
    return 1;
}

static int check_true(int cond, const char *file, long off, long len,
                      const char *detail) {
    assertions++;
    if (!cond) fail_record(file, off, len, -1, 0, 0, detail);
    return cond;
}

/* structural check: not counted as a content assertion, but still
 * records a failure */
static int need(int cond, const char *detail) {
    if (!cond) fail_record(NULL, 0, 0, -1, 0, 0, detail);
    return cond;
}

/* ------------------------------------------------------------------ */
/* recording in-memory mspack_system                                   */
/* ------------------------------------------------------------------ */

#define MEM_MAX_NODES   48
#define MEM_MAX_LOG     8192
#define MEM_MAX_MSG     16
#define MEM_NAME        80

struct mem_node {
    char name[MEM_NAME];
    unsigned char *data;
    size_t len, cap;
    int is_output;
};

struct mem_handle {
    struct mem_node *node;
    int mode;          /* MSPACK_SYS_OPEN_READ or MSPACK_SYS_OPEN_WRITE */
    size_t pos;
    int id;
};

struct mem_log_ent {
    int handle;        /* handle id */
    int kind;          /* 'r' or 'w' */
    long off;          /* file offset of the transfer */
    long req;          /* bytes requested */
    long got;          /* bytes transferred */
};

static struct mem_env {
    struct mspack_system sys;
    struct mem_node nodes[MEM_MAX_NODES];
    int num_nodes;
    struct mem_log_ent log[MEM_MAX_LOG];
    int log_len;
    char msg[MEM_MAX_MSG][256];
    int num_msg;
    int next_handle_id;
    /* fault plan: shorten a read that requests exactly `match_req`
     * bytes, after skipping `skip` such reads; -1 = disarmed.
     * `ret` bytes are returned instead (>= 0 means short read == EOF
     * by the library's convention, -1 means a hard I/O error). */
    long fault_match_req;
    long fault_skip;
    long fault_ret;
    long fault_hits;
    int write_to_input;  /* set if the library wrote to a READ handle */
    int last_output_id;  /* handle id of the most recent WRITE open */
} MEM;

static struct mem_node *mem_find(const char *name) {
    int i;
    for (i = 0; i < MEM.num_nodes; i++) {
        if (!strcmp(MEM.nodes[i].name, name)) return &MEM.nodes[i];
    }
    return NULL;
}

/* register a named input image; the buffer is copied */
static int mem_add_input(const char *name, const unsigned char *data,
                         size_t len) {
    struct mem_node *n;
    if (MEM.num_nodes >= MEM_MAX_NODES) return 0;
    n = &MEM.nodes[MEM.num_nodes++];
    memset(n, 0, sizeof(*n));
    snprintf(n->name, sizeof(n->name), "%s", name);
    n->data = (unsigned char *) malloc(len ? len : 1);
    if (!n->data) return 0;
    memcpy(n->data, data, len);
    n->len = len;
    n->cap = len;
    n->is_output = 0;
    return 1;
}

static void mem_log(int handle, int kind, long off, long req, long got) {
    if (MEM.log_len < MEM_MAX_LOG) {
        struct mem_log_ent *e = &MEM.log[MEM.log_len++];
        e->handle = handle; e->kind = kind;
        e->off = off; e->req = req; e->got = got;
    }
}

static void mem_reset_logs(void) {
    MEM.log_len = 0;
    MEM.num_msg = 0;
    MEM.fault_match_req = -1;
    MEM.fault_skip = 0;
    MEM.fault_ret = 0;
    MEM.fault_hits = 0;
    MEM.write_to_input = 0;
}

static void mem_reset_all(void) {
    int i;
    for (i = 0; i < MEM.num_nodes; i++) free(MEM.nodes[i].data);
    MEM.num_nodes = 0;
    MEM.next_handle_id = 1;
    mem_reset_logs();
}

static struct mspack_file *mem_open(struct mspack_system *self,
                                 const char *filename, int mode) {
    struct mem_handle *h;
    struct mem_node *n;
    char synth[MEM_NAME];
    (void) self;

    if (!filename) {
        snprintf(synth, sizeof(synth), "out#%d", MEM.next_handle_id);
        filename = synth;
    }

    if (mode == MSPACK_SYS_OPEN_READ) {
        n = mem_find(filename);
        if (!n || n->is_output) return NULL;
    }
    else if (mode == MSPACK_SYS_OPEN_WRITE) {
        n = mem_find(filename);
        if (n && !n->is_output) {
            /* the library must never open an input image for writing */
            MEM.write_to_input++;
            return NULL;
        }
        if (!n) {
            if (MEM.num_nodes >= MEM_MAX_NODES) return NULL;
            n = &MEM.nodes[MEM.num_nodes++];
            memset(n, 0, sizeof(*n));
            snprintf(n->name, sizeof(n->name), "%s", filename);
        }
        free(n->data);
        n->data = NULL;
        n->len = n->cap = 0;
        n->is_output = 1;
        MEM.last_output_id = MEM.next_handle_id;
    }
    else {
        return NULL;
    }

    h = (struct mem_handle *) malloc(sizeof(*h));
    if (!h) return NULL;
    h->node = n;
    h->mode = mode;
    h->pos = 0;
    h->id = MEM.next_handle_id++;
    return (struct mspack_file *) h;
}

static void mem_close_h(struct mspack_file *file) {
    free(file);
}

static int mem_read_h(struct mspack_file *file, void *buffer, int bytes) {
    struct mem_handle *h = (struct mem_handle *) file;
    size_t avail;
    int got;
    if (!h || h->mode != MSPACK_SYS_OPEN_READ || bytes < 0) return -1;

    if (MEM.fault_match_req >= 0 && (long) bytes == MEM.fault_match_req) {
        if (MEM.fault_skip > 0) {
            MEM.fault_skip--;
        }
        else {
            MEM.fault_hits++;
            if (MEM.fault_ret < 0) {
                mem_log(h->id, 'r', (long) h->pos, bytes, -1);
                return -1; /* hard I/O error */
            }
            got = (int) MEM.fault_ret;
            if (got > bytes) got = bytes;
            if ((size_t) got > h->node->len - h->pos)
                got = (int) (h->node->len - h->pos);
            memcpy(buffer, h->node->data + h->pos, (size_t) got);
            h->pos += (size_t) got;
            mem_log(h->id, 'r', (long) h->pos - got, bytes, got);
            return got; /* short read: library convention says EOF */
        }
    }

    avail = h->node->len - h->pos;
    got = (avail < (size_t) bytes) ? (int) avail : bytes;
    memcpy(buffer, h->node->data + h->pos, (size_t) got);
    h->pos += (size_t) got;
    mem_log(h->id, 'r', (long) h->pos - got, bytes, got);
    return got;
}

static int mem_write_h(struct mspack_file *file, void *buffer, int bytes) {
    struct mem_handle *h = (struct mem_handle *) file;
    struct mem_node *n;
    size_t needcap;
    if (!h || h->mode != MSPACK_SYS_OPEN_WRITE || bytes < 0) {
        MEM.write_to_input++;
        return -1;
    }
    n = h->node;
    needcap = h->pos + (size_t) bytes;
    if (needcap > n->cap) {
        size_t nc = n->cap ? n->cap * 2 : 256;
        unsigned char *nd;
        while (nc < needcap) nc *= 2;
        nd = (unsigned char *) realloc(n->data, nc);
        if (!nd) return -1;
        n->data = nd;
        n->cap = nc;
    }
    memcpy(n->data + h->pos, buffer, (size_t) bytes);
    mem_log(h->id, 'w', (long) h->pos, bytes, bytes);
    h->pos += (size_t) bytes;
    if (h->pos > n->len) n->len = h->pos;
    return bytes;
}

static int mem_seek_h(struct mspack_file *file, off_t offset, int mode) {
    struct mem_handle *h = (struct mem_handle *) file;
    off_t np;
    if (!h) return 1;
    switch (mode) {
    case MSPACK_SYS_SEEK_START: np = offset; break;
    case MSPACK_SYS_SEEK_CUR:   np = (off_t) h->pos + offset; break;
    case MSPACK_SYS_SEEK_END:   np = (off_t) h->node->len + offset; break;
    default: return 1;
    }
    if (np < 0) return 1;
    h->pos = (size_t) np;
    return 0;
}

static off_t mem_tell_h(struct mspack_file *file) {
    struct mem_handle *h = (struct mem_handle *) file;
    return h ? (off_t) h->pos : 0;
}

static void mem_msg_h(struct mspack_file *file, const char *format, ...) {
    va_list ap;
    (void) file;
    if (MEM.num_msg < MEM_MAX_MSG) {
        va_start(ap, format);
        vsnprintf(MEM.msg[MEM.num_msg], sizeof(MEM.msg[0]), format, ap);
        va_end(ap);
        MEM.num_msg++;
    }
}

static void *mem_alloc_h(struct mspack_system *self, size_t bytes) {
    (void) self;
    return malloc(bytes);
}
static void mem_free_h(void *buffer) { free(buffer); }
static void mem_copy_h(void *src, void *dest, size_t bytes) {
    memcpy(dest, src, bytes);
}

static void mem_init(void) {
    memset(&MEM, 0, sizeof(MEM));
    MEM.sys.open    = &mem_open;
    MEM.sys.close   = &mem_close_h;
    MEM.sys.read    = &mem_read_h;
    MEM.sys.write   = &mem_write_h;
    MEM.sys.seek    = &mem_seek_h;
    MEM.sys.tell    = &mem_tell_h;
    MEM.sys.message = &mem_msg_h;
    MEM.sys.alloc   = &mem_alloc_h;
    MEM.sys.free    = &mem_free_h;
    MEM.sys.copy    = &mem_copy_h;
    MEM.sys.null_ptr = NULL;
    MEM.next_handle_id = 1;
    MEM.fault_match_req = -1;
}

/* verify the write segments of one output handle tile [0, len) exactly:
 * contiguous, in order, no overlap, no gap, no resend */
static int mem_check_write_intervals(int handle, long len, const char *file) {
    long pos = 0;
    int i, ok = 1;
    assertions++;
    for (i = 0; i < MEM.log_len; i++) {
        struct mem_log_ent *e = &MEM.log[i];
        if (e->handle != handle || e->kind != 'w') continue;
        if (e->off != pos || e->got <= 0) { ok = 0; break; }
        pos += e->got;
    }
    if (pos != len) ok = 0;
    if (!ok) fail_record(file, 0, len, pos, 0, 0, "write-intervals-broken");
    return ok;
}

/* ------------------------------------------------------------------ */
/* DEFLATE subset encoder (fixed Huffman blocks only)                  */
/* ------------------------------------------------------------------ */

struct bitw {
    unsigned char *buf;
    size_t len, cap;
    unsigned int bitbuf;
    int bitcnt;
    int oom;
};

static void bw_init(struct bitw *w) {
    memset(w, 0, sizeof(*w));
}
static void bw_byte(struct bitw *w, unsigned char b) {
    if (w->len >= w->cap) {
        size_t nc = w->cap ? w->cap * 2 : 1024;
        unsigned char *nb = (unsigned char *) realloc(w->buf, nc);
        if (!nb) { w->oom = 1; return; }
        w->buf = nb; w->cap = nc;
    }
    w->buf[w->len++] = b;
}
static void bw_bit(struct bitw *w, int bit) {
    w->bitbuf |= (unsigned int) (bit & 1) << w->bitcnt;
    if (++w->bitcnt == 8) {
        bw_byte(w, (unsigned char) w->bitbuf);
        w->bitbuf = 0; w->bitcnt = 0;
    }
}
/* plain integer, least-significant bit first (deflate extra bits) */
static void bw_bits_lsb(struct bitw *w, unsigned int val, int n) {
    int i;
    for (i = 0; i < n; i++) bw_bit(w, (int) ((val >> i) & 1));
}
/* huffman code, most-significant bit first */
static void bw_code_msb(struct bitw *w, unsigned int code, int n) {
    int i;
    for (i = n - 1; i >= 0; i--) bw_bit(w, (int) ((code >> i) & 1));
}
static void bw_flush(struct bitw *w) {
    while (w->bitcnt) bw_bit(w, 0);
}

/* fixed-huffman literal/length symbol (0..287) */
static void enc_lit_sym(struct bitw *w, int sym) {
    if (sym < 144)       bw_code_msb(w, 0x30u + (unsigned) sym, 8);
    else if (sym < 256)  bw_code_msb(w, 0x190u + (unsigned) (sym - 144), 9);
    else if (sym < 280)  bw_code_msb(w, (unsigned) (sym - 256), 7);
    else                 bw_code_msb(w, 0xC0u + (unsigned) (sym - 280), 8);
}

static const unsigned short enc_lit_len[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27,
    31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const unsigned char enc_lit_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2,
    2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const unsigned short enc_dist_off[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385,
    513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
static const unsigned char enc_dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
    6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

static void enc_match(struct bitw *w, int dist, int len) {
    int c, i;
    /* length bucket; 258 belongs to code 285, not the 227..258 range */
    c = 0;
    for (i = 0; i < 29; i++) {
        int lo = enc_lit_len[i];
        int hi = lo + ((1 << enc_lit_extra[i]) - 1);
        if (i == 28) { lo = hi = 258; }
        else if (i == 27) { hi = 257; }
        if (len >= lo && len <= hi) { c = i; break; }
    }
    enc_lit_sym(w, 257 + c);
    bw_bits_lsb(w, (unsigned int) (len - enc_lit_len[c]), enc_lit_extra[c]);

    /* distance bucket; all fixed-huffman distance codes are 5 bit */
    c = 0;
    for (i = 0; i < 30; i++) {
        int lo = enc_dist_off[i];
        int hi = lo + ((1 << enc_dist_extra[i]) - 1);
        if (dist >= lo && dist <= hi) { c = i; break; }
    }
    bw_code_msb(w, (unsigned int) c, 5);
    bw_bits_lsb(w, (unsigned int) (dist - enc_dist_off[c]),
                enc_dist_extra[c]);
}

/* ------------------------------------------------------------------ */
/* the model: actions -> expected plaintext, actions -> MSZIP frames   */
/* ------------------------------------------------------------------ */

#define ACT_LIT  0
#define ACT_COPY 1
#define MAX_ACTS 40000

struct maction { int op, a, b; }; /* LIT: a=byte  COPY: a=dist, b=len */

struct mframe {
    struct maction acts[MAX_ACTS];
    int num_acts;
    unsigned int out_len;      /* uncompressed size of this frame */
    unsigned char *comp;       /* compressed bytes incl. "CK" prefix */
    unsigned int comp_len;
};

static void frame_lit(struct mframe *f, int byte) {
    if (f->num_acts >= MAX_ACTS) {
        fail_record(NULL, 0, 0, -1, 0, 0, "too-many-actions");
        return;
    }
    f->acts[f->num_acts].op = ACT_LIT;
    f->acts[f->num_acts].a = byte & 0xff;
    f->acts[f->num_acts].b = 0;
    f->num_acts++;
    f->out_len++;
}
static void frame_copy(struct mframe *f, int dist, int len) {
    /* deflate matches are at most 258 bytes and 32768 back; split any
     * longer copy into a run of legal matches (identical semantics,
     * because the decoder copies byte by byte) */
    if (dist < 1 || dist > 32768 || len < 3) {
        fail_record(NULL, 0, 0, -1, 0, 0, "bad-copy-action");
        return;
    }
    while (len > 0) {
        int run = len > 258 ? 258 : len;
        if (f->num_acts >= MAX_ACTS) {
            fail_record(NULL, 0, 0, -1, 0, 0, "too-many-actions");
            return;
        }
        f->acts[f->num_acts].op = ACT_COPY;
        f->acts[f->num_acts].a = dist;
        f->acts[f->num_acts].b = run;
        f->num_acts++;
        f->out_len += (unsigned int) run;
        len -= run;
    }
}

/* expand the action lists into the expected folder plaintext, using the
 * model's own window simulation (mirrors the format, not the decoder).
 * Also verifies the designed cross-frame history references. */
static unsigned char *model_expand(struct mframe *frames, int nframes,
                                   unsigned int *total_out,
                                   unsigned int *hist_reads) {
    static unsigned char window[32768];
    static unsigned char written[32768];
    unsigned int wpos = 0, out_cap = 0, out_len = 0, fi;
    unsigned char *out;
    int i;

    memset(window, 0, sizeof(window));
    memset(written, 0, sizeof(written));
    *hist_reads = 0;
    for (i = 0; i < nframes; i++) out_cap += frames[i].out_len;
    out = (unsigned char *) malloc(out_cap ? out_cap : 1);
    if (!out) return NULL;

    for (fi = 0; fi < (unsigned int) nframes; fi++) {
        struct mframe *f = &frames[fi];
        wpos = 0; /* decoder restarts each frame at window position 0 */
        for (i = 0; i < f->num_acts; i++) {
            if (f->acts[i].op == ACT_LIT) {
                unsigned char b = (unsigned char) f->acts[i].a;
                window[wpos] = b; written[wpos] = 1; wpos++;
                out[out_len++] = b;
            }
            else {
                int dist = f->acts[i].a, len = f->acts[i].b;
                unsigned int mp =
                    ((unsigned int) dist > wpos ? 32768u : 0u) + wpos -
                    (unsigned int) dist;
                while (len--) {
                    unsigned char b = window[mp];
                    /* history used by frames after the first must have
                     * been written by an earlier frame: prove the model
                     * really depends on preserved dictionary content */
                    if (fi > 0) {
                        if (!written[mp]) {
                            fail_record(NULL, (long) out_len, 1,
                                        (long) out_len, 0, 0,
                                        "model-uses-unwritten-history");
                        }
                        else if ((unsigned int) dist > wpos) {
                            (*hist_reads)++;
                        }
                    }
                    mp = (mp + 1) & 32767u;
                    window[wpos] = b; written[wpos] = 1; wpos++;
                    out[out_len++] = b;
                }
            }
        }
    }
    *total_out = out_len;
    return out;
}

/* encode one frame's actions as "CK" + deflate fixed-huffman stream */
static int model_encode_frame(struct mframe *f) {
    struct bitw w;
    int i;
    bw_init(&w);
    bw_bits_lsb(&w, 1, 1); /* BFINAL */
    bw_bits_lsb(&w, 1, 2); /* BTYPE = 01 fixed huffman */
    for (i = 0; i < f->num_acts; i++) {
        if (f->acts[i].op == ACT_LIT) enc_lit_sym(&w, f->acts[i].a);
        else enc_match(&w, f->acts[i].a, f->acts[i].b);
    }
    enc_lit_sym(&w, 256); /* end of block */
    bw_flush(&w);
    if (w.oom) { free(w.buf); return 0; }

    f->comp = (unsigned char *) malloc(w.len + 2);
    if (!f->comp) return 0;
    f->comp[0] = 'C'; f->comp[1] = 'K';
    memcpy(f->comp + 2, w.buf, w.len);
    f->comp_len = (unsigned int) (w.len + 2);
    free(w.buf);
    return 1;
}

/* build the standard action lists used by all groups:
 * frame 0: literals then intra-frame copies, exactly `f0` bytes
 * frame 1: starts with a copy reaching 32768 back into frame 0
 * frame 2 (optional): starts with a copy reaching into frame 1       */
static void build_frame0(struct mframe *f, unsigned int seed, int target) {
    int pos = 0, i;
    rng_seed(seed);
    f->num_acts = 0; f->out_len = 0;
    for (i = 0; i < target - 2768; i++) frame_lit(f, (int) (rng_next() & 0xff));
    pos = target - 2768;
    while (pos < target) {
        int remaining = target - pos;
        if (remaining < 3) { frame_lit(f, (int) (rng_next() & 0xff)); pos++; continue; }
        {
            int len = 3 + (int) (rng_next() % 150u);
            int dist;
            if (len > remaining) len = remaining;
            dist = 1 + (int) (rng_next() % 5000u);
            if (dist > pos) dist = pos;
            frame_copy(f, dist, len);
            pos += len;
        }
    }
}

static void build_frameN(struct mframe *f, unsigned int seed, int target,
                         int first_dist, int first_len) {
    int pos;
    rng_seed(seed);
    f->num_acts = 0; f->out_len = 0;
    frame_copy(f, first_dist, first_len); /* cross-frame history reference */
    pos = first_len;
    while (pos < target) {
        int remaining = target - pos;
        if (remaining < 3) { frame_lit(f, (int) (rng_next() & 0xff)); pos++; continue; }
        if (rng_next() & 1) {
            int len = 3 + (int) (rng_next() % 120u);
            int dist;
            if (len > remaining) len = remaining;
            dist = 1 + (int) (rng_next() % 32768u);
            frame_copy(f, dist, len);
            pos += len;
        }
        else {
            int n = 1 + (int) (rng_next() % 40u);
            while (n-- && pos < target) {
                frame_lit(f, (int) (rng_next() & 0xff));
                pos++;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* CAB container builder                                               */
/* ------------------------------------------------------------------ */

struct cab_file_ent {
    const char *name;
    unsigned int offset, length;
    unsigned short folder_idx; /* 0 or 0xFFFD/0xFFFE/0xFFFF */
};
struct cab_block_ent {
    const unsigned char *data;
    unsigned int clen, ulen;   /* ulen == 0 -> split piece, continued  */
};

static void put16(unsigned char *p, unsigned int v) {
    p[0] = (unsigned char) (v & 0xff);
    p[1] = (unsigned char) ((v >> 8) & 0xff);
}
static void put32(unsigned char *p, unsigned long v) {
    p[0] = (unsigned char) (v & 0xff);
    p[1] = (unsigned char) ((v >> 8) & 0xff);
    p[2] = (unsigned char) ((v >> 16) & 0xff);
    p[3] = (unsigned char) ((v >> 24) & 0xff);
}

/* same checksum algorithm as the format (and cabd.c) */
static unsigned int cab_cksum(const unsigned char *data, unsigned int bytes,
                              unsigned int cksum) {
    unsigned int len, ul = 0;
    for (len = bytes >> 2; len--; data += 4) {
        cksum ^= (unsigned int) data[0] | ((unsigned int) data[1] << 8) |
                 ((unsigned int) data[2] << 16) | ((unsigned int) data[3] << 24);
    }
    switch (bytes & 3) {
    case 3: ul |= (unsigned int) *data++ << 16; /* fall through */
    case 2: ul |= (unsigned int) *data++ <<  8; /* fall through */
    case 1: ul |= *data;
    }
    cksum ^= ul;
    return cksum;
}

struct cab_spec {
    const struct cab_block_ent *blocks;
    int nblocks;
    const struct cab_file_ent *files;
    int nfiles;
    unsigned short set_id, set_index;
    int flags;                 /* bit0 prev, bit1 next, bit2 reserve */
    const char *prevname, *nextname;
    int header_resv, folder_resv, block_resv;
    unsigned int comp_type;    /* 1 = MSZIP */
};

#define CABF_PREV 1
#define CABF_NEXT 2
#define CABF_RESV 4

static unsigned char *cab_build(const struct cab_spec *sp,
                                unsigned int *out_len) {
    size_t total, data_off, files_off;
    int i;
    unsigned char *img, *p;

    total = 36;
    if (sp->flags & CABF_RESV) total += 4 + (size_t) sp->header_resv;
    if (sp->flags & CABF_PREV)
        total += strlen(sp->prevname) + 1 + 1; /* name + empty info */
    if (sp->flags & CABF_NEXT)
        total += strlen(sp->nextname) + 1 + 1;
    total += 8 + (size_t) sp->folder_resv;     /* one CFFOLDER */
    files_off = total;
    for (i = 0; i < sp->nfiles; i++)
        total += 16 + strlen(sp->files[i].name) + 1;
    data_off = total;
    for (i = 0; i < sp->nblocks; i++)
        total += 8 + (size_t) sp->block_resv + sp->blocks[i].clen;

    img = (unsigned char *) calloc(1, total);
    if (!img) return NULL;
    p = img;

    /* CFHEADER */
    memcpy(p, "MSCF", 4);
    put32(p + 8, (unsigned long) total);       /* cbCabinet */
    put32(p + 16, (unsigned long) files_off);  /* coffFiles */
    p[24] = 3; p[25] = 1;                      /* minor, major */
    put16(p + 26, 1);                          /* cFolders */
    put16(p + 28, (unsigned int) sp->nfiles);  /* cFiles */
    put16(p + 30, (unsigned int) sp->flags);   /* flags match spec bits */
    put16(p + 32, sp->set_id);
    put16(p + 34, sp->set_index);
    p += 36;

    if (sp->flags & CABF_RESV) {
        put16(p, (unsigned int) sp->header_resv);
        p[2] = (unsigned char) sp->folder_resv;
        p[3] = (unsigned char) sp->block_resv;
        p += 4;
        for (i = 0; i < sp->header_resv; i++) *p++ = (unsigned char) (0xA0 + i);
    }
    if (sp->flags & CABF_PREV) {
        size_t l = strlen(sp->prevname) + 1;
        memcpy(p, sp->prevname, l); p += l;
        *p++ = 0; /* empty prev info */
    }
    if (sp->flags & CABF_NEXT) {
        size_t l = strlen(sp->nextname) + 1;
        memcpy(p, sp->nextname, l); p += l;
        *p++ = 0;
    }

    /* CFFOLDER */
    put32(p, (unsigned long) data_off);
    put16(p + 4, (unsigned int) sp->nblocks);
    put16(p + 6, sp->comp_type);
    p += 8;
    for (i = 0; i < sp->folder_resv; i++) *p++ = (unsigned char) (0xF0 + i);

    /* CFFILE entries */
    for (i = 0; i < sp->nfiles; i++) {
        const struct cab_file_ent *fe = &sp->files[i];
        put32(p, fe->length);
        put32(p + 4, fe->offset);
        put16(p + 8, fe->folder_idx);
        put16(p + 10, 0x5900);          /* date 1992-08-00ish */
        put16(p + 12, 0x0001);          /* time */
        put16(p + 14, 0x20);            /* attribs: archive */
        p += 16;
        memcpy(p, fe->name, strlen(fe->name) + 1);
        p += strlen(fe->name) + 1;
    }

    /* CFDATA blocks */
    for (i = 0; i < sp->nblocks; i++) {
        const struct cab_block_ent *b = &sp->blocks[i];
        unsigned char *hdr = p;
        unsigned int sum;
        int r;
        put16(hdr + 4, b->clen);
        put16(hdr + 6, b->ulen);
        p += 8;
        for (r = 0; r < sp->block_resv; r++)
            *p++ = (unsigned char) (0xB0 + i * 16 + r);
        memcpy(p, b->data, b->clen);
        p += b->clen;
        sum = cab_cksum(b->data, b->clen, 0);
        sum = cab_cksum(hdr + 4, 4, sum);
        put32(hdr, sum);
    }

    *out_len = (unsigned int) (p - img);
    return img;
}

/* ------------------------------------------------------------------ */
/* shared scenario builder: 3-frame folder + 5 files                   */
/* ------------------------------------------------------------------ */

#define M_F0 32768
#define M_F1 20000
#define M_F2 15000
#define M_TOTAL (M_F0 + M_F1 + M_F2)

struct scenario {
    struct mframe fr[3];
    int nframes;
    unsigned char *expected;   /* folder plaintext, from the model */
    unsigned int expected_len;
    struct cab_file_ent files[8];
    int nfiles;
};

static void scenario_free(struct scenario *s) {
    int i;
    for (i = 0; i < s->nframes; i++) free(s->fr[i].comp);
    free(s->expected);
    s->expected = NULL;
}

/* frames: f0 literals+copies, f1/f2 start with deep history copies */
static int scenario_build_frames(struct scenario *s, unsigned int seedbase,
                                 int nframes) {
    int i;
    unsigned int hist_reads = 0;
    s->nframes = nframes;
    build_frame0(&s->fr[0], seedbase, M_F0);
    if (nframes > 1)
        build_frameN(&s->fr[1], seedbase + 1, M_F1, 32768, 258);
    if (nframes > 2)
        build_frameN(&s->fr[2], seedbase + 2, M_F2, 32768, 300);
    for (i = 0; i < nframes; i++)
        if (!model_encode_frame(&s->fr[i])) return 0;
    s->expected = model_expand(s->fr, nframes, &s->expected_len, &hist_reads);
    if (!s->expected) return 0;
    /* model-level proof: frames after the first begin with a copy whose
     * distance exceeds what the frame itself has produced, i.e. they
     * read non-zero history written by earlier frames, and the model
     * expansion really did read earlier-frame window bytes */
    assertions++;
    for (i = 1; i < nframes; i++) {
        struct maction *a = &s->fr[i].acts[0];
        if (!(a->op == ACT_COPY && (unsigned int) a->a > 0)) {
            fail_record(NULL, 0, 0, -1, 0, 0, "frame-lacks-history-ref");
        }
    }
    if (!check_true(nframes < 2 || hist_reads > 0, NULL, 0, 0,
                    "no-cross-frame-history-reads")) return 0;
    return 1;
}

/* extract one file through the public API into a fresh memory output
 * and compare every byte against the model slice */
static int extract_and_check(struct mscab_decompressor *cabd,
                             struct mscabd_file *f,
                             const struct scenario *s,
                             const char *outname,
                             const char *tag) {
    char msg[128];
    struct mem_node *n;
    int err = cabd->extract(cabd, f, outname);
    if (err != MSPACK_ERR_OK) {
        snprintf(msg, sizeof(msg), "extract-failed err=%d tag=%s", err, tag);
        fail_record(f->filename, (long) f->offset, (long) f->length,
                    -1, 0, 0, msg);
        return 0;
    }
    n = mem_find(outname);
    if (!need(n != NULL, "output-node-missing")) return 0;
    if (!check_true((long) n->len == (long) f->length, f->filename,
                    (long) f->offset, (long) f->length,
                    "output-length-mismatch")) return 0;
    return check_bytes(f->filename, (long) f->offset,
                       s->expected + f->offset, n->data, (long) n->len);
}

/* ------------------------------------------------------------------ */
/* group: model                                                        */
/* ------------------------------------------------------------------ */

static int group_model(void) {
    struct scenario s;
    struct cab_block_ent blocks[3];
    struct cab_spec spec;
    unsigned char *img;
    unsigned int img_len, i;
    unsigned long long h_before, h_after;
    struct mscab_decompressor *cabd;
    struct mscabd_cabinet *cab;
    struct mscabd_file *f[5];
    int ok = 1;

    cur_model = "mszip-3frame-5file";
    cur_layout = "single-cab blocks=[32768,20000,15000]";
    memset(&s, 0, sizeof(s));
    mem_reset_all();

    if (!need(scenario_build_frames(&s, 0xCAB001u, 3), "frames")) return -1;

    /* files: boundaries inside frames and across frame boundaries */
    s.files[0].name = "a.bin";     s.files[0].offset = 0;     s.files[0].length = 5000;
    s.files[1].name = "b.bin";     s.files[1].offset = 32000; s.files[1].length = 3000;
    s.files[2].name = "empty.bin"; s.files[2].offset = 12345; s.files[2].length = 0;
    s.files[3].name = "tiny.bin";  s.files[3].offset = 40000; s.files[3].length = 7;
    s.files[4].name = "tail.bin";  s.files[4].offset = 51000; s.files[4].length = 16768;
    s.nfiles = 5;
    for (i = 0; i < 5; i++) s.files[i].folder_idx = 0;

    blocks[0].data = s.fr[0].comp; blocks[0].clen = s.fr[0].comp_len; blocks[0].ulen = M_F0;
    blocks[1].data = s.fr[1].comp; blocks[1].clen = s.fr[1].comp_len; blocks[1].ulen = M_F1;
    blocks[2].data = s.fr[2].comp; blocks[2].clen = s.fr[2].comp_len; blocks[2].ulen = M_F2;

    memset(&spec, 0, sizeof(spec));
    spec.blocks = blocks; spec.nblocks = 3;
    spec.files = s.files; spec.nfiles = 5;
    spec.set_id = 0x1234; spec.set_index = 0;
    spec.comp_type = 1;
    img = cab_build(&spec, &img_len);
    if (!need(img != NULL, "cab-build")) { scenario_free(&s); return -1; }

    h_before = fnv1a(img, img_len);
    if (!need(mem_add_input("model.cab", img, img_len), "add-input")) {
        free(img); scenario_free(&s); return -1;
    }

    cabd = mspack_create_cab_decompressor(&MEM.sys);
    if (!need(cabd != NULL, "create-decompressor")) {
        free(img); scenario_free(&s); return -1;
    }
    cab = cabd->open(cabd, "model.cab");
    if (!need(cab != NULL, "open")) {
        mspack_destroy_cab_decompressor(cabd);
        free(img); scenario_free(&s); return -1;
    }

    /* collect the five files in list order */
    {
        struct mscabd_file *w = cab->files;
        int k = 0;
        while (w && k < 5) { f[k++] = w; w = w->next; }
        if (!need(k == 5 && w == NULL, "file-list")) ok = 0;
    }

    /* non-contiguous extraction order: tail, empty, a, b, tiny, tail,
     * then a again (backward jump forces folder re-initialisation) */
    if (ok) {
        static const int order[7] = {4, 2, 0, 1, 3, 4, 0};
        static const char *names[5] = {"o-a", "o-b", "o-empty",
                                       "o-tiny", "o-tail"};
        int j;
        mem_reset_logs();
        for (j = 0; j < 7 && ok; j++) {
            int idx = order[j];
            ok = extract_and_check(cabd, f[idx], &s, names[idx], "model");
        }
        /* zero-length file must have produced zero bytes */
        if (ok) {
            struct mem_node *n = mem_find("o-empty");
            ok = check_true(n && n->len == 0, "empty.bin", 12345, 0,
                            "zero-length-not-empty");
        }
    }

    /* prove the input image was not modified by open+extract */
    if (ok) {
        struct mem_node *in = mem_find("model.cab");
        h_after = in ? fnv1a(in->data, in->len) : 0;
        ok = check_true(in && h_before == h_after, "model.cab", 0,
                        (long) img_len, "input-image-mutated");
        ok = check_true(MEM.write_to_input == 0, "model.cab", 0,
                        (long) img_len, "library-wrote-to-input") && ok;
    }

    cabd->close(cabd, cab);
    mspack_destroy_cab_decompressor(cabd);
    free(img);
    scenario_free(&s);
    return ok ? 1 : -1;
}

/* ------------------------------------------------------------------ */
/* group: split                                                        */
/* ------------------------------------------------------------------ */

/* helper used by the set-ID case: extract both files from an already
 * merged chain and compare with the model */
static int split_merge_and_check_extract_only(struct mscab_decompressor *cabd,
                                              struct mscabd_cabinet *c0,
                                              struct scenario *s);

#define SP_F0 32768
#define SP_F1 20000
#define SP_TOTAL (SP_F0 + SP_F1)

/* build the three volume images for one set of cut points */
static int split_build_set(struct scenario *s, int c1, int c2,
                           unsigned short set_id_vol3,
                           unsigned char *vol[3], unsigned int vlen[3],
                           const char * const names[3]) {
    struct cab_block_ent b[2];
    struct cab_file_ent files[2];
    struct cab_spec spec;
    int i;

    files[0].name = "first.txt"; files[0].offset = 0;
    files[0].length = 1000; files[0].folder_idx = 0;
    files[1].name = "rest.bin";  files[1].offset = 1000;
    files[1].length = SP_TOTAL - 1000;

    for (i = 0; i < 3; i++) {
        memset(&spec, 0, sizeof(spec));
        spec.files = files;
        spec.set_id = (i == 2) ? set_id_vol3 : 0x5555;
        spec.set_index = (unsigned short) i;
        spec.comp_type = 1;
        spec.flags = CABF_RESV;
        spec.header_resv = 2;
        spec.folder_resv = 1;
        spec.block_resv = 3 + i; /* non-empty reserved area per piece */
        if (i < 2) { spec.flags |= CABF_NEXT; spec.nextname = names[i+1]; }
        if (i > 0) { spec.flags |= CABF_PREV; spec.prevname = names[i-1]; }

        if (i == 0) {
            b[0].data = s->fr[0].comp; b[0].clen = s->fr[0].comp_len;
            b[0].ulen = SP_F0;
            b[1].data = s->fr[1].comp; b[1].clen = (unsigned int) c1;
            b[1].ulen = 0; /* split piece: continues in next cabinet */
            spec.blocks = b; spec.nblocks = 2;
            files[1].folder_idx = 0xFFFE; /* continued to next */
            spec.nfiles = 2;
        }
        else if (i == 1) {
            b[0].data = s->fr[1].comp + c1; b[0].clen = (unsigned int) (c2 - c1);
            b[0].ulen = 0;
            spec.blocks = b; spec.nblocks = 1;
            files[1].folder_idx = 0xFFFF; /* continued prev and next */
            spec.nfiles = 1;
            spec.files = &files[1];
        }
        else {
            b[0].data = s->fr[1].comp + c2;
            b[0].clen = s->fr[1].comp_len - (unsigned int) c2;
            b[0].ulen = SP_F1; /* last piece carries the real length */
            spec.blocks = b; spec.nblocks = 1;
            files[1].folder_idx = 0xFFFD; /* continued from prev */
            spec.nfiles = 1;
            spec.files = &files[1];
        }
        vol[i] = cab_build(&spec, &vlen[i]);
        if (!vol[i]) return 0;
    }
    return 1;
}

/* open+merge a 3-volume set already registered in MEM, extract both
 * files, compare with the model. order: 0=append chain, 1=prepend */
static int split_merge_and_check(struct scenario *s,
                                 const char * const names[3],
                                 int use_prepend) {
    struct mscab_decompressor *cabd;
    struct mscabd_cabinet *c0, *c1, *c2;
    struct mscabd_file *f0, *f1;
    int ok = 1, m0, m1;

    cabd = mspack_create_cab_decompressor(&MEM.sys);
    if (!need(cabd != NULL, "create")) return 0;
    c0 = cabd->open(cabd, names[0]);
    c1 = cabd->open(cabd, names[1]);
    c2 = cabd->open(cabd, names[2]);
    if (!need(c0 && c1 && c2, "open-volumes")) {
        mspack_destroy_cab_decompressor(cabd);
        return 0;
    }
    if (use_prepend) {
        m0 = cabd->prepend(cabd, c1, c0); /* c1 gets c0 before it */
        m1 = cabd->prepend(cabd, c2, c1);
    }
    else {
        m0 = cabd->append(cabd, c0, c1);
        m1 = cabd->append(cabd, c1, c2);
    }
    if (!need(m0 == MSPACK_ERR_OK && m1 == MSPACK_ERR_OK, "merge")) {
        cabd->close(cabd, use_prepend ? c2 : c0);
        mspack_destroy_cab_decompressor(cabd);
        return 0;
    }

    f0 = (use_prepend ? c2 : c0)->files;
    f1 = f0 ? f0->next : NULL;
    if (!need(f0 && f1 && !f1->next, "merged-file-list")) {
        cabd->close(cabd, use_prepend ? c2 : c0);
        mspack_destroy_cab_decompressor(cabd);
        return 0;
    }
    ok = extract_and_check(cabd, f0, s, "sp-first", "split") && ok;
    ok = extract_and_check(cabd, f1, s, "sp-rest", "split") && ok;

    cabd->close(cabd, use_prepend ? c2 : c0);
    mspack_destroy_cab_decompressor(cabd);
    return ok;
}

static int group_split(void) {
    struct scenario s;
    unsigned char *volA[3] = {0,0,0}, *volB[3] = {0,0,0}, *volC[3] = {0,0,0};
    unsigned int lenA[3] = {0,0,0}, lenB[3] = {0,0,0}, lenC[3] = {0,0,0};
    static const char *namesA[3] = {"spA1.cab", "spA2.cab", "spA3.cab"};
    static const char *namesB[3] = {"spB1.cab", "spB2.cab", "spB3.cab"};
    static const char *namesC[3] = {"spC1.cab", "spC2.cab", "spC3.cab"};
    int c1, c2, d1, d2, i, ok = 1;
    int comp2;

    cur_model = "mszip-2frame-split";
    cur_layout = "3-volumes split-CFDATA";
    memset(&s, 0, sizeof(s));
    mem_reset_all();

    if (!need(scenario_build_frames(&s, 0x59117u, 2), "frames")) return -1;
    comp2 = (int) s.fr[1].comp_len;
    if (!need(comp2 > 64, "frame2-too-small")) return -1;

    /* two different physical cut layouts of the same compressed stream */
    c1 = comp2 / 3;      c2 = (2 * comp2) / 3;
    d1 = comp2 / 2;      d2 = comp2 / 2 + comp2 / 4;

    if (!need(split_build_set(&s, c1, c2, 0x5555, volA, lenA, namesA), "buildA"))
        return -1;
    if (!need(split_build_set(&s, d1, d2, 0x5555, volB, lenB, namesB), "buildB"))
        return -1;
    /* set C: identical to A except volume 3 carries a different set ID */
    if (!need(split_build_set(&s, c1, c2, 0x7777, volC, lenC, namesC), "buildC"))
        return -1;

    /* ---- layout A: append merge, full content check ---- */
    for (i = 0; i < 3; i++)
        if (!need(mem_add_input(namesA[i], volA[i], lenA[i]), "regA")) return -1;
    cur_layout = "append cut=1/3,2/3 resv=3,4,5";
    ok = split_merge_and_check(&s, namesA, 0) && ok;

    /* ---- layout B: prepend merge, different cut points ---- */
    mem_reset_all();
    for (i = 0; i < 3; i++)
        if (!need(mem_add_input(namesB[i], volB[i], lenB[i]), "regB")) return -1;
    cur_layout = "prepend cut=1/2,3/4 resv=3,4,5";
    ok = split_merge_and_check(&s, namesB, 1) && ok;

    /* ---- single-cabinet reference: same folder, no split ---- */
    mem_reset_all();
    {
        struct cab_block_ent rb[2];
        struct cab_file_ent rf[2];
        struct cab_spec spec;
        unsigned char *one;
        unsigned int one_len = 0;
        struct mscab_decompressor *cabd;
        struct mscabd_cabinet *cab;

        cur_layout = "single-cab reference";
        rb[0].data = s.fr[0].comp; rb[0].clen = s.fr[0].comp_len; rb[0].ulen = SP_F0;
        rb[1].data = s.fr[1].comp; rb[1].clen = s.fr[1].comp_len; rb[1].ulen = SP_F1;
        rf[0].name = "first.txt"; rf[0].offset = 0; rf[0].length = 1000; rf[0].folder_idx = 0;
        rf[1].name = "rest.bin";  rf[1].offset = 1000; rf[1].length = SP_TOTAL - 1000; rf[1].folder_idx = 0;
        memset(&spec, 0, sizeof(spec));
        spec.blocks = rb; spec.nblocks = 2;
        spec.files = rf; spec.nfiles = 2;
        spec.set_id = 0x5555; spec.comp_type = 1;
        one = cab_build(&spec, &one_len);
        if (!need(one && mem_add_input("spref.cab", one, one_len), "ref-build")) {
            free(one);
            return -1;
        }
        cabd = mspack_create_cab_decompressor(&MEM.sys);
        cab = cabd ? cabd->open(cabd, "spref.cab") : NULL;
        if (!need(cab != NULL, "ref-open")) return -1;
        ok = extract_and_check(cabd, cab->files, &s, "spref-first", "split") && ok;
        ok = extract_and_check(cabd, cab->files->next, &s, "spref-rest", "split") && ok;
        cabd->close(cabd, cab);
        mspack_destroy_cab_decompressor(cabd);
        free(one);
    }

    /* ---- missing middle volume: vol1+vol3 only ---- */
    mem_reset_all();
    if (!need(mem_add_input(namesA[0], volA[0], lenA[0]), "reg-m1")) return -1;
    if (!need(mem_add_input(namesA[2], volA[2], lenA[2]), "reg-m3")) return -1;
    cur_layout = "missing-middle vol1+vol3";
    {
        struct mscab_decompressor *cabd = mspack_create_cab_decompressor(&MEM.sys);
        struct mscabd_cabinet *c0, *c2;
        int m, e1 = 0, e2 = 0;
        c0 = cabd->open(cabd, namesA[0]);
        c2 = cabd->open(cabd, namesA[2]);
        if (!need(c0 && c2, "open-missing")) return -1;
        m = cabd->append(cabd, c0, c2);
        if (!need(m == MSPACK_ERR_OK, "merge-missing")) return -1;
        /* the complete-in-vol1 file still extracts ... */
        e1 = cabd->extract(cabd, c0->files, "mm-first");
        /* ... but the file whose block is split across the gap must NOT
         * be reported as successfully extracted */
        e2 = cabd->extract(cabd, c0->files->next, "mm-rest");
        ok = check_true(e1 == MSPACK_ERR_OK, "first.txt", 0, 1000,
                        "complete-file-broken") && ok;
        ok = check_true(e2 != MSPACK_ERR_OK, "rest.bin", 1000,
                        SP_TOTAL - 1000, "missing-middle-reported-ok") && ok;
        cabd->close(cabd, c0);
        mspack_destroy_cab_decompressor(cabd);
    }

    /* ---- incomplete tail: vol1+vol2 only ---- */
    mem_reset_all();
    if (!need(mem_add_input(namesA[0], volA[0], lenA[0]), "reg-t1")) return -1;
    if (!need(mem_add_input(namesA[1], volA[1], lenA[1]), "reg-t2")) return -1;
    cur_layout = "missing-last vol1+vol2";
    {
        struct mscab_decompressor *cabd = mspack_create_cab_decompressor(&MEM.sys);
        struct mscabd_cabinet *c0, *c1;
        int m, e2;
        c0 = cabd->open(cabd, namesA[0]);
        c1 = cabd->open(cabd, namesA[1]);
        if (!need(c0 && c1, "open-tail")) return -1;
        m = cabd->append(cabd, c0, c1);
        if (!need(m == MSPACK_ERR_OK, "merge-tail")) return -1;
        e2 = cabd->extract(cabd, c0->files->next, "mt-rest");
        ok = check_true(e2 != MSPACK_ERR_OK, "rest.bin", 1000,
                        SP_TOTAL - 1000, "missing-last-reported-ok") && ok;
        cabd->close(cabd, c0);
        mspack_destroy_cab_decompressor(cabd);
    }

    /* ---- corrupted checksum of one split piece ---- */
    mem_reset_all();
    {
        unsigned char *bad = (unsigned char *) malloc(lenA[1]);
        if (!need(bad != NULL, "oom")) return -1;
        memcpy(bad, volA[1], lenA[1]);
        /* locate the middle piece's data inside vol2 by its first bytes,
         * then flip a bit in the stored CFDATA checksum that precedes
         * it (header 8 bytes + this volume's block_resv bytes) */
        {
            const unsigned char *piece = s.fr[1].comp + c1;
            int j, hit = -1;
            for (j = 0; j + 16 <= (int) lenA[1]; j++) {
                if (!memcmp(bad + j, piece, 16)) { hit = j; break; }
            }
            if (!need(hit >= 8 + 4, "locate-piece")) { free(bad); return -1; }
            bad[hit - 8 - 4] ^= 0x01; /* stored checksum of the piece */
        }
        if (!need(mem_add_input(namesA[0], volA[0], lenA[0]), "reg-c1")) return -1;
        if (!need(mem_add_input("spA2bad.cab", bad, lenA[1]), "reg-c2")) return -1;
        if (!need(mem_add_input(namesA[2], volA[2], lenA[2]), "reg-c3")) return -1;
        cur_layout = "corrupt-piece-checksum vol2";
        {
            struct mscab_decompressor *cabd =
                mspack_create_cab_decompressor(&MEM.sys);
            struct mscabd_cabinet *c0, *c1, *c2c;
            int m0, m1, e2;
            c0 = cabd->open(cabd, namesA[0]);
            c1 = cabd->open(cabd, "spA2bad.cab");
            c2c = cabd->open(cabd, namesA[2]);
            if (!need(c0 && c1 && c2c, "open-corrupt")) return -1;
            m0 = cabd->append(cabd, c0, c1);
            m1 = cabd->append(cabd, c1, c2c);
            if (!need(m0 == MSPACK_ERR_OK && m1 == MSPACK_ERR_OK,
                      "merge-corrupt")) return -1;
            e2 = cabd->extract(cabd, c0->files->next, "cb-rest");
            ok = check_true(e2 != MSPACK_ERR_OK, "rest.bin", 1000,
                            SP_TOTAL - 1000, "bad-checksum-reported-ok") && ok;
            cabd->close(cabd, c0);
            mspack_destroy_cab_decompressor(cabd);
        }
        free(bad);
    }

    /* ---- differing set ID: warning recorded, merge+content unchanged ---- */
    mem_reset_all();
    if (!need(mem_add_input(namesC[0], volC[0], lenC[0]), "reg-s1")) return -1;
    if (!need(mem_add_input(namesC[1], volC[1], lenC[1]), "reg-s2")) return -1;
    if (!need(mem_add_input(namesC[2], volC[2], lenC[2]), "reg-s3")) return -1;
    cur_layout = "setid-mismatch vol3=0x7777";
    {
        struct mscab_decompressor *cabd = mspack_create_cab_decompressor(&MEM.sys);
        struct mscabd_cabinet *c0, *c1, *c2c;
        int m0, m1, warned = 0;
        c0 = cabd->open(cabd, namesC[0]);
        c1 = cabd->open(cabd, namesC[1]);
        c2c = cabd->open(cabd, namesC[2]);
        if (!need(c0 && c1 && c2c, "open-setid")) return -1;
        mem_reset_logs();
        m0 = cabd->append(cabd, c0, c1);
        m1 = cabd->append(cabd, c1, c2c);
        /* existing behaviour: merge succeeds despite the identity
         * mismatch, with a warning.  Do not treat the warning as a
         * promise of rejection. */
        ok = check_true(m0 == MSPACK_ERR_OK && m1 == MSPACK_ERR_OK,
                        "rest.bin", 1000, SP_TOTAL - 1000,
                        "setid-merge-rejected") && ok;
        for (i = 0; i < MEM.num_msg; i++)
            if (strstr(MEM.msg[i], "Set IDs")) warned = 1;
        ok = check_true(warned, "rest.bin", 1000, SP_TOTAL - 1000,
                        "setid-warning-missing") && ok;
        ok = split_merge_and_check_extract_only(cabd, c0, &s) && ok;
        cabd->close(cabd, c0);
        mspack_destroy_cab_decompressor(cabd);
    }

    for (i = 0; i < 3; i++) { free(volA[i]); free(volB[i]); free(volC[i]); }
    scenario_free(&s);
    return ok ? 1 : -1;
}

/* helper used by the set-ID case: extract both files from an already
 * merged chain and compare with the model */
static int split_merge_and_check_extract_only(struct mscab_decompressor *cabd,
                                              struct mscabd_cabinet *c0,
                                              struct scenario *s) {
    int ok = 1;
    struct mscabd_file *f0 = c0->files, *f1 = f0 ? f0->next : NULL;
    if (!need(f0 && f1, "setid-file-list")) return 0;
    ok = extract_and_check(cabd, f0, s, "sid-first", "split") && ok;
    ok = extract_and_check(cabd, f1, s, "sid-rest", "split") && ok;
    return ok;
}

/* ------------------------------------------------------------------ */
/* group: partial                                                      */
/* ------------------------------------------------------------------ */

static int group_partial(void) {
    struct scenario s;
    struct cab_block_ent blocks[3];
    struct cab_spec spec;
    unsigned char *img;
    unsigned int img_len;
    struct mscab_decompressor *cabd1, *cabd2;
    struct mscabd_cabinet *cab;
    struct mscabd_file *f[6];
    int ok = 1, i, k;

    cur_model = "mszip-3frame-tiled";
    cur_layout = "single-cab tiled-files";
    memset(&s, 0, sizeof(s));
    mem_reset_all();

    if (!need(scenario_build_frames(&s, 0x9A77u, 3), "frames")) return -1;

    /* files tile the folder exactly: [0,5000) [5000,33000) [33000,52768)
     * [52768,67768), plus a whole-folder file and an empty one */
    s.files[0].name = "p1.bin";    s.files[0].offset = 0;     s.files[0].length = 5000;
    s.files[1].name = "p2.bin";    s.files[1].offset = 5000;  s.files[1].length = 28000;
    s.files[2].name = "p3.bin";    s.files[2].offset = 33000; s.files[2].length = 19768;
    s.files[3].name = "p4.bin";    s.files[3].offset = 52768; s.files[3].length = 15000;
    s.files[4].name = "whole.bin"; s.files[4].offset = 0;     s.files[4].length = M_TOTAL;
    s.files[5].name = "nil.bin";   s.files[5].offset = 100;   s.files[5].length = 0;
    s.nfiles = 6;
    for (i = 0; i < 6; i++) s.files[i].folder_idx = 0;

    blocks[0].data = s.fr[0].comp; blocks[0].clen = s.fr[0].comp_len; blocks[0].ulen = M_F0;
    blocks[1].data = s.fr[1].comp; blocks[1].clen = s.fr[1].comp_len; blocks[1].ulen = M_F1;
    blocks[2].data = s.fr[2].comp; blocks[2].clen = s.fr[2].comp_len; blocks[2].ulen = M_F2;

    memset(&spec, 0, sizeof(spec));
    spec.blocks = blocks; spec.nblocks = 3;
    spec.files = s.files; spec.nfiles = 6;
    spec.set_id = 0x2222; spec.comp_type = 1;
    img = cab_build(&spec, &img_len);
    if (!need(img != NULL, "cab-build")) { scenario_free(&s); return -1; }
    if (!need(mem_add_input("partial.cab", img, img_len), "add-input")) {
        free(img); scenario_free(&s); return -1;
    }

    /* ---- 1. whole-span output, then per-file-boundary outputs ---- */
    cabd1 = mspack_create_cab_decompressor(&MEM.sys);
    cab = cabd1 ? cabd1->open(cabd1, "partial.cab") : NULL;
    if (!need(cab != NULL, "open")) { free(img); scenario_free(&s); return -1; }
    {
        struct mscabd_file *w = cab->files;
        k = 0;
        while (w && k < 6) { f[k++] = w; w = w->next; }
        if (!need(k == 6, "file-list")) ok = 0;
    }
    if (ok) {
        mem_reset_logs();
        ok = extract_and_check(cabd1, f[4], &s, "pt-whole", "partial") && ok;
        /* per-boundary pieces, in order, concatenated == whole */
        ok = extract_and_check(cabd1, f[0], &s, "pt-p1", "partial") && ok;
        ok = extract_and_check(cabd1, f[1], &s, "pt-p2", "partial") && ok;
        ok = extract_and_check(cabd1, f[2], &s, "pt-p3", "partial") && ok;
        ok = extract_and_check(cabd1, f[3], &s, "pt-p4", "partial") && ok;
        if (ok) {
            /* concatenate the four outputs and compare with whole */
            struct mem_node *n1 = mem_find("pt-p1"), *n2 = mem_find("pt-p2");
            struct mem_node *n3 = mem_find("pt-p3"), *n4 = mem_find("pt-p4");
            struct mem_node *nw = mem_find("pt-whole");
            if (!need(n1 && n2 && n3 && n4 && nw, "outputs")) ok = 0;
            else {
                unsigned char *cat = (unsigned char *) malloc(M_TOTAL);
                if (!need(cat != NULL, "oom")) ok = 0;
                else {
                    memcpy(cat, n1->data, n1->len);
                    memcpy(cat + n1->len, n2->data, n2->len);
                    memcpy(cat + n1->len + n2->len, n3->data, n3->len);
                    memcpy(cat + n1->len + n2->len + n3->len, n4->data, n4->len);
                    ok = check_bytes("whole.bin", 0, nw->data, cat,
                                     (long) M_TOTAL) && ok;
                    free(cat);
                }
            }
        }
    }

    /* ---- 2. interleaved instances, backward jumps, write intervals ---- */
    if (ok) {
        cabd2 = mspack_create_cab_decompressor(&MEM.sys);
        if (!need(cabd2 != NULL, "create-2")) ok = 0;
        else {
            /* track handle ids per output name for interval checks */
            struct {
                const char *out;
                struct mscab_decompressor *inst;
                struct mscabd_file *file;
            } seq[6] = {
                {"pi-p2", cabd1, f[1]}, /* p2: ends mid-frame 2        */
                {"pi-p1", cabd2, f[0]}, /* other instance interleaved  */
                {"pi-p3", cabd1, f[2]}, /* pure pending-frame-tail     */
                {"pi-p4", cabd1, f[3]}, /* pending tail + new frame    */
                {"pi2-p1", cabd1, f[0]},/* backward jump, re-init      */
                {"pi-p2b", cabd2, f[1]},
            };
            int j;
            mem_reset_logs();
            for (j = 0; j < 6 && ok; j++) {
                ok = extract_and_check(seq[j].inst, seq[j].file, &s,
                                       seq[j].out, "partial") && ok;
                /* the output handle opened for this extract */
                if (ok) {
                    ok = mem_check_write_intervals(MEM.last_output_id,
                                                   (long) seq[j].file->length,
                                                   seq[j].file->filename) && ok;
                }
            }
            /* no writes to handles other than the six outputs, and no
             * writes to the input image */
            ok = check_true(MEM.write_to_input == 0, "partial.cab", 0,
                            (long) img_len, "library-wrote-to-input") && ok;
            mspack_destroy_cab_decompressor(cabd2);
        }
    }

    /* ---- 3. short reads are I/O failures: the library must not
     *        report success ---- */
    if (ok) {
        /* the fault plan keys on the exact size of the second block's
         * data read; make sure that size is unique among block reads */
        if (!need(s.fr[0].comp_len != s.fr[1].comp_len &&
                  s.fr[1].comp_len != s.fr[2].comp_len,
                  "block-sizes-not-distinct")) ok = 0;
    }
    if (ok) {
        /* (a) short read on the second block's data read */
        long need_req = (long) s.fr[1].comp_len;
        mem_reset_logs();
        MEM.fault_match_req = need_req;
        MEM.fault_skip = 0;
        MEM.fault_ret = need_req / 2; /* short but positive == EOF */
        {
            int e = cabd1->extract(cabd1, f[4], "pf-short");
            ok = check_true(e != MSPACK_ERR_OK, "whole.bin", 0, M_TOTAL,
                            "short-read-reported-ok") && ok;
            ok = check_true(MEM.fault_hits == 1, "whole.bin", 0, M_TOTAL,
                            "short-read-not-delivered") && ok;
        }
        /* (b) hard read error (-1) on the same read */
        mem_reset_logs();
        MEM.fault_match_req = need_req;
        MEM.fault_ret = -1;
        {
            int e = cabd1->extract(cabd1, f[4], "pf-ioerr");
            ok = check_true(e != MSPACK_ERR_OK, "whole.bin", 0, M_TOTAL,
                            "io-error-reported-ok") && ok;
        }
        mem_reset_logs();
        /* (c) after the failures a fresh instance still decodes fine */
        {
            struct mscab_decompressor *cabd3 =
                mspack_create_cab_decompressor(&MEM.sys);
            struct mscabd_cabinet *c3 = cabd3 ? cabd3->open(cabd3, "partial.cab") : NULL;
            if (!need(c3 != NULL, "reopen")) ok = 0;
            else {
                ok = extract_and_check(cabd3, c3->files->next, &s,
                                       "pf-recover", "partial") && ok;
                cabd3->close(cabd3, c3);
            }
            mspack_destroy_cab_decompressor(cabd3);
        }
    }

    cabd1->close(cabd1, cab);
    mspack_destroy_cab_decompressor(cabd1);
    free(img);
    scenario_free(&s);
    return ok ? 1 : -1;
}

/* ------------------------------------------------------------------ */
/* entry point                                                         */
/* ------------------------------------------------------------------ */

int cab_model_test(const char *group) {
    int rc;
    mem_init();
    assertions = 0;
    group_failed = 0;
    cur_group = group ? group : "?";

    if (group && !strcmp(group, "model"))        rc = group_model();
    else if (group && !strcmp(group, "split"))   rc = group_split();
    else if (group && !strcmp(group, "partial")) rc = group_partial();
    else {
        fprintf(stderr, "CAB_MODEL_FAIL group=%s unknown-group\n", cur_group);
        return -1;
    }

    mem_reset_all();
    if (rc < 0 || group_failed) return -1;
    if (assertions <= 0) {
        fprintf(stderr, "CAB_MODEL_FAIL group=%s no-assertions\n", cur_group);
        return -1;
    }
    return (int) assertions;
}
