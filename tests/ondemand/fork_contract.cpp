// Fork contract tests for ondemand::value::fork() / ondemand::forked_value.
//
// Self-contained: brings its own main() and its own global allocation
// instrumentation. Designed to pass in both the default build and a build with
// SIMDJSON_DEVELOPMENT_CHECKS=1. On full success it prints FORK_CONTRACT_OK.
// Any failed assertion returns a non-zero exit code.

#include "simdjson.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <new>
#include <string>
#include <string_view>
#include <vector>

// Reuse the real example pipeline (without its main()). All parsing and
// cleaning in that translation unit goes through value::fork().
#define COLLECT_ROWS_NO_MAIN
#include "../../examples/collect_rows.cpp"
#undef COLLECT_ROWS_NO_MAIN

using namespace simdjson;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    ++g_checks;                                                                \
    if (!(cond)) {                                                             \
      ++g_failures;                                                            \
      std::printf("CHECK FAILED at %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
    }                                                                          \
  } while (0)

#define CHECK_ERROR(expr, expected)                                            \
  do {                                                                         \
    ++g_checks;                                                                \
    auto _e = (expr);                                                          \
    if (_e.error() != (expected)) {                                            \
      ++g_failures;                                                            \
      std::printf("CHECK FAILED at %s:%d: %s -> expected %s got %s\n",         \
                  __FILE__, __LINE__, #expr,                                   \
                  error_message(expected), error_message(_e.error()));         \
    }                                                                          \
  } while (0)

// ---------------------------------------------------------------------------
// Actual-allocation instrumentation. These are replacement global allocators,
// so every byte counted is a real malloc request made during the measured
// window (the fork storage and anything it genuinely allocates).
// ---------------------------------------------------------------------------
bool g_measuring = false;
long long g_window_bytes = 0;   // gross bytes requested during the window

} // namespace

void *operator new(std::size_t n) {
  if (g_measuring) { g_window_bytes += static_cast<long long>(n); }
  void *p = std::malloc(n ? n : 1);
  if (!p) { throw std::bad_alloc(); }
  return p;
}
void *operator new[](std::size_t n) {
  if (g_measuring) { g_window_bytes += static_cast<long long>(n); }
  void *p = std::malloc(n ? n : 1);
  if (!p) { throw std::bad_alloc(); }
  return p;
}
void *operator new(std::size_t n, std::align_val_t a) {
  if (g_measuring) { g_window_bytes += static_cast<long long>(n); }
  void *p = std::aligned_alloc(static_cast<std::size_t>(a),
                               ((n + static_cast<std::size_t>(a) - 1) /
                                static_cast<std::size_t>(a)) *
                                   static_cast<std::size_t>(a));
  if (!p) { throw std::bad_alloc(); }
  return p;
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }
void operator delete(void *p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void *p, std::size_t, std::align_val_t) noexcept { std::free(p); }

