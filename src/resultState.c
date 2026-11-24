/*
** resultState.c
**
** Implementation of status-conditioned ownership hand-off contracts.
**
** This is a focused, source-level association analysis.  It deliberately
** keeps its own finite models for declarations, call sites and branch
** states; the hook in cscannerHelp only makes the annotation spelling known
** to Splint's lexical layer, while llmain drives this pass after parsing.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>

#include "resultState.h"

#define RS_MAX_PATHS 64
#define RS_MAX_RELS 512
#define RS_MAX_PARAMS 24
#define RS_MAX_FUNCS 128
#define RS_MAX_DIAGS 4096

typedef struct
{
  char file[512];
  int line;
  int col;
} RSLoc;

typedef struct
{
  char name[96];
  int stars;
  int special;
  int only;
} RSParam;

typedef struct
{
  int used;
  char name[128];
  char file[512];
  int headerPos;
  int openParen;
  int closeParen;
  int bodyOpen;
  int bodyClose;
  int declEnd;
  RSLoc loc;
  RSParam params[RS_MAX_PARAMS];
  int nparams;
} RSFunc;

typedef struct
{
  int used;
  int loaded;
  char func[128];
  int status;
  char src[96];
  char dst[96];
  char text[224];
  RSLoc loc;
  int funcIndex;
} RSRel;

static char *rs_paths[RS_MAX_PATHS];
static int rs_npaths = 0;
static RSRel rs_rels[RS_MAX_RELS];
static int rs_nrels = 0;
static RSFunc rs_funcs[RS_MAX_FUNCS];
static int rs_nfuncs = 0;
static int rs_diags = 0;
static char rs_dump[1024];
static char rs_load[1024];

/* ----------------------------- basic helpers ---------------------------- */

static char *rs_xstrdup(const char *s)
{
  size_t n;
  char *r;

  if (!s)
    return NULL;
  n = strlen(s) + 1;
  r = (char *)malloc(n);
  if (r)
    memcpy(r, s, n);
  return r;
}

static int rs_eqci(const char *a, const char *b)
{
  while (*a && *b)
    {
      if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
        return 0;
      ++a;
      ++b;
    }
  return *a == '\0' && *b == '\0';
}

static int rs_isIdent0(int c)
{
  return isalpha(c) || c == '_' || c == '$';
}

static int rs_isIdent(int c)
{
  return isalnum(c) || c == '_' || c == '$';
}

static int rs_wordAt(const char *s, int pos, const char *word)
{
  size_t n = strlen(word);
  if (pos < 0 || strncmp(s + pos, word, n) != 0)
    return 0;
  if (pos > 0 && rs_isIdent((unsigned char)s[pos - 1]))
    return 0;
  if (rs_isIdent((unsigned char)s[pos + (int)n]))
    return 0;
  return 1;
}

static int rs_findWord(const char *s, int from, const char *word)
{
  int i;
  for (i = from; s[i]; ++i)
    {
      if (rs_wordAt(s, i, word))
        return i;
    }
  return -1;
}

static void rs_skipSpace(const char *s, int *p)
{
  while (s[*p] && isspace((unsigned char)s[*p]))
    ++*p;
}

static RSLoc rs_locAt(const char *file, const char *s, int pos)
{
  RSLoc loc;
  int i;
  int line = 1;
  int lineStart = 0;

  memset(&loc, 0, sizeof(loc));
  strncpy(loc.file, file ? file : "<unknown>", sizeof(loc.file) - 1);
  for (i = 0; i < pos && s[i]; ++i)
    {
      if (s[i] == '\n')
        {
          ++line;
          lineStart = i + 1;
        }
    }
  loc.line = line;
  loc.col = pos - lineStart + 1;
  return loc;
}

static void rs_printLoc(FILE *f, RSLoc loc)
{
  fprintf(f, "%s:%d:%d", loc.file, loc.line, loc.col);
}

static void rs_error(RSLoc loc, const char *fmt, ...)
{
  va_list ap;

  rs_printLoc(stderr, loc);
  fputs(": ", stderr);
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
  ++rs_diags;
}

static void rs_note(RSLoc loc, const char *fmt, ...)
{
  va_list ap;

  rs_printLoc(stderr, loc);
  fputs(": note: ", stderr);
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
}

static char *rs_readFile(const char *path, long *lenOut)
{
  FILE *f;
  long n;
  char *buf;
  size_t got;

  if (lenOut)
    *lenOut = 0;
  f = fopen(path, "rb");
  if (!f)
    return NULL;
  if (fseek(f, 0, SEEK_END) != 0)
    {
      fclose(f);
      return NULL;
    }
  n = ftell(f);
  if (n < 0)
    {
      fclose(f);
      return NULL;
    }
  rewind(f);
  buf = (char *)malloc((size_t)n + 1);
  if (!buf)
    {
      fclose(f);
      return NULL;
    }
  got = fread(buf, 1, (size_t)n, f);
  fclose(f);
  buf[got] = '\0';
  if (lenOut)
    *lenOut = (long)got;
  return buf;
}

static int rs_matchBracket(const char *s, int openPos, int openCh, int closeCh)
{
  int depth = 0;
  int i = openPos;
  char c;

  for (; s[i]; ++i)
    {
      c = s[i];
      if (c == openCh)
        ++depth;
      else if (c == closeCh)
        {
          --depth;
          if (depth == 0)
            return i;
        }
    }
  return -1;
}

static int rs_nextBrace(const char *s, int from)
{
  int i = from;
  for (; s[i]; ++i)
    {
      if (s[i] == '{')
        return i;
      if (s[i] == ';')
        return -1;
    }
  return -1;
}

/* Remove comments and string/character contents while preserving lines. */
static char *rs_maskCode(const char *src)
{
  size_t n = strlen(src);
  char *out = (char *)malloc(n + 1);
  size_t i = 0;

  if (!out)
    return NULL;
  memcpy(out, src, n + 1);

  while (i < n)
    {
      if (src[i] == '/' && src[i + 1] == '/')
        {
          while (i < n && src[i] != '\n')
            {
              if (out[i] != '\n')
                out[i] = ' ';
              ++i;
            }
        }
      else if (src[i] == '/' && src[i + 1] == '*')
        {
          out[i] = out[i + 1] = ' ';
          i += 2;
          while (i < n && !(src[i] == '*' && src[i + 1] == '/'))
            {
              if (out[i] != '\n')
                out[i] = ' ';
              ++i;
            }
          if (i < n)
            {
              out[i] = out[i + 1] = ' ';
              i += 2;
            }
        }
      else if (src[i] == '"' || src[i] == '\'')
        {
          int q = src[i];
          out[i] = ' ';
          ++i;
          while (i < n && src[i] != q)
            {
              if (src[i] == '\\' && i + 1 < n)
                {
                  if (out[i + 1] != '\n')
                    out[i + 1] = ' ';
                  ++i;
                }
              if (out[i] != '\n')
                out[i] = ' ';
              ++i;
            }
          if (i < n)
            out[i] = ' ';
          ++i;
        }
      else
        ++i;
    }
  return out;
}

static int rs_lastIdentBefore(const char *s, int pos, char *out, int outSize)
{
  int end = pos;
  int start;
  int n;

  while (end > 0 && isspace((unsigned char)s[end - 1]))
    --end;
  if (end > 0 && s[end - 1] == ')' )
    {
      int close = end - 1;
      int open = rs_matchBracket(s, close, ')', '(');
      if (open >= 0)
        end = open;
      while (end > 0 && isspace((unsigned char)s[end - 1]))
        --end;
    }
  start = end;
  while (start > 0 && rs_isIdent((unsigned char)s[start - 1]))
    --start;
  if (start == end)
    return 0;
  n = end - start;
  if (n >= outSize)
    n = outSize - 1;
  memcpy(out, s + start, (size_t)n);
  out[n] = '\0';
  return 1;
}

