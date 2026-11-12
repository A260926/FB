#include "mpc.h"

/*
** State Type
*/

static mpc_state_t mpc_state_invalid(void) {
  mpc_state_t s;
  s.pos = -1;
  s.row = -1;
  s.col = -1;
  s.term = 0;
  return s;
}

static mpc_state_t mpc_state_new(void) {
  mpc_state_t s;
  s.pos = 0;
  s.row = 0;
  s.col = 0;
  s.term = 0;
  return s;
}

/*
** Input Type
*/

/*
** In mpc the input type has three modes of
** operation: String, File and Pipe.
**
** String is easy. The whole contents are
** loaded into a buffer and scanned through.
** The cursor can jump around at will making
** backtracking easy.
**
** The second is a File which is also somewhat
** easy. The contents are never loaded into
** memory but backtracking can still be achieved
** by seeking in the file at different positions.
**
** The final mode is Pipe. This is the difficult
** one. As we assume pipes cannot be seeked - and
** only support a single character lookahead at
** any point, when the input is marked for a
** potential backtracking we start buffering any
** input.
**
** This means that if we are requested to seek
** back we can simply start reading from the
** buffer instead of the input.
**
** Of course using `mpc_predictive` will disable
** backtracking and make LL(1) grammars easy
** to parse for all input methods.
**
*/

enum {
  MPC_INPUT_STRING = 0,
  MPC_INPUT_FILE   = 1,
  MPC_INPUT_PIPE   = 2
};

enum {
  MPC_INPUT_MARKS_MIN = 32
};

enum {
  MPC_INPUT_MEM_NUM = 512
};

typedef struct {
  char mem[64];
} mpc_mem_t;

typedef struct {

  int type;
  char *filename;
  mpc_state_t state;

  char *string;
  char *buffer;
  FILE *file;

  int suppress;
  int backtrack;
  int marks_slots;
  int marks_num;
  mpc_state_t *marks;

  char *lasts;
  char last;

  size_t mem_index;
  char mem_full[MPC_INPUT_MEM_NUM];
  mpc_mem_t mem[MPC_INPUT_MEM_NUM];

} mpc_input_t;

static mpc_input_t *mpc_input_new_string(const char *filename, const char *string) {

  mpc_input_t *i = malloc(sizeof(mpc_input_t));

  i->filename = malloc(strlen(filename) + 1);
  strcpy(i->filename, filename);
  i->type = MPC_INPUT_STRING;

  i->state = mpc_state_new();

  i->string = malloc(strlen(string) + 1);
  strcpy(i->string, string);
  i->buffer = NULL;
  i->file = NULL;

  i->suppress = 0;
  i->backtrack = 1;
  i->marks_num = 0;
  i->marks_slots = MPC_INPUT_MARKS_MIN;
  i->marks = malloc(sizeof(mpc_state_t) * i->marks_slots);
  i->lasts = malloc(sizeof(char) * i->marks_slots);
  i->last = '\0';

  i->mem_index = 0;
  memset(i->mem_full, 0, sizeof(char) * MPC_INPUT_MEM_NUM);

  return i;
}

static mpc_input_t *mpc_input_new_nstring(const char *filename, const char *string, size_t length) {

  mpc_input_t *i = malloc(sizeof(mpc_input_t));

  i->filename = malloc(strlen(filename) + 1);
  strcpy(i->filename, filename);
  i->type = MPC_INPUT_STRING;

  i->state = mpc_state_new();

  i->string = malloc(length + 1);
  strncpy(i->string, string, length);
  i->string[length] = '\0';
  i->buffer = NULL;
  i->file = NULL;

  i->suppress = 0;
  i->backtrack = 1;
  i->marks_num = 0;
  i->marks_slots = MPC_INPUT_MARKS_MIN;
  i->marks = malloc(sizeof(mpc_state_t) * i->marks_slots);
  i->lasts = malloc(sizeof(char) * i->marks_slots);
  i->last = '\0';

  i->mem_index = 0;
  memset(i->mem_full, 0, sizeof(char) * MPC_INPUT_MEM_NUM);

  return i;

}

static mpc_input_t *mpc_input_new_pipe(const char *filename, FILE *pipe) {

  mpc_input_t *i = malloc(sizeof(mpc_input_t));

  i->filename = malloc(strlen(filename) + 1);
  strcpy(i->filename, filename);

  i->type = MPC_INPUT_PIPE;
  i->state = mpc_state_new();

  i->string = NULL;
  i->buffer = NULL;
  i->file = pipe;

  i->suppress = 0;
  i->backtrack = 1;
  i->marks_num = 0;
  i->marks_slots = MPC_INPUT_MARKS_MIN;
  i->marks = malloc(sizeof(mpc_state_t) * i->marks_slots);
  i->lasts = malloc(sizeof(char) * i->marks_slots);
  i->last = '\0';

  i->mem_index = 0;
  memset(i->mem_full, 0, sizeof(char) * MPC_INPUT_MEM_NUM);

  return i;

}

static mpc_input_t *mpc_input_new_file(const char *filename, FILE *file) {

  mpc_input_t *i = malloc(sizeof(mpc_input_t));

  i->filename = malloc(strlen(filename) + 1);
  strcpy(i->filename, filename);
  i->type = MPC_INPUT_FILE;
  i->state = mpc_state_new();

  i->string = NULL;
  i->buffer = NULL;
  i->file = file;

  i->suppress = 0;
  i->backtrack = 1;
  i->marks_num = 0;
  i->marks_slots = MPC_INPUT_MARKS_MIN;
  i->marks = malloc(sizeof(mpc_state_t) * i->marks_slots);
  i->lasts = malloc(sizeof(char) * i->marks_slots);
  i->last = '\0';

  i->mem_index = 0;
  memset(i->mem_full, 0, sizeof(char) * MPC_INPUT_MEM_NUM);

  return i;
}

static void mpc_input_delete(mpc_input_t *i) {

  free(i->filename);

  if (i->type == MPC_INPUT_STRING) { free(i->string); }
  if (i->type == MPC_INPUT_PIPE) { free(i->buffer); }

  free(i->marks);
  free(i->lasts);
  free(i);
}

static int mpc_mem_ptr(mpc_input_t *i, void *p) {
  return
    (char*)p >= (char*)(i->mem) &&
    (char*)p <  (char*)(i->mem) + (MPC_INPUT_MEM_NUM * sizeof(mpc_mem_t));
}

static void *mpc_malloc(mpc_input_t *i, size_t n) {
  size_t j;
  char *p;

  if (n > sizeof(mpc_mem_t)) { return malloc(n); }

  j = i->mem_index;
  do {
    if (!i->mem_full[i->mem_index]) {
      p = (void*)(i->mem + i->mem_index);
      i->mem_full[i->mem_index] = 1;
      i->mem_index = (i->mem_index+1) % MPC_INPUT_MEM_NUM;
      return p;
    }
    i->mem_index = (i->mem_index+1) % MPC_INPUT_MEM_NUM;
  } while (j != i->mem_index);

  return malloc(n);
}

static void *mpc_calloc(mpc_input_t *i, size_t n, size_t m) {
  char *x = mpc_malloc(i, n * m);
  memset(x, 0, n * m);
  return x;
}

static void mpc_free(mpc_input_t *i, void *p) {
  size_t j;
  if (!mpc_mem_ptr(i, p)) { free(p); return; }
  j = ((size_t)(((char*)p) - ((char*)i->mem))) / sizeof(mpc_mem_t);
  i->mem_full[j] = 0;
}

static void *mpc_realloc(mpc_input_t *i, void *p, size_t n) {

  char *q = NULL;

  if (!mpc_mem_ptr(i, p)) { return realloc(p, n); }

  if (n > sizeof(mpc_mem_t)) {
    q = malloc(n);
    memcpy(q, p, sizeof(mpc_mem_t));
    mpc_free(i, p);
    return q;
  }

  return p;
}

static void *mpc_export(mpc_input_t *i, void *p) {
  char *q = NULL;
  if (!mpc_mem_ptr(i, p)) { return p; }
  q = malloc(sizeof(mpc_mem_t));
  memcpy(q, p, sizeof(mpc_mem_t));
  mpc_free(i, p);
  return q;
}

static void mpc_input_backtrack_disable(mpc_input_t *i) { i->backtrack--; }
static void mpc_input_backtrack_enable(mpc_input_t *i) { i->backtrack++; }

static void mpc_input_suppress_disable(mpc_input_t *i) { i->suppress--; }
static void mpc_input_suppress_enable(mpc_input_t *i) { i->suppress++; }

static void mpc_input_mark(mpc_input_t *i) {

  if (i->backtrack < 1) { return; }

  i->marks_num++;

  if (i->marks_num > i->marks_slots) {
    i->marks_slots = i->marks_num + i->marks_num / 2;
    i->marks = realloc(i->marks, sizeof(mpc_state_t) * i->marks_slots);
    i->lasts = realloc(i->lasts, sizeof(char) * i->marks_slots);
  }

  i->marks[i->marks_num-1] = i->state;
  i->lasts[i->marks_num-1] = i->last;

  if (i->type == MPC_INPUT_PIPE && i->marks_num == 1) {
    i->buffer = calloc(1, 1);
  }

}

static void mpc_input_unmark(mpc_input_t *i) {
  int j;

  if (i->backtrack < 1) { return; }

  i->marks_num--;

  if (i->marks_slots > i->marks_num + i->marks_num / 2
  &&  i->marks_slots > MPC_INPUT_MARKS_MIN) {
    i->marks_slots =
      i->marks_num > MPC_INPUT_MARKS_MIN ?
      i->marks_num : MPC_INPUT_MARKS_MIN;
    i->marks = realloc(i->marks, sizeof(mpc_state_t) * i->marks_slots);
    i->lasts = realloc(i->lasts, sizeof(char) * i->marks_slots);
  }

  if (i->type == MPC_INPUT_PIPE && i->marks_num == 0) {
    for (j = strlen(i->buffer) - 1; j >= 0; j--)
      ungetc(i->buffer[j], i->file);

    free(i->buffer);
    i->buffer = NULL;
  }

}

static void mpc_input_rewind(mpc_input_t *i) {

  if (i->backtrack < 1) { return; }

  i->state = i->marks[i->marks_num-1];
  i->last  = i->lasts[i->marks_num-1];

  if (i->type == MPC_INPUT_FILE) {
    fseek(i->file, i->state.pos, SEEK_SET);
  }

  mpc_input_unmark(i);
}

static int mpc_input_buffer_in_range(mpc_input_t *i) {
  return i->state.pos < (long)(strlen(i->buffer) + i->marks[0].pos);
}

static char mpc_input_buffer_get(mpc_input_t *i) {
  return i->buffer[i->state.pos - i->marks[0].pos];
}

static char mpc_input_getc(mpc_input_t *i) {

  char c = '\0';

  switch (i->type) {

    case MPC_INPUT_STRING: return i->string[i->state.pos];
    case MPC_INPUT_FILE: c = fgetc(i->file); return c;
    case MPC_INPUT_PIPE:

      if (!i->buffer) { c = getc(i->file); return c; }

      if (i->buffer && mpc_input_buffer_in_range(i)) {
        c = mpc_input_buffer_get(i);
        return c;
      } else {
        c = getc(i->file);
        return c;
      }

    default: return c;
  }
}

static char mpc_input_peekc(mpc_input_t *i) {

  char c = '\0';

  switch (i->type) {
    case MPC_INPUT_STRING: return i->string[i->state.pos];
    case MPC_INPUT_FILE:

      c = fgetc(i->file);
      if (feof(i->file)) { return '\0'; }

      fseek(i->file, -1, SEEK_CUR);
      return c;

    case MPC_INPUT_PIPE:

      if (!i->buffer) {
        c = getc(i->file);
        if (feof(i->file)) { return '\0'; }
        ungetc(c, i->file);
        return c;
      }

      if (i->buffer && mpc_input_buffer_in_range(i)) {
        return mpc_input_buffer_get(i);
      } else {
        c = getc(i->file);
        if (feof(i->file)) { return '\0'; }
        ungetc(c, i->file);
        return c;
      }

    default: return c;
  }

}

static int mpc_input_terminated(mpc_input_t *i) {
  return mpc_input_peekc(i) == '\0';
}

static int mpc_input_failure(mpc_input_t *i, char c) {

  switch (i->type) {
    case MPC_INPUT_STRING: { break; }
    case MPC_INPUT_FILE: fseek(i->file, -1, SEEK_CUR); { break; }
    case MPC_INPUT_PIPE: {

      if (!i->buffer) { ungetc(c, i->file); break; }

      if (i->buffer && mpc_input_buffer_in_range(i)) {
        break;
      } else {
        ungetc(c, i->file);
      }
    }
    default: { break; }
  }
  return 0;
}

static int mpc_input_success(mpc_input_t *i, char c, char **o) {

  if (i->type == MPC_INPUT_PIPE
  &&  i->buffer && !mpc_input_buffer_in_range(i)) {
    i->buffer = realloc(i->buffer, strlen(i->buffer) + 2);
    i->buffer[strlen(i->buffer) + 1] = '\0';
    i->buffer[strlen(i->buffer) + 0] = c;
  }

  i->last = c;
  i->state.pos++;
  i->state.col++;

  if (c == '\n') {
    i->state.col = 0;
    i->state.row++;
  }

  if (o) {
    (*o) = mpc_malloc(i, 2);
    (*o)[0] = c;
    (*o)[1] = '\0';
  }

  return 1;
}

static int mpc_input_any(mpc_input_t *i, char **o) {
  char x;
  if (mpc_input_terminated(i)) { return 0; }
  x = mpc_input_getc(i);
  return mpc_input_success(i, x, o);
}

static int mpc_input_char(mpc_input_t *i, char c, char **o) {
  char x;
  if (mpc_input_terminated(i)) { return 0; }
  x = mpc_input_getc(i);
  return x == c ? mpc_input_success(i, x, o) : mpc_input_failure(i, x);
}

static int mpc_input_range(mpc_input_t *i, char c, char d, char **o) {
  char x;
  if (mpc_input_terminated(i)) { return 0; }
  x = mpc_input_getc(i);
  return x >= c && x <= d ? mpc_input_success(i, x, o) : mpc_input_failure(i, x);
}

static int mpc_input_oneof(mpc_input_t *i, const char *c, char **o) {
  char x;
  if (mpc_input_terminated(i)) { return 0; }
  x = mpc_input_getc(i);
  return strchr(c, x) != 0 ? mpc_input_success(i, x, o) : mpc_input_failure(i, x);
}

static int mpc_input_noneof(mpc_input_t *i, const char *c, char **o) {
  char x;
  if (mpc_input_terminated(i)) { return 0; }
  x = mpc_input_getc(i);
  return strchr(c, x) == 0 ? mpc_input_success(i, x, o) : mpc_input_failure(i, x);
}

static int mpc_input_satisfy(mpc_input_t *i, int(*cond)(char), char **o) {
  char x;
  if (mpc_input_terminated(i)) { return 0; }
  x = mpc_input_getc(i);
  return cond(x) ? mpc_input_success(i, x, o) : mpc_input_failure(i, x);
}

static int mpc_input_string(mpc_input_t *i, const char *c, char **o) {

  const char *x = c;

  mpc_input_mark(i);
  while (*x) {
    if (!mpc_input_char(i, *x, NULL)) {
      mpc_input_rewind(i);
      return 0;
    }
    x++;
  }
  mpc_input_unmark(i);

  *o = mpc_malloc(i, strlen(c) + 1);
  strcpy(*o, c);
  return 1;
}

static int mpc_input_anchor(mpc_input_t* i, int(*f)(char,char), char **o) {
  *o = NULL;
  return f(i->last, mpc_input_peekc(i));
}

static int mpc_input_soi(mpc_input_t* i, char **o) {
  *o = NULL;
  return i->last == '\0';
}

static int mpc_input_eoi(mpc_input_t* i, char **o) {
  *o = NULL;
  if (i->state.term) {
    return 0;
  } else if (mpc_input_terminated(i)) {
    i->state.term = 1;
    return 1;
  } else {
    return 0;
  }
}

static mpc_state_t *mpc_input_state_copy(mpc_input_t *i) {
  mpc_state_t *r = mpc_malloc(i, sizeof(mpc_state_t));
  memcpy(r, &i->state, sizeof(mpc_state_t));
  return r;
}

/*
** Error Type
*/

void mpc_err_delete(mpc_err_t *x) {
  int i;
  for (i = 0; i < x->expected_num; i++) { free(x->expected[i]); }
  free(x->expected);
  free(x->filename);
  free(x->failure);
  free(x);
}

void mpc_err_print(mpc_err_t *x) {
  mpc_err_print_to(x, stdout);
}

void mpc_err_print_to(mpc_err_t *x, FILE *f) {
  char *str = mpc_err_string(x);
  fprintf(f, "%s", str);
  free(str);
}

static void mpc_err_string_cat(char *buffer, int *pos, int *max, char const *fmt, ...) {
  /* TODO: Error Checking on Length */
  int left = ((*max) - (*pos));
  va_list va;
  va_start(va, fmt);
  if (left < 0) { left = 0;}
  (*pos) += vsprintf(buffer + (*pos), fmt, va);
  va_end(va);
}

static const char *mpc_err_char_unescape(char c, char char_unescape_buffer[4]) {

  char_unescape_buffer[0] = '\'';
  char_unescape_buffer[1] = ' ';
  char_unescape_buffer[2] = '\'';
  char_unescape_buffer[3] = '\0';

  switch (c) {
    case '\a': return "bell";
    case '\b': return "backspace";
    case '\f': return "formfeed";
    case '\r': return "carriage return";
    case '\v': return "vertical tab";
    case '\0': return "end of input";
    case '\n': return "newline";
    case '\t': return "tab";
    case ' ' : return "space";
    default:
      char_unescape_buffer[1] = c;
      return char_unescape_buffer;
  }

}

char *mpc_err_string(mpc_err_t *x) {

  int i;
  int pos = 0;
  int max = 1023;
  char *buffer = calloc(1, 1024);
  char char_unescape_buffer[4];

  if (x->failure) {
    mpc_err_string_cat(buffer, &pos, &max,
    "%s: error: %s\n", x->filename, x->failure);
    return buffer;
  }

  mpc_err_string_cat(buffer, &pos, &max,
    "%s:%li:%li: error: expected ", x->filename, x->state.row+1, x->state.col+1);

  if (x->expected_num == 0) { mpc_err_string_cat(buffer, &pos, &max, "ERROR: NOTHING EXPECTED"); }
  if (x->expected_num == 1) { mpc_err_string_cat(buffer, &pos, &max, "%s", x->expected[0]); }
  if (x->expected_num >= 2) {

    for (i = 0; i < x->expected_num-2; i++) {
      mpc_err_string_cat(buffer, &pos, &max, "%s, ", x->expected[i]);
    }

    mpc_err_string_cat(buffer, &pos, &max, "%s or %s",
      x->expected[x->expected_num-2],
      x->expected[x->expected_num-1]);
  }

  mpc_err_string_cat(buffer, &pos, &max, " at ");
  mpc_err_string_cat(buffer, &pos, &max, mpc_err_char_unescape(x->received, char_unescape_buffer));
  mpc_err_string_cat(buffer, &pos, &max, "\n");

  return realloc(buffer, strlen(buffer) + 1);
}

static mpc_err_t *mpc_err_new(mpc_input_t *i, const char *expected) {
  mpc_err_t *x;
  if (i->suppress) { return NULL; }
  x = mpc_malloc(i, sizeof(mpc_err_t));
  x->filename = mpc_malloc(i, strlen(i->filename) + 1);
  strcpy(x->filename, i->filename);
  x->state = i->state;
  x->expected_num = 1;
  x->expected = mpc_malloc(i, sizeof(char*));
  x->expected[0] = mpc_malloc(i, strlen(expected) + 1);
  strcpy(x->expected[0], expected);
  x->failure = NULL;
  x->received = mpc_input_peekc(i);
  return x;
}

static mpc_err_t *mpc_err_fail(mpc_input_t *i, const char *failure) {
  mpc_err_t *x;
  if (i->suppress) { return NULL; }
  x = mpc_malloc(i, sizeof(mpc_err_t));
  x->filename = mpc_malloc(i, strlen(i->filename) + 1);
  strcpy(x->filename, i->filename);
  x->state = i->state;
  x->expected_num = 0;
  x->expected = NULL;
  x->failure = mpc_malloc(i, strlen(failure) + 1);
  strcpy(x->failure, failure);
  x->received = ' ';
  return x;
}

static mpc_err_t *mpc_err_file(const char *filename, const char *failure) {
  mpc_err_t *x;
  x = malloc(sizeof(mpc_err_t));
  x->filename = malloc(strlen(filename) + 1);
  strcpy(x->filename, filename);
  x->state = mpc_state_new();
  x->expected_num = 0;
  x->expected = NULL;
  x->failure = malloc(strlen(failure) + 1);
  strcpy(x->failure, failure);
  x->received = ' ';
  return x;
}

static void mpc_err_delete_internal(mpc_input_t *i, mpc_err_t *x) {
  int j;
  if (x == NULL) { return; }
  for (j = 0; j < x->expected_num; j++) { mpc_free(i, x->expected[j]); }
  mpc_free(i, x->expected);
  mpc_free(i, x->filename);
  mpc_free(i, x->failure);
  mpc_free(i, x);
}

static mpc_err_t *mpc_err_export(mpc_input_t *i, mpc_err_t *x) {
  int j;
  for (j = 0; j < x->expected_num; j++) {
    x->expected[j] = mpc_export(i, x->expected[j]);
  }
  x->expected = mpc_export(i, x->expected);
  x->filename = mpc_export(i, x->filename);
  x->failure = mpc_export(i, x->failure);
  return mpc_export(i, x);
}

static int mpc_err_contains_expected(mpc_input_t *i, mpc_err_t *x, char *expected) {
  int j;
  (void)i;
  for (j = 0; j < x->expected_num; j++) {
    if (strcmp(x->expected[j], expected) == 0) { return 1; }
  }
  return 0;
}

static void mpc_err_add_expected(mpc_input_t *i, mpc_err_t *x, char *expected) {
  (void)i;
  x->expected_num++;
  x->expected = mpc_realloc(i, x->expected, sizeof(char*) * x->expected_num);
  x->expected[x->expected_num-1] = mpc_malloc(i, strlen(expected) + 1);
  strcpy(x->expected[x->expected_num-1], expected);
}

static mpc_err_t *mpc_err_or(mpc_input_t *i, mpc_err_t** x, int n) {

  int j, k, fst;
  mpc_err_t *e;

  fst = -1;
  for (j = 0; j < n; j++) {
    if (x[j] != NULL) { fst = j; }
  }

  if (fst == -1) { return NULL; }

  e = mpc_malloc(i, sizeof(mpc_err_t));
  e->state = mpc_state_invalid();
  e->expected_num = 0;
  e->expected = NULL;
  e->failure = NULL;
  e->filename = mpc_malloc(i, strlen(x[fst]->filename)+1);
  strcpy(e->filename, x[fst]->filename);

  for (j = 0; j < n; j++) {
    if (x[j] == NULL) { continue; }
    if (x[j]->state.pos > e->state.pos) { e->state = x[j]->state; }
  }

  for (j = 0; j < n; j++) {
    if (x[j] == NULL) { continue; }
    if (x[j]->state.pos < e->state.pos) { continue; }

    if (x[j]->failure) {
      e->failure = mpc_malloc(i, strlen(x[j]->failure)+1);
      strcpy(e->failure, x[j]->failure);
      break;
    }

    e->received = x[j]->received;

    for (k = 0; k < x[j]->expected_num; k++) {
      if (!mpc_err_contains_expected(i, e, x[j]->expected[k])) {
        mpc_err_add_expected(i, e, x[j]->expected[k]);
      }
    }
  }

  for (j = 0; j < n; j++) {
    if (x[j] == NULL) { continue; }
    mpc_err_delete_internal(i, x[j]);
  }

  return e;
}