namespace {

// ---------------------------------------------------------------------------
// Basic shapes: scalars, empty containers, nested values.
// ---------------------------------------------------------------------------
void test_scalars_and_empty_containers() {
  auto json = R"({
    "num": 42,
    "neg": -7,
    "dbl": 2.5,
    "txt": "plain-utf8-茶",
    "esc": "a\nb\tc\\d\"e",
    "uni": "café 中",
    "flag": true,
    "nil": null,
    "empty_obj": {},
    "empty_arr": [],
    "deep": {"outer": [{"inner": {"k": [1, 2, 3]}}]}
  })"_padded;

  ondemand::parser parser;
  auto doc = parser.iterate(json);
  ondemand::object root;
  CHECK(!doc.get_object().get(root));

  // Number scalars.
  {
    auto f = root["num"].fork();
    CHECK(!f.error());
    auto v = f.value_unsafe().get_value();
    CHECK(!v.error());
    CHECK(int64_t(v.value_unsafe()) == 42);
    CHECK_ERROR(f.value_unsafe().get_value(), OUT_OF_ORDER_ITERATION);
  }
  {
    auto f = root["neg"].fork();
    auto v = f.value_unsafe().get_value();
    CHECK(int64_t(v.value_unsafe()) == -7);
  }
  {
    auto f = root["dbl"].fork();
    auto v = f.value_unsafe().get_value();
    CHECK(double(v.value_unsafe()) == 2.5);
  }
  // Boolean and null.
  {
    auto f = root["flag"].fork();
    auto v = f.value_unsafe().get_value();
    CHECK(bool(v.value_unsafe()) == true);
  }
  {
    auto f = root["nil"].fork();
    auto v = f.value_unsafe().get_value();
    CHECK(v.value_unsafe().is_null().value_unsafe() == true);
  }
  // Escaped / unicode string decoded into the fork's private buffer.
  {
    auto f = root["esc"].fork();
    auto v = f.value_unsafe().get_value();
    std::string_view s;
    CHECK(!v.value_unsafe().get_string().get(s));
    CHECK(s == std::string_view("a\nb\tc\\d\"e"));
  }
  {
    auto f = root["uni"].fork();
    auto v = f.value_unsafe().get_value();
    std::string_view s;
    CHECK(!v.value_unsafe().get_string().get(s));
    CHECK(s == std::string_view("caf\xc3\xa9 \xe4\xb8\xad"));
  }
  // type() must not consume: a type() inspection followed by fork() is legal.
  {
    auto tv = root["txt"];
    CHECK(tv.type().value_unsafe() == ondemand::json_type::string);
    auto f = tv.fork();
    CHECK(!f.error());
    auto v = f.value_unsafe().get_value();
    std::string_view s;
    CHECK(!v.value_unsafe().get_string().get(s));
    CHECK(s == std::string_view("plain-utf8-茶"));
  }
  // Empty containers.
  {
    auto f = root["empty_obj"].fork();
    auto v = f.value_unsafe().get_value();
    ondemand::object o;
    CHECK(!v.value_unsafe().get_object().get(o));
    CHECK(o.is_empty().value_unsafe() == true);
    std::size_t fields = 0;
    for (auto field : o) { ++fields; (void)field; }
    CHECK(fields == 0);
  }
  {
    auto f = root["empty_arr"].fork();
    auto v = f.value_unsafe().get_value();
    ondemand::array a;
    CHECK(!v.value_unsafe().get_array().get(a));
    CHECK(a.is_empty().value_unsafe() == true);
    std::size_t elems = 0;
    for (auto elem : a) { ++elems; (void)elem; }
    CHECK(elems == 0);
  }
  // Multi-level nesting reached entirely through one fork.
  {
    auto f = root["deep"].fork();
    auto v = f.value_unsafe().get_value();
    int64_t sum = 0;
    ondemand::array ks;
    CHECK(!v.value_unsafe()["outer"].at(0)["inner"]["k"].get_array().get(ks));
    for (auto elem : ks) { sum += int64_t(elem); }
    CHECK(sum == 6);
  }
}