/* ----------------------------- parameters ------------------------------- */

static int rs_paramHasWord(const char *p, int len, const char *word)
{
  int i;
  for (i = 0; i + (int)strlen(word) <= len; ++i)
    {
      if (rs_wordAt(p, i, word))
        return 1;
    }
  return 0;
}

static void rs_parseParam(const char *p, int len, RSParam *param)
{
  int i;
  int idStart = -1, idEnd = -1;
  char clean[512];
  int cleanLen = len < (int)sizeof(clean) - 1 ? len : (int)sizeof(clean) - 1;

  memset(param, 0, sizeof(*param));
  param->stars = 0;
  param->special = rs_paramHasWord(p, len, "special");
  param->only = rs_paramHasWord(p, len, "only");

  memcpy(clean, p, (size_t)cleanLen);
  clean[cleanLen] = '\0';
  for (i = 0; i + 1 < cleanLen; ++i)
    {
      if (clean[i] == '/' && clean[i + 1] == '*')
        {
          int j = i;
          while (j + 1 < cleanLen
                 && !(clean[j] == '*' && clean[j + 1] == '/'))
            {
              clean[j] = ' ';
              ++j;
            }
          if (j + 1 < cleanLen)
            {
              clean[j] = clean[j + 1] = ' ';
              i = j + 1;
            }
        }
    }

  for (i = 0; i < cleanLen; ++i)
    {
      if (clean[i] == '*')
        ++param->stars;
    }

  for (i = cleanLen - 1; i >= 0; --i)
    {
      if (rs_isIdent((unsigned char)clean[i]))
        {
          idEnd = i + 1;
          while (i >= 0 && rs_isIdent((unsigned char)clean[i]))
            --i;
          idStart = i + 1;
          break;
        }
    }

  if (idStart >= 0 && idEnd > idStart)
    {
      int n = idEnd - idStart;
      if (n >= (int)sizeof(param->name))
        n = (int)sizeof(param->name) - 1;
      memcpy(param->name, clean + idStart, (size_t)n);
      param->name[n] = '\0';
    }

  /* Array declarators are equivalent to one pointer indirection. */
  for (i = 0; i < cleanLen; ++i)
    {
      if (clean[i] == '[' && param->name[0] && strstr(clean, param->name))
        ++param->stars;
    }
}

static int rs_splitParams(const char *s, int openPos, int closePos,
                          RSParam *params, int maxParams)
{
  int start = openPos + 1;
  int i = start;
  int depth = 0;
  int count = 0;

  while (i <= closePos)
    {
      if (i == closePos || (s[i] == ',' && depth == 0))
        {
          int end = i;
          while (end > start && isspace((unsigned char)s[end - 1]))
            --end;
          if (end > start && count < maxParams)
            {
              rs_parseParam(s + start, end - start, &params[count]);
              if (params[count].name[0])
                ++count;
            }
          start = i + 1;
        }
      else if (s[i] == '(' || s[i] == '[' )
        ++depth;
      else if (s[i] == ')' || s[i] == ']')
        --depth;
      ++i;
    }
  return count;
}

static RSFunc *rs_addFunc(const char *file, const char *code, int openParen,
                          int closeParen, int bodyOpen)
{
  RSFunc *f;
  char name[128];
  int i;

  if (!rs_lastIdentBefore(code, openParen, name, sizeof(name)))
    return NULL;

  for (i = 0; i < rs_nfuncs; ++i)
    {
      if (rs_funcs[i].used && strcmp(rs_funcs[i].name, name) == 0
          && strcmp(rs_funcs[i].file, file) == 0
          && rs_funcs[i].openParen == openParen)
        return &rs_funcs[i];
    }

  if (rs_nfuncs >= RS_MAX_FUNCS)
    return NULL;
  f = &rs_funcs[rs_nfuncs++];
  memset(f, 0, sizeof(*f));
  f->used = 1;
  strncpy(f->name, name, sizeof(f->name) - 1);
  strncpy(f->file, file, sizeof(f->file) - 1);
  f->openParen = openParen;
  f->closeParen = closeParen;
  f->bodyOpen = bodyOpen;
  f->bodyClose = bodyOpen >= 0 ? rs_matchBracket(code, bodyOpen, '{', '}') : -1;
  f->declEnd = bodyOpen;
  if (f->declEnd < 0)
    {
      int semi = closeParen + 1;
      while (code[semi] && code[semi] != ';')
        ++semi;
      if (code[semi] == ';')
        f->declEnd = semi;
    }
  f->loc = rs_locAt(file, code, openParen);
  f->nparams = rs_splitParams(code, openParen, closeParen, f->params,
                               RS_MAX_PARAMS);
  return f;
}

static int rs_isControlName(const char *name)
{
  static const char *words[] = {
    "if", "for", "while", "switch", "return", "sizeof", "catch", NULL
  };
  int i;
  for (i = 0; words[i]; ++i)
    {
      if (strcmp(name, words[i]) == 0)
        return 1;
    }
  return 0;
}

static void rs_collectFunctions(const char *file, const char *src,
                                const char *masked)
{
  int i;
  int n = (int)strlen(masked);

  for (i = 0; i < n; ++i)
    {
      int closeParen;
      int p;
      char name[128];

      if (masked[i] != '(')
        continue;
      closeParen = rs_matchBracket(masked, i, '(', ')');
      if (closeParen < 0)
        continue;
      p = closeParen + 1;
      while (masked[p] && isspace((unsigned char)masked[p]))
        ++p;
      if (masked[p] == ';')
        {
          if (rs_lastIdentBefore(masked, i, name, sizeof(name))
              && !rs_isControlName(name))
            (void)rs_addFunc(file, src, i, closeParen, -1);
          i = closeParen;
          continue;
        }
      if (masked[p] != '{')
        continue;
      if (!rs_lastIdentBefore(masked, i, name, sizeof(name)))
        continue;
      if (rs_isControlName(name))
        continue;
      (void)rs_addFunc(file, src, i, closeParen, p);
      i = closeParen;
    }
}

static RSFunc *rs_funcForAnnotation(const char *file, int kwPos)
{
  int i;
  RSFunc *best = NULL;
  int bestDistance = 0x7fffffff;

  for (i = 0; i < rs_nfuncs; ++i)
    {
      RSFunc *f = &rs_funcs[i];
      int d;

      if (!f->used || strcmp(f->file, file) != 0)
        continue;
      if (f->declEnd > f->closeParen
          && kwPos >= f->closeParen && kwPos < f->declEnd)
        {
          d = kwPos - f->closeParen;
          if (d < bestDistance)
            {
              bestDistance = d;
              best = f;
            }
        }
      else if (kwPos < f->openParen && f->openParen - kwPos < 4000)
        {
          d = f->openParen - kwPos;
          if (d < bestDistance)
            {
              bestDistance = d;
              best = f;
            }
        }
    }
  return best;
}

/* --------------------------- contract parsing --------------------------- */

static RSRel *rs_addRel(void)
{
  if (rs_nrels >= RS_MAX_RELS)
    return NULL;
  return &rs_rels[rs_nrels++];
}

static int rs_parseInt(const char *s, int *p, int *value)
{
  int sign = 1;
  int v = 0;
  int digits = 0;

  rs_skipSpace(s, p);
  if (s[*p] == '-')
    {
      sign = -1;
      ++*p;
    }
  else if (s[*p] == '+')
    ++*p;

  while (isdigit((unsigned char)s[*p]))
    {
      v = v * 10 + (s[*p] - '0');
      ++*p;
      ++digits;
    }
  if (!digits)
    return 0;
  *value = sign * v;
  return 1;
}