static mpc_err_t *mpc_err_repeat(mpc_input_t *i, mpc_err_t *x, const char *prefix) {

  int j = 0;
  size_t l = 0;
  char *expect = NULL;

  if (x == NULL) { return NULL; }

  if (x->expected_num == 0) {
    expect = mpc_calloc(i, 1, 1);
    x->expected_num = 1;
    x->expected = mpc_realloc(i, x->expected, sizeof(char*) * x->expected_num);
    x->expected[0] = expect;
    return x;
  }

  else if (x->expected_num == 1) {
    expect = mpc_malloc(i, strlen(prefix) + strlen(x->expected[0]) + 1);
    strcpy(expect, prefix);
    strcat(expect, x->expected[0]);
    mpc_free(i, x->expected[0]);
    x->expected[0] = expect;
    return x;
  }

  else if (x->expected_num > 1) {

    l += strlen(prefix);
    for (j = 0; j < x->expected_num-2; j++) {
      l += strlen(x->expected[j]) + strlen(", ");
    }
    l += strlen(x->expected[x->expected_num-2]);
    l += strlen(" or ");
    l += strlen(x->expected[x->expected_num-1]);

    expect = mpc_malloc(i, l + 1);

    strcpy(expect, prefix);
    for (j = 0; j < x->expected_num-2; j++) {
      strcat(expect, x->expected[j]); strcat(expect, ", ");
    }
    strcat(expect, x->expected[x->expected_num-2]);
    strcat(expect, " or ");
    strcat(expect, x->expected[x->expected_num-1]);

    for (j = 0; j < x->expected_num; j++) { mpc_free(i, x->expected[j]); }

    x->expected_num = 1;
    x->expected = mpc_realloc(i, x->expected, sizeof(char*) * x->expected_num);
    x->expected[0] = expect;
    return x;
  }

  return NULL;
}

static mpc_err_t *mpc_err_many1(mpc_input_t *i, mpc_err_t *x) {
  return mpc_err_repeat(i, x, "one or more of ");
}

static mpc_err_t *mpc_err_count(mpc_input_t *i, mpc_err_t *x, int n) {
  mpc_err_t *y;
  int digits = n/10 + 1;
  char *prefix;
  prefix = mpc_malloc(i, digits + strlen(" of ") + 1);
  if (!prefix) {
    return NULL;
  }
  sprintf(prefix, "%i of ", n);
  y = mpc_err_repeat(i, x, prefix);
  mpc_free(i, prefix);
  return y;
}

static mpc_err_t *mpc_err_merge(mpc_input_t *i, mpc_err_t *x, mpc_err_t *y) {
  mpc_err_t *errs[2];
  errs[0] = x;
  errs[1] = y;
  return mpc_err_or(i, errs, 2);
}

/*
** Parser Type
*/

enum {
  MPC_TYPE_UNDEFINED  = 0,
  MPC_TYPE_PASS       = 1,
  MPC_TYPE_FAIL       = 2,
  MPC_TYPE_LIFT       = 3,
  MPC_TYPE_LIFT_VAL   = 4,
  MPC_TYPE_EXPECT     = 5,
  MPC_TYPE_ANCHOR     = 6,
  MPC_TYPE_STATE      = 7,

  MPC_TYPE_ANY        = 8,
  MPC_TYPE_SINGLE     = 9,
  MPC_TYPE_ONEOF      = 10,
  MPC_TYPE_NONEOF     = 11,
  MPC_TYPE_RANGE      = 12,
  MPC_TYPE_SATISFY    = 13,
  MPC_TYPE_STRING     = 14,

  MPC_TYPE_APPLY      = 15,
  MPC_TYPE_APPLY_TO   = 16,
  MPC_TYPE_PREDICT    = 17,
  MPC_TYPE_NOT        = 18,
  MPC_TYPE_MAYBE      = 19,
  MPC_TYPE_MANY       = 20,
  MPC_TYPE_MANY1      = 21,
  MPC_TYPE_COUNT      = 22,

  MPC_TYPE_OR         = 23,
  MPC_TYPE_AND        = 24,

  MPC_TYPE_CHECK      = 25,
  MPC_TYPE_CHECK_WITH = 26,

  MPC_TYPE_SOI        = 27,
  MPC_TYPE_EOI        = 28,

  MPC_TYPE_SEPBY1     = 29
};

typedef struct { char *m; } mpc_pdata_fail_t;
typedef struct { mpc_ctor_t lf; void *x; } mpc_pdata_lift_t;
typedef struct { mpc_parser_t *x; char *m; } mpc_pdata_expect_t;
typedef struct { int(*f)(char,char); } mpc_pdata_anchor_t;
typedef struct { char x; } mpc_pdata_single_t;
typedef struct { char x; char y; } mpc_pdata_range_t;
typedef struct { int(*f)(char); } mpc_pdata_satisfy_t;
typedef struct { char *x; } mpc_pdata_string_t;
typedef struct { mpc_parser_t *x; mpc_apply_t f; } mpc_pdata_apply_t;
typedef struct { mpc_parser_t *x; mpc_apply_to_t f; void *d; } mpc_pdata_apply_to_t;
typedef struct { mpc_parser_t *x; mpc_dtor_t dx; mpc_check_t f; char *e; } mpc_pdata_check_t;
typedef struct { mpc_parser_t *x; mpc_dtor_t dx; mpc_check_with_t f; void *d; char *e; } mpc_pdata_check_with_t;
typedef struct { mpc_parser_t *x; } mpc_pdata_predict_t;
typedef struct { mpc_parser_t *x; mpc_dtor_t dx; mpc_ctor_t lf; } mpc_pdata_not_t;
typedef struct { int n; mpc_fold_t f; mpc_parser_t *x; mpc_dtor_t dx; } mpc_pdata_repeat_t;
typedef struct { int n; mpc_parser_t **xs; } mpc_pdata_or_t;
typedef struct { int n; mpc_fold_t f; mpc_parser_t **xs; mpc_dtor_t *dxs;  } mpc_pdata_and_t;
typedef struct { int n; mpc_fold_t f; mpc_parser_t *x; mpc_parser_t *sep; } mpc_pdata_sepby1;

typedef union {
  mpc_pdata_fail_t fail;
  mpc_pdata_lift_t lift;
  mpc_pdata_expect_t expect;
  mpc_pdata_anchor_t anchor;
  mpc_pdata_single_t single;
  mpc_pdata_range_t range;
  mpc_pdata_satisfy_t satisfy;
  mpc_pdata_string_t string;
  mpc_pdata_apply_t apply;
  mpc_pdata_apply_to_t apply_to;
  mpc_pdata_check_t check;
  mpc_pdata_check_with_t check_with;
  mpc_pdata_predict_t predict;
  mpc_pdata_not_t not;
  mpc_pdata_repeat_t repeat;
  mpc_pdata_and_t and;
  mpc_pdata_or_t or;
  mpc_pdata_sepby1 sepby1;
} mpc_pdata_t;

struct mpc_parser_t {
  char *name;
  mpc_pdata_t data;
  char type;
  char retained;
};

static mpc_val_t *mpcf_input_nth_free(mpc_input_t *i, int n, mpc_val_t **xs, int x) {
  int j;
  for (j = 0; j < n; j++) { if (j != x) { mpc_free(i, xs[j]); } }
  return xs[x];
}

static mpc_val_t *mpcf_input_fst_free(mpc_input_t *i, int n, mpc_val_t **xs) { return mpcf_input_nth_free(i, n, xs, 0); }
static mpc_val_t *mpcf_input_snd_free(mpc_input_t *i, int n, mpc_val_t **xs) { return mpcf_input_nth_free(i, n, xs, 1); }
static mpc_val_t *mpcf_input_trd_free(mpc_input_t *i, int n, mpc_val_t **xs) { return mpcf_input_nth_free(i, n, xs, 2); }

static mpc_val_t *mpcf_input_strfold(mpc_input_t *i, int n, mpc_val_t **xs) {
  int j;
  size_t l = 0;
  if (n == 0) { return mpc_calloc(i, 1, 1); }
  for (j = 0; j < n; j++) { l += strlen(xs[j]); }
  xs[0] = mpc_realloc(i, xs[0], l + 1);
  for (j = 1; j < n; j++) { strcat(xs[0], xs[j]); mpc_free(i, xs[j]); }
  return xs[0];
}

static mpc_val_t *mpcf_input_state_ast(mpc_input_t *i, int n, mpc_val_t **xs) {
  mpc_state_t *s = ((mpc_state_t**)xs)[0];
  mpc_ast_t *a = ((mpc_ast_t**)xs)[1];
  a = mpc_ast_state(a, *s);
  mpc_free(i, s);
  (void) n;
  return a;
}

static mpc_val_t *mpc_parse_fold(mpc_input_t *i, mpc_fold_t f, int n, mpc_val_t **xs) {
  int j;
  if (f == mpcf_null)      { return mpcf_null(n, xs); }
  if (f == mpcf_fst)       { return mpcf_fst(n, xs); }
  if (f == mpcf_snd)       { return mpcf_snd(n, xs); }
  if (f == mpcf_trd)       { return mpcf_trd(n, xs); }
  if (f == mpcf_fst_free)  { return mpcf_input_fst_free(i, n, xs); }
  if (f == mpcf_snd_free)  { return mpcf_input_snd_free(i, n, xs); }
  if (f == mpcf_trd_free)  { return mpcf_input_trd_free(i, n, xs); }
  if (f == mpcf_strfold)   { return mpcf_input_strfold(i, n, xs); }
  if (f == mpcf_state_ast) { return mpcf_input_state_ast(i, n, xs); }
  for (j = 0; j < n; j++) { xs[j] = mpc_export(i, xs[j]); }
  return f(j, xs);
}

static mpc_val_t *mpcf_input_free(mpc_input_t *i, mpc_val_t *x) {
  mpc_free(i, x);
  return NULL;
}

static mpc_val_t *mpcf_input_str_ast(mpc_input_t *i, mpc_val_t *c) {
  mpc_ast_t *a = mpc_ast_new("", c);
  mpc_free(i, c);
  return a;
}

static mpc_val_t *mpc_parse_apply(mpc_input_t *i, mpc_apply_t f, mpc_val_t *x) {
  if (f == mpcf_free)     { return mpcf_input_free(i, x); }
  if (f == mpcf_str_ast)  { return mpcf_input_str_ast(i, x); }
  return f(mpc_export(i, x));
}

static mpc_val_t *mpc_parse_apply_to(mpc_input_t *i, mpc_apply_to_t f, mpc_val_t *x, mpc_val_t *d) {
  return f(mpc_export(i, x), d);
}

static void mpc_parse_dtor(mpc_input_t *i, mpc_dtor_t d, mpc_val_t *x) {
  if (d == free) { mpc_free(i, x); return; }
  d(mpc_export(i, x));
}

enum {
  MPC_PARSE_STACK_MIN = 4
};

#define MPC_SUCCESS(x) r->output = x; return 1
#define MPC_FAILURE(x) r->error = x; return 0
#define MPC_PRIMITIVE(x) \
  if (x) { MPC_SUCCESS(r->output); } \
  else { MPC_FAILURE(NULL); }

#define MPC_MAX_RECURSION_DEPTH 1000

static mpc_result_t *mpc_grow_results(mpc_input_t *i, int j, mpc_result_t *results_stk, mpc_result_t *results){
  mpc_result_t *tmp_results = results;

  if (j == MPC_PARSE_STACK_MIN) {
    int results_slots = j + j / 2;
    tmp_results = mpc_malloc(i, sizeof(mpc_result_t) * results_slots);
    memcpy(tmp_results, results_stk, sizeof(mpc_result_t) * MPC_PARSE_STACK_MIN);
  } else if (j >= MPC_PARSE_STACK_MIN) {
    int results_slots = j + j / 2;
    tmp_results = mpc_realloc(i, tmp_results, sizeof(mpc_result_t) * results_slots);
  }

  return tmp_results;
}

static int mpc_parse_run(mpc_input_t *i, mpc_parser_t *p, mpc_result_t *r, mpc_err_t **e, int depth) {

  int j = 0, k = 0;
  mpc_result_t results_stk[MPC_PARSE_STACK_MIN];
  mpc_result_t *results;

  if (depth == MPC_MAX_RECURSION_DEPTH)
  {
    MPC_FAILURE(mpc_err_fail(i, "Maximum recursion depth exceeded!"));
  }

  switch (p->type) {

    /* Basic Parsers */

    case MPC_TYPE_ANY:     MPC_PRIMITIVE(mpc_input_any(i, (char**)&r->output));
    case MPC_TYPE_SINGLE:  MPC_PRIMITIVE(mpc_input_char(i, p->data.single.x, (char**)&r->output));
    case MPC_TYPE_RANGE:   MPC_PRIMITIVE(mpc_input_range(i, p->data.range.x, p->data.range.y, (char**)&r->output));
    case MPC_TYPE_ONEOF:   MPC_PRIMITIVE(mpc_input_oneof(i, p->data.string.x, (char**)&r->output));
    case MPC_TYPE_NONEOF:  MPC_PRIMITIVE(mpc_input_noneof(i, p->data.string.x, (char**)&r->output));
    case MPC_TYPE_SATISFY: MPC_PRIMITIVE(mpc_input_satisfy(i, p->data.satisfy.f, (char**)&r->output));
    case MPC_TYPE_STRING:  MPC_PRIMITIVE(mpc_input_string(i, p->data.string.x, (char**)&r->output));
    case MPC_TYPE_ANCHOR:  MPC_PRIMITIVE(mpc_input_anchor(i, p->data.anchor.f, (char**)&r->output));
    case MPC_TYPE_SOI:     MPC_PRIMITIVE(mpc_input_soi(i, (char**)&r->output));
    case MPC_TYPE_EOI:     MPC_PRIMITIVE(mpc_input_eoi(i, (char**)&r->output));

    /* Other parsers */

    case MPC_TYPE_UNDEFINED: MPC_FAILURE(mpc_err_fail(i, "Parser Undefined!"));
    case MPC_TYPE_PASS:      MPC_SUCCESS(NULL);
    case MPC_TYPE_FAIL:      MPC_FAILURE(mpc_err_fail(i, p->data.fail.m));
    case MPC_TYPE_LIFT:      MPC_SUCCESS(p->data.lift.lf());
    case MPC_TYPE_LIFT_VAL:  MPC_SUCCESS(p->data.lift.x);
    case MPC_TYPE_STATE:     MPC_SUCCESS(mpc_input_state_copy(i));

    /* Application Parsers */

    case MPC_TYPE_APPLY:
      if (mpc_parse_run(i, p->data.apply.x, r, e, depth+1)) {
        MPC_SUCCESS(mpc_parse_apply(i, p->data.apply.f, r->output));
      } else {
        MPC_FAILURE(r->output);
      }

    case MPC_TYPE_APPLY_TO:
      if (mpc_parse_run(i, p->data.apply_to.x, r, e, depth+1)) {
        MPC_SUCCESS(mpc_parse_apply_to(i, p->data.apply_to.f, r->output, p->data.apply_to.d));
      } else {
        MPC_FAILURE(r->error);
      }

    case MPC_TYPE_CHECK:
      if (mpc_parse_run(i, p->data.check.x, r, e, depth+1)) {
        if (p->data.check.f(&r->output)) {
          MPC_SUCCESS(r->output);
        } else {
          mpc_parse_dtor(i, p->data.check.dx, r->output);
          MPC_FAILURE(mpc_err_fail(i, p->data.check.e));
        }
      } else {
        MPC_FAILURE(r->error);
      }

    case MPC_TYPE_CHECK_WITH:
      if (mpc_parse_run(i, p->data.check_with.x, r, e, depth+1)) {
        if (p->data.check_with.f(&r->output, p->data.check_with.d)) {
          MPC_SUCCESS(r->output);
        } else {
          mpc_parse_dtor(i, p->data.check.dx, r->output);
          MPC_FAILURE(mpc_err_fail(i, p->data.check_with.e));
        }
      } else {
        MPC_FAILURE(r->error);
      }

    case MPC_TYPE_EXPECT:
      mpc_input_suppress_enable(i);
      if (mpc_parse_run(i, p->data.expect.x, r, e, depth+1)) {
        mpc_input_suppress_disable(i);
        MPC_SUCCESS(r->output);
      } else {
        mpc_input_suppress_disable(i);
        MPC_FAILURE(mpc_err_new(i, p->data.expect.m));
      }

    case MPC_TYPE_PREDICT:
      mpc_input_backtrack_disable(i);
      if (mpc_parse_run(i, p->data.predict.x, r, e, depth+1)) {
        mpc_input_backtrack_enable(i);
        MPC_SUCCESS(r->output);
      } else {
        mpc_input_backtrack_enable(i);
        MPC_FAILURE(r->error);
      }

    /* Optional Parsers */

    /* TODO: Update Not Error Message */

    case MPC_TYPE_NOT:
      mpc_input_mark(i);
      mpc_input_suppress_enable(i);
      if (mpc_parse_run(i, p->data.not.x, r, e, depth+1)) {
        mpc_input_rewind(i);
        mpc_input_suppress_disable(i);
        mpc_parse_dtor(i, p->data.not.dx, r->output);
        MPC_FAILURE(mpc_err_new(i, "opposite"));
      } else {
        mpc_input_unmark(i);
        mpc_input_suppress_disable(i);
        MPC_SUCCESS(p->data.not.lf());
      }

    case MPC_TYPE_MAYBE:
      if (mpc_parse_run(i, p->data.not.x, r, e, depth+1)) {
        MPC_SUCCESS(r->output);
      } else {
        *e = mpc_err_merge(i, *e, r->error);
        MPC_SUCCESS(p->data.not.lf());
      }

    /* Repeat Parsers */

    case MPC_TYPE_MANY:

      results = results_stk;

      while (mpc_parse_run(i, p->data.repeat.x, &results[j], e, depth+1)) {
        j++;
        results = mpc_grow_results(i, j, results_stk, results);
      }

      *e = mpc_err_merge(i, *e, results[j].error);

      MPC_SUCCESS(
        mpc_parse_fold(i, p->data.repeat.f, j, (mpc_val_t**)results);
        if (j >= MPC_PARSE_STACK_MIN) { mpc_free(i, results); });

    case MPC_TYPE_MANY1:

      results = results_stk;

      while (mpc_parse_run(i, p->data.repeat.x, &results[j], e, depth+1)) {
        j++;
        results = mpc_grow_results(i, j, results_stk, results);
      }

      if (j == 0) {
        MPC_FAILURE(
          mpc_err_many1(i, results[j].error);
          if (j >= MPC_PARSE_STACK_MIN) { mpc_free(i, results); });
      } else {

        *e = mpc_err_merge(i, *e, results[j].error);

        MPC_SUCCESS(
          mpc_parse_fold(i, p->data.repeat.f, j, (mpc_val_t**)results);
          if (j >= MPC_PARSE_STACK_MIN) { mpc_free(i, results); });
      }

   case MPC_TYPE_SEPBY1:

      results = results_stk;

      if(mpc_parse_run(i, p->data.sepby1.x, &results[j], e, depth+1)){
        j++;
        results = mpc_grow_results(i, j, results_stk, results);

        while (
          mpc_parse_run(i, p->data.sepby1.sep, &results[j], e, depth+1) &&
          mpc_parse_run(i, p->data.sepby1.x, &results[j], e, depth+1)
        ) {
          j++;
          results = mpc_grow_results(i, j, results_stk, results);
        }
      }

      if (j == 0) {
        MPC_FAILURE(
          mpc_err_many1(i, results[j].error);
          if (j >= MPC_PARSE_STACK_MIN) { mpc_free(i, results); });
      } else {
        *e = mpc_err_merge(i, *e, results[j].error);

        MPC_SUCCESS(
          mpc_parse_fold(i, p->data.repeat.f, j, (mpc_val_t**)results);
          if (j >= MPC_PARSE_STACK_MIN) { mpc_free(i, results); });
      }

    case MPC_TYPE_COUNT:

      results = p->data.repeat.n > MPC_PARSE_STACK_MIN
        ? mpc_malloc(i, sizeof(mpc_result_t) * p->data.repeat.n)
        : results_stk;

      while (mpc_parse_run(i, p->data.repeat.x, &results[j], e, depth+1)) {
        j++;
        if (j == p->data.repeat.n) { break; }
      }

      if (j == p->data.repeat.n) {
        MPC_SUCCESS(
          mpc_parse_fold(i, p->data.repeat.f, j, (mpc_val_t**)results);
          if (p->data.repeat.n > MPC_PARSE_STACK_MIN) { mpc_free(i, results); });
      } else {
        for (k = 0; k < j; k++) {
          mpc_parse_dtor(i, p->data.repeat.dx, results[k].output);
        }
        MPC_FAILURE(
          mpc_err_count(i, results[j].error, p->data.repeat.n);
          if (p->data.repeat.n > MPC_PARSE_STACK_MIN) { mpc_free(i, results); });
      }

    /* Combinatory Parsers */

    case MPC_TYPE_OR:

      if (p->data.or.n == 0) { MPC_SUCCESS(NULL); }

      results = p->data.or.n > MPC_PARSE_STACK_MIN
        ? mpc_malloc(i, sizeof(mpc_result_t) * p->data.or.n)
        : results_stk;

      for (j = 0; j < p->data.or.n; j++) {
        if (mpc_parse_run(i, p->data.or.xs[j], &results[j], e, depth+1)) {
          MPC_SUCCESS(results[j].output;
            if (p->data.or.n > MPC_PARSE_STACK_MIN) { mpc_free(i, results); });
        } else {
          *e = mpc_err_merge(i, *e, results[j].error);
        }
      }

      MPC_FAILURE(NULL;
        if (p->data.or.n > MPC_PARSE_STACK_MIN) { mpc_free(i, results); });

    case MPC_TYPE_AND:
      if (p->data.and.n == 0) { MPC_SUCCESS(NULL); }

      results = p->data.or.n > MPC_PARSE_STACK_MIN
        ? mpc_malloc(i, sizeof(mpc_result_t) * p->data.or.n)
        : results_stk;

      mpc_input_mark(i);
      for (j = 0; j < p->data.and.n; j++) {
        if (!mpc_parse_run(i, p->data.and.xs[j], &results[j], e, depth+1)) {
          mpc_input_rewind(i);
          for (k = 0; k < j; k++) {
            mpc_parse_dtor(i, p->data.and.dxs[k], results[k].output);
          }
          MPC_FAILURE(results[j].error;
            if (p->data.or.n > MPC_PARSE_STACK_MIN) { mpc_free(i, results); });
        }
      }
      mpc_input_unmark(i);
      MPC_SUCCESS(
        mpc_parse_fold(i, p->data.and.f, j, (mpc_val_t**)results);
        if (p->data.or.n > MPC_PARSE_STACK_MIN) { mpc_free(i, results); });

    /* End */

    default:

      MPC_FAILURE(mpc_err_fail(i, "Unknown Parser Type Id!"));
  }

  return 0;

}