// ---------------------------------------------------------------------------
// Three-way interleaving. The parent loop advances to the next row while two
// forks of the previous row are still live; each fork reads its nested fields
// in reverse document order (using the unordered lookup), and a string captured
// before the parent moved must still decode correctly afterwards.
// ---------------------------------------------------------------------------
void test_three_way_interleave() {
  auto json = R"({"rows":[
    {"id":1,"price":{"amount":100,"currency":"EUR"},"desc":{"en":"A\nB","zh":"茶"}},
    {"id":2,"price":{"amount":250,"currency":"USD"},"desc":{"en":"café","zh":"中文"}}
  ]})"_padded;

  ondemand::parser parser;
  auto doc = parser.iterate(json);
  ondemand::array rows;
  CHECK(!doc["rows"].get_array().get(rows));

  struct Price { int64_t amount; std::string_view currency; };
  struct Desc { std::string_view en; std::string_view zh; };
  std::vector<Price> prices;
  std::vector<Desc> descs;
  std::vector<std::string_view> early_strings;

  // Keep every owner alive for the whole test: the obtained string_views are
  // only guaranteed valid until their forked_value is destroyed. The owners are
  // moved into these vectors; because each cursor is heap-held, its address and
  // every value/view derived from it stay valid across those moves.
  struct LiveFork {
    ondemand::forked_value owner;
    ondemand::value root;
  };
  std::vector<LiveFork> price_live;
  std::vector<LiveFork> desc_live;

  std::size_t row_index = 0;
  for (auto row_result : rows) {
    ondemand::value row;
    CHECK(!row_result.get(row));

    LiveFork pf{row.fork().value_unsafe(), {}};
    LiveFork df{row.fork().value_unsafe(), {}};
    CHECK(!pf.owner.get_value().get(pf.root));
    CHECK(!df.owner.get_value().get(df.root));
    ondemand::value pv = pf.root;
    ondemand::value dv = df.root;

    // Capture an escaped string from the desc fork *before* the parent moves.
    std::string_view zh_early;
    CHECK(!dv["desc"]["zh"].get_string().get(zh_early));
    early_strings.emplace_back(zh_early);

    // The parent path consumes the row itself and then advances to the next row
    // (the for-loop's next iteration), while both forks stay live.
    int64_t id = int64_t(row["id"]);
    CHECK((row_index == 0 ? id == 1 : id == 2));
    ++row_index;

    // Fork A: read currency before amount (reverse of JSON order).
    {
      Price p;
      CHECK(!pv["price"]["currency"].get_string().get(p.currency));
      CHECK(!pv["price"]["amount"].get_int64().get(p.amount));
      prices.push_back(p);
    }
    // Fork B: read zh before en (reverse of JSON order); the previously captured
    // view must remain valid and equal.
    {
      Desc d;
      CHECK(!dv["desc"]["zh"].get_string().get(d.zh));
      CHECK(!dv["desc"]["en"].get_string().get(d.en));
      descs.push_back(d);
      CHECK(d.zh == early_strings.back());
    }

    // Move the owners into long-lived storage and confirm the already-derived
    // views still compare equal after the move.
    price_live.push_back(std::move(pf));
    desc_live.push_back(std::move(df));
    CHECK(prices.back().currency == (row_index == 1 ? std::string_view("EUR")
                                                    : std::string_view("USD")));
    CHECK(descs.back().zh == early_strings.back());
  }

  // All owners are still alive here, so every retained view must be intact.
  CHECK(prices.size() == 2 && descs.size() == 2);
  CHECK(prices[0].amount == 100 && prices[0].currency == "EUR");
  CHECK(prices[1].amount == 250 && prices[1].currency == "USD");
  CHECK(descs[0].en == std::string_view("A\nB"));
  CHECK(descs[0].zh == std::string_view("茶"));
  CHECK(descs[1].en == std::string_view("caf\xc3\xa9"));
  CHECK(descs[1].zh == std::string_view("\xe4\xb8\xad\xe6\x96\x87"));
  CHECK(early_strings.size() == 2);
  CHECK(early_strings[0] == std::string_view("茶"));
  CHECK(early_strings[1] == std::string_view("\xe4\xb8\xad\xe6\x96\x87"));
}

// ---------------------------------------------------------------------------
// Nested fork: a fork may itself be forked. The inner fork borrows only the
// original input and structural indexes, so the outer owner can be destroyed
// first while the inner fork keeps reading.
// ---------------------------------------------------------------------------
void test_nested_fork_outer_destroyed_first() {
  auto json = R"({"row":{"price":{"amount":42,"currency":"X"},"keep":1}})"_padded;
  ondemand::parser parser;
  auto doc = parser.iterate(json);

  ondemand::forked_value outer;
  {
    auto made = doc["row"].fork();
    CHECK(!made.error());
    outer = std::move(made.value_unsafe());
  }
  ondemand::value outer_v;
  CHECK(!outer.get_value().get(outer_v));

  // Fork the nested "price" value out of the outer fork's value.
  ondemand::forked_value inner = outer_v["price"].fork().value_unsafe();

  // Destroy the outer owner. The inner owner must survive because it only
  // borrows the original parser/input/indexes, not the outer cursor.
  {
    ondemand::forked_value tomb = std::move(outer);
    (void)tomb;
  }

  ondemand::value iv;
  CHECK(!inner.get_value().get(iv));
  int64_t amount;
  std::string_view currency;
  CHECK(!iv["amount"].get_int64().get(amount));
  CHECK(!iv["currency"].get_string().get(currency));
  CHECK(amount == 42 && currency == "X");
}