static int rs_parseIdentAt(const char *s, int *p, char *out, int outSize)
{
  int start, n;

  rs_skipSpace(s, p);
  if (!rs_isIdent0((unsigned char)s[*p]))
    return 0;
  start = *p;
  while (rs_isIdent((unsigned char)s[*p]))
    ++*p;
  n = *p - start;
  if (n >= outSize)
    n = outSize - 1;
  memcpy(out, s + start, (size_t)n);
  out[n] = '\0';
  return 1;
}

static void rs_parseFileContracts(const char *path, const char *src)
{
  char *masked = rs_maskCode(src);
  int search = 0;

  if (!masked)
    return;
  rs_collectFunctions(path, src, masked);

  while (src[search])
    {
      int kw = -1;
      int i;
      int commentStart = -1;
      int p;
      int status;
      char source[96], target[96];
      int targetStars = 0;
      RSRel *rel;
      RSFunc *func = NULL;
      char fname[128] = "";

      for (i = search; src[i]; ++i)
        {
          if (rs_wordAt(src, i, "transferswhen")
              || rs_wordAt(src, i, "TRANSFERSWHEN"))
            {
              kw = i;
              break;
            }
        }
      if (kw < 0)
        break;
      search = kw + 13;

      for (i = kw; i >= 4; --i)
        {
          if (src[i] == '@' && src[i - 1] == '*' && src[i - 2] == '/')
            {
              commentStart = i - 2;
              break;
            }
        }

      p = kw + (int)strlen("transferswhen");
      if (!rs_parseInt(src, &p, &status))
        {
          rs_error(rs_locAt(path, src, kw),
                   "invalid transferswhen declaration: expected integer status");
          continue;
        }
      if (!rs_parseIdentAt(src, &p, source, sizeof(source)))
        {
          rs_error(rs_locAt(path, src, kw),
                   "invalid transferswhen declaration: expected source parameter");
          continue;
        }
      rs_skipSpace(src, &p);
      if (src[p] != '-' || src[p + 1] != '>')
        {
          rs_error(rs_locAt(path, src, kw),
                   "invalid transferswhen declaration: expected '->'");
          continue;
        }
      p += 2;
      rs_skipSpace(src, &p);
      while (src[p] == '*')
        {
          ++targetStars;
          ++p;
          rs_skipSpace(src, &p);
        }
      if (!rs_parseIdentAt(src, &p, target, sizeof(target)))
        {
          rs_error(rs_locAt(path, src, kw),
                   "invalid transferswhen declaration: expected target parameter");
          continue;
        }
      if (targetStars == 0)
        {
          rs_error(rs_locAt(path, src, kw),
                   "invalid transferswhen declaration: target must be written as *%s",
                   target);
          continue;
        }

      /* Function clauses appear between ')' and '{'; leading comments work too. */
      func = rs_funcForAnnotation(path, kw);
      if (func)
        strncpy(fname, func->name, sizeof(fname) - 1);

      rel = rs_addRel();
      if (!rel)
        break;
      rel->used = 1;
      rel->loaded = 0;
      rel->status = status;
      rel->loc = rs_locAt(path, src, kw);
      strncpy(rel->src, source, sizeof(rel->src) - 1);
      strncpy(rel->dst, target, sizeof(rel->dst) - 1);
      if (fname[0])
        strncpy(rel->func, fname, sizeof(rel->func) - 1);
      else if (func)
        strncpy(rel->func, func->name, sizeof(rel->func) - 1);
      snprintf(rel->text, sizeof(rel->text), "transferswhen %d %s -> *%s",
               status, source, target);
      if (func)
        rel->funcIndex = (int)(func - rs_funcs);

      (void)commentStart;
    }

  free(masked);
}

static RSFunc *rs_findFunc(const RSRel *rel)
{
  int i;
  if (rel->funcIndex >= 0 && rel->funcIndex < rs_nfuncs
      && rs_funcs[rel->funcIndex].used)
    return &rs_funcs[rel->funcIndex];

  for (i = 0; i < rs_nfuncs; ++i)
    {
      if (rs_funcs[i].used && strcmp(rs_funcs[i].name, rel->func) == 0
          && strcmp(rs_funcs[i].file, rel->loc.file) == 0)
        return &rs_funcs[i];
    }
  return NULL;
}

static RSParam *rs_findParam(RSFunc *f, const char *name)
{
  int i;
  for (i = 0; i < f->nparams; ++i)
    {
      if (strcmp(f->params[i].name, name) == 0)
        return &f->params[i];
    }
  return NULL;
}

static void rs_validateDeclarations(void)
{
  int i, j;

  for (i = 0; i < rs_nrels; ++i)
    {
      RSRel *a = &rs_rels[i];
      RSFunc *f;
      RSParam *sp, *dp;

      if (!a->used || a->loaded || a->func[0] == '\0')
        continue;
      f = rs_findFunc(a);
      if (!f)
        continue;

      sp = rs_findParam(f, a->src);
      dp = rs_findParam(f, a->dst);
      if (!sp || !sp->special)
        {
          rs_error(a->loc,
                   "transferswhen source '%s' in %s must be declared special",
                   a->src, a->func);
        }
      if (!dp || !dp->special)
        {
          rs_error(a->loc,
                   "transferswhen target '%s' in %s must be declared special",
                   a->dst, a->func);
        }
      else if (dp->stars < 2)
        {
          rs_error(a->loc,
                   "transferswhen target '%s' in %s must be a pointer-to-pointer output parameter",
                   a->dst, a->func);
        }

      for (j = i + 1; j < rs_nrels; ++j)
        {
          RSRel *b = &rs_rels[j];
          if (!b->used || b->loaded)
            continue;
          if (strcmp(a->func, b->func) != 0 || a->status != b->status)
            continue;

          if (strcmp(a->src, b->src) == 0)
            {
              rs_error(b->loc,
                       "duplicate transferswhen declaration conflict: source '%s' is consumed twice on status %d in %s",
                       b->src, b->status, b->func);
              rs_note(a->loc, "previous relation declaration here: %s", a->text);
            }
          if (strcmp(a->dst, b->dst) == 0)
            {
              rs_error(b->loc,
                       "duplicate transferswhen declaration conflict: target '*%s' is written twice on status %d in %s",
                       b->dst, b->status, b->func);
              rs_note(a->loc, "previous relation declaration here: %s", a->text);
            }
        }
    }
}

/* ---------------------------- callee checking --------------------------- */

static int rs_returnStatus(const char *code, int retKw, int *status)
{
  int p = retKw + 6;
  rs_skipSpace(code, &p);
  if (rs_wordAt(code, p, "return"))
    p += 6;
  rs_skipSpace(code, &p);
  if (!rs_parseInt(code, &p, status))
    return 0;
  return 1;
}

static int rs_isNewObjectRhs(const char *s, int eqPos)
{
  int p = eqPos + 1;
  char name[128];

  rs_skipSpace(s, &p);
  if (s[p] == '&')
    return 1;
  if (rs_parseIdentAt(s, &p, name, sizeof(name)))
    {
      if (strstr(name, "alloc") || strstr(name, "make")
          || strstr(name, "new") || strstr(name, "create")
          || strstr(name, "dup") || strstr(name, "copy"))
        return 1;
      rs_skipSpace(s, &p);
      if (s[p] == '(')
        return 1; /* unknown call result: cannot be entry identity */
    }
  return 0;
}

