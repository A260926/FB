/**
 * capture_values.c
 *
 * Resolved-style entry point for offline resource capture.
 * See capture_values.h for the contract.
 *
 * The implementation is a self-contained CSS component-value tokenizer
 * (CSS Syntax Level 3 boundaries: comments, strings, escapes, functions and
 * matched parentheses) plus the CSS Custom Properties / var() resolution
 * rules from https://www.w3.org/TR/css-variables-1/.
 */

#include "capture_values.h"

#include <assert.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Allocation with a per-style registry, so destroy() frees everything */
/* ------------------------------------------------------------------ */

typedef struct PtrRegistry {
    void** data;
    size_t length;
    size_t capacity;
} PtrRegistry;

struct KatanaResolvedStyleInternal;

static void* reg_alloc(struct KatanaResolvedStyleInternal* st, size_t size);
static void* reg_realloc(struct KatanaResolvedStyleInternal* st, void* old,
                         size_t size);
static char* reg_strndup(struct KatanaResolvedStyleInternal* st,
                         const char* s, size_t n);
static char* reg_strdup(struct KatanaResolvedStyleInternal* st,
                        const char* s);

static void registry_push(PtrRegistry* reg, void* p) {
    if (reg->length == reg->capacity) {
        size_t nc = reg->capacity ? reg->capacity * 2 : 32;
        reg->data = (void**)realloc(reg->data, nc * sizeof(void*));
        assert(reg->data);
        reg->capacity = nc;
    }
    reg->data[reg->length++] = p;
}

/* ------------------------------------------------------------------ */
/* Character predicates                                                */
/* ------------------------------------------------------------------ */

static bool is_ws_char(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
           c == '\f' || c == '\v';
}

static bool is_hex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return c - 'A' + 10;
}

static bool is_name_code(char c) {
    unsigned char u = (unsigned char)c;
    if (u >= 0x80) return true; /* non-ASCII, UTF-8 passes through */
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-';
}

