#pragma once
// Exact rational geometry used by the independent reference implementation.
//
// Bound on intermediate values.
//
// Inputs have integer coordinates with |x|,|y| <= 32 and edges that are
// horizontal, vertical, or have slope +-1.  Two such lines therefore meet at
// integer or half-integer points (denominator 1 or 2).  All reference
// coordinates and all accepted output vertices are thus rationals with
// denominator at most 2.
//
// Every event y is one of those coordinates, so a band height built as
// lo + span/4 or lo + 3*span/4 has a denominator of at most 32.  The x
// coordinate where a triangle or reference segment crosses such a height is
// a rational with denominator at most 2^9, because it divides by a coordinate
// difference whose denominator divides 4.  Comparing two such x values
// multiplies a numerator by a denominator, i.e. at most 2^14 * 2^9 = 2^23,
// and the orientation tests in point-in-triangle multiply at most two such
// quantities (below 2^40).  All of this is far below 2^127, so __int128 is
// ample even before the gcd normalisation performed by ratNorm().
//
// Scaled projection variants are normalised back to the base lattice before
// they enter this code, so the same bound applies there.

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace tref {

typedef __int128 i128;

static inline i128 abs128(i128 v) { return v < 0 ? -v : v; }

static inline i128 gcd128(i128 a, i128 b) {
  a = abs128(a);
  b = abs128(b);
  while (b != 0) {
    i128 t = a % b;
    a = b;
    b = t;
  }
  return a;
}

struct Rat {
  i128 n;
  i128 d;
};

static inline Rat ratNorm(i128 n, i128 d) {
  if (d < 0) {
    n = -n;
    d = -d;
  }
  if (n == 0) {
    d = 1;
  } else {
    i128 g = gcd128(n, d);
    if (g > 1) {
      n /= g;
      d /= g;
    }
  }
  Rat r;
  r.n = n;
  r.d = d;
  return r;
}

static inline Rat rat(long long n, long long d = 1) {
  return ratNorm((i128)n, (i128)d);
}

static inline int ratCmp(const Rat& a, const Rat& b) {
  i128 lhs = a.n * b.d;
  i128 rhs = b.n * a.d;
  return lhs < rhs ? -1 : (lhs > rhs ? 1 : 0);
}

static inline bool ratEq(const Rat& a, const Rat& b) { return ratCmp(a, b) == 0; }
static inline bool ratLt(const Rat& a, const Rat& b) { return ratCmp(a, b) < 0; }
static inline bool ratGt(const Rat& a, const Rat& b) { return ratCmp(a, b) > 0; }
static inline bool ratIsZero(const Rat& a) { return a.n == 0; }

static inline Rat ratAdd(const Rat& a, const Rat& b) {
  return ratNorm(a.n * b.d + b.n * a.d, a.d * b.d);
}
static inline Rat ratSub(const Rat& a, const Rat& b) {
  return ratNorm(a.n * b.d - b.n * a.d, a.d * b.d);
}
static inline Rat ratMul(const Rat& a, const Rat& b) {
  return ratNorm(a.n * b.n, a.d * b.d);
}
static inline Rat ratDiv(const Rat& a, const Rat& b) {
  return ratNorm(a.n * b.d, a.d * b.n);
}
static inline Rat ratNeg(const Rat& a) { return ratNorm(-a.n, a.d); }

static inline std::string i128Str(i128 v) {
  if (v == 0) return "0";
  bool neg = v < 0;
  if (neg) v = -v;
  std::string s;
  while (v > 0) {
    int digit = (int)(v % 10);
    s.push_back((char)('0' + digit));
    v /= 10;
  }
  if (neg) s.push_back('-');
  for (size_t i = 0, j = s.size(); i + 1 < j; ++i, --j) {
    char t = s[i];
    s[i] = s[j - 1];
    s[j - 1] = t;
  }
  return s;
}

static inline std::string ratStr(const Rat& a) {
  if (a.d == 1) return i128Str(a.n);
  return i128Str(a.n) + "/" + i128Str(a.d);
}

struct RVec2 {
  Rat x;
  Rat y;
};

static inline RVec2 vec(Rat x, Rat y) {
  RVec2 p;
  p.x = x;
  p.y = y;
  return p;
}

static inline RVec2 vec(long long x, long long y) { return vec(rat(x), rat(y)); }

static inline RVec2 vAdd(const RVec2& a, const RVec2& b) {
  return vec(ratAdd(a.x, b.x), ratAdd(a.y, b.y));
}
static inline RVec2 vSub(const RVec2& a, const RVec2& b) {
  return vec(ratSub(a.x, b.x), ratSub(a.y, b.y));
}
static inline RVec2 vScale(const RVec2& a, const Rat& s) {
  return vec(ratMul(a.x, s), ratMul(a.y, s));
}
static inline Rat cross2(const RVec2& a, const RVec2& b) {
  return ratSub(ratMul(a.x, b.y), ratMul(a.y, b.x));
}
static inline std::string vecStr(const RVec2& p) {
  return "(" + ratStr(p.x) + "," + ratStr(p.y) + ")";
}

static inline RVec2 ratMid(const RVec2& a, const RVec2& b) {
  return vec(ratDiv(ratAdd(a.x, b.x), rat(2)), ratDiv(ratAdd(a.y, b.y), rat(2)));
}

static inline std::vector<Rat> ratSortedUnique(std::vector<Rat> v) {
  // insertion sort is sufficient for the small event sets used here; it also
  // avoids requiring a comparator that is not provided by std::sort in C++17.
  for (size_t i = 1; i < v.size(); ++i) {
    Rat key = v[i];
    size_t j = i;
    while (j > 0 && ratLt(key, v[j - 1])) {
      v[j] = v[j - 1];
      --j;
    }
    v[j] = key;
  }
  std::vector<Rat> out;
  for (size_t i = 0; i < v.size(); ++i) {
    if (out.empty() || !ratEq(out.back(), v[i])) out.push_back(v[i]);
  }
  return out;
}

}  // namespace tref