static int rs_hasDirectAssignment(const char *code, int from, int to,
                                  const char *dst, const char *src,
                                  int *assignPos, int *isNew)
{
  int i;
  char lhs[128];

  *assignPos = -1;
  *isNew = 0;
  snprintf(lhs, sizeof(lhs), "*%s", dst);

  for (i = from; i < to; ++i)
    {
      if (code[i] == '/' && code[i + 1] == '*')
        {
          int end = i + 2;
          while (code[end] && !(code[end] == '*' && code[end + 1] == '/'))
            ++end;
          i = end + 1;
          continue;
        }
      if (strncmp(code + i, lhs, strlen(lhs)) == 0)
        {
          int p = i + (int)strlen(lhs);
          rs_skipSpace(code, &p);
          if (code[p] == '=')
            {
              int q = p + 1;
              char rhs[96];
              int rp;
              rs_skipSpace(code, &q);
              rp = q;
              if (rs_parseIdentAt(code, &rp, rhs, sizeof(rhs))
                  && strcmp(rhs, src) == 0)
                {
                  *assignPos = i;
                  return 1;
                }
              *assignPos = i;
              if (rs_isNewObjectRhs(code, p))
                *isNew = 1;
              else
                *isNew = 0;
              return 0;
            }
        }
    }
  return 0;
}

static void rs_checkCalleeReturn(RSRel *rel, RSFunc *f, int retPos)
{
  int status;
  int searchFrom = f->bodyOpen + 1;
  int direct = 0, assignPos = -1, isNew = 0;
  char *fileCode = NULL;
  long len = 0;

  fileCode = rs_readFile(rel->loc.file, &len);
  if (!fileCode)
    return;

  if (!rs_returnStatus(fileCode, retPos, &status))
    {
      free(fileCode);
      return;
    }

  direct = rs_hasDirectAssignment(fileCode, searchFrom, retPos, rel->dst,
                                  rel->src, &assignPos, &isNew);

  if (status == rel->status)
    {
      if (!direct)
        {
          RSLoc loc = rs_locAt(rel->loc.file, fileCode, retPos);
          if (assignPos >= 0 && isNew)
            rs_error(loc,
                     "conditional ownership: relation %s in %s outputs another new object instead of entry '%s'",
                     rel->text, rel->func, rel->src);
          else
            rs_error(loc,
                     "conditional ownership: successful return in %s does not transfer entry object from '%s' to *%s (%s)",
                     rel->func, rel->src, rel->dst, rel->text);
          rs_note(rel->loc, "transferswhen relation and declaration location");
        }
    }
  else
    {
      if (assignPos >= 0)
        {
          RSLoc loc = rs_locAt(rel->loc.file, fileCode, retPos);
          rs_error(loc,
                   "conditional ownership: non-success return %d in %s has already transferred '%s' to *%s (%s)",
                   status, rel->func, rel->src, rel->dst, rel->text);
          rs_note(rel->loc, "transferswhen relation and declaration location");
        }
    }
  free(fileCode);
}

static void rs_checkCallees(void)
{
  int i;

  for (i = 0; i < rs_nrels; ++i)
    {
      RSRel *rel = &rs_rels[i];
      RSFunc *f;
      char *code;
      long len;
      int p;

      if (!rel->used || rel->loaded)
        continue;
      f = rs_findFunc(rel);
      if (!f || f->bodyOpen < 0 || f->bodyClose < 0)
        continue;
      code = rs_readFile(rel->loc.file, &len);
      if (!code)
        continue;

      p = f->bodyOpen;
      while (p >= 0 && p < f->bodyClose)
        {
          int ret = rs_findWord(code, p, "return");
          int semi;
          if (ret < 0 || ret >= f->bodyClose)
            break;
          /* Ensure this is the keyword, not part of an identifier. */
          semi = ret;
          while (semi < f->bodyClose && code[semi] != ';')
            ++semi;
          rs_checkCalleeReturn(rel, f, ret);
          p = semi + 1;
        }
      free(code);
    }
}

/* ----------------------------- public hooks ----------------------------- */

int resultState_isContractComment(const char *text)
{
  if (!text)
    return 0;
  while (*text && isspace((unsigned char)*text))
    ++text;
  return rs_wordAt(text, 0, "transferswhen")
      || rs_wordAt(text, 0, "TRANSFERSWHEN");
}

void resultState_notePath(const char *path)
{
  if (!path || !*path || rs_npaths >= RS_MAX_PATHS)
    return;
  rs_paths[rs_npaths++] = rs_xstrdup(path);
}

int resultState_sourcesHaveContracts(void)
{
  int i;
  for (i = 0; i < rs_npaths; ++i)
    {
      long len;
      char *s = rs_readFile(rs_paths[i], &len);
      if (!s)
        continue;
      if (strstr(s, "transferswhen") || strstr(s, "TRANSFERSWHEN"))
        {
          free(s);
          return 1;
        }
      free(s);
    }
  return 0;
}

void resultState_configure(const char *dumpPath, const char *loadPath)
{
  rs_dump[0] = rs_load[0] = '\0';
  if (dumpPath)
    strncpy(rs_dump, dumpPath, sizeof(rs_dump) - 1);
  if (loadPath)
    strncpy(rs_load, loadPath, sizeof(rs_load) - 1);
}

int resultState_diagnosticCount(void)
{
  return rs_diags;
}

int resultState_contractCount(void)
{
  return rs_nrels;
}

/* --------------------------- library round trip ------------------------- */

static const char *rs_libCandidates(const char *name, int which)
{
  static char buf[1100];
  if (which == 0)
    return name;
  if (which == 1)
    {
      snprintf(buf, sizeof(buf), "%s.lcd", name);
      return buf;
    }
  if (which == 2 && strlen(name) > 4
      && strcmp(name + strlen(name) - 4, ".lcd") == 0)
    {
      snprintf(buf, sizeof(buf), "%.*s.rs", (int)strlen(name) - 4, name);
      return buf;
    }
  snprintf(buf, sizeof(buf), "%s.rs", name);
  return buf;
}

static void rs_decode(char *s)
{
  char *o = s;
  while (*s)
    {
      if (*s == '%' && s[1] && s[2])
        {
          unsigned int v;
          char h[3] = { s[1], s[2], 0 };
          if (sscanf(h, "%02x", &v) == 1)
            {
              *o++ = (char)v;
              s += 3;
              continue;
            }
        }
      *o++ = *s++;
    }
  *o = '\0';
}

static void rs_encodeField(FILE *f, const char *s)
{
  for (; *s; ++s)
    {
      unsigned char c = (unsigned char)*s;
      if (c == '\t' || c == '\n' || c == '\r' || c == '%' || c < 32)
        fprintf(f, "%%%02X", c);
      else
        fputc((int)c, f);
    }
}