#undef MPC_SUCCESS
#undef MPC_FAILURE
#undef MPC_PRIMITIVE

int mpc_parse_input(mpc_input_t *i, mpc_parser_t *p, mpc_result_t *r) {
  int x;
  mpc_err_t *e = mpc_err_fail(i, "Unknown Error");
  e->state = mpc_state_invalid();
  x = mpc_parse_run(i, p, r, &e, 0);
  if (x) {
    mpc_err_delete_internal(i, e);
    r->output = mpc_export(i, r->output);
  } else {
    r->error = mpc_err_export(i, mpc_err_merge(i, e, r->error));
  }
  return x;
}

int mpc_parse(const char *filename, const char *string, mpc_parser_t *p, mpc_result_t *r) {
  int x;
  mpc_input_t *i = mpc_input_new_string(filename, string);
  x = mpc_parse_input(i, p, r);
  mpc_input_delete(i);
  return x;
}

int mpc_nparse(const char *filename, const char *string, size_t length, mpc_parser_t *p, mpc_result_t *r) {
  int x;
  mpc_input_t *i = mpc_input_new_nstring(filename, string, length);
  x = mpc_parse_input(i, p, r);
  mpc_input_delete(i);
  return x;
}

int mpc_parse_file(const char *filename, FILE *file, mpc_parser_t *p, mpc_result_t *r) {
  int x;
  mpc_input_t *i = mpc_input_new_file(filename, file);
  x = mpc_parse_input(i, p, r);
  mpc_input_delete(i);
  return x;
}

int mpc_parse_pipe(const char *filename, FILE *pipe, mpc_parser_t *p, mpc_result_t *r) {
  int x;
  mpc_input_t *i = mpc_input_new_pipe(filename, pipe);
  x = mpc_parse_input(i, p, r);
  mpc_input_delete(i);
  return x;
}

int mpc_parse_contents(const char *filename, mpc_parser_t *p, mpc_result_t *r) {

  FILE *f = fopen(filename, "rb");
  int res;

  if (f == NULL) {
    r->output = NULL;
    r->error = mpc_err_file(filename, "Unable to open file!");
    return 0;
  }

  res = mpc_parse_file(filename, f, p, r);
  fclose(f);
  return res;
}

/*
** Building a Parser
*/

static void mpc_undefine_unretained(mpc_parser_t *p, int force);

static void mpc_undefine_or(mpc_parser_t *p) {

  int i;
  for (i = 0; i < p->data.or.n; i++) {
    mpc_undefine_unretained(p->data.or.xs[i], 0);
  }
  free(p->data.or.xs);

}

static void mpc_undefine_and(mpc_parser_t *p) {

  int i;
  for (i = 0; i < p->data.and.n; i++) {
    mpc_undefine_unretained(p->data.and.xs[i], 0);
  }
  free(p->data.and.xs);
  free(p->data.and.dxs);

}

static void mpc_undefine_unretained(mpc_parser_t *p, int force) {

  if (p->retained && !force) { return; }

  switch (p->type) {

    case MPC_TYPE_FAIL: free(p->data.fail.m); break;

    case MPC_TYPE_ONEOF:
    case MPC_TYPE_NONEOF:
    case MPC_TYPE_STRING:
      free(p->data.string.x);
      break;

    case MPC_TYPE_APPLY:    mpc_undefine_unretained(p->data.apply.x, 0);    break;
    case MPC_TYPE_APPLY_TO: mpc_undefine_unretained(p->data.apply_to.x, 0); break;
    case MPC_TYPE_PREDICT:  mpc_undefine_unretained(p->data.predict.x, 0);  break;

    case MPC_TYPE_MAYBE:
    case MPC_TYPE_NOT:
      mpc_undefine_unretained(p->data.not.x, 0);
      break;

    case MPC_TYPE_EXPECT:
      mpc_undefine_unretained(p->data.expect.x, 0);
      free(p->data.expect.m);
      break;

    case MPC_TYPE_MANY:
    case MPC_TYPE_MANY1:
    case MPC_TYPE_COUNT:
      mpc_undefine_unretained(p->data.repeat.x, 0);
      break;

    case MPC_TYPE_SEPBY1:
      mpc_undefine_unretained(p->data.sepby1.x, 0);
      mpc_undefine_unretained(p->data.sepby1.sep, 0);
      break;

    case MPC_TYPE_OR:  mpc_undefine_or(p);  break;
    case MPC_TYPE_AND: mpc_undefine_and(p); break;

    case MPC_TYPE_CHECK:
      mpc_undefine_unretained(p->data.check.x, 0);
      free(p->data.check.e);
      break;

    case MPC_TYPE_CHECK_WITH:
      mpc_undefine_unretained(p->data.check_with.x, 0);
      free(p->data.check_with.e);
      break;

    default: break;
  }

  if (!force) {
    free(p->name);
    free(p);
  }

}

void mpc_delete(mpc_parser_t *p) {
  if (p->retained) {

    if (p->type != MPC_TYPE_UNDEFINED) {
      mpc_undefine_unretained(p, 0);
    }

    free(p->name);
    free(p);

  } else {
    mpc_undefine_unretained(p, 0);
  }
}

static void mpc_soft_delete(mpc_val_t *x) {
  mpc_undefine_unretained(x, 0);
}

static mpc_parser_t *mpc_undefined(void) {
  mpc_parser_t *p = calloc(1, sizeof(mpc_parser_t));
  p->retained = 0;
  p->type = MPC_TYPE_UNDEFINED;
  p->name = NULL;
  return p;
}

mpc_parser_t *mpc_new(const char *name) {
  mpc_parser_t *p = mpc_undefined();
  p->retained = 1;
  p->name = realloc(p->name, strlen(name) + 1);
  strcpy(p->name, name);
  return p;
}

mpc_parser_t *mpc_copy(mpc_parser_t *a) {
  int i = 0;
  mpc_parser_t *p;

  if (a->retained) { return a; }

  p = mpc_undefined();
  p->retained = a->retained;
  p->type = a->type;
  p->data = a->data;

  if (a->name) {
    p->name = malloc(strlen(a->name)+1);
    strcpy(p->name, a->name);
  }

  switch (a->type) {

    case MPC_TYPE_FAIL:
      p->data.fail.m = malloc(strlen(a->data.fail.m)+1);
      strcpy(p->data.fail.m, a->data.fail.m);
    break;

    case MPC_TYPE_ONEOF:
    case MPC_TYPE_NONEOF:
    case MPC_TYPE_STRING:
      p->data.string.x = malloc(strlen(a->data.string.x)+1);
      strcpy(p->data.string.x, a->data.string.x);
      break;

    case MPC_TYPE_APPLY:    p->data.apply.x    = mpc_copy(a->data.apply.x);    break;
    case MPC_TYPE_APPLY_TO: p->data.apply_to.x = mpc_copy(a->data.apply_to.x); break;
    case MPC_TYPE_PREDICT:  p->data.predict.x  = mpc_copy(a->data.predict.x);  break;

    case MPC_TYPE_MAYBE:
    case MPC_TYPE_NOT:
      p->data.not.x = mpc_copy(a->data.not.x);
      break;

    case MPC_TYPE_EXPECT:
      p->data.expect.x = mpc_copy(a->data.expect.x);
      p->data.expect.m = malloc(strlen(a->data.expect.m)+1);
      strcpy(p->data.expect.m, a->data.expect.m);
      break;

    case MPC_TYPE_MANY:
    case MPC_TYPE_MANY1:
    case MPC_TYPE_COUNT:
      p->data.repeat.x = mpc_copy(a->data.repeat.x);
      break;

    case MPC_TYPE_SEPBY1:
      p->data.sepby1.x   = mpc_copy(a->data.sepby1.x);
      p->data.sepby1.sep = mpc_copy(a->data.sepby1.sep);
      break;

    case MPC_TYPE_OR:
      p->data.or.xs = malloc(a->data.or.n * sizeof(mpc_parser_t*));
      for (i = 0; i < a->data.or.n; i++) {
        p->data.or.xs[i] = mpc_copy(a->data.or.xs[i]);
      }
    break;
    case MPC_TYPE_AND:
      p->data.and.xs = malloc(a->data.and.n * sizeof(mpc_parser_t*));
      for (i = 0; i < a->data.and.n; i++) {
        p->data.and.xs[i] = mpc_copy(a->data.and.xs[i]);
      }
      if (a->data.and.n > 0) {
          p->data.and.dxs = malloc((a->data.and.n-1) * sizeof(mpc_dtor_t));
          for (i = 0; i < a->data.and.n-1; i++) {
            p->data.and.dxs[i] = a->data.and.dxs[i];
          }
      }
    break;

    case MPC_TYPE_CHECK:
      p->data.check.x      = mpc_copy(a->data.check.x);
      p->data.check.e      = malloc(strlen(a->data.check.e)+1);
      strcpy(p->data.check.e, a->data.check.e);
      break;
    case MPC_TYPE_CHECK_WITH:
      p->data.check_with.x = mpc_copy(a->data.check_with.x);
      p->data.check_with.e = malloc(strlen(a->data.check_with.e)+1);
      strcpy(p->data.check_with.e, a->data.check_with.e);
      break;

    default: break;
  }


  return p;
}

mpc_parser_t *mpc_undefine(mpc_parser_t *p) {
  mpc_undefine_unretained(p, 1);
  p->type = MPC_TYPE_UNDEFINED;
  return p;
}

mpc_parser_t *mpc_define(mpc_parser_t *p, mpc_parser_t *a) {

  if (p->retained) {
    p->type = a->type;
    p->data = a->data;
  } else {
    mpc_parser_t *a2 = mpc_failf("Attempt to assign to Unretained Parser!");
    p->type = a2->type;
    p->data = a2->data;
    free(a2);
  }

  free(a);
  return p;
}

void mpc_cleanup(int n, ...) {
  int i;
  mpc_parser_t **list = malloc(sizeof(mpc_parser_t*) * n);

  va_list va;
  va_start(va, n);
  for (i = 0; i < n; i++) { list[i] = va_arg(va, mpc_parser_t*); }
  for (i = 0; i < n; i++) { mpc_undefine(list[i]); }
  for (i = 0; i < n; i++) { mpc_delete(list[i]); }
  va_end(va);

  free(list);
}

mpc_parser_t *mpc_pass(void) {
  mpc_parser_t *p = mpc_undefined();
  p->type = MPC_TYPE_PASS;
  return p;
}

mpc_parser_t *mpc_fail(const char *m) {
  mpc_parser_t *p = mpc_undefined();
  p->type = MPC_TYPE_FAIL;
  p->data.fail.m = malloc(strlen(m) + 1);
  strcpy(p->data.fail.m, m);
  return p;
}

/*
** As `snprintf` is not ANSI standard this
** function `mpc_failf` should be considered
** unsafe.
**
** You have a few options if this is going to be
** trouble.
**
** - Ensure the format string does not exceed
**   the buffer length using precision specifiers
**   such as `%.512s`.
**
** - Patch this function in your code base to
**   use `snprintf` or whatever variant your
**   system supports.
**
** - Avoid it altogether.
**
*/

mpc_parser_t *mpc_failf(const char *fmt, ...) {

  va_list va;
  char *buffer;

  mpc_parser_t *p = mpc_undefined();
  p->type = MPC_TYPE_FAIL;

  va_start(va, fmt);
  buffer = malloc(2048);
  if (!buffer) {
    return NULL;
  }
  vsprintf(buffer, fmt, va);
  va_end(va);

  buffer = realloc(buffer, strlen(buffer) + 1);
  p->data.fail.m = buffer;
  return p;

}

mpc_parser_t *mpc_lift_val(mpc_val_t *x) {
  mpc_parser_t *p = mpc_undefined();
  p->type = MPC_TYPE_LIFT_VAL;
  p->data.lift.x = x;
  return p;
}

mpc_parser_t *mpc_lift(mpc_ctor_t lf) {
  mpc_parser_t *p = mpc_undefined();
  p->type = MPC_TYPE_LIFT;
  p->data.lift.lf = lf;
  return p;
}

mpc_parser_t *mpc_anchor(int(*f)(char,char)) {
  mpc_parser_t *p = mpc_undefined();
  p->type = MPC_TYPE_ANCHOR;
  p->data.anchor.f = f;
  return mpc_expect(p, "anchor");
}

mpc_parser_t *mpc_state(void) {
  mpc_parser_t *p = mpc_undefined();
  p->type = MPC_TYPE_STATE;
  return p;
}

mpc_parser_t *mpc_expect(mpc_parser_t *a, const char *expected) {
  mpc_parser_t *p = mpc_undefined();
  p->type = MPC_TYPE_EXPECT;
  p->data.expect.x = a;
  p->data.expect.m = malloc(strlen(expected) + 1);
  strcpy(p->data.expect.m, expected);
  return p;
}

/*
** As `snprintf` is not ANSI standard this
** function `mpc_expectf` should be considered
** unsafe.
**
** You have a few options if this is going to be
** trouble.
**
** - Ensure the format string does not exceed
**   the buffer length using precision specifiers
**   such as `%.512s`.
**
** - Patch this function in your code base to
**   use `snprintf` or whatever variant your
**   system supports.
**
** - Avoid it altogether.
**
*/

mpc_parser_t *mpc_expectf(mpc_parser_t *a, const char *fmt, ...) {
  va_list va;
  char *buffer;

  mpc_parser_t *p = mpc_undefined();
  p->type = MPC_TYPE_EXPECT;

  va_start(va, fmt);
  buffer = malloc(2048);
  if (!buffer) {
    return NULL;
  }
  vsprintf(buffer, fmt, va);
  va_end(va);

  buffer = realloc(buffer, strlen(buffer) + 1);
  p->data.expect.x = a;
  p->data.expect.m = buffer;
  return p;
}

/*
** Basic Parsers
*/

mpc_parser_t *mpc_any(void) {
  mpc_parser_t *p = mpc_undefined();
  p->type = MPC_TYPE_ANY;
  return mpc_expect(p, "any character");
}

mpc_parser_t *mpc_char(char c) {
  mpc_parser_t *p = mpc_undefined();
  p->type = MPC_TYPE_SINGLE;
  p->data.single.x = c;
  return mpc_expectf(p, "'%c'", c);
}

mpc_parser_t *mpc_range(char s, char e) {
  mpc_parser_t *p = mpc_undefined();
  p->type = MPC_TYPE_RANGE;
  p->data.range.x = s;
  p->data.range.y = e;
  return mpc_expectf(p, "character between '%c' and '%c'", s, e);
}

mpc_parser_t *mpc_oneof(const char *s) {
  mpc_parser_t *p = mpc_undefined();
  p->type = MPC_TYPE_ONEOF;
  p->data.string.x = malloc(strlen(s) + 1);
  strcpy(p->data.string.x, s);
  return mpc_expectf(p, "one of '%s'", s);
}

mpc_parser_t *mpc_noneof(const char *s) {
  mpc_parser_t *p = mpc_undefined();
  p->type = MPC_TYPE_NONEOF;
  p->data.string.x = malloc(strlen(s) + 1);
  strcpy(p->data.string.x, s);
  return mpc_expectf(p, "none of '%s'", s);

}

mpc_parser_t *mpc_satisfy(int(*f)(char)) {
  mpc_parser_t *p = mpc_undefined();
  p->type = MPC_TYPE_SATISFY;
  p->data.satisfy.f = f;
  return mpc_expectf(p, "character satisfying function %p", f);
}

mpc_parser_t *mpc_string(const char *s) {
  mpc_parser_t *p = mpc_undefined();
  p->type = MPC_TYPE_STRING;
  p->data.string.x = malloc(strlen(s) + 1);
  strcpy(p->data.string.x, s);
  return mpc_expectf(p, "\"%s\"", s);
}

/*
** Core Parsers
*/

mpc_parser_t *mpc_apply(mpc_parser_t *a, mpc_apply_t f) {
  mpc_parser_t *p = mpc_undefined();
  p->type = MPC_TYPE_APPLY;
  p->data.apply.x = a;
  p->data.apply.f = f;
  return p;
}

mpc_parser_t *mpc_apply_to(mpc_parser_t *a, mpc_apply_to_t f, void *x) {
  mpc_parser_t *p = mpc_undefined();
  p->type = MPC_TYPE_APPLY_TO;
  p->data.apply_to.x = a;
  p->data.apply_to.f = f;
  p->data.apply_to.d = x;
  return p;
}

mpc_parser_t *mpc_check(mpc_parser_t *a, mpc_dtor_t da, mpc_check_t f, const char *e) {
  mpc_parser_t  *p = mpc_undefined();
  p->type = MPC_TYPE_CHECK;
  p->data.check.x = a;
  p->data.check.dx = da;
  p->data.check.f = f;
  p->data.check.e = malloc(strlen(e) + 1);
  strcpy(p->data.check.e, e);
  return p;
}

mpc_parser_t *mpc_check_with(mpc_parser_t *a, mpc_dtor_t da, mpc_check_with_t f, void *x, const char *e) {
  mpc_parser_t  *p = mpc_undefined();
  p->type = MPC_TYPE_CHECK_WITH;
  p->data.check_with.x = a;
  p->data.check_with.dx = da;
  p->data.check_with.f = f;
  p->data.check_with.d = x;
  p->data.check_with.e = malloc(strlen(e) + 1);
  strcpy(p->data.check_with.e, e);
  return p;
}

mpc_parser_t *mpc_checkf(mpc_parser_t *a, mpc_dtor_t da, mpc_check_t f, const char *fmt, ...) {
  va_list va;
  char *buffer;
  mpc_parser_t *p;

  va_start(va, fmt);
  buffer = malloc(2048);
  vsprintf(buffer, fmt, va);
  va_end(va);

  p = mpc_check(a, da, f, buffer);
  free(buffer);

  return p;
}

mpc_parser_t *mpc_check_withf(mpc_parser_t *a, mpc_dtor_t da, mpc_check_with_t f, void *x, const char *fmt, ...) {
  va_list va;
  char *buffer;
  mpc_parser_t *p;

  va_start(va, fmt);
  buffer = malloc(2048);
  vsprintf(buffer, fmt, va);
  va_end(va);

  p = mpc_check_with(a, da, f, x, buffer);
  free(buffer);

  return p;
}

mpc_parser_t *mpc_predictive(mpc_parser_t *a) {
  mpc_parser_t *p = mpc_undefined();
  p->type = MPC_TYPE_PREDICT;
  p->data.predict.x = a;
  return p;
}

mpc_parser_t *mpc_not_lift(mpc_parser_t *a, mpc_dtor_t da, mpc_ctor_t lf) {
  mpc_parser_t *p = mpc_undefined();
  p->type = MPC_TYPE_NOT;
  p->data.not.x = a;
  p->data.not.dx = da;
  p->data.not.lf = lf;
  return p;
}

mpc_parser_t *mpc_not(mpc_parser_t *a, mpc_dtor_t da) {
  return mpc_not_lift(a, da, mpcf_ctor_null);
}

mpc_parser_t *mpc_maybe_lift(mpc_parser_t *a, mpc_ctor_t lf) {
  mpc_parser_t *p = mpc_undefined();
  p->type = MPC_TYPE_MAYBE;
  p->data.not.x = a;
  p->data.not.lf = lf;
  return p;
}

mpc_parser_t *mpc_maybe(mpc_parser_t *a) {
  return mpc_maybe_lift(a, mpcf_ctor_null);
}

mpc_parser_t *mpc_many(mpc_fold_t f, mpc_parser_t *a) {
  mpc_parser_t *p = mpc_undefined();
  p->type = MPC_TYPE_MANY;
  p->data.repeat.x = a;
  p->data.repeat.f = f;
  return p;
}

mpc_parser_t *mpc_many1(mpc_fold_t f, mpc_parser_t *a) {
  mpc_parser_t *p = mpc_undefined();
  p->type = MPC_TYPE_MANY1;
  p->data.repeat.x = a;
  p->data.repeat.f = f;
  return p;
}

mpc_parser_t *mpc_count(int n, mpc_fold_t f, mpc_parser_t *a, mpc_dtor_t da) {
  mpc_parser_t *p = mpc_undefined();
  p->type = MPC_TYPE_COUNT;
  p->data.repeat.n = n;
  p->data.repeat.f = f;
  p->data.repeat.x = a;
  p->data.repeat.dx = da;
  return p;
}

mpc_parser_t *mpc_sepby1(mpc_fold_t f, mpc_parser_t *sep, mpc_parser_t *a) {
  mpc_parser_t *p = mpc_undefined();
  p->type = MPC_TYPE_SEPBY1;
  p->data.sepby1.x   = a;
  p->data.sepby1.f   = f;
  p->data.sepby1.sep = sep;
  return p;
}

mpc_parser_t *mpc_or(int n, ...) {

  int i;
  va_list va;

  mpc_parser_t *p = mpc_undefined();

  p->type = MPC_TYPE_OR;
  p->data.or.n = n;
  p->data.or.xs = malloc(sizeof(mpc_parser_t*) * n);

  va_start(va, n);
  for (i = 0; i < n; i++) {
    p->data.or.xs[i] = va_arg(va, mpc_parser_t*);
  }
  va_end(va);

  return p;
}

mpc_parser_t *mpc_and(int n, mpc_fold_t f, ...) {

  int i;
  va_list va;

  mpc_parser_t *p = mpc_undefined();

  p->type = MPC_TYPE_AND;
  p->data.and.n = n;
  p->data.and.f = f;
  p->data.and.xs = malloc(sizeof(mpc_parser_t*) * n);
  p->data.and.dxs = malloc(sizeof(mpc_dtor_t) * (n-1));

  va_start(va, f);
  for (i = 0; i < n; i++) {
    p->data.and.xs[i] = va_arg(va, mpc_parser_t*);
  }
  for (i = 0; i < (n-1); i++) {
    p->data.and.dxs[i] = va_arg(va, mpc_dtor_t);
  }
  va_end(va);

  return p;
}

/*
** Common Parsers
*/

mpc_parser_t *mpc_soi(void) {
  mpc_parser_t *p = mpc_undefined();
  p->type = MPC_TYPE_SOI;
  return mpc_expect(p, "start of input");
}

mpc_parser_t *mpc_eoi(void) {
  mpc_parser_t *p = mpc_undefined();
  p->type = MPC_TYPE_EOI;
  return mpc_expect(p, "end of input");
}

static int mpc_boundary_anchor(char prev, char next) {
  const char* word = "abcdefghijklmnopqrstuvwxyz"
                     "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                     "0123456789_";
  if ( strchr(word, next) &&  prev == '\0') { return 1; }
  if ( strchr(word, prev) &&  next == '\0') { return 1; }
  if ( strchr(word, next) && !strchr(word, prev)) { return 1; }
  if (!strchr(word, next) &&  strchr(word, prev)) { return 1; }
  return 0;
}

static int mpc_boundary_newline_anchor(char prev, char next) {
  (void)next;
  return prev == '\n';
}