// ---------------------------------------------------------------------------
// After handing a value out of a fork, moving the owner must not invalidate the
// already-held view: the cursor is heap-held, so its address is stable.
// ---------------------------------------------------------------------------
void test_old_view_survives_owner_move() {
  auto json = R"({"o":{"a":1,"b":"two","c":[10,20]}})"_padded;
  ondemand::parser parser;
  auto doc = parser.iterate(json);

  ondemand::forked_value owner = doc["o"].fork().value_unsafe();
  ondemand::value v;
  CHECK(!owner.get_value().get(v));

  ondemand::forked_value moved = std::move(owner);

  // The moved-from owner is empty; the moved-to owner already dispensed its
  // single value. Both reflect the documented error codes.
  CHECK_ERROR(owner.get_value(), UNINITIALIZED);
  CHECK_ERROR(moved.get_value(), OUT_OF_ORDER_ITERATION);

  // The old view v keeps reading through the relocated owner.
  CHECK(int64_t(v["a"]) == 1);
  std::string_view b;
  CHECK(!v["b"].get_string().get(b));
  CHECK(b == std::string_view("two"));
  int64_t sum = 0;
  ondemand::array c = v["c"].get_array().value_unsafe();
  for (auto e : c) { sum += int64_t(e); }
  CHECK(sum == 30);

  // A default-constructed owner has no fork.
  ondemand::forked_value empty;
  CHECK_ERROR(empty.get_value(), UNINITIALIZED);
}

// ---------------------------------------------------------------------------
// Error isolation: a missing field, a wrong type and a lazily detected format
// error inside one fork must not corrupt the parent path or a sibling fork.
// ---------------------------------------------------------------------------
void test_error_isolation() {
  // Missing field / wrong type document.
  {
    auto json = R"({"a":{"x":1},"b":[1,2]})"_padded;
    ondemand::parser parser;
    auto doc = parser.iterate(json);

    auto f_missing = doc["a"].fork();
    auto f_wrong = doc["a"].fork();
    auto f_clean = doc["a"].fork();
    CHECK(!f_missing.error() && !f_wrong.error() && !f_clean.error());

    // Missing field in the first fork.
    {
      ondemand::value v = f_missing.value_unsafe().get_value().value_unsafe();
      CHECK_ERROR(v["nope"], NO_SUCH_FIELD);
    }
    // Wrong type in the second fork: an object is not an integer.
    {
      ondemand::value v = f_wrong.value_unsafe().get_value().value_unsafe();
      CHECK_ERROR(v.get_int64(), INCORRECT_TYPE);
      // INCORRECT_TYPE is non-fatal and non-consuming: the fork can still read
      // the object correctly.
      CHECK(int64_t(v["x"]) == 1);
    }
    // A third fork is completely unaffected by the two failures above.
    {
      ondemand::value v = f_clean.value_unsafe().get_value().value_unsafe();
      CHECK(int64_t(v["x"]) == 1);
    }
    // The parent path is also unaffected and continues to the next field.
    ondemand::array b;
    CHECK(!doc["b"].get_array().get(b));
    int64_t sum = 0;
    for (auto e : b) { sum += int64_t(e); }
    CHECK(sum == 3);
  }

  // A lazily detectable format error (missing comma between object fields) is
  // reported only inside the fork that walks into it; siblings stay healthy.
  {
    auto json = R"({"bad":{"x":1 "y":2},"ok":7})"_padded;
    ondemand::parser parser;
    auto doc = parser.iterate(json); // stage 1 is lazy and does not reject here

    auto f_bad = doc["bad"].fork();
    auto f_ok = doc["ok"].fork();
    CHECK(!f_bad.error() && !f_ok.error());

    bool saw_tape_error = false;
    {
      ondemand::value v = f_bad.value_unsafe().get_value().value_unsafe();
      // The first field reads fine. Reaching the second field requires stepping
      // over the missing comma, which the lazy structural walk rejects.
      auto first = v.find_field("x");
      if (!first.error() && int64_t(first.value_unsafe()) == 1) {
        auto second = v.find_field("y");
        saw_tape_error = (second.error() != SUCCESS);
      } else {
        saw_tape_error = true;
      }
    }
    CHECK(saw_tape_error);

    // The sibling fork and the parent are untouched by the fork's fatal error.
    ondemand::value okv = f_ok.value_unsafe().get_value().value_unsafe();
    CHECK(int64_t(okv) == 7);
    CHECK(int64_t(doc["ok"]) == 7);
  }
}