void resultState_loadLibrary(const char *libraryName)
{
  int cand;
  char *data = NULL;
  const char *used = NULL;
  int i;
  static int loadedOnce = 0;

  if (!libraryName || loadedOnce)
    return;
  loadedOnce = 1;

  for (cand = 0; cand < 2; ++cand)
    {
      long len;
      const char *p = rs_libCandidates(libraryName, cand);
      data = rs_readFile(p, &len);
      if (data)
        {
          used = p;
          break;
        }
    }
  if (!data)
    return;

  /* Explicit rejection for a future/unknown result-state format. */
  for (i = 0; data[i]; ++i)
    {
      if ((strncmp(data + i, ";;RS-FORMAT", 11) == 0
           || strncmp(data + i, ";;resultState-format", 20) == 0
           || strncmp(data + i, ";;result-state-format", 21) == 0))
        {
          int p = i;
          int version = 0;
          while (data[p] && data[p] != '\n'
                 && !isdigit((unsigned char)data[p]))
            ++p;
          if (data[p] && isdigit((unsigned char)data[p]))
            version = atoi(data + p);
          if (version > 1)
            {
              RSLoc loc;
              memset(&loc, 0, sizeof(loc));
              strncpy(loc.file, used ? used : libraryName,
                      sizeof(loc.file) - 1);
              loc.line = 1;
              rs_error(loc,
                       "unsupported resultState library format version %d",
                       version);
              free(data);
              return;
            }
        }
    }

  {
    char *line = data;
    while (line && *line)
      {
        char *eol = strchr(line, '\n');
        char **fields = NULL;
        int nfields = 0;
        char *p;
        RSRel *rel;

        if (eol)
          *eol = '\0';

        if (strncmp(line, ";;RS1\t", 6) == 0)
          {
            p = line + 6;
            fields = (char **)calloc(10, sizeof(char *));
            if (fields)
              {
                fields[nfields++] = p;
                while (*p && nfields < 10)
                  {
                    if (*p == '\t')
                      {
                        *p = '\0';
                        fields[nfields++] = p + 1;
                      }
                    ++p;
                  }
              }
            if (nfields >= 8)
              {
                rel = rs_addRel();
                if (rel)
                  {
                    rel->used = 1;
                    rel->loaded = 1;
                    rel->funcIndex = -1;
                    rel->status = atoi(fields[0]);
                    strncpy(rel->src, fields[1], sizeof(rel->src) - 1);
                    strncpy(rel->dst, fields[2], sizeof(rel->dst) - 1);
                    strncpy(rel->func, fields[3], sizeof(rel->func) - 1);
                    strncpy(rel->loc.file, fields[4],
                            sizeof(rel->loc.file) - 1);
                    rel->loc.line = atoi(fields[5]);
                    rel->loc.col = atoi(fields[6]);
                    strncpy(rel->text, fields[7], sizeof(rel->text) - 1);
                    rs_decode(rel->src);
                    rs_decode(rel->dst);
                    rs_decode(rel->func);
                    rs_decode(rel->loc.file);
                    rs_decode(rel->text);
                  }
              }
            free(fields);
          }

        if (!eol)
          break;
        line = eol + 1;
      }
  }

  free(data);
}

void resultState_saveLibrary(const char *libraryName)
{
  const char *paths[2];
  int npaths = 0;
  int i, j;
  FILE *side;

  if (!libraryName)
    return;

  /* Splint may create either NAME or NAME.lcd depending on path/version. */
  paths[npaths++] = rs_libCandidates(libraryName, 0);
  paths[npaths++] = rs_libCandidates(libraryName, 1);

  for (i = 0; i < npaths; ++i)
    {
      FILE *append = fopen(paths[i], "a");
      if (append)
        {
          fprintf(append, ";;RS-FORMAT 1\n");
          for (j = 0; j < rs_nrels; ++j)
            {
              RSRel *r = &rs_rels[j];
              if (!r->used || r->loaded)
                continue;
              fputs(";;RS1\t", append);
              fprintf(append, "%d\t", r->status);
              rs_encodeField(append, r->src); fputc('\t', append);
              rs_encodeField(append, r->dst); fputc('\t', append);
              rs_encodeField(append, r->func); fputc('\t', append);
              rs_encodeField(append, r->loc.file); fputc('\t', append);
              fprintf(append, "%d\t%d\t", r->loc.line, r->loc.col);
              rs_encodeField(append, r->text);
              fputc('\n', append);
            }
          fclose(append);
        }
    }

  /* A standalone sidecar makes the round trip independent of .lcd naming. */
  side = fopen(rs_libCandidates(libraryName, 3), "w");
  if (side)
    {
      fprintf(side, "RS-FORMAT 1\n");
      for (i = 0; i < rs_nrels; ++i)
        {
          RSRel *r = &rs_rels[i];
          if (!r->used || r->loaded)
            continue;
          fprintf(side, "RS1\t%d\t", r->status);
          rs_encodeField(side, r->src); fputc('\t', side);
          rs_encodeField(side, r->dst); fputc('\t', side);
          rs_encodeField(side, r->func); fputc('\t', side);
          rs_encodeField(side, r->loc.file); fputc('\t', side);
          fprintf(side, "%d\t%d\t", r->loc.line, r->loc.col);
          rs_encodeField(side, r->text);
          fputc('\n', side);
        }
      fclose(side);
    }
}

/* ------------------------------ caller flow ----------------------------- */

typedef struct
{
  char name[64];
  int obj;
  int owned;
  int moved;
  int uncertain;
  char alias[64];
} RSVar;

typedef struct
{
  RSVar vars[48];
  int nvars;
  int nextObj;
} RSFlow;

typedef struct
{
  RSRel *rel;
  RSFunc *caller;
  int pos;
  int end;
  RSLoc loc;
  char srcArg[96];
  char srcVar[96];
  char dstArg[96];
  char dstVar[96];
  char resultVar[96];
  int invalidated;
  int hasIf;
  int condOpen, condClose;
  int thenOpen, thenClose;
  int elsePos, elseOpen, elseClose;
  int compareValue;
  int hasCompare;
  int negated;
  int equalOp; /* 1 for ==, 0 for != */
} RSSite;

static RSVar *rs_var(RSFlow *f, const char *name, int create)
{
  int i;
  if (!name || !*name)
    return NULL;
  for (i = 0; i < f->nvars; ++i)
    {
      if (strcmp(f->vars[i].name, name) == 0)
        return &f->vars[i];
    }
  if (!create || f->nvars >= (int)(sizeof(f->vars) / sizeof(f->vars[0])))
    return NULL;
  memset(&f->vars[f->nvars], 0, sizeof(RSVar));
  strncpy(f->vars[f->nvars].name, name,
          sizeof(f->vars[f->nvars].name) - 1);
  f->vars[f->nvars].obj = ++f->nextObj;
  ++f->nvars;
  return &f->vars[f->nvars - 1];
}

static RSFlow rs_copyFlow(const RSFlow *in)
{
  RSFlow out;
  memcpy(&out, in, sizeof(out));
  return out;
}

static int rs_simpleIdent(const char *s, int from, int to, char *out,
                          int outSize)
{
  int p = from;
  int n;
  rs_skipSpace(s, &p);
  while (p < to && (s[p] == '&' || s[p] == '*'))
    ++p;
  rs_skipSpace(s, &p);
  if (!rs_isIdent0((unsigned char)s[p]))
    return 0;
  from = p;
  while (p < to && rs_isIdent((unsigned char)s[p]))
    ++p;
  n = p - from;
  if (n >= outSize)
    n = outSize - 1;
  memcpy(out, s + from, (size_t)n);
  out[n] = '\0';
  return 1;
}

static int rs_splitCallArgs(const char *s, int openPos, int closePos,
                            char args[][128], int maxArgs)
{
  int depth = 0;
  int start = openPos + 1;
  int i = start;
  int n = 0;

  for (i = start; i <= closePos; ++i)
    {
      if (s[i] == '(' || s[i] == '[' || s[i] == '{')
        ++depth;
      else if (s[i] == ')' || s[i] == ']' || s[i] == '}')
        --depth;
      if ((s[i] == ',' && depth == 0) || i == closePos)
        {
          int a = start, b = i;
          while (a < b && isspace((unsigned char)s[a]))
            ++a;
          while (b > a && isspace((unsigned char)s[b - 1]))
            --b;
          if (b > a && n < maxArgs)
            {
              int len = b - a;
              if (len >= 128)
                len = 127;
              memcpy(args[n], s + a, (size_t)len);
              args[n][len] = '\0';
              ++n;
            }
          start = i + 1;
        }
    }
  return n;
}