static bool is_name_start(char c) {
    unsigned char u = (unsigned char)c;
    if (u >= 0x80) return true;
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool starts_escape(const char* s, size_t n, size_t p) {
    if (p >= n || s[p] != '\\') return false;
    /* A backslash followed by a newline is not an escape. */
    if (p + 1 < n && is_ws_char(s[p + 1]) &&
        (s[p + 1] == '\n' || s[p + 1] == '\r' || s[p + 1] == '\f'))
        return false;
    return p + 1 < n;
}

static bool starts_ident(const char* s, size_t n, size_t p) {
    if (p >= n) return false;
    if (is_name_start(s[p]) || starts_escape(s, n, p)) return true;
    if (s[p] == '-' && p + 1 < n) {
        char c = s[p + 1];
        return c == '-' || is_name_start(c) || starts_escape(s, n, p + 1);
    }
    return false;
}

static bool starts_number(const char* s, size_t n, size_t p) {
    if (p >= n) return false;
    if (s[p] >= '0' && s[p] <= '9') return true;
    if (s[p] == '.' && p + 1 < n && s[p + 1] >= '0' && s[p + 1] <= '9')
        return true;
    if ((s[p] == '+' || s[p] == '-') && p + 1 < n) {
        char c = s[p + 1];
        if (c >= '0' && c <= '9') return true;
        if (c == '.' && p + 2 < n && s[p + 2] >= '0' && s[p + 2] <= '9')
            return true;
    }
    return false;
}

static char ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static bool ascii_ieq(const char* a, size_t na, const char* b, size_t nb) {
    if (na != nb) return false;
    for (size_t i = 0; i < na; i++)
        if (ascii_lower(a[i]) != ascii_lower(b[i])) return false;
    return true;
}

/* ------------------------------------------------------------------ */
/* Component-value tokens                                              */
/* ------------------------------------------------------------------ */

typedef enum {
    TK_IDENT,
    TK_FUNCTION,
    TK_AT_KEYWORD,
    TK_HASH,
    TK_STRING,
    TK_BAD_STRING,
    TK_URL,
    TK_BAD_URL,
    TK_DELIM,
    TK_NUMBER,
    TK_PERCENTAGE,
    TK_DIMENSION,
    TK_WS,
    TK_CDO,
    TK_CDC,
    TK_COLON,
    TK_SEMICOLON,
    TK_COMMA,
    TK_LBRACKET,
    TK_RBRACKET,
    TK_LPAREN,
    TK_RPAREN,
    TK_LBRACE,
    TK_RBRACE
} TokenType;

typedef struct {
    TokenType type;
    const char* raw; /* points into the input css or into owned storage */
    size_t len;
    char* text;      /* decoded value for TK_STRING / TK_URL, else NULL */
} Token;

typedef struct {
    Token* data;
    size_t length;
    size_t capacity;
} TokenVec;

static void tv_push(struct KatanaResolvedStyleInternal* st, TokenVec* v,
                    Token t) {
    if (v->length == v->capacity) {
        size_t nc = v->capacity ? v->capacity * 2 : 8;
        v->data = (Token*)reg_realloc(st, v->data, nc * sizeof(Token));
        v->capacity = nc;
    }
    v->data[v->length++] = t;
}

/* Decoded-text byte buffer. */
typedef struct {
    char* data;
    size_t length;
    size_t capacity;
} ByteVec;

static void bv_put(ByteVec* b, char c) {
    if (b->length == b->capacity) {
        size_t nc = b->capacity ? b->capacity * 2 : 16;
        b->data = (char*)realloc(b->data, nc);
        assert(b->data);
        b->capacity = nc;
    }
    b->data[b->length++] = c;
}

static void bv_put_utf8(ByteVec* b, unsigned cp) {
    if (cp <= 0x7F) {
        bv_put(b, (char)cp);
    } else if (cp <= 0x7FF) {
        bv_put(b, (char)(0xC0 | (cp >> 6)));
        bv_put(b, (char)(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        bv_put(b, (char)(0xE0 | (cp >> 12)));
        bv_put(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
        bv_put(b, (char)(0x80 | (cp & 0x3F)));
    } else {
        bv_put(b, (char)(0xF0 | (cp >> 18)));
        bv_put(b, (char)(0x80 | ((cp >> 12) & 0x3F)));
        bv_put(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
        bv_put(b, (char)(0x80 | (cp & 0x3F)));
    }
}

/* Consume an escape at *p (which points at the backslash). Returns the
 * decoded code point, 0xFFFD for a stray EOF/illegal codepoint. */
static unsigned consume_escape(const char* s, size_t n, size_t* p) {
    assert(s[*p] == '\\');
    (*p)++;
    if (*p >= n) return 0xFFFD;
    if (is_hex(s[*p])) {
        unsigned v = 0;
        int cnt = 0;
        while (cnt < 6 && *p < n && is_hex(s[*p])) {
            v = v * 16 + (unsigned)hex_value(s[*p]);
            (*p)++;
            cnt++;
        }
        if (*p < n && is_ws_char(s[*p])) (*p)++;
        if (v == 0 || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF))
            return 0xFFFD;
        return v;
    }
    return (unsigned char)s[(*p)++];
}

/* ------------------------------------------------------------------ */
/* Tokenizer                                                           */
/* ------------------------------------------------------------------ */

static void skip_comments_and_ws(const char* s, size_t n, size_t* p,
                                 bool eat_semicolons) {
    for (;;) {
        while (*p < n && (is_ws_char(s[*p]) ||
                          (eat_semicolons && s[*p] == ';')))
            (*p)++;
        if (*p + 1 < n && s[*p] == '/' && s[*p + 1] == '*') {
            *p += 2;
            while (*p < n &&
                   !(*p + 1 < n && s[*p] == '*' && s[*p + 1] == '/'))
                (*p)++;
            if (*p < n) *p += 2;
            continue;
        }
        break;
    }
}

/* Consume an identifier/name run (escapes included); returns its length. */
static size_t consume_name_run(const char* s, size_t n, size_t p) {
    size_t start = p;
    while (p < n) {
        if (is_name_code(s[p])) {
            p++;
        } else if (starts_escape(s, n, p)) {
            size_t q = p;
            consume_escape(s, n, &q);
            p = q;
        } else {
            break;
        }
    }
    return p - start;
}

static size_t consume_number_run(const char* s, size_t n, size_t p) {
    if (p < n && (s[p] == '+' || s[p] == '-')) p++;
    while (p < n && s[p] >= '0' && s[p] <= '9') p++;
    if (p + 1 < n && s[p] == '.' && s[p + 1] >= '0' && s[p + 1] <= '9') {
        p += 2;
        while (p < n && s[p] >= '0' && s[p] <= '9') p++;
    }
    if (p < n && (s[p] == 'e' || s[p] == 'E')) {
        size_t q = p + 1;
        if (q < n && (s[q] == '+' || s[q] == '-')) q++;
        if (q < n && s[q] >= '0' && s[q] <= '9') {
            p = q + 1;
            while (p < n && s[p] >= '0' && s[p] <= '9') p++;
        }
    }
    return p;
}

/* Decode a quoted string whose opening quote is at s[*p]. On return *p
 * points just past the closing quote (or at EOF for a bad string). */
static char* decode_quoted(struct KatanaResolvedStyleInternal* st,
                           const char* s, size_t n, size_t* p, bool* bad) {
    char quote = s[*p];
    (*p)++;
    ByteVec out = {NULL, 0, 0};
    *bad = false;
    while (*p < n) {
        char c = s[*p];
        if (c == quote) {
            (*p)++;
            char* r = reg_strndup(st, out.data ? out.data : "", out.length);
            free(out.data);
            return r;
        }
        if (c == '\n' || c == '\r' || c == '\f') {
            *bad = true;
            free(out.data);
            /* bad string: consume to end of input */
            while (*p < n) (*p)++;
            return NULL;
        }
        if (c == '\\') {
            if (*p + 1 < n &&
                (s[*p + 1] == '\n' || s[*p + 1] == '\r' ||
                 s[*p + 1] == '\f')) {
                *p += 2;
                if (*p >= 2 && s[*p - 2] == '\r' && *p < n &&
                    s[*p - 1] == '\n' && s[*p] == '\n')
                    (*p)++;
                continue;
            }
            if (*p + 1 >= n) { /* trailing backslash: ignored */
                (*p)++;
                continue;
            }
            bv_put_utf8(&out, consume_escape(s, n, p));
            continue;
        }
        bv_put(&out, c);
        (*p)++;
    }
    *bad = true; /* unclosed */
    free(out.data);
    return NULL;
}

/* Parse the body of url( once the opening paren has been consumed.
 * Emits either a TK_URL / TK_BAD_URL token, or, when the argument starts
 * with a quote, leaves the input positioned on the quote and returns
 * false so the caller emits a plain url FUNCTION token instead. */
static bool parse_url_body(struct KatanaResolvedStyleInternal* st,
                           const char* s, size_t n, size_t* p,
                           size_t func_start, Token* out) {
    skip_comments_and_ws(s, n, p, false);
    if (*p >= n) {
        out->type = TK_BAD_URL;
        out->raw = s + func_start;
        out->len = n - func_start;
        out->text = NULL;
        return true;
    }
    if (s[*p] == '"' || s[*p] == '\'') {
        *p = func_start + 3; /* back to right after "url" */
        return false;
    }
    if (s[*p] == ')') {
        (*p)++;
        out->type = TK_URL;
        out->raw = s + func_start;
        out->len = *p - func_start;
        out->text = reg_strndup(st, "", 0);
        return true;
    }
    ByteVec body = {NULL, 0, 0};
    bool bad = false;
    for (;;) {
        if (*p >= n) {
            bad = true;
            break;
        }
        char c = s[*p];
        if (c == ')') {
            (*p)++;
            break;
        }
        if (is_ws_char(c)) {
            skip_comments_and_ws(s, n, p, false);
            if (*p < n && s[*p] == ')') {
                (*p)++;
                break;
            }
            /* Internal whitespace followed by more content: a parse error,
             * the whole thing becomes a bad-url token. */
            bad = true;
            while (*p < n && s[*p] != ')') (*p)++;
            if (*p < n) (*p)++;
            break;
        }
        if (c == '"' || c == '\'' || c == '(' ||
            (unsigned char)c <= 0x1F || c == 0x7F) {
            bad = true;
            while (*p < n && s[*p] != ')') (*p)++;
            if (*p < n) (*p)++;
            break;
        }
        if (c == '\\') {
            if (!starts_escape(s, n, *p)) {
                bad = true;
                while (*p < n && s[*p] != ')') (*p)++;
                if (*p < n) (*p)++;
                break;
            }
            bv_put_utf8(&body, consume_escape(s, n, p));
            continue;
        }
        bv_put(&body, c);
        (*p)++;
    }
    out->raw = s + func_start;
    out->len = *p - func_start;
    if (bad) {
        free(body.data);
        out->type = TK_BAD_URL;
        out->text = NULL;
    } else {
        out->type = TK_URL;
        out->text = reg_strndup(st, body.data ? body.data : "", body.length);
        free(body.data);
    }
    return true;
}

static Token next_token_raw(struct KatanaResolvedStyleInternal* st,
                            const char* s, size_t n, size_t* p) {
    Token t = {TK_DELIM, NULL, 0, NULL};
    skip_comments_and_ws(s, n, p, false);
    if (*p >= n) {
        t.type = TK_DELIM;
        t.raw = s + n;
        t.len = 0;
        return t;
    }
    size_t start = *p;
    char c = s[*p];

    if (is_ws_char(c)) {
        while (*p < n && is_ws_char(s[*p])) (*p)++;
        t.type = TK_WS;
        t.raw = s + start;
        t.len = *p - start;
        return t;
    }
    if (c == '"' || c == '\'') {
        bool bad;
        char* text = decode_quoted(st, s, n, p, &bad);
        t.raw = s + start;
        t.len = *p - start;
        if (bad) {
            t.type = TK_BAD_STRING;
        } else {
            t.type = TK_STRING;
            t.text = text;
        }
        return t;
    }
    if (c == '#') {
        (*p)++;
        if (*p < n && (is_name_code(s[*p]) || starts_escape(s, n, *p)))
            *p += consume_name_run(s, n, *p);
        t.type = TK_HASH;
        t.raw = s + start;
        t.len = *p - start;
        return t;
    }
    if (c == '(') { (*p)++; t.type = TK_LPAREN; t.raw = s + start; t.len = 1; return t; }
    if (c == ')') { (*p)++; t.type = TK_RPAREN; t.raw = s + start; t.len = 1; return t; }
    if (c == '[') { (*p)++; t.type = TK_LBRACKET; t.raw = s + start; t.len = 1; return t; }
    if (c == ']') { (*p)++; t.type = TK_RBRACKET; t.raw = s + start; t.len = 1; return t; }
    if (c == '{') { (*p)++; t.type = TK_LBRACE; t.raw = s + start; t.len = 1; return t; }
    if (c == '}') { (*p)++; t.type = TK_RBRACE; t.raw = s + start; t.len = 1; return t; }
    if (c == ',') { (*p)++; t.type = TK_COMMA; t.raw = s + start; t.len = 1; return t; }
    if (c == ':') { (*p)++; t.type = TK_COLON; t.raw = s + start; t.len = 1; return t; }
    if (c == ';') { (*p)++; t.type = TK_SEMICOLON; t.raw = s + start; t.len = 1; return t; }

    if (c == '<' && *p + 3 < n && memcmp(s + *p, "<!--", 4) == 0) {
        *p += 4; t.type = TK_CDO; t.raw = s + start; t.len = 4; return t;
    }
    if (c == '-' && *p + 2 < n && s[*p + 1] == '-' && s[*p + 2] == '>') {
        *p += 3; t.type = TK_CDC; t.raw = s + start; t.len = 3; return t;
    }
    if (c == '@') {
        (*p)++;
        if (starts_ident(s, n, *p)) {
            size_t nl = consume_name_run(s, n, *p);
            *p += nl;
            t.type = TK_AT_KEYWORD;
            t.raw = s + start;
            t.len = *p - start;
            return t;
        }
        t.type = TK_DELIM; t.raw = s + start; t.len = 1; return t;
    }
    if (c == '\\') {
        if (starts_escape(s, n, *p)) {
            *p += consume_name_run(s, n, *p);
            if (*p < n && s[*p] == '(') {
                (*p)++;
                t.type = TK_FUNCTION;
                t.raw = s + start;
                t.len = *p - start;
                return t;
            }
            t.type = TK_IDENT;
            t.raw = s + start;
            t.len = *p - start;
            return t;
        }
        (*p)++;
        t.type = TK_DELIM; t.raw = s + start; t.len = 1; return t;
    }
    if (starts_number(s, n, *p)) {
        size_t after = consume_number_run(s, n, *p);
        if (after < n && (is_name_start(s[after]) || s[after] == '-' ||
                          starts_escape(s, n, after))) {
            after += consume_name_run(s, n, after);
            *p = after;
            t.type = TK_DIMENSION;
        } else if (after < n && s[after] == '%') {
            *p = after + 1;
            t.type = TK_PERCENTAGE;
        } else {
            *p = after;
            t.type = TK_NUMBER;
        }
        t.raw = s + start;
        t.len = *p - start;
        return t;
    }
    if (starts_ident(s, n, *p)) {
        size_t nl = consume_name_run(s, n, *p);
        size_t after = *p + nl;
        if (after < n && s[after] == '(' &&
            ascii_ieq(s + start, nl, "url", 3)) {
            *p = after + 1;
            if (parse_url_body(st, s, n, p, start, &t)) return t;
            /* quoted argument: fall through as a FUNCTION token */
        }
        *p = after;
        if (*p < n && s[*p] == '(') {
            (*p)++;
            t.type = TK_FUNCTION;
            t.raw = s + start;
            t.len = *p - start;
            return t;
        }
        t.type = TK_IDENT;
        t.raw = s + start;
        t.len = nl;
        return t;
    }

    /* Anything else is a delim token. */
    (*p)++;
    t.type = TK_DELIM;
    t.raw = s + start;
    t.len = 1;
    return t;
}

/* ------------------------------------------------------------------ */
/* Declarations and cascade                                            */
/* ------------------------------------------------------------------ */

/* Tokenize then copy raw spans into style-owned storage, so the result
 * never points into the caller's (mutable) input buffer. */
static Token next_token(struct KatanaResolvedStyleInternal* st,
                        const char* s, size_t n, size_t* p) {
    Token t = next_token_raw(st, s, n, p);
    if (t.len)
        t.raw = reg_strndup(st, t.raw, t.len);
    return t;
}

typedef struct Declaration {
    char* name;
    bool custom;
    bool important;
    bool frozen;       /* inherited custom whose value is already computed */
    TokenVec value;
    size_t order;
    struct Declaration* next;
} Declaration;

struct KatanaResolvedStyleInternal {
    PtrRegistry allocs;
    Declaration* decls;          /* cascade winners, parse order */
    size_t order_counter;

    /* resolved custom properties */
    Declaration** customs;
    size_t customs_count;
    int* custom_state;           /* 0 unresolved, 1 resolving, 2 valid, 3 invalid */
    TokenVec* custom_cache;
    size_t** custom_edges;       /* dependency graph (indices) */
    size_t* custom_edge_counts;
    bool* cyclic;

    /* frozen custom-property values inherited from the parent (by value) */
    char** inherit_names;
    TokenVec* inherit_values;
    size_t inherit_count;

    /* resolved normal properties, built lazily then cached */
    struct ResolvedProp* props;
    size_t props_count;
};

typedef struct ResolvedProp {
    char* name;                  /* ASCII-lowercased */
    bool valid;
    char* value;                 /* normalized serialization, "" if empty */
    KatanaArray* urls;           /* non-NULL empty array when valid */
} ResolvedProp;

/* Allocation wrappers: every block is registered so destroy() frees it. */
static void* reg_alloc(struct KatanaResolvedStyleInternal* st, size_t size) {
    void* p = calloc(1, size ? size : 1);
    assert(p);
    registry_push(&st->allocs, p);
    return p;
}

/* Tracked realloc: keeps the registry slot pointing at the live block. */
static void* reg_realloc(struct KatanaResolvedStyleInternal* st, void* old,
                         size_t size) {
    if (!old) return reg_alloc(st, size);
    void* p = realloc(old, size ? size : 1);
    assert(p);
    for (size_t i = 0; i < st->allocs.length; i++) {
        if (st->allocs.data[i] == old) {
            st->allocs.data[i] = p;
            break;
        }
    }
    return p;
}

static char* reg_strdup(struct KatanaResolvedStyleInternal* st,
                        const char* s) {
    return reg_strndup(st, s, strlen(s));
}

static char* reg_strndup(struct KatanaResolvedStyleInternal* st,
                         const char* s, size_t n) {
    char* p = (char*)malloc(n + 1);
    assert(p);
    memcpy(p, s, n);
    p[n] = '\0';
    registry_push(&st->allocs, p);
    return p;
}

static Declaration* decl_find(struct KatanaResolvedStyleInternal* st,
                              const char* name, bool custom) {
    for (Declaration* d = st->decls; d; d = d->next) {
        if (d->custom != custom) continue;
        if (custom) {
            if (strcmp(d->name, name) == 0) return d;
        } else if (ascii_ieq(d->name, strlen(d->name), name,
                             strlen(name))) {
            return d;
        }
    }
    return NULL;
}

static void decl_store(struct KatanaResolvedStyleInternal* st,
                       const char* name, size_t name_len, bool custom,
                       bool important, TokenVec value) {
    Declaration* existing = NULL;
    char* name_copy;
    if (custom) {
        name_copy = reg_strndup(st, name, name_len);
        existing = decl_find(st, name_copy, true);
    } else {
        name_copy = (char*)reg_alloc(st, name_len + 1);
        for (size_t i = 0; i < name_len; i++)
            name_copy[i] = (char)ascii_lower(name[i]);
        name_copy[name_len] = '\0';
        existing = decl_find(st, name_copy, false);
    }

    if (existing) {
        if (!existing->important || important) {
            /* important beats normal; equal importance: later wins */
            existing->important = important;
            existing->frozen = false;
            existing->value = value;
            existing->order = st->order_counter++;
        }
        return;
    }

    Declaration* d = (Declaration*)reg_alloc(st, sizeof(Declaration));
    d->name = name_copy;
    d->custom = custom;
    d->important = important;
    d->value = value;
    d->order = st->order_counter++;
    d->next = st->decls;
    st->decls = d;
}

/* A top-level !important marker sits at paren depth 0 and leaves only
 * whitespace before the end. */
static bool strip_important(TokenVec* v) {
    int depth = 0;
    for (size_t i = 0; i < v->length; i++) {
        TokenType ty = v->data[i].type;
        if (ty == TK_FUNCTION || ty == TK_LPAREN || ty == TK_LBRACKET ||
            ty == TK_LBRACE)
            depth++;
        else if (ty == TK_RPAREN || ty == TK_RBRACKET || ty == TK_RBRACE)
            depth--;
        else if (depth == 0 && ty == TK_DELIM && v->data[i].len == 1 &&
                 v->data[i].raw[0] == '!') {
            size_t j = i + 1;
            while (j < v->length && v->data[j].type == TK_WS) j++;
            if (j < v->length && v->data[j].type == TK_IDENT &&
                ascii_ieq(v->data[j].raw, v->data[j].len,
                          "important", 9)) {
                size_t k = j + 1;
                while (k < v->length && v->data[k].type == TK_WS) k++;
                if (k == v->length) {
                    v->length = i;
                    return true;
                }
            }
        }
    }
    return false;
}

static void parse_declarations(struct KatanaResolvedStyleInternal* st,
                               const char* css, size_t len) {
    size_t p = 0;
    for (;;) {
        skip_comments_and_ws(css, len, &p, true);
        if (p >= len) break;

        size_t name_pos = p;
        (void)name_pos;
        Token first = next_token(st, css, len, &p);
        if (first.type != TK_IDENT) {
            /* recovery: skip to the next top-level semicolon */
            int depth = 0;
            while (p < len) {
                Token t = next_token(st, css, len, &p);
                if (t.len == 0) break;
                if (t.type == TK_FUNCTION || t.type == TK_LPAREN ||
                    t.type == TK_LBRACKET || t.type == TK_LBRACE)
                    depth++;
                else if (t.type == TK_RPAREN || t.type == TK_RBRACKET ||
                         t.type == TK_RBRACE)
                    depth--;
                else if (depth == 0 && t.type == TK_SEMICOLON)
                    break;
            }
            continue;
        }

        /* colon at top level */
        bool have_colon = false;
        int depth = 0;
        size_t q = p;
        while (q < len) {
            Token t = next_token(st, css, len, &q);
            if (t.len == 0) break;
            if (t.type == TK_FUNCTION || t.type == TK_LPAREN ||
                t.type == TK_LBRACKET || t.type == TK_LBRACE)
                depth++;
            else if (t.type == TK_RPAREN || t.type == TK_RBRACKET ||
                     t.type == TK_RBRACE)
                depth--;
            else if (depth == 0 && t.type == TK_COLON) {
                have_colon = true;
                break;
            }
            if (depth == 0 && t.type == TK_SEMICOLON) break;
        }
        if (!have_colon) {
            p = q; /* malformed remainder, resume recovery at next ';' */
            continue;
        }
        p = q;

        TokenVec value = {NULL, 0, 0};
        depth = 0;
        while (p < len) {
            Token t = next_token(st, css, len, &p);
            if (t.len == 0) break;
            if (t.type == TK_FUNCTION || t.type == TK_LPAREN ||
                t.type == TK_LBRACKET || t.type == TK_LBRACE)
                depth++;
            else if (t.type == TK_RPAREN || t.type == TK_RBRACKET ||
                     t.type == TK_RBRACE)
                depth--;
            else if (depth == 0 && t.type == TK_SEMICOLON)
                break;
            tv_push(st, &value, t);
        }

        bool important = strip_important(&value);
        bool custom = first.len >= 2 && first.raw[0] == '-' &&
                      first.raw[1] == '-';
        decl_store(st, first.raw, first.len, custom, important, value);
    }
}

/* ------------------------------------------------------------------ */
/* Variable dependency graph and cycle detection                       */
/* ------------------------------------------------------------------ */

static bool token_is_var(const Token* t) {
    return t->type == TK_FUNCTION &&
           ascii_ieq(t->raw, t->len - 1, "var", 3);
}

/* Match the paren opened by tokens[func]; returns RPAREN index. */
static size_t match_paren(const Token* tk, size_t n, size_t func) {
    int depth = 0;
    for (size_t i = func; i < n; i++) {
        TokenType ty = tk[i].type;
        if (ty == TK_FUNCTION || ty == TK_LPAREN || ty == TK_LBRACKET ||
            ty == TK_LBRACE)
            depth++;
        else if (ty == TK_RPAREN || ty == TK_RBRACKET || ty == TK_RBRACE) {
            depth--;
            if (depth == 0) return i;
        }
    }
    return n;
}

/* Extract the custom-property name of a var() call; NULL if malformed.
 * Also reports the first top-level comma (fallback start), or end. */
static const Token* var_name_token(const Token* tk, size_t func,
                                   size_t close, size_t* comma) {
    *comma = close;
    size_t i = func + 1;
    while (i < close && tk[i].type == TK_WS) i++;
    if (i >= close || tk[i].type != TK_IDENT || tk[i].len < 2 ||
        tk[i].raw[0] != '-' || tk[i].raw[1] != '-')
        return NULL;
    const Token* name = &tk[i];
    i++;
    while (i < close && tk[i].type == TK_WS) i++;
    if (i < close && tk[i].type != TK_COMMA)
        return NULL; /* junk between the name and the fallback: malformed */
    if (i < close && tk[i].type == TK_COMMA) *comma = i;
    return name;
}

static void graph_add_edge(struct KatanaResolvedStyleInternal* st,
                           size_t node, const char* name, size_t name_len) {
    for (size_t i = 0; i < st->customs_count; i++) {
        if (strlen(st->customs[i]->name) == name_len &&
            memcmp(st->customs[i]->name, name, name_len) == 0) {
            size_t* edges = st->custom_edges[node];
            size_t cnt = st->custom_edge_counts[node];
            for (size_t k = 0; k < cnt; k++)
                if (edges[k] == i) return; /* dedup */
            edges = (size_t*)reg_realloc(st, edges,
                                         (cnt + 1) * sizeof(size_t));
            st->custom_edges[node] = edges;
            st->custom_edges[node][cnt] = i;
            st->custom_edge_counts[node] = cnt + 1;
            return;
        }
    }
}

/* Collect every var() reference (fallbacks and nested calls included) in
 * a token range. The whole range is walked rather than skipping a matched
 * var(), because references inside fallbacks also create dependencies. */
static void graph_scan(struct KatanaResolvedStyleInternal* st, size_t node,
                       const Token* tk, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (token_is_var(&tk[i])) {
            size_t close = match_paren(tk, n, i);
            size_t comma;
            const Token* name = var_name_token(tk, i, close, &comma);
            if (name)
                graph_add_edge(st, node, name->raw, name->len);
        }
    }
}

/* DFS: can node `target` be reached again from node `from`? */
static bool reaches_node(struct KatanaResolvedStyleInternal* st, size_t from,
                         size_t target, unsigned char* seen) {
    seen[from] = 1;
    for (size_t k = 0; k < st->custom_edge_counts[from]; k++) {
        size_t to = st->custom_edges[from][k];
        if (to == target) return true;
        if (!seen[to] && reaches_node(st, to, target, seen)) return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Substitution                                                        */
/* ------------------------------------------------------------------ */

static bool substitute_range(struct KatanaResolvedStyleInternal* st,
                             const Token* in, size_t n, TokenVec* out);

static int custom_index(struct KatanaResolvedStyleInternal* st,
                        const char* name, size_t name_len) {
    for (size_t i = 0; i < st->customs_count; i++) {
        if (strlen(st->customs[i]->name) == name_len &&
            memcmp(st->customs[i]->name, name, name_len) == 0)
            return (int)i;
    }
    return -1;
}

/* Look up a frozen value inherited from the parent. Returns NULL if no
 * such inherited custom property. */
static TokenVec* inherited_lookup(struct KatanaResolvedStyleInternal* st,
                                  const char* name, size_t name_len) {
    for (size_t i = 0; i < st->inherit_count; i++) {
        if (strlen(st->inherit_names[i]) == name_len &&
            memcmp(st->inherit_names[i], name, name_len) == 0)
            return &st->inherit_values[i];
    }
    return NULL;
}

static bool is_single_keyword(const TokenVec* v, const char* kw) {
    size_t i = 0;
    while (i < v->length && v->data[i].type == TK_WS) i++;
    if (i >= v->length || v->data[i].type != TK_IDENT ||
        !ascii_ieq(v->data[i].raw, v->data[i].len, kw, strlen(kw)))
        return false;
    i++;
    while (i < v->length && v->data[i].type == TK_WS) i++;
    return i == v->length;
}

static bool resolve_custom(struct KatanaResolvedStyleInternal* st,
                           size_t idx, TokenVec* out);

static bool append_var(struct KatanaResolvedStyleInternal* st,
                       const Token* tk, size_t n, size_t func,
                       TokenVec* out) {
    size_t close = match_paren(tk, n, func);
    size_t comma;
    const Token* name = var_name_token(tk, func, close, &comma);
    if (!name) return false; /* malformed var(): invalid at computed time */

    int idx = custom_index(st, name->raw, name->len);
    TokenVec resolved = {NULL, 0, 0};
    bool ok = false;
    if (idx >= 0 && !st->cyclic[idx])
        ok = resolve_custom(st, (size_t)idx, &resolved);

    if (!ok && idx < 0) {
        /* Not declared here: a frozen parent value is visible. */
        TokenVec* inherited = inherited_lookup(st, name->raw, name->len);
        if (inherited) {
            resolved = *inherited;
            ok = true;
        }
    }

    if (ok) {
        for (size_t i = 0; i < resolved.length; i++)
            tv_push(st, out, resolved.data[i]);
        return true;
    }

    if (comma < close) {
        /* The whole sequence after the first top-level comma is the
         * fallback; an empty fallback is legal. */
        return substitute_range(st, tk + comma + 1,
                                close - comma - 1, out);
    }
    return false;
}

static bool substitute_range(struct KatanaResolvedStyleInternal* st,
                             const Token* in, size_t n, TokenVec* out) {
    for (size_t i = 0; i < n; i++) {
        if (token_is_var(&in[i])) {
            size_t close = match_paren(in, n, i);
            if (!append_var(st, in, n, i, out)) return false;
            i = close;
            continue;
        }
        tv_push(st, out, in[i]);
    }
    return true;
}

static bool resolve_custom(struct KatanaResolvedStyleInternal* st,
                           size_t idx, TokenVec* out) {
    int state = st->custom_state[idx];
    if (state == 2) {
        *out = st->custom_cache[idx];
        return true;
    }
    if (state == 3) return false;
    if (state == 1 || st->cyclic[idx]) {
        /* On the active resolution stack, or flagged as part of a
         * dependency cycle (fallback references count): every variable on
         * the cycle is invalid at computed-value time. */
        st->custom_state[idx] = 3;
        return false;
    }

    st->custom_state[idx] = 1;
    Declaration* d = st->customs[idx];
    TokenVec result = {NULL, 0, 0};
    bool ok;

    if (d->frozen) {
        /* Inherited value: substitution already happened in the parent. */
        result = d->value;
        ok = true;
    } else if (d->important) {
        /* css-variables-1: an !important custom property is invalid at
         * computed-value time (it still won the cascade). */
        ok = false;
    } else if (is_single_keyword(&d->value, "initial")) {
        ok = false; /* guaranteed-invalid value */
    } else if (is_single_keyword(&d->value, "inherit") ||
               is_single_keyword(&d->value, "unset")) {
        /* Take the parent's frozen value; the child's own declarations can
         * never re-expand an expression the parent already computed. */
        TokenVec* inherited = inherited_lookup(st, d->name,
                                               strlen(d->name));
        if (inherited) {
            result = *inherited;
            ok = true;
        } else {
            ok = false;
        }
    } else {
        ok = substitute_range(st, d->value.data, d->value.length, &result);
    }

    st->custom_state[idx] = ok ? 2 : 3;
    if (ok) st->custom_cache[idx] = result;
    if (ok) *out = result;
    return ok;
}

/* ------------------------------------------------------------------ */
/* Serialization and URL collection over resolved tokens               */
/* ------------------------------------------------------------------ */

static void serialize_into(struct KatanaResolvedStyleInternal* st,
                           const Token* tk, size_t n, ByteVec* out) {
    (void)st;
    for (size_t i = 0; i < n; i++) {
        if (tk[i].type == TK_WS) {
            if (out->length && out->data[out->length - 1] != ' ')
                bv_put(out, ' ');
            continue;
        }
        if (out->length && out->data[out->length - 1] != ' ')
            bv_put(out, ' ');
        const char* src = tk[i].type == TK_STRING ? NULL : tk[i].raw;
        if (src) {
            for (size_t k = 0; k < tk[i].len; k++) bv_put(out, src[k]);
        } else {
            /* strings keep their quotes in the normalized serialization */
            bv_put(out, '"');
            for (const char* p = tk[i].text; *p; p++) {
                if (*p == '"' || *p == '\\') bv_put(out, '\\');
                bv_put(out, *p);
            }
            bv_put(out, '"');
        }
    }
    while (out->length && out->data[out->length - 1] == ' ')
        out->length--;
}

static void urls_add(struct KatanaResolvedStyleInternal* st,
                     KatanaArray* arr, const char* s) {
    char* copy = reg_strdup(st, s);
    if (arr->length == arr->capacity) {
        size_t nc = arr->capacity ? arr->capacity * 2 : 4;
        arr->data = (void**)reg_realloc(st, arr->data,
                                        nc * sizeof(void*));
        arr->capacity = nc;
    }
    arr->data[arr->length++] = copy;
}

static void collect_urls(struct KatanaResolvedStyleInternal* st,
                         const Token* tk, size_t n, KatanaArray* out) {
    for (size_t i = 0; i < n; i++) {
        if (tk[i].type == TK_URL) {
            urls_add(st, out, tk[i].text ? tk[i].text : "");
        } else if (tk[i].type == TK_FUNCTION &&
                   ascii_ieq(tk[i].raw, tk[i].len - 1, "url", 3)) {
            /* url("...") with a quoted single-string argument */
            size_t close = match_paren(tk, n, i);
            size_t j = i + 1;
            while (j < close && tk[j].type == TK_WS) j++;
            size_t e = close;
            while (e > j && tk[e - 1].type == TK_WS) e--;
            if (j < close && e == j + 1 && tk[j].type == TK_STRING) {
                urls_add(st, out, tk[j].text ? tk[j].text : "");
            } else {
                collect_urls(st, tk + i + 1, close - i - 1, out);
            }
            i = close;
        } else if (tk[i].type == TK_FUNCTION || tk[i].type == TK_LPAREN ||
                   tk[i].type == TK_LBRACKET || tk[i].type == TK_LBRACE) {
            size_t close = match_paren(tk, n, i);
            collect_urls(st, tk + i + 1, close - i - 1, out);
            i = close;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Frozen parent inheritance                                           */
/* ------------------------------------------------------------------ */

static TokenVec tv_deep_copy(struct KatanaResolvedStyleInternal* st,
                             const TokenVec* src) {
    TokenVec dst = {NULL, 0, 0};
    for (size_t i = 0; i < src->length; i++) {
        Token t = src->data[i];
        if (t.text) t.text = reg_strdup(st, t.text);
        if (t.raw)
            t.raw = reg_strndup(st, t.raw, t.len);
        tv_push(st, &dst, t);
    }
    return dst;
}

/* Append an inherited custom declaration at the lowest cascade position.
 * Its value is already fully substituted and stays frozen. */
static void frozen_decl_store(struct KatanaResolvedStyleInternal* st,
                              const char* name, TokenVec value) {
    Declaration* d = (Declaration*)reg_alloc(st, sizeof(Declaration));
    d->name = reg_strdup(st, name);
    d->custom = true;
    d->important = false;
    d->frozen = true;
    d->value = value;
    d->order = st->order_counter++;
    d->next = NULL;
    if (!st->decls) {
        st->decls = d;
        return;
    }
    Declaration* tail = st->decls;
    while (tail->next) tail = tail->next;
    tail->next = d;
}

static void import_parent(struct KatanaResolvedStyleInternal* st,
                          const struct KatanaResolvedStyleInternal* parent) {
    if (!parent || !parent->customs_count) return;

    size_t cap = parent->customs_count;
    st->inherit_names = (char**)reg_alloc(st, cap * sizeof(char*));
    st->inherit_values = (TokenVec*)reg_alloc(st, cap * sizeof(TokenVec));
    st->inherit_count = 0;

    for (size_t i = 0; i < parent->customs_count; i++) {
        /* Only a valid, fully substituted parent value is inheritable;
         * invalid customs are skipped, as if the property were absent. */
        if (parent->custom_state[i] != 2) continue;

        TokenVec frozen = tv_deep_copy(st, &parent->custom_cache[i]);

        /* Explicit table used by the inherit/unset keywords. */
        st->inherit_names[st->inherit_count] =
            reg_strdup(st, parent->customs[i]->name);
        st->inherit_values[st->inherit_count] =
            tv_deep_copy(st, &parent->custom_cache[i]);
        st->inherit_count++;

        /* Also enter it as a frozen declaration so the variable resolves
         * directly and can be inherited again by a grandchild. A child
         * declaration of the same name overrides it via the cascade. */
        frozen_decl_store(st, parent->customs[i]->name, frozen);
    }
}

/* ------------------------------------------------------------------ */
/* Resolution driver                                                   */
/* ------------------------------------------------------------------ */

static void build_custom_tables(struct KatanaResolvedStyleInternal* st) {
    size_t count = 0;
    for (Declaration* d = st->decls; d; d = d->next)
        if (d->custom) count++;
    st->customs_count = count;
    if (!count) return;

    st->customs = (Declaration**)reg_alloc(st, count * sizeof(Declaration*));
    st->custom_state = (int*)reg_alloc(st, count * sizeof(int));
    st->custom_cache = (TokenVec*)reg_alloc(st, count * sizeof(TokenVec));
    st->custom_edges = (size_t**)reg_alloc(st, count * sizeof(size_t*));
    st->custom_edge_counts = (size_t*)reg_alloc(st, count * sizeof(size_t));
    st->cyclic = (bool*)reg_alloc(st, count * sizeof(bool));

    size_t i = 0;
    for (Declaration* d = st->decls; d; d = d->next) {
        if (d->custom) st->customs[i++] = d;
    }
    for (i = 0; i < count; i++) {
        graph_scan(st, i, st->customs[i]->value.data,
                   st->customs[i]->value.length);
    }
    unsigned char* seen = (unsigned char*)reg_alloc(st, count);
    for (i = 0; i < count; i++) {
        memset(seen, 0, count);
        st->cyclic[i] = reaches_node(st, i, i, seen);
    }

    /* Eagerly resolve every custom property so the parent style can be
     * destroyed as soon as this call returns. */
    for (i = 0; i < count; i++) {
        TokenVec v = {NULL, 0, 0};
        resolve_custom(st, i, &v);
    }
}

static ResolvedProp* prop_find(struct KatanaResolvedStyleInternal* st,
                               const char* property) {
    for (size_t i = 0; i < st->props_count; i++) {
        if (ascii_ieq(st->props[i].name, strlen(st->props[i].name),
                      property, strlen(property)))
            return &st->props[i];
    }
    return NULL;
}

static void resolve_property(struct KatanaResolvedStyleInternal* st,
                             Declaration* d) {
    ResolvedProp* rp = prop_find(st, d->name);
    if (rp) return;

    ResolvedProp* grown = (ResolvedProp*)reg_realloc(
        st, st->props, (st->props_count + 1) * sizeof(ResolvedProp));
    st->props = grown;
    rp = &st->props[st->props_count++];
    rp->name = reg_strdup(st, d->name);
    rp->valid = false;
    rp->value = NULL;
    rp->urls = NULL;

    TokenVec resolved = {NULL, 0, 0};
    if (!substitute_range(st, d->value.data, d->value.length, &resolved))
        return; /* computed-invalid: NULL value/urls, earlier decl kept lost */

    rp->valid = true;

    ByteVec ser = {NULL, 0, 0};
    serialize_into(st, resolved.data, resolved.length, &ser);
    rp->value = reg_strndup(st, ser.data ? ser.data : "", ser.length);
    free(ser.data);

    KatanaArray* urls = (KatanaArray*)reg_alloc(st, sizeof(KatanaArray));
    urls->data = NULL;
    urls->length = 0;
    urls->capacity = 0;
    collect_urls(st, resolved.data, resolved.length, urls);
    rp->urls = urls;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

KatanaResolvedStyle* katana_resolve_style(const char* css, size_t len,
                                          const KatanaResolvedStyle* parent) {
    KatanaResolvedStyle* st =
        (KatanaResolvedStyle*)calloc(1, sizeof(KatanaResolvedStyle));
    assert(st);
    /* The style struct itself is freed directly in destroy(). */

    if (parent) import_parent(st, parent);
    if (css && len) parse_declarations(st, css, len);
    build_custom_tables(st);
    return st;
}

static Declaration* find_winner(struct KatanaResolvedStyleInternal* st,
                                const char* property) {
    size_t plen = strlen(property);
    bool custom = plen >= 2 && property[0] == '-' && property[1] == '-';
    for (Declaration* d = st->decls; d; d = d->next) {
        if (d->custom != custom) continue;
        if (custom) {
            if (strcmp(d->name, property) == 0) return d;
        } else if (ascii_ieq(d->name, strlen(d->name), property, plen)) {
            return d;
        }
    }
    return NULL;
}

const char* katana_resolved_value(const KatanaResolvedStyle* style,
                                  const char* property) {
    if (!style || !property) return NULL;
    struct KatanaResolvedStyleInternal* st =
        (struct KatanaResolvedStyleInternal*)style;
    size_t plen = strlen(property);
    if (plen >= 2 && property[0] == '-' && property[1] == '-')
        return NULL; /* custom properties are not exposed as values */

    Declaration* d = find_winner(st, property);
    if (!d) return NULL;
    resolve_property(st, d);
    ResolvedProp* rp = prop_find(st, property);
    if (!rp || !rp->valid) return NULL;
    return rp->value;
}

const KatanaArray* katana_resolved_urls(const KatanaResolvedStyle* style,
                                        const char* property) {
    if (!style || !property) return NULL;
    struct KatanaResolvedStyleInternal* st =
        (struct KatanaResolvedStyleInternal*)style;
    size_t plen = strlen(property);
    if (plen >= 2 && property[1] == '-' && property[0] == '-')
        return NULL;

    Declaration* d = find_winner(st, property);
    if (!d) return NULL;
    resolve_property(st, d);
    ResolvedProp* rp = prop_find(st, property);
    if (!rp || !rp->valid) return NULL;
    return rp->urls;
}

void katana_resolved_style_destroy(KatanaResolvedStyle* style) {
    if (!style) return;
    for (size_t i = 0; i < style->allocs.length; i++)
        free(style->allocs.data[i]);
    free(style->allocs.data);
    free(style);
}