// ---------------------------------------------------------------------------
// A fork is bounded to its own sub-value: fully consuming it never reaches the
// next sibling, which the parent path can still read separately.
// ---------------------------------------------------------------------------
void test_no_cross_into_sibling() {
  // Object fork followed by a scalar sibling inside the same array.
  {
    auto json = R"([{"k":1}, 999, {"z":2}])"_padded;
    ondemand::parser parser;
    auto doc = parser.iterate(json);
    ondemand::array arr = doc.get_array().value_unsafe();

    auto it = arr.begin().value_unsafe();
    ondemand::value first = (*it).value_unsafe();
    ondemand::forked_value f = first.fork().value_unsafe();
    ondemand::value fv = f.get_value().value_unsafe();

    // The fork sees exactly its own one-field object, never the 999 sibling.
    std::size_t fields = 0;
    int64_t k = 0;
    ondemand::object fobj = fv.get_object().value_unsafe();
    for (auto field : fobj) {
      ++fields;
      if (std::string_view(field.unescaped_key().value_unsafe()) == "k") {
        k = int64_t(field.value());
      }
    }
    CHECK(fields == 1 && k == 1);

    // The parent iterator still owns the array and reads the real siblings.
    ++it;
    CHECK(int64_t((*it).value_unsafe()) == 999);
    ++it;
    CHECK(int64_t((*it).value_unsafe()["z"]) == 2);
  }

  // Array fork: the fork counts exactly its own elements and cannot spill into
  // the string that follows it.
  {
    auto json = R"([[1,2,3],"sibling"])"_padded;
    ondemand::parser parser;
    auto doc = parser.iterate(json);
    ondemand::array outer = doc.get_array().value_unsafe();

    auto it = outer.begin().value_unsafe();
    ondemand::forked_value f = (*it).value_unsafe().fork().value_unsafe();
    ondemand::value fv = f.get_value().value_unsafe();

    std::size_t elems = 0;
    int64_t sum = 0;
    ondemand::array farr = fv.get_array().value_unsafe();
    for (auto e : farr) { ++elems; sum += int64_t(e); }
    CHECK(elems == 3 && sum == 6);

    ++it;
    CHECK(std::string_view((*it).value_unsafe().get_string().value_unsafe()) ==
          std::string_view("sibling"));
  }
}

// ---------------------------------------------------------------------------
// Deterministic resource check built on REAL allocations. Two documents differ
// only in the length of an unrelated large array placed after a fixed small
// object. Both are fully iterated first; allocation bytes are counted only
// while building and reading the small-object fork. The measured cost must be
// identical and bounded by a constant that does not depend on the array length.
// ---------------------------------------------------------------------------
padded_string make_resource_input(std::size_t big_count) {
  std::string s = R"({"head":[{"obj":{"a":1,"b":"x"}}],"big":[)";
  for (std::size_t i = 0; i < big_count; ++i) {
    if (i) { s.push_back(','); }
    s += "1234567";
  }
  s += "]}";
  return padded_string(s);
}

long long measure_small_object_fork_bytes(padded_string &json) {
  ondemand::parser parser;
  auto doc = parser.iterate(json);
  // Locate the small object and confirm the document is fully usable first.
  ondemand::value small;
  CHECK(!doc["head"].at(0)["obj"].get(small));

  g_window_bytes = 0;
  g_measuring = true;
  // Only the fork build and its reads are inside the measured window.
  ondemand::forked_value f = small.fork().value_unsafe();
  ondemand::value fv = f.get_value().value_unsafe();
  int64_t a;
  std::string_view b;
  auto e1 = fv["a"].get_int64().get(a);
  auto e2 = fv["b"].get_string().get(b);
  g_measuring = false;

  CHECK(!e1 && !e2 && a == 1 && b == std::string_view("x"));
  return g_window_bytes;
}