static int rs_isAllocatorName(const char *name)
{
  return strstr(name, "alloc") || strstr(name, "make")
      || strstr(name, "new") || strstr(name, "create")
      || strstr(name, "dup") || strstr(name, "copy");
}

static int rs_isReleaseName(const char *name)
{
  return strcmp(name, "free") == 0 || strstr(name, "drop") != NULL
      || strstr(name, "release") != NULL || strstr(name, "destroy") != NULL
      || strstr(name, "close") != NULL;
}

static void rs_initFlowUntil(RSFlow *flow, RSFunc *caller, const char *code,
                             int until)
{
  int i;

  memset(flow, 0, sizeof(*flow));
  flow->nextObj = 100;

  /* Only parameters explicitly carrying only ownership start owned. */
  for (i = 0; i < caller->nparams; ++i)
    {
      RSParam *p = &caller->params[i];
      RSVar *v;
      if (!p->name[0])
        continue;
      v = rs_var(flow, p->name, 1);
      if (v && p->only)
        v->owned = 1;
      else if (v)
        v->obj = 0;
    }

  for (i = caller->bodyOpen + 1; i < until; ++i)
    {
      char name[96];
      int p = i;

      /* pointer alias: [type ...] alias = &target; */
      if (rs_isIdent0((unsigned char)code[i])
          && rs_parseIdentAt(code, &p, name, sizeof(name)))
        {
          int q = p;
          char maybe[96];
          rs_skipSpace(code, &q);
          while (code[q] == '*')
            ++q;
          rs_skipSpace(code, &q);
          if (code[q] == '=' && code[q + 1] != '=')
            {
              int r = q + 1;
              rs_skipSpace(code, &r);
              if (code[r] == '&')
                {
                  char target[64];
                  ++r;
                  rs_skipSpace(code, &r);
                  if (rs_parseIdentAt(code, &r, target, sizeof(target)))
                    {
                      RSVar *v = rs_var(flow, name, 1);
                      if (v)
                        {
                          strncpy(v->alias, target, sizeof(v->alias) - 1);
                          v->owned = 0;
                          v->obj = 0;
                        }
                    }
                }
              else
                {
                  char rhs[96];
                  int rp = r;
                  if (rs_parseIdentAt(code, &rp, rhs, sizeof(rhs)))
                    {
                      RSVar *v = rs_var(flow, name, 1);
                      if (v && rs_isAllocatorName(rhs))
                        v->owned = 1;
                    }
                }
              i = q;
              continue;
            }
          if (rs_parseIdentAt(code, &q, maybe, sizeof(maybe)))
            {
              rs_skipSpace(code, &q);
              if (code[q] == '=')
                {
                  int r = q + 1;
                  rs_skipSpace(code, &r);
                  if (code[r] == '&')
                    {
                      char target[64];
                      ++r;
                      rs_skipSpace(code, &r);
                      if (rs_parseIdentAt(code, &r, target, sizeof(target)))
                        {
                          RSVar *v = rs_var(flow, maybe, 1);
                          if (v)
                            {
                              strncpy(v->alias, target,
                                      sizeof(v->alias) - 1);
                              v->owned = 0;
                              v->obj = 0;
                            }
                        }
                    }
                  else
                    {
                      char rhs[96];
                      int rp = r;
                      if (rs_parseIdentAt(code, &rp, rhs, sizeof(rhs)))
                        {
                          if (rs_isAllocatorName(rhs))
                            {
                              RSVar *v = rs_var(flow, maybe, 1);
                              if (v)
                                v->owned = 1;
                            }
                          else
                            {
                              RSVar *src = rs_var(flow, rhs, 0);
                              RSVar *dst = rs_var(flow, maybe, 1);
                              if (src && dst)
                                {
                                  dst->obj = src->obj;
                                  dst->owned = src->owned;
                                }
                            }
                        }
                    }
                  i = q;
                  continue;
                }
            }

          if (rs_isReleaseName(name) && code[p] == '(')
            {
              int close = rs_matchBracket(code, p, '(', ')');
              char arg[96];
              if (close > 0
                  && rs_simpleIdent(code, p + 1, close, arg, sizeof(arg)))
                {
                  RSVar *v = rs_var(flow, arg, 0);
                  if (v)
                    {
                      v->owned = 0;
                      v->moved = 0;
                    }
                }
              i = close > i ? close : i;
            }
        }
    }
}

static const char *rs_resolveDst(RSFlow *flow, const char *arg, char *out,
                                 int outSize)
{
  char name[96];
  int p = 0;

  if (!arg || !*arg)
    return NULL;
  if (arg[0] == '&')
    {
      while (arg[p] == '&' || isspace((unsigned char)arg[p]))
        ++p;
      if (rs_simpleIdent(arg, p, (int)strlen(arg), name, sizeof(name)))
        {
          strncpy(out, name, outSize - 1);
          return out;
        }
    }
  if (rs_simpleIdent(arg, 0, (int)strlen(arg), name, sizeof(name)))
    {
      RSVar *v = rs_var(flow, name, 0);
      if (v && v->alias[0])
        {
          strncpy(out, v->alias, outSize - 1);
          return out;
        }
      /* One-level pointer alias may be assigned rather than initialized. */
      if (v && !v->alias[0])
        {
          strncpy(out, name, outSize - 1);
          return out;
        }
    }
  return NULL;
}

static void rs_applyOutcome(RSFlow *flow, RSSite *site, int success)
{
  RSRel *rel = site->rel;
  RSVar *src = rs_var(flow, site->srcVar[0] ? site->srcVar : rel->src, 0);
  RSVar *dst = rs_var(flow, site->dstVar, 1);
  int obj;

  if (!src)
    src = rs_var(flow, site->srcVar[0] ? site->srcVar : rel->src, 1);
  if (!dst)
    dst = rs_var(flow, site->dstVar, 1);

  obj = src->obj ? src->obj : flow->nextObj++;
  src->obj = obj;
  if (success)
    {
      dst->obj = obj;
      dst->owned = 1;
      dst->moved = 0;
      dst->uncertain = 0;
      src->owned = 0;
      src->moved = 1;
      src->uncertain = 0;
    }
  else
    {
      src->obj = obj;
      src->owned = 1;
      src->moved = 0;
      src->uncertain = 0;
    }
}

static int rs_rangeHasTerminalReturn(const char *code, int from, int to)
{
  int p = from;
  while (p < to)
    {
      int r = rs_findWord(code, p, "return");
      int semi;
      if (r < 0 || r >= to)
        break;
      semi = r;
      while (semi < to && code[semi] != ';')
        ++semi;
      return 1;
    }
  return 0;
}