mpc_parser_t *mpc_boundary(void) { return mpc_expect(mpc_anchor(mpc_boundary_anchor), "word boundary"); }
mpc_parser_t *mpc_boundary_newline(void) { return mpc_expect(mpc_anchor(mpc_boundary_newline_anchor), "start of newline"); }

mpc_parser_t *mpc_whitespace(void) { return mpc_expect(mpc_oneof(" \f\n\r\t\v"), "whitespace"); }
mpc_parser_t *mpc_whitespaces(void) { return mpc_expect(mpc_many(mpcf_strfold, mpc_whitespace()), "spaces"); }
mpc_parser_t *mpc_blank(void) { return mpc_expect(mpc_apply(mpc_whitespaces(), mpcf_free), "whitespace"); }

mpc_parser_t *mpc_newline(void) { return mpc_expect(mpc_char('\n'), "newline"); }
mpc_parser_t *mpc_tab(void) { return mpc_expect(mpc_char('\t'), "tab"); }
mpc_parser_t *mpc_escape(void) { return mpc_and(2, mpcf_strfold, mpc_char('\\'), mpc_any(), free); }

mpc_parser_t *mpc_digit(void) { return mpc_expect(mpc_oneof("0123456789"), "digit"); }
mpc_parser_t *mpc_hexdigit(void) { return mpc_expect(mpc_oneof("0123456789ABCDEFabcdef"), "hex digit"); }
mpc_parser_t *mpc_octdigit(void) { return mpc_expect(mpc_oneof("01234567"), "oct digit"); }
mpc_parser_t *mpc_digits(void) { return mpc_expect(mpc_many1(mpcf_strfold, mpc_digit()), "digits"); }
mpc_parser_t *mpc_hexdigits(void) { return mpc_expect(mpc_many1(mpcf_strfold, mpc_hexdigit()), "hex digits"); }
mpc_parser_t *mpc_octdigits(void) { return mpc_expect(mpc_many1(mpcf_strfold, mpc_octdigit()), "oct digits"); }

mpc_parser_t *mpc_lower(void) { return mpc_expect(mpc_oneof("abcdefghijklmnopqrstuvwxyz"), "lowercase letter"); }
mpc_parser_t *mpc_upper(void) { return mpc_expect(mpc_oneof("ABCDEFGHIJKLMNOPQRSTUVWXYZ"), "uppercase letter"); }
mpc_parser_t *mpc_alpha(void) { return mpc_expect(mpc_oneof("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"), "letter"); }
mpc_parser_t *mpc_underscore(void) { return mpc_expect(mpc_char('_'), "underscore"); }
mpc_parser_t *mpc_alphanum(void) { return mpc_expect(mpc_or(3, mpc_alpha(), mpc_digit(), mpc_underscore()), "alphanumeric"); }

mpc_parser_t *mpc_int(void) { return mpc_expect(mpc_apply(mpc_digits(), mpcf_int), "integer"); }
mpc_parser_t *mpc_hex(void) { return mpc_expect(mpc_apply(mpc_hexdigits(), mpcf_hex), "hexadecimal"); }
mpc_parser_t *mpc_oct(void) { return mpc_expect(mpc_apply(mpc_octdigits(), mpcf_oct), "octadecimal"); }
mpc_parser_t *mpc_number(void) { return mpc_expect(mpc_or(3, mpc_int(), mpc_hex(), mpc_oct()), "number"); }

mpc_parser_t *mpc_real(void) {

  /* [+-]?\d+(\.\d+)?([eE][+-]?[0-9]+)? */

  mpc_parser_t *p0, *p1, *p2, *p30, *p31, *p32, *p3;

  p0 = mpc_maybe_lift(mpc_oneof("+-"), mpcf_ctor_str);
  p1 = mpc_digits();
  p2 = mpc_maybe_lift(mpc_and(2, mpcf_strfold, mpc_char('.'), mpc_digits(), free), mpcf_ctor_str);
  p30 = mpc_oneof("eE");
  p31 = mpc_maybe_lift(mpc_oneof("+-"), mpcf_ctor_str);
  p32 = mpc_digits();
  p3 = mpc_maybe_lift(mpc_and(3, mpcf_strfold, p30, p31, p32, free, free), mpcf_ctor_str);

  return mpc_expect(mpc_and(4, mpcf_strfold, p0, p1, p2, p3, free, free, free), "real");

}

mpc_parser_t *mpc_float(void) {
  return mpc_expect(mpc_apply(mpc_real(), mpcf_float), "float");
}

mpc_parser_t *mpc_char_lit(void) {
  return mpc_expect(mpc_between(mpc_or(2, mpc_escape(), mpc_any()), free, "'", "'"), "char");
}

mpc_parser_t *mpc_string_lit(void) {
  mpc_parser_t *strchar = mpc_or(2, mpc_escape(), mpc_noneof("\""));
  return mpc_expect(mpc_between(mpc_many(mpcf_strfold, strchar), free, "\"", "\""), "string");
}

mpc_parser_t *mpc_regex_lit(void) {
  mpc_parser_t *regexchar = mpc_or(2, mpc_escape(), mpc_noneof("/"));
  return mpc_expect(mpc_between(mpc_many(mpcf_strfold, regexchar), free, "/", "/"), "regex");
}

mpc_parser_t *mpc_ident(void) {
  mpc_parser_t *p0, *p1;
  p0 = mpc_or(2, mpc_alpha(), mpc_underscore());
  p1 = mpc_many(mpcf_strfold, mpc_alphanum());
  return mpc_and(2, mpcf_strfold, p0, p1, free);
}

/*
** Useful Parsers
*/

mpc_parser_t *mpc_startwith(mpc_parser_t *a) { return mpc_and(2, mpcf_snd, mpc_soi(), a, mpcf_dtor_null); }
mpc_parser_t *mpc_endwith(mpc_parser_t *a, mpc_dtor_t da) { return mpc_and(2, mpcf_fst, a, mpc_eoi(), da); }
mpc_parser_t *mpc_whole(mpc_parser_t *a, mpc_dtor_t da) { return mpc_and(3, mpcf_snd, mpc_soi(), a, mpc_eoi(), mpcf_dtor_null, da); }

mpc_parser_t *mpc_stripl(mpc_parser_t *a) { return mpc_and(2, mpcf_snd, mpc_blank(), a, mpcf_dtor_null); }
mpc_parser_t *mpc_stripr(mpc_parser_t *a) { return mpc_and(2, mpcf_fst, a, mpc_blank(), mpcf_dtor_null); }
mpc_parser_t *mpc_strip(mpc_parser_t *a) { return mpc_and(3, mpcf_snd, mpc_blank(), a, mpc_blank(), mpcf_dtor_null, mpcf_dtor_null); }
mpc_parser_t *mpc_tok(mpc_parser_t *a) { return mpc_and(2, mpcf_fst, a, mpc_blank(), mpcf_dtor_null); }
mpc_parser_t *mpc_sym(const char *s) { return mpc_tok(mpc_string(s)); }

mpc_parser_t *mpc_total(mpc_parser_t *a, mpc_dtor_t da) { return mpc_whole(mpc_strip(a), da); }

mpc_parser_t *mpc_between(mpc_parser_t *a, mpc_dtor_t ad, const char *o, const char *c) {
  return mpc_and(3, mpcf_snd_free,
    mpc_string(o), a, mpc_string(c),
    free, ad);
}

mpc_parser_t *mpc_parens(mpc_parser_t *a, mpc_dtor_t ad)   { return mpc_between(a, ad, "(", ")"); }
mpc_parser_t *mpc_braces(mpc_parser_t *a, mpc_dtor_t ad)   { return mpc_between(a, ad, "<", ">"); }
mpc_parser_t *mpc_brackets(mpc_parser_t *a, mpc_dtor_t ad) { return mpc_between(a, ad, "{", "}"); }
mpc_parser_t *mpc_squares(mpc_parser_t *a, mpc_dtor_t ad)  { return mpc_between(a, ad, "[", "]"); }

mpc_parser_t *mpc_tok_between(mpc_parser_t *a, mpc_dtor_t ad, const char *o, const char *c) {
  return mpc_and(3, mpcf_snd_free,
    mpc_sym(o), mpc_tok(a), mpc_sym(c),
    free, ad);
}

mpc_parser_t *mpc_tok_parens(mpc_parser_t *a, mpc_dtor_t ad)   { return mpc_tok_between(a, ad, "(", ")"); }
mpc_parser_t *mpc_tok_braces(mpc_parser_t *a, mpc_dtor_t ad)   { return mpc_tok_between(a, ad, "<", ">"); }
mpc_parser_t *mpc_tok_brackets(mpc_parser_t *a, mpc_dtor_t ad) { return mpc_tok_between(a, ad, "{", "}"); }
mpc_parser_t *mpc_tok_squares(mpc_parser_t *a, mpc_dtor_t ad)  { return mpc_tok_between(a, ad, "[", "]"); }

/*
** Regular Expression Parsers
*/

/*
** So here is a cute bootstrapping.
**
** I'm using the previously defined
** mpc constructs and functions to
** parse the user regex string and
** construct a parser from it.
**
** As it turns out lots of the standard
** mpc functions look a lot like `fold`
** functions and so can be used indirectly
** by many of the parsing functions to build
** a parser directly - as we are parsing.
**
** This is certainly something that
** would be less elegant/interesting
** in a two-phase parser which first
** builds an AST and then traverses it
** to generate the object.
**
** This whole thing acts as a great
** case study for how trivial it can be
** to write a great parser in a few
** lines of code using mpc.
*/

/*
**
**  ### Regular Expression Grammar
**
**      <regex> : <term> | (<term> "|" <regex>)
**
**      <term> : <factor>*
**
**      <factor> : <base>
**               | <base> "*"
**               | <base> "+"
**               | <base> "?"
**               | <base> "{" <digits> "}"
**
**      <base> : <char>
**             | "\" <char>
**             | "(" <regex> ")"
**             | "[" <range> "]"
*/

static mpc_val_t *mpcf_re_or(int n, mpc_val_t **xs) {
  (void) n;
  if (xs[1] == NULL) { return xs[0]; }
  else { return mpc_or(2, xs[0], xs[1]); }
}

static mpc_val_t *mpcf_re_and(int n, mpc_val_t **xs) {
  int i;
  mpc_parser_t *p = mpc_lift(mpcf_ctor_str);
  for (i = 0; i < n; i++) {
    p = mpc_and(2, mpcf_strfold, p, xs[i], free);
  }
  return p;
}

static mpc_val_t *mpcf_re_repeat(int n, mpc_val_t **xs) {
  int num;
  (void) n;
  if (xs[1] == NULL) { return xs[0]; }
  switch(((char*)xs[1])[0])
  {
    case '*': { free(xs[1]); return mpc_many(mpcf_strfold, xs[0]); }; break;
    case '+': { free(xs[1]); return mpc_many1(mpcf_strfold, xs[0]); }; break;
    case '?': { free(xs[1]); return mpc_maybe_lift(xs[0], mpcf_ctor_str); }; break;
    default:
      num = *(int*)xs[1];
      free(xs[1]);
  }

  return mpc_count(num, mpcf_strfold, xs[0], free);
}

static mpc_parser_t *mpc_re_escape_char(char c) {
  switch (c) {
    case 'a': return mpc_char('\a');
    case 'f': return mpc_char('\f');
    case 'n': return mpc_char('\n');
    case 'r': return mpc_char('\r');
    case 't': return mpc_char('\t');
    case 'v': return mpc_char('\v');
    case 'b': return mpc_and(2, mpcf_snd, mpc_boundary(), mpc_lift(mpcf_ctor_str), free);
    case 'B': return mpc_not_lift(mpc_boundary(), free, mpcf_ctor_str);
    case 'A': return mpc_and(2, mpcf_snd, mpc_soi(), mpc_lift(mpcf_ctor_str), free);
    case 'Z': return mpc_and(2, mpcf_snd, mpc_eoi(), mpc_lift(mpcf_ctor_str), free);
    case 'd': return mpc_digit();
    case 'D': return mpc_not_lift(mpc_digit(), free, mpcf_ctor_str);
    case 's': return mpc_whitespace();
    case 'S': return mpc_not_lift(mpc_whitespace(), free, mpcf_ctor_str);
    case 'w': return mpc_alphanum();
    case 'W': return mpc_not_lift(mpc_alphanum(), free, mpcf_ctor_str);
    default: return NULL;
  }
}

static mpc_val_t *mpcf_re_escape(mpc_val_t *x, void* data) {

  int mode = *((int*)data);
  char *s = x;
  mpc_parser_t *p;

  /* Any Character */
  if (s[0] == '.') {
    free(s);
    if (mode & MPC_RE_DOTALL) {
      return mpc_any();
    } else {
      return mpc_expect(mpc_noneof("\n"), "any character except a newline");
    }
  }

  /* Start of Input */
  if (s[0] == '^') {
    free(s);
    if (mode & MPC_RE_MULTILINE) {
      return mpc_and(2, mpcf_snd, mpc_or(2, mpc_soi(), mpc_boundary_newline()), mpc_lift(mpcf_ctor_str), free);
    } else {
      return mpc_and(2, mpcf_snd, mpc_soi(), mpc_lift(mpcf_ctor_str), free);
    }
  }

  /* End of Input */
  if (s[0] == '$') {
    free(s);
    if (mode & MPC_RE_MULTILINE) {
      return mpc_or(2,
        mpc_newline(),
        mpc_and(2, mpcf_snd, mpc_eoi(), mpc_lift(mpcf_ctor_str), free));
    } else {
      return mpc_or(2,
        mpc_and(2, mpcf_fst, mpc_newline(), mpc_eoi(), free),
        mpc_and(2, mpcf_snd, mpc_eoi(), mpc_lift(mpcf_ctor_str), free));
    }
  }

  /* Regex Escape */
  if (s[0] == '\\') {
    p = mpc_re_escape_char(s[1]);
    p = (p == NULL) ? mpc_char(s[1]) : p;
    free(s);
    return p;
  }

  /* Regex Standard */
  p = mpc_char(s[0]);
  free(s);
  return p;
}

static const char *mpc_re_range_escape_char(char c) {
  switch (c) {
    case '-': return "-";
    case 'a': return "\a";
    case 'f': return "\f";
    case 'n': return "\n";
    case 'r': return "\r";
    case 't': return "\t";
    case 'v': return "\v";
    case 'b': return "\b";
    case 'd': return "0123456789";
    case 's': return " \f\n\r\t\v";
    case 'w': return "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_";
    default: return NULL;
  }
}

static mpc_val_t *mpcf_re_range(mpc_val_t *x) {

  mpc_parser_t *out;
  size_t i, j;
  size_t start, end;
  const char *tmp = NULL;
  const char *s = x;
  int comp = s[0] == '^' ? 1 : 0;
  char *range = calloc(1,1);

  if (s[0] == '\0') { free(range); free(x); return mpc_fail("Invalid Regex Range Expression"); }
  if (s[0] == '^' &&
      s[1] == '\0') { free(range); free(x); return mpc_fail("Invalid Regex Range Expression"); }

  for (i = comp; i < strlen(s); i++){

    /* Regex Range Escape */
    if (s[i] == '\\') {
      tmp = mpc_re_range_escape_char(s[i+1]);
      if (tmp != NULL) {
        range = realloc(range, strlen(range) + strlen(tmp) + 1);
        strcat(range, tmp);
      } else {
        range = realloc(range, strlen(range) + 1 + 1);
        range[strlen(range) + 1] = '\0';
        range[strlen(range) + 0] = s[i+1];
      }
      i++;
    }

    /* Regex Range...Range */
    else if (s[i] == '-') {
      if (s[i+1] == '\0' || i == 0) {
          range = realloc(range, strlen(range) + strlen("-") + 1);
          strcat(range, "-");
      } else {
        start = s[i-1]+1;
        end = s[i+1]-1;
        for (j = start; j <= end; j++) {
          range = realloc(range, strlen(range) + 1 + 1 + 1);
          range[strlen(range) + 1] = '\0';
          range[strlen(range) + 0] = (char)j;
        }
      }
    }

    /* Regex Range Normal */
    else {
      range = realloc(range, strlen(range) + 1 + 1);
      range[strlen(range) + 1] = '\0';
      range[strlen(range) + 0] = s[i];
    }

  }

  out = comp == 1 ? mpc_noneof(range) : mpc_oneof(range);

  free(x);
  free(range);

  return out;
}

mpc_parser_t *mpc_re(const char *re) {
  return mpc_re_mode(re, MPC_RE_DEFAULT);
}

mpc_parser_t *mpc_re_mode(const char *re, int mode) {

  char *err_msg;
  mpc_parser_t *err_out;
  mpc_result_t r;
  mpc_parser_t *Regex, *Term, *Factor, *Base, *Range, *RegexEnclose;

  Regex  = mpc_new("regex");
  Term   = mpc_new("term");
  Factor = mpc_new("factor");
  Base   = mpc_new("base");
  Range  = mpc_new("range");

  mpc_define(Regex, mpc_and(2, mpcf_re_or,
    Term,
    mpc_maybe(mpc_and(2, mpcf_snd_free, mpc_char('|'), Regex, free)),
    (mpc_dtor_t)mpc_delete
  ));

  mpc_define(Term, mpc_many(mpcf_re_and, Factor));

  mpc_define(Factor, mpc_and(2, mpcf_re_repeat,
    Base,
    mpc_or(5,
      mpc_char('*'), mpc_char('+'), mpc_char('?'),
      mpc_brackets(mpc_int(), free),
      mpc_pass()),
    (mpc_dtor_t)mpc_delete
  ));

  mpc_define(Base, mpc_or(4,
    mpc_parens(Regex, (mpc_dtor_t)mpc_delete),
    mpc_squares(Range, (mpc_dtor_t)mpc_delete),
    mpc_apply_to(mpc_escape(), mpcf_re_escape, &mode),
    mpc_apply_to(mpc_noneof(")|"), mpcf_re_escape, &mode)
  ));

  mpc_define(Range, mpc_apply(
    mpc_many(mpcf_strfold, mpc_or(2, mpc_escape(), mpc_noneof("]"))),
    mpcf_re_range
  ));

  RegexEnclose = mpc_whole(mpc_predictive(Regex), (mpc_dtor_t)mpc_delete);

  mpc_optimise(RegexEnclose);
  mpc_optimise(Regex);
  mpc_optimise(Term);
  mpc_optimise(Factor);
  mpc_optimise(Base);
  mpc_optimise(Range);

  if(!mpc_parse("<mpc_re_compiler>", re, RegexEnclose, &r)) {
    err_msg = mpc_err_string(r.error);
    err_out = mpc_failf("Invalid Regex: %s", err_msg);
    mpc_err_delete(r.error);
    free(err_msg);
    r.output = err_out;
  }

  mpc_cleanup(6, RegexEnclose, Regex, Term, Factor, Base, Range);

  mpc_optimise(r.output);

  return r.output;

}

/*
** Common Fold Functions
*/

void mpcf_dtor_null(mpc_val_t *x) { (void) x; return; }

mpc_val_t *mpcf_ctor_null(void) { return NULL; }
mpc_val_t *mpcf_ctor_str(void) { return calloc(1, 1); }
mpc_val_t *mpcf_free(mpc_val_t *x) { free(x); return NULL; }

mpc_val_t *mpcf_int(mpc_val_t *x) {
  int *y = malloc(sizeof(int));
  *y = strtol(x, NULL, 10);
  free(x);
  return y;
}

mpc_val_t *mpcf_hex(mpc_val_t *x) {
  int *y = malloc(sizeof(int));
  *y = strtol(x, NULL, 16);
  free(x);
  return y;
}

mpc_val_t *mpcf_oct(mpc_val_t *x) {
  int *y = malloc(sizeof(int));
  *y = strtol(x, NULL, 8);
  free(x);
  return y;
}

mpc_val_t *mpcf_float(mpc_val_t *x) {
  float *y = malloc(sizeof(float));
  *y = strtod(x, NULL);
  free(x);
  return y;
}

mpc_val_t *mpcf_strtriml(mpc_val_t *x) {
  char *s = x;
  while (isspace((unsigned char)*s)) {
    memmove(s, s+1, strlen(s));
  }
  return s;
}

mpc_val_t *mpcf_strtrimr(mpc_val_t *x) {
  char *s = x;
  size_t l = strlen(s);
  while (l > 0 && isspace((unsigned char)s[l-1])) {
    s[l-1] = '\0'; l--;
  }
  return s;
}

mpc_val_t *mpcf_strtrim(mpc_val_t *x) {
  return mpcf_strtriml(mpcf_strtrimr(x));
}

static const char mpc_escape_input_c[]  = {
  '\a', '\b', '\f', '\n', '\r',
  '\t', '\v', '\\', '\'', '\"', '\0'};

static const char *mpc_escape_output_c[] = {
  "\\a", "\\b", "\\f", "\\n", "\\r", "\\t",
  "\\v", "\\\\", "\\'", "\\\"", "\\0", NULL};

static const char mpc_escape_input_raw_re[] = { '/' };
static const char *mpc_escape_output_raw_re[] = { "\\/", NULL };

static const char mpc_escape_input_raw_cstr[] = { '"' };
static const char *mpc_escape_output_raw_cstr[] = { "\\\"", NULL };

static const char mpc_escape_input_raw_cchar[] = { '\'' };
static const char *mpc_escape_output_raw_cchar[] = { "\\'", NULL };

static mpc_val_t *mpcf_escape_new(mpc_val_t *x, const char *input, const char **output) {

  int i;
  int found;
  char buff[2];
  char *s = x;
  char *y = calloc(1, 1);

  while (*s) {

    i = 0;
    found = 0;

    while (output[i]) {
      if (*s == input[i]) {
        y = realloc(y, strlen(y) + strlen(output[i]) + 1);
        strcat(y, output[i]);
        found = 1;
        break;
      }
      i++;
    }

    if (!found) {
      y = realloc(y, strlen(y) + 2);
      buff[0] = *s; buff[1] = '\0';
      strcat(y, buff);
    }

    s++;
  }


  return y;
}

static mpc_val_t *mpcf_unescape_new(mpc_val_t *x, const char *input, const char **output) {

  int i;
  int found = 0;
  char buff[2];
  char *s = x;
  char *y = calloc(1, 1);

  while (*s) {

    i = 0;
    found = 0;

    while (output[i]) {
      if ((*(s+0)) == output[i][0] &&
          (*(s+1)) == output[i][1]) {
        y = realloc(y, strlen(y) + 1 + 1);
        buff[0] = input[i]; buff[1] = '\0';
        strcat(y, buff);
        found = 1;
        s++;
        break;
      }
      i++;
    }

    if (!found) {
      y = realloc(y, strlen(y) + 1 + 1);
      buff[0] = *s; buff[1] = '\0';
      strcat(y, buff);
    }

    if (*s == '\0') { break; }
    else { s++; }
  }

  return y;

}

mpc_val_t *mpcf_escape(mpc_val_t *x) {
  mpc_val_t *y = mpcf_escape_new(x, mpc_escape_input_c, mpc_escape_output_c);
  free(x);
  return y;
}

mpc_val_t *mpcf_unescape(mpc_val_t *x) {
  mpc_val_t *y = mpcf_unescape_new(x, mpc_escape_input_c, mpc_escape_output_c);
  free(x);
  return y;
}