void test_resource_cost_independent_of_unrelated_sibling() {
  constexpr std::size_t small_n = 500;
  constexpr std::size_t large_n = 50000; // 100x more unrelated data
  auto json_small = make_resource_input(small_n);
  auto json_large = make_resource_input(large_n);

  const long long bytes_small = measure_small_object_fork_bytes(json_small);
  const long long bytes_large = measure_small_object_fork_bytes(json_large);

  // Sanitizers intercept/replace the global allocator and hide the counter, so
  // the numeric assertions only apply to ordinary (non-sanitized) builds. The
  // functional fork correctness above is still exercised in every build.
#if defined(__SANITIZE_ADDRESS__) || (defined(__has_feature) && __has_feature(address_sanitizer))
  std::printf("(allocation counter suppressed under AddressSanitizer)\n");
  (void)bytes_small; (void)bytes_large;
#else
  // Real, positive allocations happened inside the window.
  CHECK(bytes_small > 0);
  // A 100x larger unrelated array must not change the fork's measured cost.
  CHECK(bytes_large == bytes_small);
  // And that cost is a small constant, nowhere near either document's size.
  constexpr long long fixed_cap = 8192;
  CHECK(bytes_small <= fixed_cap);
#endif
  CHECK(static_cast<std::size_t>(json_large.size()) >
        static_cast<std::size_t>(json_small.size()) * 50);
}

// ---------------------------------------------------------------------------
// Edge cases: forking the document root, the last element of a container, and
// helper operations (count, raw_json) performed entirely through a fork.
// ---------------------------------------------------------------------------
void test_root_last_element_and_helpers() {
  // Fork the whole document root object (depth 1).
  {
    auto json = R"({"a":1,"b":[2,3]})"_padded;
    ondemand::parser parser;
    auto doc = parser.iterate(json);
    ondemand::value root;
    CHECK(!doc.get_value().get(root));
    ondemand::forked_value f = root.fork().value_unsafe();
    ondemand::value fv = f.get_value().value_unsafe();
    CHECK(int64_t(fv["a"]) == 1);
    int64_t sum = 0;
    ondemand::array barr = fv["b"].get_array().value_unsafe();
    for (auto e : barr) { sum += int64_t(e); }
    CHECK(sum == 5);
  }
  // Fork the last element of an array (its end coincides with the document end).
  {
    auto json = R"([1,2,{"last":[4,5,6]}])"_padded;
    ondemand::parser parser;
    auto doc = parser.iterate(json);
    ondemand::array arr = doc.get_array().value_unsafe();
    auto it = arr.begin().value_unsafe();
    ++it; ++it;
    ondemand::value last = (*it).value_unsafe();
    ondemand::forked_value f = last.fork().value_unsafe();
    ondemand::value fv = f.get_value().value_unsafe();
    ondemand::array nested;
    CHECK(!fv["last"].get_array().get(nested));
    CHECK(nested.count_elements().value_unsafe() == 3);
    int64_t sum = 0;
    for (auto e : nested) { sum += int64_t(e); }
    CHECK(sum == 15);
  }
  // count_fields and raw_json through a fork, including an escaped value.
  {
    auto json = R"({"o":{"x":1,"y":"z","w":null},"tail":0})"_padded;
    ondemand::parser parser;
    auto doc = parser.iterate(json);
    ondemand::forked_value f = doc["o"].fork().value_unsafe();
    ondemand::value fv = f.get_value().value_unsafe();
    ondemand::object o = fv.get_object().value_unsafe();
    CHECK(o.count_fields().value_unsafe() == 3);
    std::string_view raw = o.raw_json().value_unsafe();
    CHECK(raw.front() == '{' && raw.back() == '}');
    CHECK(raw.find("\"x\":1") != std::string_view::npos);
    // The parent still reads the trailing field after the fork is consumed.
    CHECK(int64_t(doc["tail"]) == 0);
  }
}