static void rs_scanPath(RSSite *site, const char *code, int from, int to,
                        RSFlow *flow, int outcomeKnown)
{
  int p = from;
  int i;

  /* Releases anywhere on this straight/branch path satisfy the obligation. */
  for (i = from; i < to; ++i)
    {
      char name[96];
      int q = i;
      if (rs_isIdent0((unsigned char)code[i])
          && rs_parseIdentAt(code, &q, name, sizeof(name))
          && rs_isReleaseName(name) && code[q] == '(')
        {
          int close = rs_matchBracket(code, q, '(', ')');
          char arg[96];
          if (close > 0 && close <= to
              && rs_simpleIdent(code, q + 1, close, arg, sizeof(arg)))
            {
              RSVar *v = rs_var(flow, arg, 0);
              if (v)
                v->owned = 0;
            }
          i = close > i ? close : i;
        }
    }

  p = from;
  while (p < to)
    {
      int r = rs_findWord(code, p, "return");
      int semi;
      int next;

      if (r < 0 || r >= to)
        break;
      semi = r + 6;
      while (semi < to && code[semi] != ';')
        ++semi;

      for (i = 0; i < flow->nvars; ++i)
        {
          RSVar *v = &flow->vars[i];
          int k;
          int returnedAway = 0;
          if (!v->owned || v->moved || v->uncertain)
            continue;

          /* Returning the pointer itself transfers ownership to caller. */
          for (k = r; k < semi; ++k)
            {
              if (rs_wordAt(code, k, v->name))
                {
                  returnedAway = 1;
                  break;
                }
            }
          if (!returnedAway)
            {
              RSLoc loc = rs_locAt(site->caller->file, code, r);
              rs_error(loc,
                       "conditional ownership: return drops obligation for '%s' after call to %s (%s)",
                       v->name, site->rel->func, site->rel->text);
              rs_note(site->loc, "call location");
              rs_note(site->rel->loc,
                      "transferswhen relation and declaration location");
            }
          /* Whether reported or returned, this reference ends here. */
          v->owned = 0;
        }

      next = semi + 1;
      p = next;
    }

  /* Implicit void return at block end. */
  if (outcomeKnown)
    {
      for (i = 0; i < flow->nvars; ++i)
        {
          RSVar *v = &flow->vars[i];
          if (v->owned && !v->moved && !v->uncertain)
            {
              RSLoc loc = rs_locAt(site->caller->file, code, to);
              rs_error(loc,
                       "conditional ownership: end of path still must release '%s' after call to %s (%s)",
                       v->name, site->rel->func, site->rel->text);
              rs_note(site->loc, "call location");
              rs_note(site->rel->loc,
                      "transferswhen relation and declaration location");
              v->owned = 0;
            }
        }
    }
}

static int rs_findIfForSite(const char *code, RSSite *site)
{
  int searchFrom = site->pos > 600 ? site->pos - 600 : 0;
  int p = site->pos;
  int ifPos = -1;

  /* A direct call is usually inside its controlling if (...) condition. */
  {
    int k;
    for (k = site->pos - 2; k >= searchFrom; --k)
      {
        if (rs_wordAt(code, k, "if"))
          {
            int q = k + 2;
            rs_skipSpace(code, &q);
            if (code[q] == '(')
              {
                int c = rs_matchBracket(code, q, '(', ')');
                if (q < site->pos && c > site->end)
                  {
                    ifPos = k;
                    site->condOpen = q;
                    site->condClose = c;
                    break;
                  }
              }
          }
      }
  }

  /* For a stored result, look for the following status test. */
  if (ifPos < 0)
    {
      int fwd = site->end;
      int limit = site->caller->bodyClose;
      int k;
      for (k = fwd; k < limit && k < fwd + 500; ++k)
        {
          if (rs_wordAt(code, k, "if"))
            {
              int q = k + 2;
              rs_skipSpace(code, &q);
              if (code[q] == '(')
                {
                  int c = rs_matchBracket(code, q, '(', ')');
                  if (c > q)
                    {
                      ifPos = k;
                      site->condOpen = q;
                      site->condClose = c;
                      break;
                    }
                }
            }
        }
    }

  if (ifPos < 0)
    return 0;

  site->hasIf = 1;
  p = site->condClose + 1;
  rs_skipSpace(code, &p);
  if (code[p] == '{')
    {
      site->thenOpen = p;
      site->thenClose = rs_matchBracket(code, p, '{', '}');
      p = site->thenClose + 1;
      rs_skipSpace(code, &p);
      if (rs_wordAt(code, p, "else"))
        {
          site->elsePos = p;
          p += 4;
          rs_skipSpace(code, &p);
          if (code[p] == '{')
            {
              site->elseOpen = p;
              site->elseClose = rs_matchBracket(code, p, '{', '}');
            }
        }
    }
  return 1;
}

static int rs_siteThenIsSuccess(RSSite *site, const char *code)
{
  int from = site->condOpen + 1;
  int to = site->condClose;
  int p;

  site->hasCompare = 0;
  site->negated = 0;

  for (p = from; p < to - 1; ++p)
    {
      if (code[p] == '=' && code[p + 1] == '=')
        {
          int q = p + 2;
          site->equalOp = 1;
          site->hasCompare = 1;
          rs_skipSpace(code, &q);
          if (!rs_parseInt(code, &q, &site->compareValue))
            {
              char id[64];
              if (rs_parseIdentAt(code, &q, id, sizeof(id)))
                site->compareValue = site->rel->status;
            }
          return site->compareValue == site->rel->status;
        }
      if (code[p] == '!' && code[p + 1] == '=')
        {
          int q = p + 2;
          site->equalOp = 0;
          site->hasCompare = 1;
          rs_skipSpace(code, &q);
          if (!rs_parseInt(code, &q, &site->compareValue))
            site->compareValue = site->rel->status;
          /* then is true when value != compareValue. */
          return site->compareValue != site->rel->status;
        }
    }

  /* A leading '!' applies to the result/call. */
  p = from;
  rs_skipSpace(code, &p);
  if (code[p] == '!')
    {
      site->negated = 1;
      return site->rel->status == 0;
    }

  /* if (result) means nonzero; only treat as success for nonzero status. */
  if (site->resultVar[0] || 1)
    return site->rel->status != 0;
}

static char rs_movedNames[32][96];
static int rs_movedEnd[32];
static int rs_nmoved;

static void rs_resultLValue(const char *code, int callPos, char *out,
                            int outSize)
{
  int start = callPos > 300 ? callPos - 300 : 0;
  int k;

  out[0] = '\0';
  for (k = callPos - 1; k > start; --k)
    {
      if (code[k] == ';' || code[k] == '{' || code[k] == '}')
        {
          start = k + 1;
          break;
        }
    }
  for (k = callPos - 1; k >= start; --k)
    {
      if (code[k] == '=' && code[k + 1] != '=' && code[k - 1] != '='
          && code[k - 1] != '!' && code[k - 1] != '<' && code[k - 1] != '>')
        {
          (void)rs_lastIdentBefore(code, k, out, outSize);
          return;
        }
    }
}

static int rs_wasAssignedBetween(const char *code, int from, int to,
                                 const char *name)
{
  int k;
  if (!name || !name[0] || to <= from)
    return 0;
  for (k = from; k < to; ++k)
    {
      if (rs_wordAt(code, k, name))
        {
          int q = k + (int)strlen(name);
          rs_skipSpace(code, &q);
          if (code[q] == '=' && code[q + 1] != '=')
            return 1;
        }
    }
  return 0;
}