mpc_val_t *mpcf_escape_regex(mpc_val_t *x) {
  mpc_val_t *y = mpcf_escape_new(x, mpc_escape_input_raw_re, mpc_escape_output_raw_re);
  free(x);
  return y;
}

mpc_val_t *mpcf_unescape_regex(mpc_val_t *x) {
  mpc_val_t *y = mpcf_unescape_new(x, mpc_escape_input_raw_re, mpc_escape_output_raw_re);
  free(x);
  return y;
}

mpc_val_t *mpcf_escape_string_raw(mpc_val_t *x) {
  mpc_val_t *y = mpcf_escape_new(x, mpc_escape_input_raw_cstr, mpc_escape_output_raw_cstr);
  free(x);
  return y;
}

mpc_val_t *mpcf_unescape_string_raw(mpc_val_t *x) {
  mpc_val_t *y = mpcf_unescape_new(x, mpc_escape_input_raw_cstr, mpc_escape_output_raw_cstr);
  free(x);
  return y;
}

mpc_val_t *mpcf_escape_char_raw(mpc_val_t *x) {
  mpc_val_t *y = mpcf_escape_new(x, mpc_escape_input_raw_cchar, mpc_escape_output_raw_cchar);
  free(x);
  return y;
}

mpc_val_t *mpcf_unescape_char_raw(mpc_val_t *x) {
  mpc_val_t *y = mpcf_unescape_new(x, mpc_escape_input_raw_cchar, mpc_escape_output_raw_cchar);
  free(x);
  return y;
}

mpc_val_t *mpcf_null(int n, mpc_val_t** xs) { (void) n; (void) xs; return NULL; }
mpc_val_t *mpcf_fst(int n, mpc_val_t **xs) { (void) n; return xs[0]; }
mpc_val_t *mpcf_snd(int n, mpc_val_t **xs) { (void) n; return xs[1]; }
mpc_val_t *mpcf_trd(int n, mpc_val_t **xs) { (void) n; return xs[2]; }

static mpc_val_t *mpcf_nth_free(int n, mpc_val_t **xs, int x) {
  int i;
  for (i = 0; i < n; i++) {
    if (i != x) { free(xs[i]); }
  }
  return xs[x];
}

mpc_val_t *mpcf_fst_free(int n, mpc_val_t **xs) { return mpcf_nth_free(n, xs, 0); }
mpc_val_t *mpcf_snd_free(int n, mpc_val_t **xs) { return mpcf_nth_free(n, xs, 1); }
mpc_val_t *mpcf_trd_free(int n, mpc_val_t **xs) { return mpcf_nth_free(n, xs, 2); }
mpc_val_t *mpcf_all_free(int n, mpc_val_t** xs) {
  int i;
  for (i = 0; i < n; i++) {
    free(xs[i]);
  }
  return NULL;
}

mpc_val_t *mpcf_strfold(int n, mpc_val_t **xs) {
  int i;
  size_t l = 0;

  if (n == 0) { return calloc(1, 1); }

  for (i = 0; i < n; i++) { l += strlen(xs[i]); }

  xs[0] = realloc(xs[0], l + 1);

  for (i = 1; i < n; i++) {
    strcat(xs[0], xs[i]); free(xs[i]);
  }

  return xs[0];
}

/*
** Printing
*/

static void mpc_print_unretained(mpc_parser_t *p, int force) {

  /* TODO: Print Everything Escaped */

  int i;
  char *s, *e;
  char buff[2];

  if (p->retained && !force) {;
    if (p->name) { printf("<%s>", p->name); }
    else { printf("<anon>"); }
    return;
  }

  if (p->type == MPC_TYPE_UNDEFINED) { printf("<?>"); }
  if (p->type == MPC_TYPE_PASS)   { printf("<:>"); }
  if (p->type == MPC_TYPE_FAIL)   { printf("<!>"); }
  if (p->type == MPC_TYPE_LIFT)   { printf("<#>"); }
  if (p->type == MPC_TYPE_STATE)  { printf("<S>"); }
  if (p->type == MPC_TYPE_ANCHOR) { printf("<@>"); }
  if (p->type == MPC_TYPE_EXPECT) {
    printf("%s", p->data.expect.m);
    /*mpc_print_unretained(p->data.expect.x, 0);*/
  }

  if (p->type == MPC_TYPE_ANY) { printf("<.>"); }
  if (p->type == MPC_TYPE_SATISFY) { printf("<f>"); }

  if (p->type == MPC_TYPE_SINGLE) {
    buff[0] = p->data.single.x; buff[1] = '\0';
    s = mpcf_escape_new(
      buff,
      mpc_escape_input_c,
      mpc_escape_output_c);
    printf("'%s'", s);
    free(s);
  }

  if (p->type == MPC_TYPE_RANGE) {
    buff[0] = p->data.range.x; buff[1] = '\0';
    s = mpcf_escape_new(
      buff,
      mpc_escape_input_c,
      mpc_escape_output_c);
    buff[0] = p->data.range.y; buff[1] = '\0';
    e = mpcf_escape_new(
      buff,
      mpc_escape_input_c,
      mpc_escape_output_c);
    printf("[%s-%s]", s, e);
    free(s);
    free(e);
  }

  if (p->type == MPC_TYPE_ONEOF) {
    s = mpcf_escape_new(
      p->data.string.x,
      mpc_escape_input_c,
      mpc_escape_output_c);
    printf("[%s]", s);
    free(s);
  }

  if (p->type == MPC_TYPE_NONEOF) {
    s = mpcf_escape_new(
      p->data.string.x,
      mpc_escape_input_c,
      mpc_escape_output_c);
    printf("[^%s]", s);
    free(s);
  }

  if (p->type == MPC_TYPE_STRING) {
    s = mpcf_escape_new(
      p->data.string.x,
      mpc_escape_input_c,
      mpc_escape_output_c);
    printf("\"%s\"", s);
    free(s);
  }

  if (p->type == MPC_TYPE_APPLY)    { mpc_print_unretained(p->data.apply.x, 0); }
  if (p->type == MPC_TYPE_APPLY_TO) { mpc_print_unretained(p->data.apply_to.x, 0); }
  if (p->type == MPC_TYPE_PREDICT)  { mpc_print_unretained(p->data.predict.x, 0); }

  if (p->type == MPC_TYPE_NOT)   { mpc_print_unretained(p->data.not.x, 0); printf("!"); }
  if (p->type == MPC_TYPE_MAYBE) { mpc_print_unretained(p->data.not.x, 0); printf("?"); }

  if (p->type == MPC_TYPE_MANY)  { mpc_print_unretained(p->data.repeat.x, 0); printf("*"); }
  if (p->type == MPC_TYPE_MANY1) { mpc_print_unretained(p->data.repeat.x, 0); printf("+"); }
  if (p->type == MPC_TYPE_COUNT) { mpc_print_unretained(p->data.repeat.x, 0); printf("{%i}", p->data.repeat.n); }
  if (p->type == MPC_TYPE_SEPBY1) {
    mpc_print_unretained(p->data.sepby1.x, 0);
    printf(" (");
    mpc_print_unretained(p->data.sepby1.sep, 0);
    printf(" ");
    mpc_print_unretained(p->data.sepby1.x, 0);
    printf(")");
    printf("*");
  }

  if (p->type == MPC_TYPE_OR) {
    printf("(");
    for(i = 0; i < p->data.or.n-1; i++) {
      mpc_print_unretained(p->data.or.xs[i], 0);
      printf(" | ");
    }
    mpc_print_unretained(p->data.or.xs[p->data.or.n-1], 0);
    printf(")");
  }

  if (p->type == MPC_TYPE_AND) {
    printf("(");
    for(i = 0; i < p->data.and.n-1; i++) {
      mpc_print_unretained(p->data.and.xs[i], 0);
      printf(" ");
    }
    mpc_print_unretained(p->data.and.xs[p->data.and.n-1], 0);
    printf(")");
  }

  if (p->type == MPC_TYPE_CHECK) {
    mpc_print_unretained(p->data.check.x, 0);
    printf("->?");
  }
  if (p->type == MPC_TYPE_CHECK_WITH) {
    mpc_print_unretained(p->data.check_with.x, 0);
    printf("->?");
  }

}

void mpc_print(mpc_parser_t *p) {
  mpc_print_unretained(p, 1);
  printf("\n");
}

/*
** Testing
*/

/*
** These functions are slightly unwieldy and
** also the whole of the testing suite for mpc
** mpc is pretty shaky.
**
** It could do with a lot more tests and more
** precision. Currently I am only really testing
** changes off of the examples.
**
*/

int mpc_test_fail(mpc_parser_t *p, const char *s, const void *d,
  int(*tester)(const void*, const void*),
  mpc_dtor_t destructor,
  void(*printer)(const void*)) {
  mpc_result_t r;
  (void) printer;
  if (mpc_parse("<test>", s, p, &r)) {

    if (tester(r.output, d)) {
      destructor(r.output);
      return 0;
    } else {
      destructor(r.output);
      return 1;
    }

  } else {
    mpc_err_delete(r.error);
    return 1;
  }

}

int mpc_test_pass(mpc_parser_t *p, const char *s, const void *d,
  int(*tester)(const void*, const void*),
  mpc_dtor_t destructor,
  void(*printer)(const void*)) {

  mpc_result_t r;
  if (mpc_parse("<test>", s, p, &r)) {

    if (tester(r.output, d)) {
      destructor(r.output);
      return 1;
    } else {
      printf("Got "); printer(r.output); printf("\n");
      printf("Expected "); printer(d); printf("\n");
      destructor(r.output);
      return 0;
    }

  } else {
    mpc_err_print(r.error);
    mpc_err_delete(r.error);
    return 0;

  }

}


/*
** AST
*/

void mpc_ast_delete(mpc_ast_t *a) {

  int i;

  if (a == NULL) { return; }

  for (i = 0; i < a->children_num; i++) {
    mpc_ast_delete(a->children[i]);
  }

  free(a->children);
  free(a->tag);
  free(a->contents);
  free(a);

}

static void mpc_ast_delete_no_children(mpc_ast_t *a) {
  free(a->children);
  free(a->tag);
  free(a->contents);
  free(a);
}

mpc_ast_t *mpc_ast_new(const char *tag, const char *contents) {

  mpc_ast_t *a = malloc(sizeof(mpc_ast_t));

  a->tag = malloc(strlen(tag) + 1);
  strcpy(a->tag, tag);

  a->contents = malloc(strlen(contents) + 1);
  strcpy(a->contents, contents);

  a->state = mpc_state_new();

  a->children_num = 0;
  a->children = NULL;
  return a;

}

mpc_ast_t *mpc_ast_build(int n, const char *tag, ...) {

  mpc_ast_t *a = mpc_ast_new(tag, "");

  int i;
  va_list va;
  va_start(va, tag);

  for (i = 0; i < n; i++) {
    mpc_ast_add_child(a, va_arg(va, mpc_ast_t*));
  }

  va_end(va);

  return a;

}

mpc_ast_t *mpc_ast_add_root(mpc_ast_t *a) {

  mpc_ast_t *r;

  if (a == NULL) { return a; }
  if (a->children_num == 0) { return a; }
  if (a->children_num == 1) { return a; }

  r = mpc_ast_new(">", "");
  mpc_ast_add_child(r, a);
  return r;
}

int mpc_ast_eq(mpc_ast_t *a, mpc_ast_t *b) {

  int i;

  if (strcmp(a->tag, b->tag) != 0) { return 0; }
  if (strcmp(a->contents, b->contents) != 0) { return 0; }
  if (a->children_num != b->children_num) { return 0; }

  for (i = 0; i < a->children_num; i++) {
    if (!mpc_ast_eq(a->children[i], b->children[i])) { return 0; }
  }

  return 1;
}

mpc_ast_t *mpc_ast_add_child(mpc_ast_t *r, mpc_ast_t *a) {
  r->children_num++;
  r->children = realloc(r->children, sizeof(mpc_ast_t*) * r->children_num);
  r->children[r->children_num-1] = a;
  return r;
}

mpc_ast_t *mpc_ast_add_tag(mpc_ast_t *a, const char *t) {
  if (a == NULL) { return a; }
  a->tag = realloc(a->tag, strlen(t) + 1 + strlen(a->tag) + 1);
  memmove(a->tag + strlen(t) + 1, a->tag, strlen(a->tag)+1);
  memmove(a->tag, t, strlen(t));
  memmove(a->tag + strlen(t), "|", 1);
  return a;
}

mpc_ast_t *mpc_ast_add_root_tag(mpc_ast_t *a, const char *t) {
  if (a == NULL) { return a; }
  a->tag = realloc(a->tag, (strlen(t)-1) + strlen(a->tag) + 1);
  memmove(a->tag + (strlen(t)-1), a->tag, strlen(a->tag)+1);
  memmove(a->tag, t, (strlen(t)-1));
  return a;
}

mpc_ast_t *mpc_ast_tag(mpc_ast_t *a, const char *t) {
  a->tag = realloc(a->tag, strlen(t) + 1);
  strcpy(a->tag, t);
  return a;
}

mpc_ast_t *mpc_ast_state(mpc_ast_t *a, mpc_state_t s) {
  if (a == NULL) { return a; }
  a->state = s;
  return a;
}

static void mpc_ast_print_depth(mpc_ast_t *a, int d, FILE *fp) {

  int i;

  if (a == NULL) {
    fprintf(fp, "NULL\n");
    return;
  }

  for (i = 0; i < d; i++) { fprintf(fp, "  "); }

  if (strlen(a->contents)) {
    fprintf(fp, "%s:%lu:%lu '%s'\n", a->tag,
      (long unsigned int)(a->state.row+1),
      (long unsigned int)(a->state.col+1),
      a->contents);
  } else {
    fprintf(fp, "%s \n", a->tag);
  }

  for (i = 0; i < a->children_num; i++) {
    mpc_ast_print_depth(a->children[i], d+1, fp);
  }

}

void mpc_ast_print(mpc_ast_t *a) {
  mpc_ast_print_depth(a, 0, stdout);
}

void mpc_ast_print_to(mpc_ast_t *a, FILE *fp) {
  mpc_ast_print_depth(a, 0, fp);
}

int mpc_ast_get_index(mpc_ast_t *ast, const char *tag) {
  return mpc_ast_get_index_lb(ast, tag, 0);
}

int mpc_ast_get_index_lb(mpc_ast_t *ast, const char *tag, int lb) {
  int i;

  for(i=lb; i<ast->children_num; i++) {
    if(strcmp(ast->children[i]->tag, tag) == 0) {
      return i;
    }
  }

  return -1;
}

mpc_ast_t *mpc_ast_get_child(mpc_ast_t *ast, const char *tag) {
  return mpc_ast_get_child_lb(ast, tag, 0);
}

mpc_ast_t *mpc_ast_get_child_lb(mpc_ast_t *ast, const char *tag, int lb) {
  int i;

  for(i=lb; i<ast->children_num; i++) {
    if(strcmp(ast->children[i]->tag, tag) == 0) {
      return ast->children[i];
    }
  }

  return NULL;
}

mpc_ast_trav_t *mpc_ast_traverse_start(mpc_ast_t *ast,
                                       mpc_ast_trav_order_t order)
{
  mpc_ast_trav_t *trav, *n_trav;
  mpc_ast_t *cnode = ast;

  /* Create the traversal structure */
  trav = malloc(sizeof(mpc_ast_trav_t));
  trav->curr_node = cnode;
  trav->parent = NULL;
  trav->curr_child = 0;
  trav->order = order;

  /* Get start node */
  switch(order) {
    case mpc_ast_trav_order_pre:
      /* Nothing else is needed for pre order start */
      break;

    case mpc_ast_trav_order_post:
      while(cnode->children_num > 0) {
        cnode = cnode->children[0];

        n_trav = malloc(sizeof(mpc_ast_trav_t));
        n_trav->curr_node = cnode;
        n_trav->parent = trav;
        n_trav->curr_child = 0;
        n_trav->order = order;

        trav = n_trav;
      }

      break;

    default:
      /* Unreachable, but compiler complaints */
      break;
  }

  return trav;
}

mpc_ast_t *mpc_ast_traverse_next(mpc_ast_trav_t **trav) {
  mpc_ast_trav_t *n_trav, *to_free;
  mpc_ast_t *ret = NULL;
  int cchild;

  /* The end of traversal was reached */
  if(*trav == NULL) return NULL;

  switch((*trav)->order) {
    case mpc_ast_trav_order_pre:
      ret = (*trav)->curr_node;

      /* If there aren't any more children, go up */
      while(*trav != NULL &&
        (*trav)->curr_child >= (*trav)->curr_node->children_num)
      {
        to_free = *trav;
        *trav = (*trav)->parent;
        free(to_free);
      }

      /* If trav is NULL, the end was reached */
      if(*trav == NULL) {
        break;
      }

      /* Go to next child */
      n_trav = malloc(sizeof(mpc_ast_trav_t));

      cchild = (*trav)->curr_child;
      n_trav->curr_node = (*trav)->curr_node->children[cchild];
      n_trav->parent = *trav;
      n_trav->curr_child = 0;
      n_trav->order = (*trav)->order;

      (*trav)->curr_child++;
      *trav = n_trav;

      break;

    case mpc_ast_trav_order_post:
      ret = (*trav)->curr_node;

      /* Move up tree to the parent If the parent doesn't have any more nodes,
       * then this is the current node. If it does, move down to its left most
       * child. Also, free the previous traversal node */
      to_free = *trav;
      *trav = (*trav)->parent;
      free(to_free);

      if(*trav == NULL)
        break;

      /* Next child */
      (*trav)->curr_child++;

      /* If there aren't any more children, this is the next node */
      if((*trav)->curr_child >= (*trav)->curr_node->children_num) {
        break;
      }

      /* If there are still more children, find the leftmost child from this
       * node */
      while((*trav)->curr_node->children_num > 0) {
        n_trav = malloc(sizeof(mpc_ast_trav_t));

        cchild = (*trav)->curr_child;
        n_trav->curr_node = (*trav)->curr_node->children[cchild];
        n_trav->parent = *trav;
        n_trav->curr_child = 0;
        n_trav->order = (*trav)->order;

        *trav = n_trav;
      }

    default:
      /* Unreachable, but compiler complaints */
      break;
  }

  return ret;
}

void mpc_ast_traverse_free(mpc_ast_trav_t **trav) {
  mpc_ast_trav_t *n_trav;

  /* Go through parents until all are free */
  while(*trav != NULL) {
      n_trav = (*trav)->parent;
      free(*trav);
      *trav = n_trav;
  }
}

mpc_val_t *mpcf_fold_ast(int n, mpc_val_t **xs) {

  int i, j;
  mpc_ast_t** as = (mpc_ast_t**)xs;
  mpc_ast_t *r;

  if (n == 0) { return NULL; }
  if (n == 1) { return xs[0]; }
  if (n == 2 && xs[1] == NULL) { return xs[0]; }
  if (n == 2 && xs[0] == NULL) { return xs[1]; }

  r = mpc_ast_new(">", "");

  for (i = 0; i < n; i++) {

    if (as[i] == NULL) { continue; }

    if        (as[i] && as[i]->children_num == 0) {
      mpc_ast_add_child(r, as[i]);
    } else if (as[i] && as[i]->children_num == 1) {
      mpc_ast_add_child(r, mpc_ast_add_root_tag(as[i]->children[0], as[i]->tag));
      mpc_ast_delete_no_children(as[i]);
    } else if (as[i] && as[i]->children_num >= 2) {
      for (j = 0; j < as[i]->children_num; j++) {
        mpc_ast_add_child(r, as[i]->children[j]);
      }
      mpc_ast_delete_no_children(as[i]);
    }

  }

  if (r->children_num) {
    r->state = r->children[0]->state;
  }

  return r;
}

mpc_val_t *mpcf_str_ast(mpc_val_t *c) {
  mpc_ast_t *a = mpc_ast_new("", c);
  free(c);
  return a;
}

mpc_val_t *mpcf_state_ast(int n, mpc_val_t **xs) {
  mpc_state_t *s = ((mpc_state_t**)xs)[0];
  mpc_ast_t *a = ((mpc_ast_t**)xs)[1];
  (void)n;
  a = mpc_ast_state(a, *s);
  free(s);
  return a;
}

mpc_parser_t *mpca_state(mpc_parser_t *a) {
  return mpc_and(2, mpcf_state_ast, mpc_state(), a, free);
}

mpc_parser_t *mpca_tag(mpc_parser_t *a, const char *t) {
  return mpc_apply_to(a, (mpc_apply_to_t)mpc_ast_tag, (void*)t);
}

mpc_parser_t *mpca_add_tag(mpc_parser_t *a, const char *t) {
  return mpc_apply_to(a, (mpc_apply_to_t)mpc_ast_add_tag, (void*)t);
}

mpc_parser_t *mpca_root(mpc_parser_t *a) {
  return mpc_apply(a, (mpc_apply_t)mpc_ast_add_root);
}

mpc_parser_t *mpca_not(mpc_parser_t *a) { return mpc_not(a, (mpc_dtor_t)mpc_ast_delete); }
mpc_parser_t *mpca_maybe(mpc_parser_t *a) { return mpc_maybe(a); }
mpc_parser_t *mpca_many(mpc_parser_t *a) { return mpc_many(mpcf_fold_ast, a); }
mpc_parser_t *mpca_many1(mpc_parser_t *a) { return mpc_many1(mpcf_fold_ast, a); }
mpc_parser_t *mpca_count(int n, mpc_parser_t *a) { return mpc_count(n, mpcf_fold_ast, a, (mpc_dtor_t)mpc_ast_delete); }

mpc_parser_t *mpca_or(int n, ...) {

  int i;
  va_list va;

  mpc_parser_t *p = mpc_undefined();

  p->type = MPC_TYPE_OR;
  p->data.or.n = n;
  p->data.or.xs = malloc(sizeof(mpc_parser_t*) * n);

  va_start(va, n);
  for (i = 0; i < n; i++) {
    p->data.or.xs[i] = va_arg(va, mpc_parser_t*);
  }
  va_end(va);

  return p;

}

mpc_parser_t *mpca_and(int n, ...) {

  int i;
  va_list va;

  mpc_parser_t *p = mpc_undefined();

  p->type = MPC_TYPE_AND;
  p->data.and.n = n;
  p->data.and.f = mpcf_fold_ast;
  p->data.and.xs = malloc(sizeof(mpc_parser_t*) * n);
  p->data.and.dxs = malloc(sizeof(mpc_dtor_t) * (n-1));

  va_start(va, n);
  for (i = 0; i < n; i++) {
    p->data.and.xs[i] = va_arg(va, mpc_parser_t*);
  }
  for (i = 0; i < (n-1); i++) {
    p->data.and.dxs[i] = (mpc_dtor_t)mpc_ast_delete;
  }
  va_end(va);

  return p;
}

mpc_parser_t *mpca_total(mpc_parser_t *a) { return mpc_total(a, (mpc_dtor_t)mpc_ast_delete); }

/*
** Grammar Parser
*/