// ---------------------------------------------------------------------------
// Escaped keys and \uXXXX Unicode escapes must decode through the fork's own
// buffer correctly, including when a sibling fork decodes in between.
// ---------------------------------------------------------------------------
void test_escaped_keys_and_unicode_escapes() {
  // \uXXXX escape and direct UTF-8, read through two interleaved forks.
  {
    auto json = R"({"u":"café","plain":"中文","after":1})"_padded;
    ondemand::parser parser;
    auto doc = parser.iterate(json);

    auto fa = doc["u"].fork();
    // Fork B is interleaved between A's decodes and must not clobber them.
    auto fb = doc["plain"].fork();

    ondemand::value va = fa.value_unsafe().get_value().value_unsafe();
    std::string_view u;
    CHECK(!va.get_string().get(u));
    CHECK(u == std::string_view("caf\xc3\xa9")); // café

    ondemand::value vb = fb.value_unsafe().get_value().value_unsafe();
    std::string_view plain;
    CHECK(!vb.get_string().get(plain));
    CHECK(plain == std::string_view("\xe4\xb8\xad\xe6\x96\x87")); // 中文

    // Re-read A after B: the first view stays valid (separate buffers).
    CHECK(u == std::string_view("caf\xc3\xa9"));
  }

  // A literal \uXXXX escape sequence (not pre-decoded source bytes).
  {
    auto json = R"({"s":"中文"})"_padded; // 中文
    ondemand::parser parser;
    auto doc = parser.iterate(json);
    ondemand::forked_value f = doc["s"].fork().value_unsafe();
    std::string_view s =
        f.get_value().value_unsafe().get_string().value_unsafe();
    CHECK(s == std::string_view("\xe4\xb8\xad\xe6\x96\x87"));
  }

  // Escaped object key decoded through a fork's private unescape buffer.
  {
    auto key_doc = R"({"outer":{"a\tb":{"deep":"x"}},"tail":2})"_padded;
    ondemand::parser p2;
    auto d2 = p2.iterate(key_doc);
    ondemand::forked_value f = d2["outer"].fork().value_unsafe();
    ondemand::value fv = f.get_value().value_unsafe();
    ondemand::object o = fv.get_object().value_unsafe();
    std::size_t fields = 0;
    std::string seen_key, seen_value;
    for (auto field : o) {
      ++fields;
      std::string_view key = field.unescaped_key().value_unsafe();
      seen_key.assign(key.data(), key.size());
      std::string_view val =
          field.value()["deep"].get_string().value_unsafe();
      seen_value.assign(val.data(), val.size());
    }
    CHECK(fields == 1);
    CHECK(seen_key == std::string_view("a\tb"));
    CHECK(seen_value == std::string_view("x"));
    CHECK(int64_t(d2["tail"]) == 2);
  }
}

// ---------------------------------------------------------------------------
// With SIMDJSON_DEVELOPMENT_CHECKS, forking an already-consumed value is
// rejected with OUT_OF_ORDER_ITERATION. (Without the checks this is simply an
// invalid call order and is not specially tracked.)
// ---------------------------------------------------------------------------
#if SIMDJSON_DEVELOPMENT_CHECKS
void test_consumed_value_rejected() {
  auto json = R"({"a":{"x":1},"b":2})"_padded;
  ondemand::parser parser;
  auto doc = parser.iterate(json);
  ondemand::value a;
  CHECK(!doc["a"].get(a));
  // Fully consume the object value by reading its only field.
  CHECK(int64_t(a["x"]) == 1);
  // It has moved off its start and cannot be forked anymore.
  CHECK_ERROR(a.fork(), OUT_OF_ORDER_ITERATION);
}
#endif