static void rs_checkCallSite(RSFunc *caller, RSRel *rel, const char *code,
                             int namePos, int callOpen, int callClose)
{
  RSSite site;
  char args[8][128];
  int nargs;
  RSFlow base, thenFlow, elseFlow;
  int thenSuccess;
  RSVar *srcVar;
  RSVar *existingDst;

  memset(&site, 0, sizeof(site));
  site.rel = rel;
  site.caller = caller;
  site.pos = namePos;
  site.end = callClose;
  site.loc = rs_locAt(caller->file, code, namePos);
  site.condOpen = site.condClose = -1;
  site.thenOpen = site.thenClose = -1;
  site.elseOpen = site.elseClose = -1;

  nargs = rs_splitCallArgs(code, callOpen, callClose, args, 8);
  rs_resultLValue(code, namePos, site.resultVar, sizeof(site.resultVar));
  if (nargs >= 1)
    {
      strncpy(site.srcArg, args[0], sizeof(site.srcArg) - 1);
      (void)rs_simpleIdent(site.srcArg, 0, (int)strlen(site.srcArg),
                           site.srcVar, sizeof(site.srcVar));
    }
  if (nargs >= 2)
    strncpy(site.dstArg, args[1], sizeof(site.dstArg) - 1);

  rs_initFlowUntil(&base, caller, code, namePos);
  if (rs_nmoved)
    {
      int mi;
      for (mi = 0; mi < rs_nmoved; ++mi)
        {
          int k;
          int reassigned = 0;
          for (k = rs_movedEnd[mi]; k < namePos; ++k)
            {
              if (rs_wordAt(code, k, rs_movedNames[mi]))
                {
                  int q = k + (int)strlen(rs_movedNames[mi]);
                  rs_skipSpace(code, &q);
                  if (code[q] == '=' && code[q + 1] != '=')
                    reassigned = 1;
                }
            }
          if (reassigned)
            {
              int j;
              for (j = mi; j < rs_nmoved - 1; ++j)
                {
                  strcpy(rs_movedNames[j], rs_movedNames[j + 1]);
                  rs_movedEnd[j] = rs_movedEnd[j + 1];
                }
              --rs_nmoved;
              --mi;
              continue;
            }
          {
            RSVar *mv = rs_var(&base, rs_movedNames[mi], 1);
            if (mv)
              mv->moved = 1;
          }
        }
    }
  if (!rs_resolveDst(&base, site.dstArg, site.dstVar,
                     sizeof(site.dstVar)))
    {
      RSLoc loc = site.loc;
      rs_error(loc,
               "conditional ownership: output argument for %s must be &local or one-level pointer alias (%s)",
               rel->func, rel->text);
      rs_note(rel->loc, "transferswhen relation and declaration location");
      return;
    }

  existingDst = rs_var(&base, site.dstVar, 0);
  if (existingDst && existingDst->owned)
    {
      rs_error(site.loc,
               "conditional ownership: call to %s overwrites live object held by '%s' and invalidates the previous relation (%s)",
               rel->func, site.dstVar, rel->text);
      rs_note(rel->loc, "transferswhen relation and declaration location");
      existingDst->owned = 0;
    }

  srcVar = rs_var(&base, site.srcVar[0] ? site.srcVar : rel->src, 0);
  if (srcVar && srcVar->moved)
    {
      rs_error(site.loc,
               "conditional ownership: source '%s' was already transferred on this path (%s)",
               site.srcVar[0] ? site.srcVar : rel->src, rel->text);
      rs_note(rel->loc, "transferswhen relation and declaration location");
    }

  (void)rs_findIfForSite(code, &site);
  if (!site.hasIf)
    {
      /* Unchecked status: preserve obligations rather than guessing. */
      return;
    }

  if (site.condOpen > site.end)
    {
      if (site.resultVar[0]
          && rs_wasAssignedBetween(code, site.end + 1, site.condOpen,
                                   site.resultVar))
        site.invalidated = 1;
      if (site.srcVar[0]
          && rs_wasAssignedBetween(code, site.end + 1, site.condOpen,
                                   site.srcVar))
        site.invalidated = 1;
    }
  if (site.invalidated)
    {
      /* The status/resource association is gone, but pre-call ownership
         obligations still have to be released on the remaining paths. */
      rs_scanPath(&site, code, site.end + 1, caller->bodyClose, &base, 1);
      return;
    }

  thenSuccess = rs_siteThenIsSuccess(&site, code);
  thenFlow = rs_copyFlow(&base);
  rs_applyOutcome(&thenFlow, &site, thenSuccess);
  if (site.thenOpen >= 0)
    rs_scanPath(&site, code, site.thenOpen + 1, site.thenClose,
                &thenFlow, 1);

  if (site.elseOpen >= 0)
    {
      elseFlow = rs_copyFlow(&base);
      rs_applyOutcome(&elseFlow, &site, !thenSuccess);
      rs_scanPath(&site, code, site.elseOpen + 1, site.elseClose,
                  &elseFlow, 1);
    }
  else if (site.thenOpen >= 0 && rs_rangeHasTerminalReturn(
               code, site.thenOpen + 1, site.thenClose))
    {
      RSFlow cont = rs_copyFlow(&base);
      rs_applyOutcome(&cont, &site, !thenSuccess);
      rs_scanPath(&site, code, site.thenClose + 1, caller->bodyClose,
                  &cont, 1);
    }
  else if (site.thenOpen >= 0)
    {
      RSFlow failCont = rs_copyFlow(&base);
      rs_applyOutcome(&failCont, &site, !thenSuccess);
      rs_scanPath(&site, code, site.thenClose + 1, caller->bodyClose,
                  &failCont, 1);

      /* The fall-through state is the opposite outcome and is checked above. */
      if (!rs_rangeHasTerminalReturn(code, site.thenOpen + 1,
                                     site.thenClose))
        {
          RSVar *s = rs_var(&base, site.srcVar, 0);
          if (s)
            s->uncertain = 1;
        }
    }

  if (site.hasIf && site.srcVar[0] && rs_nmoved < 32)
    {
      int exists = 0;
      int mi;
      for (mi = 0; mi < rs_nmoved; ++mi)
        {
          if (strcmp(rs_movedNames[mi], site.srcVar) == 0)
            {
              exists = 1;
              rs_movedEnd[mi] = callClose;
            }
        }
      if (!exists)
        {
          strncpy(rs_movedNames[rs_nmoved], site.srcVar, 95);
          rs_movedEnd[rs_nmoved] = callClose;
          ++rs_nmoved;
        }
    }
}

static void rs_checkCallersInFile(const char *path, const char *src)
{
  char *masked = rs_maskCode(src);
  int fi;

  if (!masked)
    return;
  rs_collectFunctions(path, src, masked);

  for (fi = 0; fi < rs_nfuncs; ++fi)
    {
      RSFunc *caller = &rs_funcs[fi];
      int p;

      if (!caller->used || strcmp(caller->file, path) != 0
          || caller->bodyOpen < 0)
        continue;

      rs_nmoved = 0;
      for (p = caller->bodyOpen + 1; p < caller->bodyClose; ++p)
        {
          int ri;
          for (ri = 0; ri < rs_nrels; ++ri)
            {
              RSRel *rel = &rs_rels[ri];
              int q = p;
              int open, close;
              if (!rel->used || !rel->func[0])
                continue;
              if (!rs_wordAt(masked, p, rel->func))
                continue;
              q = p + (int)strlen(rel->func);
              rs_skipSpace(masked, &q);
              if (masked[q] != '(')
                continue;
              open = q;
              close = rs_matchBracket(masked, open, '(', ')');
              if (close < 0 || close > caller->bodyClose)
                continue;
              if (strcmp(caller->name, rel->func) == 0)
                {
                  p = close;
                  continue;
                }
              rs_checkCallSite(caller, rel, masked, p, open, close);
              p = close;
              break;
            }
        }
    }
  free(masked);
}

int resultState_run(void)
{
  int i;

  if (rs_load[0])
    resultState_loadLibrary(rs_load);

  for (i = 0; i < rs_npaths; ++i)
    {
      long len;
      char *src = rs_readFile(rs_paths[i], &len);
      if (!src)
        continue;
      rs_parseFileContracts(rs_paths[i], src);
      free(src);
    }

  rs_validateDeclarations();
  rs_checkCallees();

  for (i = 0; i < rs_npaths; ++i)
    {
      long len;
      char *src = rs_readFile(rs_paths[i], &len);
      if (!src)
        continue;
      rs_checkCallersInFile(rs_paths[i], src);
      free(src);
    }

  return rs_diags;
}