/*
** This is another interesting bootstrapping.
**
** Having a general purpose AST type allows
** users to specify the grammar alone and
** let all fold rules be automatically taken
** care of by existing functions.
**
** You don't get to control the type spat
** out but this means you can make a nice
** parser to take in some grammar in nice
** syntax and spit out a parser that works.
**
** The grammar for this looks surprisingly
** like regex but the main difference is that
** it is now whitespace insensitive and the
** base type takes literals of some form.
*/

/*
**
**  ### Grammar Grammar
**
**      <grammar> : (<term> "|" <grammar>) | <term>
**
**      <term> : <factor>*
**
**      <factor> : <base>
**               | <base> "*"
**               | <base> "+"
**               | <base> "?"
**               | <base> "{" <digits> "}"
**
**      <base> : "<" (<digits> | <ident>) ">"
**             | <string_lit>
**             | <char_lit>
**             | <regex_lit> <regex_mode>
**             | "(" <grammar> ")"
*/

typedef struct {
  va_list *va;
  int parsers_num;
  mpc_parser_t **parsers;
  int flags;
} mpca_grammar_st_t;

static mpc_val_t *mpcaf_grammar_or(int n, mpc_val_t **xs) {
  (void) n;
  if (xs[1] == NULL) { return xs[0]; }
  else { return mpca_or(2, xs[0], xs[1]); }
}

static mpc_val_t *mpcaf_grammar_and(int n, mpc_val_t **xs) {
  int i;
  mpc_parser_t *p = mpc_pass();
  for (i = 0; i < n; i++) {
    if (xs[i] != NULL) { p = mpca_and(2, p, xs[i]); }
  }
  return p;
}

static mpc_val_t *mpcaf_grammar_repeat(int n, mpc_val_t **xs) {
  int num;
  (void) n;
  if (xs[1] == NULL) { return xs[0]; }
  switch(((char*)xs[1])[0])
  {
    case '*': { free(xs[1]); return mpca_many(xs[0]); }; break;
    case '+': { free(xs[1]); return mpca_many1(xs[0]); }; break;
    case '?': { free(xs[1]); return mpca_maybe(xs[0]); }; break;
    case '!': { free(xs[1]); return mpca_not(xs[0]); }; break;
    default:
      num = *((int*)xs[1]);
      free(xs[1]);
  }
  return mpca_count(num, xs[0]);
}

static mpc_val_t *mpcaf_grammar_string(mpc_val_t *x, void *s) {
  mpca_grammar_st_t *st = s;
  char *y = mpcf_unescape(x);
  mpc_parser_t *p = (st->flags & MPCA_LANG_WHITESPACE_SENSITIVE) ? mpc_string(y) : mpc_tok(mpc_string(y));
  free(y);
  return mpca_state(mpca_tag(mpc_apply(p, mpcf_str_ast), "string"));
}

static mpc_val_t *mpcaf_grammar_char(mpc_val_t *x, void *s) {
  mpca_grammar_st_t *st = s;
  char *y = mpcf_unescape(x);
  mpc_parser_t *p = (st->flags & MPCA_LANG_WHITESPACE_SENSITIVE) ? mpc_char(y[0]) : mpc_tok(mpc_char(y[0]));
  free(y);
  return mpca_state(mpca_tag(mpc_apply(p, mpcf_str_ast), "char"));
}

static mpc_val_t *mpcaf_fold_regex(int n, mpc_val_t **xs) {
  char *y = xs[0];
  char *m = xs[1];
  mpca_grammar_st_t *st = xs[2];
  mpc_parser_t *p;
  int mode = MPC_RE_DEFAULT;

  (void)n;
  if (strchr(m, 'm')) { mode |= MPC_RE_MULTILINE; }
  if (strchr(m, 's')) { mode |= MPC_RE_DOTALL; }
  y = mpcf_unescape_regex(y);
  p = (st->flags & MPCA_LANG_WHITESPACE_SENSITIVE) ? mpc_re_mode(y, mode) : mpc_tok(mpc_re_mode(y, mode));
  free(y);
  free(m);

  return mpca_state(mpca_tag(mpc_apply(p, mpcf_str_ast), "regex"));
}

/* Should this just use `isdigit` instead? */
static int is_number(const char* s) {
  size_t i;
  for (i = 0; i < strlen(s); i++) { if (!strchr("0123456789", s[i])) { return 0; } }
  return 1;
}

static mpc_parser_t *mpca_grammar_find_parser(char *x, mpca_grammar_st_t *st) {

  int i;
  mpc_parser_t *p;

  /* Case of Number */
  if (is_number(x)) {

    i = strtol(x, NULL, 10);

    while (st->parsers_num <= i) {
      st->parsers_num++;
      st->parsers = realloc(st->parsers, sizeof(mpc_parser_t*) * st->parsers_num);
      st->parsers[st->parsers_num-1] = va_arg(*st->va, mpc_parser_t*);
      if (st->parsers[st->parsers_num-1] == NULL) {
        return mpc_failf("No Parser in position %i! Only supplied %i Parsers!", i, st->parsers_num);
      }
    }

    return st->parsers[st->parsers_num-1];

  /* Case of Identifier */
  } else {

    /* Search Existing Parsers */
    for (i = 0; i < st->parsers_num; i++) {
      mpc_parser_t *q = st->parsers[i];
      if (q == NULL) { return mpc_failf("Unknown Parser '%s'!", x); }
      if (q->name && strcmp(q->name, x) == 0) { return q; }
    }

    /* Search New Parsers */
    while (1) {

      p = va_arg(*st->va, mpc_parser_t*);

      st->parsers_num++;
      st->parsers = realloc(st->parsers, sizeof(mpc_parser_t*) * st->parsers_num);
      st->parsers[st->parsers_num-1] = p;

      if (p == NULL || p->name == NULL) { return mpc_failf("Unknown Parser '%s'!", x); }
      if (p->name && strcmp(p->name, x) == 0) { return p; }

    }

  }

}

static mpc_val_t *mpcaf_grammar_id(mpc_val_t *x, void *s) {

  mpca_grammar_st_t *st = s;
  mpc_parser_t *p = mpca_grammar_find_parser(x, st);
  free(x);

  if (p->name) {
    return mpca_state(mpca_root(mpca_add_tag(p, p->name)));
  } else {
    return mpca_state(mpca_root(p));
  }
}

mpc_parser_t *mpca_grammar_st(const char *grammar, mpca_grammar_st_t *st) {

  char *err_msg;
  mpc_parser_t *err_out;
  mpc_result_t r;
  mpc_parser_t *GrammarTotal, *Grammar, *Term, *Factor, *Base;

  GrammarTotal = mpc_new("grammar_total");
  Grammar = mpc_new("grammar");
  Term = mpc_new("term");
  Factor = mpc_new("factor");
  Base = mpc_new("base");

  mpc_define(GrammarTotal,
    mpc_predictive(mpc_total(Grammar, mpc_soft_delete))
  );

  mpc_define(Grammar, mpc_and(2, mpcaf_grammar_or,
    Term,
    mpc_maybe(mpc_and(2, mpcf_snd_free, mpc_sym("|"), Grammar, free)),
    mpc_soft_delete
  ));

  mpc_define(Term, mpc_many1(mpcaf_grammar_and, Factor));

  mpc_define(Factor, mpc_and(2, mpcaf_grammar_repeat,
    Base,
      mpc_or(6,
        mpc_sym("*"),
        mpc_sym("+"),
        mpc_sym("?"),
        mpc_sym("!"),
        mpc_tok_brackets(mpc_int(), free),
        mpc_pass()),
    mpc_soft_delete
  ));

  mpc_define(Base, mpc_or(5,
    mpc_apply_to(mpc_tok(mpc_string_lit()), mpcaf_grammar_string, st),
    mpc_apply_to(mpc_tok(mpc_char_lit()),   mpcaf_grammar_char, st),
    mpc_tok(mpc_and(3, mpcaf_fold_regex, mpc_regex_lit(), mpc_many(mpcf_strfold, mpc_oneof("ms")), mpc_lift_val(st), free, free)),
    mpc_apply_to(mpc_tok_braces(mpc_or(2, mpc_digits(), mpc_ident()), free), mpcaf_grammar_id, st),
    mpc_tok_parens(Grammar, mpc_soft_delete)
  ));

  mpc_optimise(GrammarTotal);
  mpc_optimise(Grammar);
  mpc_optimise(Factor);
  mpc_optimise(Term);
  mpc_optimise(Base);

  if(!mpc_parse("<mpc_grammar_compiler>", grammar, GrammarTotal, &r)) {
    err_msg = mpc_err_string(r.error);
    err_out = mpc_failf("Invalid Grammar: %s", err_msg);
    mpc_err_delete(r.error);
    free(err_msg);
    r.output = err_out;
  }

  mpc_cleanup(5, GrammarTotal, Grammar, Term, Factor, Base);

  mpc_optimise(r.output);

  return (st->flags & MPCA_LANG_PREDICTIVE) ? mpc_predictive(r.output) : r.output;

}

mpc_parser_t *mpca_grammar(int flags, const char *grammar, ...) {
  mpca_grammar_st_t st;
  mpc_parser_t *res;
  va_list va;
  va_start(va, grammar);

  st.va = &va;
  st.parsers_num = 0;
  st.parsers = NULL;
  st.flags = flags;

  res = mpca_grammar_st(grammar, &st);
  free(st.parsers);
  va_end(va);
  return res;
}

typedef struct {
  char *ident;
  char *name;
  mpc_parser_t *grammar;
} mpca_stmt_t;

static mpc_val_t *mpca_stmt_afold(int n, mpc_val_t **xs) {
  mpca_stmt_t *stmt = malloc(sizeof(mpca_stmt_t));
  stmt->ident = ((char**)xs)[0];
  stmt->name = ((char**)xs)[1];
  stmt->grammar = ((mpc_parser_t**)xs)[3];
  (void) n;
  free(((char**)xs)[2]);
  free(((char**)xs)[4]);

  return stmt;
}

static mpc_val_t *mpca_stmt_fold(int n, mpc_val_t **xs) {

  int i;
  mpca_stmt_t **stmts = malloc(sizeof(mpca_stmt_t*) * (n+1));

  for (i = 0; i < n; i++) {
    stmts[i] = xs[i];
  }
  stmts[n] = NULL;

  return stmts;
}

static void mpca_stmt_list_delete(mpc_val_t *x) {

  mpca_stmt_t **stmts = x;

  while(*stmts) {
    mpca_stmt_t *stmt = *stmts;
    free(stmt->ident);
    free(stmt->name);
    mpc_soft_delete(stmt->grammar);
    free(stmt);
    stmts++;
  }
  free(x);

}

static mpc_val_t *mpca_stmt_list_apply_to(mpc_val_t *x, void *s) {

  mpca_grammar_st_t *st = s;
  mpca_stmt_t *stmt;
  mpca_stmt_t **stmts = x;
  mpc_parser_t *left;

  while(*stmts) {
    stmt = *stmts;
    left = mpca_grammar_find_parser(stmt->ident, st);
    if (st->flags & MPCA_LANG_PREDICTIVE) { stmt->grammar = mpc_predictive(stmt->grammar); }
    if (stmt->name) { stmt->grammar = mpc_expect(stmt->grammar, stmt->name); }
    mpc_optimise(stmt->grammar);
    mpc_define(left, stmt->grammar);
    free(stmt->ident);
    free(stmt->name);
    free(stmt);
    stmts++;
  }

  free(x);

  return NULL;
}

static mpc_err_t *mpca_lang_st(mpc_input_t *i, mpca_grammar_st_t *st) {

  mpc_result_t r;
  mpc_err_t *e;
  mpc_parser_t *Lang, *Stmt, *Grammar, *Term, *Factor, *Base;

  Lang    = mpc_new("lang");
  Stmt    = mpc_new("stmt");
  Grammar = mpc_new("grammar");
  Term    = mpc_new("term");
  Factor  = mpc_new("factor");
  Base    = mpc_new("base");

  mpc_define(Lang, mpc_apply_to(
    mpc_total(mpc_predictive(mpc_many(mpca_stmt_fold, Stmt)), mpca_stmt_list_delete),
    mpca_stmt_list_apply_to, st
  ));

  mpc_define(Stmt, mpc_and(5, mpca_stmt_afold,
    mpc_tok(mpc_ident()), mpc_maybe(mpc_tok(mpc_string_lit())), mpc_sym(":"), Grammar, mpc_sym(";"),
    free, free, free, mpc_soft_delete
  ));

  mpc_define(Grammar, mpc_and(2, mpcaf_grammar_or,
      Term,
      mpc_maybe(mpc_and(2, mpcf_snd_free, mpc_sym("|"), Grammar, free)),
      mpc_soft_delete
  ));

  mpc_define(Term, mpc_many1(mpcaf_grammar_and, Factor));

  mpc_define(Factor, mpc_and(2, mpcaf_grammar_repeat,
    Base,
      mpc_or(6,
        mpc_sym("*"),
        mpc_sym("+"),
        mpc_sym("?"),
        mpc_sym("!"),
        mpc_tok_brackets(mpc_int(), free),
        mpc_pass()),
    mpc_soft_delete
  ));

  mpc_define(Base, mpc_or(5,
    mpc_apply_to(mpc_tok(mpc_string_lit()), mpcaf_grammar_string, st),
    mpc_apply_to(mpc_tok(mpc_char_lit()),   mpcaf_grammar_char, st),
    mpc_tok(mpc_and(3, mpcaf_fold_regex, mpc_regex_lit(), mpc_many(mpcf_strfold, mpc_oneof("ms")), mpc_lift_val(st), free, free)),
    mpc_apply_to(mpc_tok_braces(mpc_or(2, mpc_digits(), mpc_ident()), free), mpcaf_grammar_id, st),
    mpc_tok_parens(Grammar, mpc_soft_delete)
  ));

  mpc_optimise(Lang);
  mpc_optimise(Stmt);
  mpc_optimise(Grammar);
  mpc_optimise(Term);
  mpc_optimise(Factor);
  mpc_optimise(Base);

  if (!mpc_parse_input(i, Lang, &r)) {
    e = r.error;
  } else {
    e = NULL;
  }

  mpc_cleanup(6, Lang, Stmt, Grammar, Term, Factor, Base);

  return e;
}

mpc_err_t *mpca_lang_file(int flags, FILE *f, ...) {
  mpca_grammar_st_t st;
  mpc_input_t *i;
  mpc_err_t *err;

  va_list va;
  va_start(va, f);

  st.va = &va;
  st.parsers_num = 0;
  st.parsers = NULL;
  st.flags = flags;

  i = mpc_input_new_file("<mpca_lang_file>", f);
  err = mpca_lang_st(i, &st);
  mpc_input_delete(i);

  free(st.parsers);
  va_end(va);
  return err;
}

mpc_err_t *mpca_lang_pipe(int flags, FILE *p, ...) {
  mpca_grammar_st_t st;
  mpc_input_t *i;
  mpc_err_t *err;

  va_list va;
  va_start(va, p);

  st.va = &va;
  st.parsers_num = 0;
  st.parsers = NULL;
  st.flags = flags;

  i = mpc_input_new_pipe("<mpca_lang_pipe>", p);
  err = mpca_lang_st(i, &st);
  mpc_input_delete(i);

  free(st.parsers);
  va_end(va);
  return err;
}

mpc_err_t *mpca_lang(int flags, const char *language, ...) {

  mpca_grammar_st_t st;
  mpc_input_t *i;
  mpc_err_t *err;

  va_list va;
  va_start(va, language);

  st.va = &va;
  st.parsers_num = 0;
  st.parsers = NULL;
  st.flags = flags;

  i = mpc_input_new_string("<mpca_lang>", language);
  err = mpca_lang_st(i, &st);
  mpc_input_delete(i);

  free(st.parsers);
  va_end(va);
  return err;
}

mpc_err_t *mpca_lang_contents(int flags, const char *filename, ...) {

  mpca_grammar_st_t st;
  mpc_input_t *i;
  mpc_err_t *err;

  va_list va;

  FILE *f = fopen(filename, "rb");

  if (f == NULL) {
    err = mpc_err_file(filename, "Unable to open file!");
    return err;
  }

  va_start(va, filename);

  st.va = &va;
  st.parsers_num = 0;
  st.parsers = NULL;
  st.flags = flags;

  i = mpc_input_new_file(filename, f);
  err = mpca_lang_st(i, &st);
  mpc_input_delete(i);

  free(st.parsers);
  va_end(va);

  fclose(f);

  return err;
}

static int mpc_nodecount_unretained(mpc_parser_t* p, int force) {

  int i, total;

  if (p->retained && !force) { return 0; }

  if (p->type == MPC_TYPE_EXPECT) { return 1 + mpc_nodecount_unretained(p->data.expect.x, 0); }

  if (p->type == MPC_TYPE_APPLY)    { return 1 + mpc_nodecount_unretained(p->data.apply.x, 0); }
  if (p->type == MPC_TYPE_APPLY_TO) { return 1 + mpc_nodecount_unretained(p->data.apply_to.x, 0); }
  if (p->type == MPC_TYPE_PREDICT)  { return 1 + mpc_nodecount_unretained(p->data.predict.x, 0); }

  if (p->type == MPC_TYPE_CHECK)    { return 1 + mpc_nodecount_unretained(p->data.check.x, 0); }
  if (p->type == MPC_TYPE_CHECK_WITH) { return 1 + mpc_nodecount_unretained(p->data.check_with.x, 0); }

  if (p->type == MPC_TYPE_NOT)   { return 1 + mpc_nodecount_unretained(p->data.not.x, 0); }
  if (p->type == MPC_TYPE_MAYBE) { return 1 + mpc_nodecount_unretained(p->data.not.x, 0); }

  if (p->type == MPC_TYPE_MANY)  { return 1 + mpc_nodecount_unretained(p->data.repeat.x, 0); }
  if (p->type == MPC_TYPE_MANY1) { return 1 + mpc_nodecount_unretained(p->data.repeat.x, 0); }
  if (p->type == MPC_TYPE_COUNT) { return 1 + mpc_nodecount_unretained(p->data.repeat.x, 0); }
  if (p->type == MPC_TYPE_SEPBY1) {
    total = 1;
    total += mpc_nodecount_unretained(p->data.sepby1.x, 0);
    total += mpc_nodecount_unretained(p->data.sepby1.sep, 0);
    total += mpc_nodecount_unretained(p->data.sepby1.x, 0);
    return total;
  }

  if (p->type == MPC_TYPE_OR) {
    total = 1;
    for(i = 0; i < p->data.or.n; i++) {
      total += mpc_nodecount_unretained(p->data.or.xs[i], 0);
    }
    return total;
  }

  if (p->type == MPC_TYPE_AND) {
    total = 1;
    for(i = 0; i < p->data.and.n; i++) {
      total += mpc_nodecount_unretained(p->data.and.xs[i], 0);
    }
    return total;
  }

  return 1;

}

void mpc_stats(mpc_parser_t* p) {
  printf("Stats\n");
  printf("=====\n");
  printf("Node Count: %i\n", mpc_nodecount_unretained(p, 1));
}