// ---------------------------------------------------------------------------
// End-to-end: drive the reusable collect_rows pipeline, write JSON Lines to a
// file, read the file back and compare every field to hand-written records.
// This exercises the real fork-based extraction, escape/Unicode handling,
// first-seen ordering, exact duplication, conflict reporting with line numbers
// and the no-half-record rule.
// ---------------------------------------------------------------------------
void test_collect_rows_pipeline_roundtrip() {
  namespace cr = collect_rows_example;
  auto json = cr::sample_response();

  const char *tmpdir = std::getenv("TMPDIR");
  const std::string path =
      std::string(tmpdir ? tmpdir : ".") + "/fork_collect_rows.jsonl";

  cr::PipelineCounters counters;
  CHECK(cr::run_to_file(json, path, counters) == 0);
  CHECK(counters.rows_seen == 7);
  CHECK(counters.rows_published == 3);
  CHECK(counters.duplicates_dropped == 1);
  CHECK(counters.conflicts == 1);
  CHECK(counters.rows_rejected == 2); // missing zh + wrong-typed amount

  // Read the produced file back and parse each JSON Line, reusing one parser.
  std::ifstream in(path, std::ios::binary);
  CHECK(static_cast<bool>(in));
  std::vector<std::string> lines;
  std::string raw_line;
  while (std::getline(in, raw_line)) {
    if (!raw_line.empty()) { lines.push_back(raw_line); }
  }
  std::remove(path.c_str());
  CHECK(lines.size() == 4);

  ondemand::parser reader;
  // Hand-written expectations, in exact first-seen output order.
  struct ExpectRecord {
    std::string id; double amount; std::string currency;
    std::string en; std::string zh;
  };
  const ExpectRecord records[] = {
    {"p1", 12.5, "EUR", "tea\ncup", "茶"},
    {"p2", 99.0, "USD", "caf\xc3\xa9", "咖啡"},
    {"p5", 1.0,  "USD", "a\\b\"c", "斜線"},
  };

  std::size_t record_index = 0;
  std::size_t conflict_index = 0;
  for (const std::string &line_text : lines) {
    padded_string padded(line_text);
    auto line_doc = reader.iterate(padded);
    CHECK(!line_doc.error());
    ondemand::object obj;
    CHECK(!line_doc.get_object().get(obj));

    // A record line has no "conflict" key; only conflict records carry it.
    auto conflict_r = obj["conflict"].get_bool();
    const bool is_conflict =
        conflict_r.error() == SUCCESS && conflict_r.value_unsafe();
    CHECK(conflict_r.error() == SUCCESS ||
          conflict_r.error() == NO_SUCH_FIELD);

    if (!is_conflict) {
      CHECK(record_index < 3);
      const ExpectRecord &e = records[record_index];
      std::string_view id, currency, en, zh;
      double amount;
      CHECK(!obj["id"].get_string().get(id));
      CHECK(!obj["price"]["amount"].get_double().get(amount));
      CHECK(!obj["price"]["currency"].get_string().get(currency));
      CHECK(!obj["desc"]["en"].get_string().get(en));
      CHECK(!obj["desc"]["zh"].get_string().get(zh));
      CHECK(id == e.id);
      CHECK(amount == e.amount);
      CHECK(currency == e.currency);
      CHECK(en == e.en);
      CHECK(zh == e.zh);
      ++record_index;
    } else {
      ++conflict_index;
      std::string_view id, field;
      uint64_t first_line, conflict_line;
      CHECK(!obj["id"].get_string().get(id));
      CHECK(!obj["field"].get_string().get(field));
      CHECK(!obj["first_line"].get_uint64().get(first_line));
      CHECK(!obj["conflict_line"].get_uint64().get(conflict_line));
      CHECK(id == std::string_view("p2"));
      CHECK(field == std::string_view("price.amount"));
      CHECK(first_line == 2);
      CHECK(conflict_line == 6);
    }
  }
  CHECK(record_index == 3);
  CHECK(conflict_index == 1);
}

} // namespace

int main() {
  test_scalars_and_empty_containers();
  test_three_way_interleave();
  test_nested_fork_outer_destroyed_first();
  test_old_view_survives_owner_move();
  test_error_isolation();
  test_no_cross_into_sibling();
  test_resource_cost_independent_of_unrelated_sibling();
  test_root_last_element_and_helpers();
  test_escaped_keys_and_unicode_escapes();
  test_collect_rows_pipeline_roundtrip();
#if SIMDJSON_DEVELOPMENT_CHECKS
  test_consumed_value_rejected();
#endif
  if (g_failures != 0) {
    std::printf("FORK_CONTRACT_FAILURES %d (of %d checks)\n", g_failures, g_checks);
    return 1;
  }
  std::printf("FORK_CONTRACT_OK checks=%d\n", g_checks);
  return 0;
}