static void mpc_optimise_unretained(mpc_parser_t *p, int force) {

  int i, n, m;
  mpc_parser_t *t;

  if (p->retained && !force) { return; }

  /* Optimise Subexpressions */

  if (p->type == MPC_TYPE_EXPECT)     { mpc_optimise_unretained(p->data.expect.x, 0); }
  if (p->type == MPC_TYPE_APPLY)      { mpc_optimise_unretained(p->data.apply.x, 0); }
  if (p->type == MPC_TYPE_APPLY_TO)   { mpc_optimise_unretained(p->data.apply_to.x, 0); }
  if (p->type == MPC_TYPE_CHECK)      { mpc_optimise_unretained(p->data.check.x, 0); }
  if (p->type == MPC_TYPE_CHECK_WITH) { mpc_optimise_unretained(p->data.check_with.x, 0); }
  if (p->type == MPC_TYPE_PREDICT)    { mpc_optimise_unretained(p->data.predict.x, 0); }
  if (p->type == MPC_TYPE_NOT)        { mpc_optimise_unretained(p->data.not.x, 0); }
  if (p->type == MPC_TYPE_MAYBE)      { mpc_optimise_unretained(p->data.not.x, 0); }
  if (p->type == MPC_TYPE_MANY)       { mpc_optimise_unretained(p->data.repeat.x, 0); }
  if (p->type == MPC_TYPE_MANY1)      { mpc_optimise_unretained(p->data.repeat.x, 0); }
  if (p->type == MPC_TYPE_COUNT)      { mpc_optimise_unretained(p->data.repeat.x, 0); }
  if (p->type == MPC_TYPE_SEPBY1)     {
    mpc_optimise_unretained(p->data.sepby1.x, 0);
    mpc_optimise_unretained(p->data.sepby1.sep, 0);
  }

  if (p->type == MPC_TYPE_OR) {
    for(i = 0; i < p->data.or.n; i++) {
      mpc_optimise_unretained(p->data.or.xs[i], 0);
    }
  }

  if (p->type == MPC_TYPE_AND) {
    for(i = 0; i < p->data.and.n; i++) {
      mpc_optimise_unretained(p->data.and.xs[i], 0);
    }
  }

  /* Perform optimisations */

  while (1) {

    /* Merge rhs `or` */
    if (p->type == MPC_TYPE_OR
    &&  p->data.or.xs[p->data.or.n-1]->type == MPC_TYPE_OR
    && !p->data.or.xs[p->data.or.n-1]->retained) {
      t = p->data.or.xs[p->data.or.n-1];
      n = p->data.or.n; m = t->data.or.n;
      p->data.or.n = n + m - 1;
      p->data.or.xs = realloc(p->data.or.xs, sizeof(mpc_parser_t*) * (n + m -1));
      memmove(p->data.or.xs + n - 1, t->data.or.xs, m * sizeof(mpc_parser_t*));
      free(t->data.or.xs); free(t->name); free(t);
      continue;
    }

    /* Merge lhs `or` */
    if (p->type == MPC_TYPE_OR
    &&  p->data.or.xs[0]->type == MPC_TYPE_OR
    && !p->data.or.xs[0]->retained) {
      t = p->data.or.xs[0];
      n = p->data.or.n; m = t->data.or.n;
      p->data.or.n = n + m - 1;
      p->data.or.xs = realloc(p->data.or.xs, sizeof(mpc_parser_t*) * (n + m -1));
      memmove(p->data.or.xs + m, p->data.or.xs + 1, (n - 1) * sizeof(mpc_parser_t*));
      memmove(p->data.or.xs, t->data.or.xs, m * sizeof(mpc_parser_t*));
      free(t->data.or.xs); free(t->name); free(t);
      continue;
    }

    /* Remove ast `pass` */
    if (p->type == MPC_TYPE_AND
    &&  p->data.and.n == 2
    &&  p->data.and.xs[0]->type == MPC_TYPE_PASS
    && !p->data.and.xs[0]->retained
    &&  p->data.and.f == mpcf_fold_ast) {
      t = p->data.and.xs[1];
      mpc_delete(p->data.and.xs[0]);
      free(p->data.and.xs); free(p->data.and.dxs); free(p->name);
      memcpy(p, t, sizeof(mpc_parser_t));
      free(t);
      continue;
    }

    /* Merge ast lhs `and` */
    if (p->type == MPC_TYPE_AND
    &&  p->data.and.f == mpcf_fold_ast
    &&  p->data.and.xs[0]->type == MPC_TYPE_AND
    && !p->data.and.xs[0]->retained
    &&  p->data.and.xs[0]->data.and.f == mpcf_fold_ast) {
      t = p->data.and.xs[0];
      n = p->data.and.n; m = t->data.and.n;
      p->data.and.n = n + m - 1;
      p->data.and.xs = realloc(p->data.and.xs, sizeof(mpc_parser_t*) * (n + m - 1));
      p->data.and.dxs = realloc(p->data.and.dxs, sizeof(mpc_dtor_t) * (n + m - 1 - 1));
      memmove(p->data.and.xs + m, p->data.and.xs + 1, (n - 1) * sizeof(mpc_parser_t*));
      memmove(p->data.and.xs, t->data.and.xs, m * sizeof(mpc_parser_t*));
      for (i = 0; i < p->data.and.n-1; i++) { p->data.and.dxs[i] = (mpc_dtor_t)mpc_ast_delete; }
      free(t->data.and.xs); free(t->data.and.dxs); free(t->name); free(t);
      continue;
    }

    /* Merge ast rhs `and` */
    if (p->type == MPC_TYPE_AND
    &&  p->data.and.f == mpcf_fold_ast
    &&  p->data.and.xs[p->data.and.n-1]->type == MPC_TYPE_AND
    && !p->data.and.xs[p->data.and.n-1]->retained
    &&  p->data.and.xs[p->data.and.n-1]->data.and.f == mpcf_fold_ast) {
      t = p->data.and.xs[p->data.and.n-1];
      n = p->data.and.n; m = t->data.and.n;
      p->data.and.n = n + m - 1;
      p->data.and.xs = realloc(p->data.and.xs, sizeof(mpc_parser_t*) * (n + m -1));
      p->data.and.dxs = realloc(p->data.and.dxs, sizeof(mpc_dtor_t) * (n + m - 1 - 1));
      memmove(p->data.and.xs + n - 1, t->data.and.xs, m * sizeof(mpc_parser_t*));
      for (i = 0; i < p->data.and.n-1; i++) { p->data.and.dxs[i] = (mpc_dtor_t)mpc_ast_delete; }
      free(t->data.and.xs); free(t->data.and.dxs); free(t->name); free(t);
      continue;
    }

    /* Remove re `lift` */
    if (p->type == MPC_TYPE_AND
    &&  p->data.and.n == 2
    &&  p->data.and.xs[0]->type == MPC_TYPE_LIFT
    &&  p->data.and.xs[0]->data.lift.lf == mpcf_ctor_str
    && !p->data.and.xs[0]->retained
    &&  p->data.and.f == mpcf_strfold) {
      t = p->data.and.xs[1];
      mpc_delete(p->data.and.xs[0]);
      free(p->data.and.xs); free(p->data.and.dxs); free(p->name);
      memcpy(p, t, sizeof(mpc_parser_t));
      free(t);
      continue;
    }

    /* Merge re lhs `and` */
    if (p->type == MPC_TYPE_AND
    &&  p->data.and.f == mpcf_strfold
    &&  p->data.and.xs[0]->type == MPC_TYPE_AND
    && !p->data.and.xs[0]->retained
    &&  p->data.and.xs[0]->data.and.f == mpcf_strfold) {
      t = p->data.and.xs[0];
      n = p->data.and.n; m = t->data.and.n;
      p->data.and.n = n + m - 1;
      p->data.and.xs = realloc(p->data.and.xs, sizeof(mpc_parser_t*) * (n + m - 1));
      p->data.and.dxs = realloc(p->data.and.dxs, sizeof(mpc_dtor_t) * (n + m - 1 - 1));
      memmove(p->data.and.xs + m, p->data.and.xs + 1, (n - 1) * sizeof(mpc_parser_t*));
      memmove(p->data.and.xs, t->data.and.xs, m * sizeof(mpc_parser_t*));
      for (i = 0; i < p->data.and.n-1; i++) { p->data.and.dxs[i] = free; }
      free(t->data.and.xs); free(t->data.and.dxs); free(t->name); free(t);
      continue;
    }

    /* Merge re rhs `and` */
    if (p->type == MPC_TYPE_AND
    &&  p->data.and.f == mpcf_strfold
    &&  p->data.and.xs[p->data.and.n-1]->type == MPC_TYPE_AND
    && !p->data.and.xs[p->data.and.n-1]->retained
    &&  p->data.and.xs[p->data.and.n-1]->data.and.f == mpcf_strfold) {
      t = p->data.and.xs[p->data.and.n-1];
      n = p->data.and.n; m = t->data.and.n;
      p->data.and.n = n + m - 1;
      p->data.and.xs = realloc(p->data.and.xs, sizeof(mpc_parser_t*) * (n + m -1));
      p->data.and.dxs = realloc(p->data.and.dxs, sizeof(mpc_dtor_t) * (n + m - 1 - 1));
      memmove(p->data.and.xs + n - 1, t->data.and.xs, m * sizeof(mpc_parser_t*));
      for (i = 0; i < p->data.and.n-1; i++) { p->data.and.dxs[i] = free; }
      free(t->data.and.xs); free(t->data.and.dxs); free(t->name); free(t);
      continue;
    }

    return;

  }

}

void mpc_optimise(mpc_parser_t *p) {
  mpc_optimise_unretained(p, 1);
}


/*
** Progress Analysis
** -----------------
**
** Read-only static analysis of a parser object graph.  The analysis only
** reads `type`, `data`, `name` and `retained`; it never parses input and
** never invokes a user callback.  Effect sets are the least fixed point of
** a monotone transfer function over the four effect bits.
*/

#define MPC_PA_FAILS \
  (MPC_PROGRESS_FAIL_EMPTY | MPC_PROGRESS_FAIL_CONSUMED)
#define MPC_PA_SUCCS \
  (MPC_PROGRESS_SUCC_EMPTY | MPC_PROGRESS_SUCC_CONSUMED)
#define MPC_PA_EMPTY  \
  (MPC_PROGRESS_FAIL_EMPTY | MPC_PROGRESS_SUCC_EMPTY)

/* Edge prefix requirements (how control can reach a structural child). */
enum {
  MPC_PA_EDGE_PLAIN = 0,  /* child is called unconditionally              */
  MPC_PA_EDGE_SUCC  = 1,  /* requires a zero-consumption success prefix   */
  MPC_PA_EDGE_FAIL  = 2,  /* requires zero-consumption failing prefixes   */
  MPC_PA_EDGE_SEP   = 3   /* sepby1 separator: element succeeded empty    */
};

typedef struct mpc_pa_edge {
  int to;
  int kind;
  int zero;
} mpc_pa_edge_t;

typedef struct mpc_pa_node {
  mpc_parser_t  *p;
  int            index;
  int            effects;
  int            unsupported;
  int            dfs;       /* Tarjan visit state */
  int            disc;
  int            lowlink;
  int            scc;
  int            edge_num;
  int            edge_slots;
  mpc_pa_edge_t *edges;
  char          *name;
} mpc_pa_node_t;

typedef struct {
  mpc_pa_node_t *nodes;
  int            node_num;
  int            node_slots;
  mpc_parser_t **keys;
  int           *vals;
  int            map_slots;
  int            map_count;
} mpc_pa_ctx_t;

static char *mpc_pa_strdup(const char *s) {
  size_t n;
  char *r;
  if (!s) { s = ""; }
  n = strlen(s) + 1;
  r = malloc(n);
  if (r) { memcpy(r, s, n); }
  return r;
}

static int mpc_pa_map_lookup(mpc_pa_ctx_t *c, mpc_parser_t *p) {
  int h, mask;
  if (c->map_slots == 0) { return -1; }
  mask = c->map_slots - 1;
  h = ((size_t)p >> 3) & mask;
  while (c->keys[h] != NULL) {
    if (c->keys[h] == p) { return c->vals[h]; }
    h = (h + 1) & mask;
  }
  return -1;
}

static int mpc_pa_map_grow(mpc_pa_ctx_t *c) {
  int n = c->map_slots == 0 ? 16 : c->map_slots * 2;
  mpc_parser_t **k = calloc((size_t)n, sizeof(mpc_parser_t*));
  int *v = malloc((size_t)n * sizeof(int));
  if (!k || !v) { free(k); free(v); return 0; }
  c->keys = k;
  c->vals = v;
  c->map_slots = n;
  return 1;
}

static int mpc_pa_map_insert(mpc_pa_ctx_t *c, mpc_parser_t *p, int idx) {
  int h, mask;
  if (c->map_slots == 0 || c->map_count * 10 >= c->map_slots * 7) {
    mpc_parser_t **oldk = c->keys;
    int *oldv = c->vals;
    int old = c->map_slots;
    int j, n;
    if (!mpc_pa_map_grow(c)) { return 0; }
    n = c->map_slots;
    for (j = 0; j < old; j++) {
      if (oldk[j]) {
        int hh, mm = n - 1;
        hh = ((size_t)oldk[j] >> 3) & mm;
        while (c->keys[hh] != NULL) { hh = (hh + 1) & mm; }
        c->keys[hh] = oldk[j];
        c->vals[hh] = oldv[j];
      }
    }
    free(oldk);
    free(oldv);
  }
  mask = c->map_slots - 1;
  h = ((size_t)p >> 3) & mask;
  while (c->keys[h] != NULL) {
    if (c->keys[h] == p) { c->vals[h] = idx; return 1; }
    h = (h + 1) & mask;
  }
  c->keys[h] = p;
  c->vals[h] = idx;
  c->map_count++;
  return 1;
}

static int mpc_pa_add_edge(mpc_pa_node_t *n, int to, int kind) {
  if (n->edge_num == n->edge_slots) {
    int s = n->edge_slots == 0 ? 2 : n->edge_slots * 2;
    mpc_pa_edge_t *e = realloc(n->edges, (size_t)s * sizeof(mpc_pa_edge_t));
    if (!e) { return 0; }
    n->edges = e;
    n->edge_slots = s;
  }
  n->edges[n->edge_num].to = to;
  n->edges[n->edge_num].kind = kind;
  n->edges[n->edge_num].zero = 0;
  n->edge_num++;
  return 1;
}

static void mpc_pa_discover(mpc_pa_ctx_t *c, mpc_parser_t *p);

/*
** Discovery is recursive and may grow c->nodes, so every node reference is
** re-derived from its index instead of holding a pointer across the call.
*/
static int mpc_pa_link(mpc_pa_ctx_t *c, int parent_idx,
                       mpc_parser_t *child, int kind) {
  int idx;
  mpc_pa_discover(c, child);
  idx = mpc_pa_map_lookup(c, child);
  if (idx < 0) { return 0; }
  return mpc_pa_add_edge(&c->nodes[parent_idx], idx, kind);
}

static void mpc_pa_discover(mpc_pa_ctx_t *c, mpc_parser_t *p) {
  int idx, j;

  if (mpc_pa_map_lookup(c, p) >= 0) { return; }

  if (c->node_num == c->node_slots) {
    int s = c->node_slots == 0 ? 16 : c->node_slots * 2;
    mpc_pa_node_t *t = realloc(c->nodes, (size_t)s * sizeof(mpc_pa_node_t));
    if (!t) { return; }
    c->nodes = t;
    c->node_slots = s;
  }

  idx = c->node_num;
  memset(&c->nodes[idx], 0, sizeof(mpc_pa_node_t));
  c->nodes[idx].p = p;
  c->nodes[idx].index = idx;
  c->nodes[idx].scc = -1;
  c->node_num++;
  if (!mpc_pa_map_insert(c, p, idx)) { return; }

  switch (p->type) {

    case MPC_TYPE_EXPECT:
      mpc_pa_link(c, idx, p->data.expect.x, MPC_PA_EDGE_PLAIN);
      break;

    case MPC_TYPE_APPLY:
      mpc_pa_link(c, idx, p->data.apply.x, MPC_PA_EDGE_PLAIN);
      break;

    case MPC_TYPE_APPLY_TO:
      mpc_pa_link(c, idx, p->data.apply_to.x, MPC_PA_EDGE_PLAIN);
      break;

    case MPC_TYPE_PREDICT:
      c->nodes[idx].unsupported = MPC_PROGRESS_UNSUPPORTED_PREDICTIVE;
      mpc_pa_link(c, idx, p->data.predict.x, MPC_PA_EDGE_PLAIN);
      break;

    case MPC_TYPE_CHECK:
      mpc_pa_link(c, idx, p->data.check.x, MPC_PA_EDGE_PLAIN);
      break;

    case MPC_TYPE_CHECK_WITH:
      mpc_pa_link(c, idx, p->data.check_with.x, MPC_PA_EDGE_PLAIN);
      break;

    case MPC_TYPE_NOT:
    case MPC_TYPE_MAYBE:
      mpc_pa_link(c, idx, p->data.not.x, MPC_PA_EDGE_PLAIN);
      break;

    case MPC_TYPE_MANY:
    case MPC_TYPE_MANY1:
      mpc_pa_link(c, idx, p->data.repeat.x, MPC_PA_EDGE_PLAIN);
      break;

    case MPC_TYPE_COUNT:
      if (p->data.repeat.n <= 0) {
        c->nodes[idx].unsupported =
          MPC_PROGRESS_UNSUPPORTED_NONPOSITIVE_COUNT;
      }
      mpc_pa_link(c, idx, p->data.repeat.x, MPC_PA_EDGE_PLAIN);
      break;

    case MPC_TYPE_SEPBY1:
      /* structural child order: element first, separator second */
      mpc_pa_link(c, idx, p->data.sepby1.x, MPC_PA_EDGE_PLAIN);
      mpc_pa_link(c, idx, p->data.sepby1.sep, MPC_PA_EDGE_SEP);
      break;

    case MPC_TYPE_OR:
      for (j = 0; j < p->data.or.n; j++) {
        mpc_pa_link(c, idx, p->data.or.xs[j],
          j == 0 ? MPC_PA_EDGE_PLAIN : MPC_PA_EDGE_FAIL);
      }
      break;

    case MPC_TYPE_AND:
      for (j = 0; j < p->data.and.n; j++) {
        mpc_pa_link(c, idx, p->data.and.xs[j],
          j == 0 ? MPC_PA_EDGE_PLAIN : MPC_PA_EDGE_SUCC);
      }
      break;

    default:
      break;
  }
}

/* Effect transfer for a count of n positive repetitions of a body. */
static int mpc_pa_count_effects(int fx, int n) {
  int z = 1, c = 0, fe = 0, fc = 0;
  int k;
  for (k = 0; k < n; k++) {
    int nfe = fe || (z && (fx & MPC_PROGRESS_FAIL_EMPTY));
    int nfc = fc
      || (z && (fx & MPC_PROGRESS_FAIL_CONSUMED))
      || (c && (fx & MPC_PA_FAILS));
    int nz = z && (fx & MPC_PROGRESS_SUCC_EMPTY);
    int nc = (z && (fx & MPC_PROGRESS_SUCC_CONSUMED))
          || (c && (fx & MPC_PA_SUCCS));
    if (nz == z && nc == c && nfe == fe && nfc == fc) { break; }
    z = nz; c = nc; fe = nfe; fc = nfc;
    if (!z && !c) { break; }
  }
  return (fe ? MPC_PROGRESS_FAIL_EMPTY    : 0)
       | (fc ? MPC_PROGRESS_FAIL_CONSUMED : 0)
       | (z  ? MPC_PROGRESS_SUCC_EMPTY    : 0)
       | (c  ? MPC_PROGRESS_SUCC_CONSUMED : 0);
}

static int mpc_pa_or_effects(mpc_pa_ctx_t *c, mpc_pa_node_t *n) {
  int z = 1, co = 0, out = 0;
  int j;
  if (n->edge_num == 0) { return MPC_PROGRESS_SUCC_EMPTY; }
  for (j = 0; j < n->edge_num; j++) {
    int fx = c->nodes[n->edges[j].to].effects;
    int nz, nco;
    if (z) { out |= fx; }
    if (co) {
      if (fx & MPC_PA_FAILS) { out |= MPC_PROGRESS_FAIL_CONSUMED; }
      if (fx & MPC_PA_SUCCS) { out |= MPC_PROGRESS_SUCC_CONSUMED; }
    }
    /* only failures carry control into the next alternative */
    nz = z && (fx & MPC_PROGRESS_FAIL_EMPTY);
    nco = (co && (fx & MPC_PA_FAILS))
       || (z && (fx & MPC_PROGRESS_FAIL_CONSUMED));
    z = nz;
    co = nco;
    if (!z && !co) { break; }
  }
  return out;
}

static int mpc_pa_and_effects(mpc_pa_ctx_t *c, mpc_pa_node_t *n) {
  int z = 1, co = 0, fe = 0;
  int j;
  if (n->edge_num == 0) { return MPC_PROGRESS_SUCC_EMPTY; }
  for (j = 0; j < n->edge_num; j++) {
    int fx = c->nodes[n->edges[j].to].effects;
    int nz, nc;
    if ((z || co) && (fx & MPC_PA_FAILS)) { fe = 1; }
    nz = z && (fx & MPC_PROGRESS_SUCC_EMPTY);
    nc = (z && (fx & MPC_PROGRESS_SUCC_CONSUMED))
      || (co && (fx & MPC_PA_SUCCS));
    z = nz;
    co = nc;
    if (!z && !co) { break; }
  }
  return (fe ? MPC_PROGRESS_FAIL_EMPTY    : 0)
       | (z  ? MPC_PROGRESS_SUCC_EMPTY    : 0)
       | (co ? MPC_PROGRESS_SUCC_CONSUMED : 0);
}

/*
** sepby1 transfer modelled on its real control flow.  States mark where
** the element or the separator is called: F is the first element call;
** R_z/R_co are later element calls at the entry position / a consumed
** position; S_z/S_co are separator calls at those positions.  Only the
** first element's failure fails the whole parser; a later failure ends
** the repetition successfully while retaining the current position.
*/
static int mpc_pa_sepby1_effects(int ex, int sp) {
  int F = 1, Rz = 0, Rco = 0, Sz = 0, Sco = 0;
  int out = 0;
  int changed = 1;

  #define MPC_PA_EMIT(bit) do { if (!(out & (bit))) { out |= (bit); changed = 1; } } while (0)

  while (changed) {
    changed = 0;

    if (F) {
      if (ex & MPC_PROGRESS_FAIL_EMPTY)    { MPC_PA_EMIT(MPC_PROGRESS_FAIL_EMPTY); }
      if (ex & MPC_PROGRESS_FAIL_CONSUMED) { MPC_PA_EMIT(MPC_PROGRESS_FAIL_CONSUMED); }
      if (ex & MPC_PROGRESS_SUCC_EMPTY)    { if (!Sz)  { Sz = 1;  changed = 1; } }
      if (ex & MPC_PROGRESS_SUCC_CONSUMED) { if (!Sco) { Sco = 1; changed = 1; } }
      F = 0;
    }

    if (Rz) {
      /* a later element failing ends the repetition successfully */
      if (ex & MPC_PA_FAILS) { MPC_PA_EMIT(MPC_PROGRESS_SUCC_EMPTY); }
      if (ex & MPC_PROGRESS_SUCC_EMPTY)    { if (!Sz)  { Sz = 1;  changed = 1; } }
      if (ex & MPC_PROGRESS_SUCC_CONSUMED) { if (!Sco) { Sco = 1; changed = 1; } }
    }

    if (Rco) {
      if (ex & MPC_PA_FAILS) { MPC_PA_EMIT(MPC_PROGRESS_SUCC_CONSUMED); }
      if (ex & MPC_PA_SUCCS) { if (!Sco) { Sco = 1; changed = 1; } }
    }

    if (Sz) {
      if (sp & MPC_PA_FAILS) { MPC_PA_EMIT(MPC_PROGRESS_SUCC_EMPTY); }
      if (sp & MPC_PROGRESS_SUCC_EMPTY)    { if (!Rz)  { Rz = 1;  changed = 1; } }
      if (sp & MPC_PROGRESS_SUCC_CONSUMED) { if (!Rco) { Rco = 1; changed = 1; } }
    }

    if (Sco) {
      if (sp & MPC_PA_FAILS) { MPC_PA_EMIT(MPC_PROGRESS_SUCC_CONSUMED); }
      if (sp & MPC_PA_SUCCS) { if (!Rco) { Rco = 1; changed = 1; } }
    }
  }

  #undef MPC_PA_EMIT
  return out;
}

static int mpc_pa_transfer(mpc_pa_ctx_t *c, mpc_pa_node_t *n) {
  mpc_parser_t *p = n->p;
  int fx;

  /* a child-bearing node without an edge only happens on allocation
     failure during discovery; contribute nothing until the graph exists */
  switch (p->type) {
    case MPC_TYPE_EXPECT:
    case MPC_TYPE_APPLY:
    case MPC_TYPE_APPLY_TO:
    case MPC_TYPE_PREDICT:
    case MPC_TYPE_CHECK:
    case MPC_TYPE_CHECK_WITH:
    case MPC_TYPE_NOT:
    case MPC_TYPE_MAYBE:
    case MPC_TYPE_MANY:
    case MPC_TYPE_MANY1:
    case MPC_TYPE_COUNT:
      if (n->edge_num == 0) { return 0; }
      break;
    case MPC_TYPE_SEPBY1:
      if (n->edge_num < 2) { return 0; }
      break;
    case MPC_TYPE_OR:
    case MPC_TYPE_AND:
      if (n->edge_num == 0) { return MPC_PROGRESS_SUCC_EMPTY; }
      break;
    default:
      break;
  }

  switch (p->type) {

    case MPC_TYPE_UNDEFINED:
    case MPC_TYPE_FAIL:
      return MPC_PROGRESS_FAIL_EMPTY;

    case MPC_TYPE_PASS:
    case MPC_TYPE_LIFT:
    case MPC_TYPE_LIFT_VAL:
    case MPC_TYPE_STATE:
      return MPC_PROGRESS_SUCC_EMPTY;

    case MPC_TYPE_ANCHOR:
    case MPC_TYPE_SOI:
    case MPC_TYPE_EOI:
      return MPC_PROGRESS_FAIL_EMPTY | MPC_PROGRESS_SUCC_EMPTY;

    case MPC_TYPE_ANY:
    case MPC_TYPE_SINGLE:
    case MPC_TYPE_ONEOF:
    case MPC_TYPE_NONEOF:
    case MPC_TYPE_RANGE:
    case MPC_TYPE_SATISFY:
      return MPC_PROGRESS_FAIL_EMPTY | MPC_PROGRESS_SUCC_CONSUMED;

    case MPC_TYPE_STRING:
      if (p->data.string.x && p->data.string.x[0] != '\0') {
        return MPC_PROGRESS_FAIL_EMPTY | MPC_PROGRESS_SUCC_CONSUMED;
      }
      return MPC_PROGRESS_SUCC_EMPTY;

    case MPC_TYPE_APPLY:
      return c->nodes[n->edges[0].to].effects;

    case MPC_TYPE_APPLY_TO:
      return c->nodes[n->edges[0].to].effects;

    case MPC_TYPE_EXPECT:
      return c->nodes[n->edges[0].to].effects;

    case MPC_TYPE_PREDICT:
      return c->nodes[n->edges[0].to].effects;

    case MPC_TYPE_CHECK:
    case MPC_TYPE_CHECK_WITH:
      fx = c->nodes[n->edges[0].to].effects;
      return (fx & MPC_PA_FAILS)
           | (fx & MPC_PA_SUCCS)
           | ((fx & MPC_PROGRESS_SUCC_EMPTY)
                ? MPC_PROGRESS_FAIL_EMPTY : 0)
           | ((fx & MPC_PROGRESS_SUCC_CONSUMED)
                ? MPC_PROGRESS_FAIL_CONSUMED : 0);

    case MPC_TYPE_NOT:
      fx = c->nodes[n->edges[0].to].effects;
      return ((fx & MPC_PROGRESS_FAIL_EMPTY)
                ? MPC_PROGRESS_SUCC_EMPTY : 0)
           | ((fx & MPC_PROGRESS_FAIL_CONSUMED)
                ? MPC_PROGRESS_SUCC_CONSUMED : 0)
           | ((fx & MPC_PA_SUCCS)
                ? MPC_PROGRESS_FAIL_EMPTY : 0);

    case MPC_TYPE_MAYBE:
      fx = c->nodes[n->edges[0].to].effects;
      return (fx & MPC_PA_SUCCS)
           | ((fx & MPC_PROGRESS_FAIL_EMPTY)
                ? MPC_PROGRESS_SUCC_EMPTY : 0)
           | ((fx & MPC_PROGRESS_FAIL_CONSUMED)
                ? MPC_PROGRESS_SUCC_CONSUMED : 0);

    case MPC_TYPE_MANY: {
      int b = c->nodes[n->edges[0].to].effects;
      /* the terminating failed attempt leaves its position behind */
      int se = (b & MPC_PROGRESS_FAIL_EMPTY) ? 1 : 0;
      int sc = ((b & MPC_PROGRESS_SUCC_CONSUMED)
                && (b & MPC_PA_FAILS))
            || ((b & MPC_PA_SUCCS)
                && (b & MPC_PROGRESS_FAIL_CONSUMED));
      return (se ? MPC_PROGRESS_SUCC_EMPTY : 0)
           | (sc ? MPC_PROGRESS_SUCC_CONSUMED : 0);
    }

    case MPC_TYPE_MANY1: {
      int b = c->nodes[n->edges[0].to].effects;
      int fe = (b & MPC_PROGRESS_FAIL_EMPTY) ? 1 : 0;
      int fc = (b & MPC_PROGRESS_FAIL_CONSUMED) ? 1 : 0;
      int se = (b & MPC_PROGRESS_SUCC_EMPTY)
            && (b & MPC_PROGRESS_FAIL_EMPTY);
      int sc = ((b & MPC_PROGRESS_SUCC_CONSUMED)
                && (b & MPC_PA_FAILS))
            || ((b & MPC_PA_SUCCS)
                && (b & MPC_PROGRESS_FAIL_CONSUMED));
      return (fe ? MPC_PROGRESS_FAIL_EMPTY    : 0)
           | (fc ? MPC_PROGRESS_FAIL_CONSUMED : 0)
           | (se ? MPC_PROGRESS_SUCC_EMPTY    : 0)
           | (sc ? MPC_PROGRESS_SUCC_CONSUMED : 0);
    }

    case MPC_TYPE_COUNT:
      if (p->data.repeat.n <= 0) { return 0; }
      fx = c->nodes[n->edges[0].to].effects;
      return mpc_pa_count_effects(fx, p->data.repeat.n);

    case MPC_TYPE_OR:
      return mpc_pa_or_effects(c, n);

    case MPC_TYPE_AND:
      return mpc_pa_and_effects(c, n);

    case MPC_TYPE_SEPBY1: {
      int ex = c->nodes[n->edges[0].to].effects;
      int sp = c->nodes[n->edges[1].to].effects;
      return mpc_pa_sepby1_effects(ex, sp);
    }

    default:
      return 0;
  }
}

static int mpc_pa_solve_effects(mpc_pa_ctx_t *c) {
  int changed = 1, passes = 0;
  while (changed) {
    int j;
    changed = 0;
    if (++passes > c->node_num * 4 + 4) { return 0; }
    for (j = 0; j < c->node_num; j++) {
      int fx = mpc_pa_transfer(c, &c->nodes[j]);
      if ((fx & ~c->nodes[j].effects) != 0) {
        c->nodes[j].effects |= fx;
        changed = 1;
      }
    }
  }
  return 1;
}

/*
** Decide which structural edges can be entered at the parent's entry
** position.  Only the prefix control flow matters: the child's own effects
** decide what happens after it is entered, not where the call happens.
*/
static void mpc_pa_compute_zero_edges(mpc_pa_ctx_t *c) {
  int j;
  for (j = 0; j < c->node_num; j++) {
    mpc_pa_node_t *n = &c->nodes[j];
    int k;

    for (k = 0; k < n->edge_num; k++) {
      mpc_pa_edge_t *e = &n->edges[k];
      int prefix_zero = 1;
      int m;

      switch (e->kind) {

        case MPC_PA_EDGE_PLAIN:
          prefix_zero = 1;
          break;

        case MPC_PA_EDGE_SUCC:
          /* AND item: all earlier items must succeed empty */
          prefix_zero = 1;
          for (m = 0; m < k; m++) {
            int pf = c->nodes[n->edges[m].to].effects;
            if (!(pf & MPC_PROGRESS_SUCC_EMPTY)) { prefix_zero = 0; }
          }
          break;

        case MPC_PA_EDGE_FAIL:
          /* OR item: every earlier item must fail empty */
          prefix_zero = 1;
          for (m = 0; m < k; m++) {
            int pf = c->nodes[n->edges[m].to].effects;
            if (!(pf & MPC_PROGRESS_FAIL_EMPTY)) { prefix_zero = 0; }
          }
          break;

        case MPC_PA_EDGE_SEP:
          /* sepby1 separator: the leading element must succeed empty */
          prefix_zero =
            (c->nodes[n->edges[0].to].effects &
             MPC_PROGRESS_SUCC_EMPTY) != 0;
          break;

        default:
          prefix_zero = 0;
          break;
      }

      e->zero = prefix_zero;
    }
  }
}

/*
** General reachability: which structural children control flow can actually
** reach.  AND items need a succeeding prefix, OR items a failing prefix and
** the sepby1 separator a succeeding element.  Consumed prefixes still count
** as reachable so local stagnation inside their subgraphs is not missed.
*/
static void mpc_pa_reach(mpc_pa_ctx_t *c, int *reach) {
  int *stk;
  int top = 0, slots = c->node_num > 0 ? c->node_num : 1;
  int j;
  stk = malloc((size_t)slots * sizeof(int));
  if (!stk) { return; }
  stk[top++] = 0;
  reach[0] = 1;
  while (top > 0) {
    mpc_pa_node_t *n;
    int u = stk[--top];
    n = &c->nodes[u];
    for (j = 0; j < n->edge_num; j++) {
      mpc_pa_edge_t *e = &n->edges[j];
      int feasible = 1;
      int m;

      switch (e->kind) {
        case MPC_PA_EDGE_PLAIN:
          feasible = 1;
          break;
        case MPC_PA_EDGE_SUCC:
          for (m = 0; m < j; m++) {
            int pf = c->nodes[n->edges[m].to].effects;
            if (!(pf & MPC_PA_SUCCS)) { feasible = 0; }
          }
          break;
        case MPC_PA_EDGE_FAIL:
          for (m = 0; m < j; m++) {
            int pf = c->nodes[n->edges[m].to].effects;
            if (!(pf & MPC_PA_FAILS)) { feasible = 0; }
          }
          break;
        case MPC_PA_EDGE_SEP:
          feasible =
            (c->nodes[n->edges[0].to].effects & MPC_PA_SUCCS) != 0;
          break;
        default:
          feasible = 0;
          break;
      }

      if (feasible && !reach[e->to]) {
        reach[e->to] = 1;
        stk[top++] = e->to;
      }
    }
  }
  free(stk);
}

/* Iterative Tarjan SCC over the zero-edge subgraph. */
typedef struct {
  int *stk;
  int top;
  int *onstack;
  int *dfs_stk;
  int *dfs_edge;
  int dfs_top;
  int clock;
  int scc_count;
} mpc_pa_tarjan_t;

static void mpc_pa_tarjan(mpc_pa_ctx_t *c, const int *reach) {
  int n = c->node_num;
  mpc_pa_tarjan_t t;
  int s;
  int j;

  t.stk = malloc((size_t)n * sizeof(int));
  t.onstack = calloc((size_t)n, sizeof(int));
  t.dfs_stk = malloc((size_t)n * sizeof(int));
  t.dfs_edge = calloc((size_t)n, sizeof(int));
  t.top = 0;
  t.dfs_top = 0;
  t.clock = 0;
  t.scc_count = 0;
  if (!t.stk || !t.onstack || !t.dfs_stk || !t.dfs_edge) {
    free(t.stk); free(t.onstack); free(t.dfs_stk); free(t.dfs_edge);
    return;
  }

  for (s = 0; s < n; s++) {
    if (!reach[s] || c->nodes[s].dfs != 0) { continue; }

    t.dfs_stk[t.dfs_top++] = s;
    c->nodes[s].disc = c->nodes[s].lowlink = t.clock++;
    c->nodes[s].dfs = 1;
    t.stk[t.top++] = s;
    t.onstack[s] = 1;
    t.dfs_edge[s] = 0;

    while (t.dfs_top > 0) {
      int u = t.dfs_stk[t.dfs_top - 1];
      mpc_pa_node_t *u_node = &c->nodes[u];
      mpc_pa_edge_t *e = NULL;
      int v = -1;

      while (t.dfs_edge[u] < u_node->edge_num) {
        e = &u_node->edges[t.dfs_edge[u]];
        if (e->zero && reach[e->to]) {
          v = e->to;
          break;
        }
        t.dfs_edge[u]++;
      }

      if (v < 0) {
        if (u_node->lowlink == u_node->disc) {
          while (t.top > 0) {
            int w = t.stk[--t.top];
            t.onstack[w] = 0;
            c->nodes[w].scc = t.scc_count;
            if (w == u) { break; }
          }
          t.scc_count++;
        }
        t.dfs_top--;
        if (t.dfs_top > 0) {
          int par = t.dfs_stk[t.dfs_top - 1];
          if (c->nodes[par].lowlink > u_node->lowlink) {
            c->nodes[par].lowlink = u_node->lowlink;
          }
        }
        continue;
      }

      t.dfs_edge[u]++;

      if (c->nodes[v].dfs == 0) {
        c->nodes[v].disc = c->nodes[v].lowlink = t.clock++;
        c->nodes[v].dfs = 1;
        t.stk[t.top++] = v;
        t.onstack[v] = 1;
        t.dfs_edge[v] = 0;
        t.dfs_stk[t.dfs_top++] = v;
      } else if (t.onstack[v]) {
        if (u_node->lowlink > c->nodes[v].disc) {
          u_node->lowlink = c->nodes[v].disc;
        }
      }
    }
  }

  for (j = 0; j < n; j++) {
    if (c->nodes[j].dfs == 0) { c->nodes[j].scc = -1; }
  }
  free(t.stk);
  free(t.onstack);
  free(t.dfs_stk);
  free(t.dfs_edge);
}

/* Find one closed cycle path inside a cyclic SCC, DFS edges in order. */
typedef struct {
  int *pos;     /* position on the DFS path, -1 absent */
  int *path;
  int len;
  int *out;
  int out_len;
  int scc;
} mpc_pa_cycle_t;

static int mpc_pa_cycle_dfs(mpc_pa_ctx_t *c, mpc_pa_cycle_t *q, int u) {
  mpc_pa_node_t *n = &c->nodes[u];
  int j;
  q->pos[u] = q->len;
  q->path[q->len++] = u;

  for (j = 0; j < n->edge_num; j++) {
    mpc_pa_edge_t *e = &n->edges[j];
    int v;
    int start, k;
    if (!e->zero || c->nodes[e->to].scc != q->scc) { continue; }
    v = e->to;
    if (q->pos[v] >= 0) {
      start = q->pos[v];
      q->out_len = q->len - start + 1;
      for (k = 0; k < q->len - start; k++) {
        q->out[k] = q->path[start + k];
      }
      q->out[q->out_len - 1] = v;
      return 1;
    }
    if (mpc_pa_cycle_dfs(c, q, v)) { return 1; }
  }

  q->pos[u] = -1;
  q->len--;
  return 0;
}

typedef struct {
  int risk;
  int node_index;
  int path_len;
  int *path;
} mpc_pa_finding_t;

static int mpc_pa_finding_cmp(const void *a, const void *b) {
  const mpc_pa_finding_t *x = a;
  const mpc_pa_finding_t *y = b;
  if (x->node_index != y->node_index) {
    return x->node_index < y->node_index ? -1 : 1;
  }
  if (x->risk != y->risk) { return x->risk < y->risk ? -1 : 1; }
  return 0;
}

static char *mpc_pa_node_name(mpc_pa_node_t *n) {
  char buf[64];
  if (n->p->name) { return mpc_pa_strdup(n->p->name); }
  snprintf(buf, sizeof(buf), "#%d", n->index);
  return mpc_pa_strdup(buf);
}

mpc_progress_report_t *mpc_analyze_progress(mpc_parser_t *root) {
  mpc_pa_ctx_t c;
  mpc_progress_report_t *report;
  mpc_progress_node_t *pub_nodes;
  mpc_progress_risk_t *pub_risks;
  mpc_progress_unsupported_t *pub_unsup;
  int *reach;
  int *scc_min;
  int *scc_cyclic;
  int scc_count = 0;
  mpc_pa_finding_t *findings = NULL;
  int finding_num = 0;
  int finding_slots = 0;
  int j, k, incomplete = 0;

  if (!root) { return NULL; }

  memset(&c, 0, sizeof(c));
  mpc_pa_discover(&c, root);

  if (!mpc_pa_solve_effects(&c)) { goto fail; }
  mpc_pa_compute_zero_edges(&c);

  reach = calloc((size_t)c.node_num, sizeof(int));
  if (!reach) { goto fail; }
  mpc_pa_reach(&c, reach);
  mpc_pa_tarjan(&c, reach);

  for (j = 0; j < c.node_num; j++) {
    if (c.nodes[j].scc >= 0 && c.nodes[j].scc + 1 > scc_count) {
      scc_count = c.nodes[j].scc + 1;
    }
  }

  scc_min = malloc((size_t)(scc_count > 0 ? scc_count : 1) * sizeof(int));
  scc_cyclic = calloc((size_t)(scc_count > 0 ? scc_count : 1), sizeof(int));
  if (!scc_min || !scc_cyclic) {
    free(reach); free(scc_min); free(scc_cyclic); goto fail;
  }
  for (j = 0; j < scc_count; j++) { scc_min[j] = c.node_num; }

  for (j = 0; j < c.node_num; j++) {
    int s = c.nodes[j].scc;
    if (s < 0) { continue; }
    if (j < scc_min[s]) { scc_min[s] = j; }
    for (k = 0; k < c.nodes[j].edge_num; k++) {
      mpc_pa_edge_t *e = &c.nodes[j].edges[k];
      /* every SCC here lies in the reachable zero-edge subgraph */
      if (e->zero && c.nodes[e->to].scc == s) {
        scc_cyclic[s] = 1;
      }
    }
  }

  for (j = 0; j < scc_count; j++) {
    mpc_pa_cycle_t q;
    int *path_copy;
    if (!scc_cyclic[j]) { continue; }
    q.scc = j;
    q.pos = malloc((size_t)c.node_num * sizeof(int));
    q.path = malloc((size_t)(c.node_num + 1) * sizeof(int));
    q.out = malloc((size_t)(c.node_num + 1) * sizeof(int));
    if (!q.pos || !q.path || !q.out) {
      free(q.pos); free(q.path); free(q.out);
      free(reach); free(scc_min); free(scc_cyclic);
      goto fail;
    }
    for (k = 0; k < c.node_num; k++) { q.pos[k] = -1; }
    q.len = 0;
    q.out_len = 0;
    if (!mpc_pa_cycle_dfs(&c, &q, scc_min[j])) {
      free(q.pos); free(q.path); free(q.out);
      continue;
    }
    path_copy = malloc((size_t)q.out_len * sizeof(int));
    if (!path_copy) {
      free(q.pos); free(q.path); free(q.out);
      free(reach); free(scc_min); free(scc_cyclic);
      goto fail;
    }
    memcpy(path_copy, q.out, (size_t)q.out_len * sizeof(int));
    if (finding_num == finding_slots) {
      int s = finding_slots == 0 ? 8 : finding_slots * 2;
      mpc_pa_finding_t *t = realloc(findings,
        (size_t)s * sizeof(mpc_pa_finding_t));
      if (!t) {
        free(path_copy); free(q.pos); free(q.path); free(q.out);
        free(reach); free(scc_min); free(scc_cyclic);
        goto fail;
      }
      findings = t;
      finding_slots = s;
    }
    findings[finding_num].risk = MPC_PROGRESS_RISK_CYCLE;
    findings[finding_num].node_index = path_copy[0];
    findings[finding_num].path_len = q.out_len;
    findings[finding_num].path = path_copy;
    finding_num++;
    free(q.pos); free(q.path); free(q.out);
  }

  /* Repetition stagnation findings (reachable nodes only). */
  for (j = 0; j < c.node_num; j++) {
    mpc_pa_node_t *n = &c.nodes[j];
    int flag = 0;
    int plen = 0;
    int *path = NULL;

    if (!reach[j]) { continue; }

    if (n->p->type == MPC_TYPE_MANY || n->p->type == MPC_TYPE_MANY1) {
      int b;
      if (n->edge_num < 1) { continue; }
      b = c.nodes[n->edges[0].to].effects;
      if (b & MPC_PROGRESS_SUCC_EMPTY) {
        flag = 1;
        plen = 3;
        path = malloc((size_t)plen * sizeof(int));
        if (path) {
          path[0] = n->index;
          path[1] = n->edges[0].to;
          path[2] = n->index;
        }
      }
    } else if (n->p->type == MPC_TYPE_SEPBY1) {
      int ex, sp;
      if (n->edge_num < 2) { continue; }
      ex = c.nodes[n->edges[0].to].effects;
      sp = c.nodes[n->edges[1].to].effects;
      if ((ex & MPC_PROGRESS_SUCC_EMPTY)
       && (sp & MPC_PROGRESS_SUCC_EMPTY)) {
        flag = 1;
        plen = 4;
        path = malloc((size_t)plen * sizeof(int));
        if (path) {
          path[0] = n->index;
          path[1] = n->edges[1].to;
          path[2] = n->edges[0].to;
          path[3] = n->index;
        }
      }
    }

    if (flag) {
      if (!path) { free(reach); free(scc_min); free(scc_cyclic); goto fail; }
      if (finding_num == finding_slots) {
        int s = finding_slots == 0 ? 8 : finding_slots * 2;
        mpc_pa_finding_t *t = realloc(findings,
          (size_t)s * sizeof(mpc_pa_finding_t));
        if (!t) {
          free(path); free(reach); free(scc_min); free(scc_cyclic);
          goto fail;
        }
        findings = t;
        finding_slots = s;
      }
      findings[finding_num].risk = MPC_PROGRESS_RISK_REPETITION;
      findings[finding_num].node_index = n->index;
      findings[finding_num].path_len = plen;
      findings[finding_num].path = path;
      finding_num++;
    }
  }

  qsort(findings, (size_t)finding_num, sizeof(mpc_pa_finding_t),
    mpc_pa_finding_cmp);

  for (j = 0; j < c.node_num; j++) {
    c.nodes[j].name = mpc_pa_node_name(&c.nodes[j]);
    if (!c.nodes[j].name) {
      free(reach); free(scc_min); free(scc_cyclic); goto fail;
    }
    if (c.nodes[j].unsupported) { incomplete = 1; }
  }

  report = calloc(1, sizeof(mpc_progress_report_t));
  pub_nodes = malloc((size_t)c.node_num * sizeof(mpc_progress_node_t));
  pub_risks = malloc((size_t)(finding_num > 0 ? finding_num : 1)
                     * sizeof(mpc_progress_risk_t));
  if (!report || !pub_nodes || !pub_risks) {
    free(report); free(pub_nodes); free(pub_risks);
    free(reach); free(scc_min); free(scc_cyclic);
    goto fail;
  }

  for (j = 0; j < c.node_num; j++) {
    pub_nodes[j].index = c.nodes[j].index;
    pub_nodes[j].parser = c.nodes[j].p;
    pub_nodes[j].name = c.nodes[j].name;
    pub_nodes[j].type = c.nodes[j].p->type;
    pub_nodes[j].effects = c.nodes[j].effects;
    pub_nodes[j].reachable = reach[j];
    pub_nodes[j].unsupported = c.nodes[j].unsupported;
  }

  for (j = 0; j < finding_num; j++) {
    pub_risks[j].risk = findings[j].risk;
    pub_risks[j].node_index = findings[j].node_index;
    pub_risks[j].path_len = findings[j].path_len;
    pub_risks[j].node_path = findings[j].path;
  }

  {
    int unsup_num = 0;
    for (j = 0; j < c.node_num; j++) {
      if (c.nodes[j].unsupported) { unsup_num++; }
    }
    pub_unsup = malloc((size_t)(unsup_num > 0 ? unsup_num : 1)
                       * sizeof(mpc_progress_unsupported_t));
    if (!pub_unsup) {
      free(report); free(pub_nodes); free(pub_risks);
      free(reach); free(scc_min); free(scc_cyclic);
      goto fail;
    }
    k = 0;
    for (j = 0; j < c.node_num; j++) {
      if (c.nodes[j].unsupported) {
        pub_unsup[k].node_index = c.nodes[j].index;
        pub_unsup[k].reason = c.nodes[j].unsupported;
        pub_unsup[k].name = c.nodes[j].name;
        k++;
      }
    }
    report->unsupported_num = unsup_num;
    report->unsupported = pub_unsup;
  }

  report->complete = !incomplete;
  report->node_num = c.node_num;
  report->nodes = pub_nodes;
  report->risk_num = finding_num;
  report->risks = pub_risks;

  for (j = 0; j < c.node_num; j++) { free(c.nodes[j].edges); }
  free(c.nodes);
  free(c.keys);
  free(c.vals);
  free(reach);
  free(scc_min);
  free(scc_cyclic);
  free(findings);

  return report;

fail:
  for (j = 0; j < c.node_num; j++) {
    free(c.nodes[j].edges);
    free(c.nodes[j].name);
  }
  free(c.nodes);
  free(c.keys);
  free(c.vals);
  for (j = 0; j < finding_num; j++) {
    free(findings[j].path);
  }
  free(findings);
  return NULL;
}

void mpc_progress_report_delete(mpc_progress_report_t *report) {
  int j;
  if (!report) { return; }
  if (report->risks) {
    for (j = 0; j < report->risk_num; j++) {
      free((void*)report->risks[j].node_path);
    }
    free((void*)report->risks);
  }
  if (report->nodes) {
    for (j = 0; j < report->node_num; j++) {
      free((void*)report->nodes[j].name);
    }
    free((void*)report->nodes);
  }
  /* unsupported names alias node names and need no separate free */
  free((void*)report->unsupported);
  free(report);
}
