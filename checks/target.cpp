// Target tests for the optional streaming string value interface
// (yajl_string_stream_callbacks / yajl_set_string_stream).
//
// All expected byte strings below are written out independently of the
// parser under test; nothing is derived from a second yajl call.

#include "cases.hpp"

#include <yajl/yajl_parse.h>

#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

/* ------------------------------------------------------------------ */
/* event recorder                                                      */

struct Rec {
    std::vector<std::string> events;      // compact event stream
    std::vector<std::string> fragments;   // individual fragments
    std::vector<std::string> strings;     // one entry per completed string
    std::string current;                  // fragments of the open string
    std::string streamed;                 // concatenation of all fragments
    std::string oldStringBytes;           // bytes delivered via yajl_string
    std::vector<std::string> keys;        // map keys
    long long lastInteger = 0;
    double lastDouble = 0;
    int begins = 0, ends = 0, stringCalls = 0;
    int phase = 0;                        // 0: outside, 1: inside a string
    bool emptyFragment = false;
    bool badOrder = false;
    // cancellation controls
    bool cancelBegin = false;
    int cancelFragment = -1;              // fragment index to cancel on
    bool cancelEnd = false;
    int fragCount = 0;
};

int rec_begin(void* ctx) {
    Rec& r = *static_cast<Rec*>(ctx);
    if (r.phase != 0) r.badOrder = true;
    r.phase = 1;
    r.current.clear();
    r.begins++;
    r.events.push_back("B");
    return r.cancelBegin ? 0 : 1;
}

int rec_fragment(void* ctx, const unsigned char* s, size_t n) {
    Rec& r = *static_cast<Rec*>(ctx);
    if (r.phase != 1) r.badOrder = true;
    if (n == 0) r.emptyFragment = true;
    r.fragments.emplace_back(reinterpret_cast<const char*>(s), n);
    r.current.append(reinterpret_cast<const char*>(s), n);
    r.streamed.append(reinterpret_cast<const char*>(s), n);
    r.events.push_back("f");
    const int idx = r.fragCount++;
    return (r.cancelFragment == idx) ? 0 : 1;
}

int rec_end(void* ctx) {
    Rec& r = *static_cast<Rec*>(ctx);
    if (r.phase != 1) r.badOrder = true;
    r.phase = 0;
    r.strings.push_back(r.current);
    r.ends++;
    r.events.push_back("E");
    return r.cancelEnd ? 0 : 1;
}

int rec_string(void* ctx, const unsigned char* s, size_t n) {
    Rec& r = *static_cast<Rec*>(ctx);
    r.stringCalls++;
    r.oldStringBytes.append(reinterpret_cast<const char*>(s), n);
    r.events.push_back("S");
    return 1;
}

int rec_map_key(void* ctx, const unsigned char* s, size_t n) {
    Rec& r = *static_cast<Rec*>(ctx);
    r.keys.emplace_back(reinterpret_cast<const char*>(s), n);
    r.events.push_back("K");
    return 1;
}

int rec_start_map(void* ctx) { static_cast<Rec*>(ctx)->events.push_back("{"); return 1; }
int rec_end_map(void* ctx) { static_cast<Rec*>(ctx)->events.push_back("}"); return 1; }
int rec_start_array(void* ctx) { static_cast<Rec*>(ctx)->events.push_back("["); return 1; }
int rec_end_array(void* ctx) { static_cast<Rec*>(ctx)->events.push_back("]"); return 1; }
int rec_null(void* ctx) { static_cast<Rec*>(ctx)->events.push_back("n"); return 1; }

int rec_bool(void* ctx, int v) {
    static_cast<Rec*>(ctx)->events.push_back(v ? "T" : "F");
    return 1;
}

int rec_integer(void* ctx, long long v) {
    Rec& r = *static_cast<Rec*>(ctx);
    r.lastInteger = v;
    r.events.push_back("i");
    return 1;
}

int rec_double(void* ctx, double v) {
    Rec& r = *static_cast<Rec*>(ctx);
    r.lastDouble = v;
    r.events.push_back("d");
    return 1;
}

yajl_callbacks old_callbacks() {
    yajl_callbacks c;
    std::memset(&c, 0, sizeof(c));
    c.yajl_null = rec_null;
    c.yajl_boolean = rec_bool;
    c.yajl_integer = rec_integer;
    c.yajl_double = rec_double;
    c.yajl_string = rec_string;
    c.yajl_start_map = rec_start_map;
    c.yajl_map_key = rec_map_key;
    c.yajl_end_map = rec_end_map;
    c.yajl_start_array = rec_start_array;
    c.yajl_end_array = rec_end_array;
    return c;
}

yajl_string_stream_callbacks stream_callbacks() {
    yajl_string_stream_callbacks c;
    c.begin = rec_begin;
    c.fragment = rec_fragment;
    c.end = rec_end;
    return c;
}

/* a handle with both classic and streaming callbacks installed */
struct Parser {
    Rec rec;
    yajl_callbacks cbs;
    yajl_handle h;

    explicit Parser(bool streaming) : cbs(old_callbacks()), h(nullptr) {
        h = yajl_alloc(&cbs, nullptr, &rec);
        if (streaming) {
            yajl_string_stream_callbacks sc = stream_callbacks();
            yajl_set_string_stream(h, &sc);
        }
    }
    ~Parser() { if (h) yajl_free(h); }
};

/* feed data in chunks of at most `chunk` bytes, stop at first non-ok */
yajl_status feed(yajl_handle h, const char* data, size_t len, size_t chunk) {
    size_t pos = 0;
    yajl_status st = yajl_status_ok;
    while (pos < len) {
        const size_t n = std::min(chunk, len - pos);
        st = yajl_parse(h, reinterpret_cast<const unsigned char*>(data + pos), n);
        if (st != yajl_status_ok) return st;
        pos += n;
    }
    return st;
}

yajl_status feed(yajl_handle h, const std::string& doc, size_t chunk) {
    return feed(h, doc.data(), doc.size(), chunk);
}

/* strict UTF-8 validation, used to prove fragments end on scalar
 * boundaries: a fragment split inside a character would be invalid */
bool valid_utf8(const std::string& s) {
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) { i++; continue; }
        unsigned int need;
        unsigned char lo = 0x80, hi = 0xBF;
        if (c >= 0xC2 && c <= 0xDF) need = 1;
        else if (c == 0xE0) { need = 2; lo = 0xA0; }
        else if (c == 0xED) { need = 2; hi = 0x9F; }
        else if (c >= 0xE1 && c <= 0xEF) need = 2;
        else if (c == 0xF0) { need = 3; lo = 0x90; }
        else if (c >= 0xF1 && c <= 0xF3) need = 3;
        else if (c == 0xF4) { need = 3; hi = 0x8F; }
        else return false;
        if (i + need >= s.size()) return false;
        const unsigned char c2 = static_cast<unsigned char>(s[i + 1]);
        if (c2 < lo || c2 > hi) return false;
        for (unsigned int k = 2; k <= need; k++) {
            const unsigned char ck = static_cast<unsigned char>(s[i + k]);
            if (ck < 0x80 || ck > 0xBF) return false;
        }
        i += need + 1;
    }
    return true;
}

/* parse doc with streaming enabled and verify the complete event
 * shape of a single streamed string value inside an array */
void check_streamed_value(const std::string& doc, const std::string& expected,
                          const std::vector<std::pair<size_t, size_t>>& slices) {
    Parser p(true);
    CHECK(p.h != nullptr);
    for (const auto& s : slices) {
        CHECK(yajl_parse(p.h, reinterpret_cast<const unsigned char*>(doc.data()) + s.first,
                         s.second) == yajl_status_ok);
    }
    CHECK(yajl_complete_parse(p.h) == yajl_status_ok);
    // event shape: [ B f... E ]
    CHECK(p.rec.events.size() >= 4);
    CHECK(p.rec.events.front() == "[");
    CHECK(p.rec.events[1] == "B");
    CHECK(p.rec.events.back() == "]");
    CHECK(p.rec.events[p.rec.events.size() - 2] == "E");
    for (size_t i = 2; i + 2 < p.rec.events.size(); i++) {
        CHECK(p.rec.events[i] == "f");
    }
    CHECK(p.rec.begins == 1);
    CHECK(p.rec.ends == 1);
    CHECK(!p.rec.badOrder);
    CHECK(!p.rec.emptyFragment);
    CHECK(p.rec.stringCalls == 0);        // yajl_string must not fire
    CHECK(p.rec.streamed == expected);    // independent expected content
    CHECK(p.rec.strings.size() == 1);
    CHECK(p.rec.strings[0] == expected);
    for (const auto& f : p.rec.fragments) CHECK(valid_utf8(f));
}

/* ------------------------------------------------------------------ */
/* allocation tracking for the bounded storage test                    */

struct Track {
    size_t current = 0;
    size_t peak = 0;
};

void* track_malloc(void* ctx, size_t sz) {
    Track* t = static_cast<Track*>(ctx);
    void* p = std::malloc(sz + sizeof(size_t));
    *static_cast<size_t*>(p) = sz;
    t->current += sz;
    if (t->current > t->peak) t->peak = t->current;
    return static_cast<char*>(p) + sizeof(size_t);
}

void track_free(void* ctx, void* ptr) {
    if (!ptr) return;
    Track* t = static_cast<Track*>(ctx);
    void* hdr = static_cast<char*>(ptr) - sizeof(size_t);
    t->current -= *static_cast<size_t*>(hdr);
    std::free(hdr);
}

void* track_realloc(void* ctx, void* ptr, size_t sz) {
    if (!ptr) return track_malloc(ctx, sz);
    Track* t = static_cast<Track*>(ctx);
    void* hdr = static_cast<char*>(ptr) - sizeof(size_t);
    t->current -= *static_cast<size_t*>(hdr);
    void* n = std::realloc(hdr, sz + sizeof(size_t));
    *static_cast<size_t*>(n) = sz;
    t->current += sz;
    if (t->current > t->peak) t->peak = t->current;
    return static_cast<char*>(n) + sizeof(size_t);
}

/* counting-only callbacks for the bounded storage test: nothing is
 * retained, the fragment bytes are only counted and summed */
struct Big {
    size_t begins = 0, ends = 0, fragments = 0, stringCalls = 0;
    size_t bytes = 0;
    unsigned long long sum = 0;
};

int big_begin(void* ctx) { static_cast<Big*>(ctx)->begins++; return 1; }
int big_end(void* ctx) { static_cast<Big*>(ctx)->ends++; return 1; }
int big_string(void* ctx, const unsigned char*, size_t) {
    static_cast<Big*>(ctx)->stringCalls++;
    return 1;
}
int big_fragment(void* ctx, const unsigned char* s, size_t n) {
    Big* b = static_cast<Big*>(ctx);
    b->fragments++;
    b->bytes += n;
    for (size_t i = 0; i < n; i++) b->sum += s[i];
    return 1;
}

} // namespace

/* ------------------------------------------------------------------ */

CASE(decoded_fragments) {
    // a string value exercising plain ASCII, raw 2/3/4-byte UTF-8,
    // simple escapes, \uXXXX escapes and a surrogate pair
    const std::string value =
        "\"AB"
        "\xC3\xA9"              // é, raw 2-byte UTF-8
        "\xE4\xB8\xAD"          // 中, raw 3-byte UTF-8
        "\xF0\x9D\x84\x9E"      // 𝄞, raw 4-byte UTF-8
        "\\n\\\\"               // escapes: newline, backslash
        "\\u0041"               // 'A'
        "\\u00E9"               // é via escape
        "\\uD834\\uDD1E"        // 𝄞 via surrogate pair
        "\"";
    // the decoded content, written out independently
    const std::string expected =
        "AB"
        "\xC3\xA9\xE4\xB8\xAD\xF0\x9D\x84\x9E"
        "\n\\"
        "A"
        "\xC3\xA9"
        "\xF0\x9D\x84\x9E";
    const std::string doc = "[" + value + "]";

    // whole buffer at once
    check_streamed_value(doc, expected, {{0, doc.size()}});

    // fixed chunk sizes
    for (size_t chunk : std::vector<size_t>{1, 2, 3, 5, 7, 16}) {
        std::vector<std::pair<size_t, size_t>> slices;
        for (size_t pos = 0; pos < doc.size(); pos += chunk) {
            slices.push_back({pos, std::min(chunk, doc.size() - pos)});
        }
        check_streamed_value(doc, expected, slices);
    }

    // every single cut point
    for (size_t cut = 0; cut <= doc.size(); cut++) {
        check_streamed_value(doc, expected, {{0, cut}, {cut, doc.size() - cut}});
    }

    // empty string: begin and end, no fragment
    check_streamed_value("[\"\"]", "", {{0, 4}});
    check_streamed_value("[\"\"]", "", {{0, 1}, {1, 1}, {2, 1}, {3, 1}});

    // NUL escape: a 1-byte fragment containing 0x00, not an empty one
    check_streamed_value("[\"\\u0000\"]", std::string("\0", 1), {{0, 10}});
    check_streamed_value("[\"\\u0000\"]", std::string("\0", 1),
                         {{0, 1}, {1, 1}, {2, 1}, {3, 1}, {4, 1}, {5, 1},
                          {6, 1}, {7, 1}, {8, 1}, {9, 1}});

    // whitespace padding around the value, split at every point
    {
        const std::string padded = std::string("[  \t\n") + value + "  ]";
        check_streamed_value(padded, expected, {{0, padded.size()}});
        for (size_t cut = 0; cut <= padded.size(); cut++) {
            check_streamed_value(padded, expected,
                                 {{0, cut}, {cut, padded.size() - cut}});
        }
    }

    // a string made only of escapes, byte by byte
    check_streamed_value("[\"\\u0041\\n\\u00E9\"]", "A\n\xC3\xA9",
                         {{0, 1}, {1, 1}, {2, 1}, {3, 1}, {4, 1}, {5, 1},
                          {6, 1}, {7, 1}, {8, 1}, {9, 1}, {10, 1}, {11, 1},
                          {12, 1}, {13, 1}, {14, 1}, {15, 1}, {16, 1},
                          {17, 1}});
}

CASE(parser_roles) {
    // object: key goes whole through yajl_map_key, value is streamed
    {
        Parser p(true);
        const std::string doc = "{\"key\":\"value\"}";
        CHECK(feed(p.h, doc, doc.size()) == yajl_status_ok);
        CHECK(yajl_complete_parse(p.h) == yajl_status_ok);
        CHECK(p.rec.events == std::vector<std::string>({"{", "K", "B", "f", "E", "}"}));
        CHECK(p.rec.keys == std::vector<std::string>({"key"}));
        CHECK(p.rec.strings == std::vector<std::string>({"value"}));
        CHECK(p.rec.stringCalls == 0);
    }

    // keys with escapes are decoded by the classic path, and a key with
    // colons, escaped quotes and a long shared prefix must not start a
    // value stream
    {
        Parser p(true);
        const std::string prefix(300, 'p');
        const std::string doc =
            "{\"k\\u0065y\":\"v\",\"pre:\\\"" + prefix + "\":\"" + prefix + "tail\"}";
        CHECK(feed(p.h, doc, 1) == yajl_status_ok);
        CHECK(yajl_complete_parse(p.h) == yajl_status_ok);
        CHECK(p.rec.keys == std::vector<std::string>({"key", "pre:\"" + prefix}));
        CHECK(p.rec.strings == std::vector<std::string>({"v", prefix + "tail"}));
        CHECK(p.rec.begins == 2);
        CHECK(p.rec.ends == 2);
        CHECK(p.rec.stringCalls == 0);
        CHECK(!p.rec.badOrder);
    }

    // nested arrays, empty array, empty string
    {
        Parser p(true);
        const std::string doc = "[\"a\",[\"bb\"],\"ccc\",[],\"\"]";
        CHECK(feed(p.h, doc, doc.size()) == yajl_status_ok);
        CHECK(yajl_complete_parse(p.h) == yajl_status_ok);
        CHECK(p.rec.events == std::vector<std::string>(
                  {"[", "B", "f", "E", "[", "B", "f", "E", "]",
                   "B", "f", "E", "[", "]", "B", "E", "]"}));
        CHECK(p.rec.strings == std::vector<std::string>({"a", "bb", "ccc", ""}));
    }

    // top level string value
    {
        Parser p(true);
        const std::string doc = "\"hello\"";
        CHECK(feed(p.h, doc, doc.size()) == yajl_status_ok);
        CHECK(yajl_complete_parse(p.h) == yajl_status_ok);
        CHECK(p.rec.events == std::vector<std::string>({"B", "f", "E"}));
        CHECK(p.rec.strings == std::vector<std::string>({"hello"}));
    }

    // adjacent documents under allow_multiple_values
    {
        Parser p(true);
        CHECK(yajl_config(p.h, yajl_allow_multiple_values, 1) == 1);
        const std::string doc = "\"a\" \"bb\" 1 \"ccc\"";
        CHECK(feed(p.h, doc, doc.size()) == yajl_status_ok);
        CHECK(yajl_complete_parse(p.h) == yajl_status_ok);
        CHECK(p.rec.events == std::vector<std::string>(
                  {"B", "f", "E", "B", "f", "E", "i", "B", "f", "E"}));
        CHECK(p.rec.strings == std::vector<std::string>({"a", "bb", "ccc"}));
        CHECK(p.rec.lastInteger == 1);
    }

    // numbers, booleans and null keep their classic behavior
    {
        Parser p(true);
        const std::string doc = "[1,2.5,true,false,null,\"s\"]";
        CHECK(feed(p.h, doc, 4) == yajl_status_ok);
        CHECK(yajl_complete_parse(p.h) == yajl_status_ok);
        CHECK(p.rec.events == std::vector<std::string>(
                  {"[", "i", "d", "T", "F", "n", "B", "f", "E", "]"}));
        CHECK(p.rec.lastInteger == 1);
        CHECK(p.rec.lastDouble == 2.5);
        CHECK(p.rec.strings == std::vector<std::string>({"s"}));
    }

    // dont_validate_strings: keys and the classic path keep the old
    // lenient meaning, streamed values are validated anyway
    {
        Parser p(true);
        CHECK(yajl_config(p.h, yajl_dont_validate_strings, 1) == 1);
        const std::string doc = "{\"\xFF\":\"ok\"}";
        CHECK(feed(p.h, doc, doc.size()) == yajl_status_ok);
        CHECK(yajl_complete_parse(p.h) == yajl_status_ok);
        CHECK(p.rec.keys == std::vector<std::string>({"\xFF"}));
        CHECK(p.rec.strings == std::vector<std::string>({"ok"}));
    }
    {
        Parser p(true);
        CHECK(yajl_config(p.h, yajl_dont_validate_strings, 1) == 1);
        const std::string doc = "{\"k\":\"\xFF\"}";
        CHECK(feed(p.h, doc, doc.size()) == yajl_status_error);
        CHECK(p.rec.ends == 0);
    }
    {
        Parser p(false);  // classic delivery
        CHECK(yajl_config(p.h, yajl_dont_validate_strings, 1) == 1);
        const std::string doc = "[\"\xFF\"]";
        CHECK(feed(p.h, doc, doc.size()) == yajl_status_ok);
        CHECK(yajl_complete_parse(p.h) == yajl_status_ok);
        CHECK(p.rec.oldStringBytes == "\xFF");
        CHECK(p.rec.stringCalls == 1);
    }

    // classic mode is the default: whole string, one callback
    {
        Parser p(false);
        const std::string doc = "[\"abc\"]";
        CHECK(feed(p.h, doc, 1) == yajl_status_ok);
        CHECK(yajl_complete_parse(p.h) == yajl_status_ok);
        CHECK(p.rec.stringCalls == 1);
        CHECK(p.rec.oldStringBytes == "abc");
        CHECK(p.rec.begins == 0 && p.rec.ends == 0);
    }

    // streaming can be switched off again before parsing starts
    {
        Parser p(true);
        CHECK(yajl_set_string_stream(p.h, nullptr) == 1);
        const std::string doc = "[\"abc\"]";
        CHECK(feed(p.h, doc, 2) == yajl_status_ok);
        CHECK(yajl_complete_parse(p.h) == yajl_status_ok);
        CHECK(p.rec.stringCalls == 1);
        CHECK(p.rec.oldStringBytes == "abc");
        CHECK(p.rec.begins == 0);
    }

    // once parsing started, configuration is refused and unchanged
    {
        Parser p(false);
        CHECK(yajl_parse(p.h, reinterpret_cast<const unsigned char*>("["), 1) == yajl_status_ok);
        yajl_string_stream_callbacks sc = stream_callbacks();
        CHECK(yajl_set_string_stream(p.h, &sc) == 0);
        const std::string rest = "\"abc\"]";
        CHECK(feed(p.h, rest, rest.size()) == yajl_status_ok);
        CHECK(yajl_complete_parse(p.h) == yajl_status_ok);
        CHECK(p.rec.stringCalls == 1);   // classic delivery still in effect
        CHECK(p.rec.oldStringBytes == "abc");
        CHECK(p.rec.begins == 0);
    }

    // an incomplete table is refused and changes nothing
    {
        Parser p(false);
        yajl_string_stream_callbacks sc;
        sc.begin = nullptr;
        sc.fragment = rec_fragment;
        sc.end = rec_end;
        CHECK(yajl_set_string_stream(p.h, &sc) == 0);
        const std::string doc = "[\"ab\"]";
        CHECK(feed(p.h, doc, doc.size()) == yajl_status_ok);
        CHECK(p.rec.stringCalls == 1);
        CHECK(p.rec.begins == 0);
    }

    // the handle keeps its own copy of the callback table
    {
        Rec r;
        yajl_callbacks cbs = old_callbacks();
        yajl_handle h = yajl_alloc(&cbs, nullptr, &r);
        {
            yajl_string_stream_callbacks sc = stream_callbacks();
            CHECK(yajl_set_string_stream(h, &sc) == 1);
            std::memset(&sc, 0, sizeof(sc));  // destroy the local copy
        }
        const std::string doc = "[\"copied\"]";
        CHECK(feed(h, doc, doc.size()) == yajl_status_ok);
        CHECK(yajl_complete_parse(h) == yajl_status_ok);
        CHECK(r.strings == std::vector<std::string>({"copied"}));
        CHECK(r.stringCalls == 0);
        yajl_free(h);
    }
}

CASE(error_completion) {
    // raw control byte after a valid prefix: prefix may be delivered,
    // but no end and no container close may follow
    {
        Parser p(true);
        CHECK(yajl_parse(p.h, reinterpret_cast<const unsigned char*>("[\"ab"), 4) == yajl_status_ok);
        CHECK(p.rec.streamed == "ab");   // legit prefix already delivered
        const char bad[] = {'c', 'd', 0x01, '"', ']'};
        CHECK(yajl_parse(p.h, reinterpret_cast<const unsigned char*>(bad), 5) == yajl_status_error);
        CHECK(p.rec.ends == 0);
        CHECK(std::find(p.rec.events.begin(), p.rec.events.end(), "]") == p.rec.events.end());
        // the error offset points into the most recent chunk, not into
        // the accumulated field
        CHECK(yajl_get_bytes_consumed(p.h) == 2);
        // the handle stays in error and stays silent
        const size_t nevents = p.rec.events.size();
        CHECK(yajl_parse(p.h, reinterpret_cast<const unsigned char*>("]"), 1) == yajl_status_error);
        CHECK(yajl_complete_parse(p.h) == yajl_status_error);
        CHECK(p.rec.events.size() == nevents);
        unsigned char* err = yajl_get_error(p.h, 0, reinterpret_cast<const unsigned char*>(bad), 5);
        CHECK(err != nullptr && std::strstr(reinterpret_cast<char*>(err),
                                            "invalid character inside string") != nullptr);
        yajl_free_error(p.h, err);
    }

    // invalid escape character
    {
        Parser p(true);
        const std::string doc = "[\"a\\q\"]";
        CHECK(feed(p.h, doc, doc.size()) == yajl_status_error);
        CHECK(p.rec.ends == 0);
        CHECK(yajl_get_bytes_consumed(p.h) == 4);
        unsigned char* err = yajl_get_error(p.h, 0, reinterpret_cast<const unsigned char*>(doc.data()), doc.size());
        CHECK(err != nullptr && std::strstr(reinterpret_cast<char*>(err),
                                            "occurs before a character") != nullptr);
        yajl_free_error(p.h, err);
    }

    // invalid hex digit after \u
    {
        Parser p(true);
        const std::string doc = "[\"a\\u12x4\"]";
        CHECK(feed(p.h, doc, doc.size()) == yajl_status_error);
        CHECK(p.rec.ends == 0);
        CHECK(yajl_get_bytes_consumed(p.h) == 7);
        unsigned char* err = yajl_get_error(p.h, 0, reinterpret_cast<const unsigned char*>(doc.data()), doc.size());
        CHECK(err != nullptr && std::strstr(reinterpret_cast<char*>(err), "non-hex") != nullptr);
        yajl_free_error(p.h, err);
    }

    // isolated surrogates are errors, a proper pair across chunks works
    {
        const std::string docs[] = {
            "[\"\\uD800\"]",          // lone high surrogate
            "[\"\\uDC00\"]",          // lone low surrogate
            "[\"\\uD800\\u0041\"]",   // high surrogate, then BMP escape
            "[\"\\uD800a\"]",         // high surrogate, then raw char
            "[\"\\uD800\\n\"]"        // high surrogate, then simple escape
        };
        for (const std::string& doc : docs) {
            Parser p(true);
            CHECK(feed(p.h, doc, doc.size()) == yajl_status_error);
            CHECK(p.rec.ends == 0);
            unsigned char* err = yajl_get_error(p.h, 0, reinterpret_cast<const unsigned char*>(doc.data()), doc.size());
            CHECK(err != nullptr && std::strstr(reinterpret_cast<char*>(err), "surrogate") != nullptr);
            yajl_free_error(p.h, err);
        }
    }
    {
        // high surrogate at the end of a chunk, quote in the next
        Parser p(true);
        CHECK(yajl_parse(p.h, reinterpret_cast<const unsigned char*>("[\"\\uD800"), 8) == yajl_status_ok);
        CHECK(yajl_parse(p.h, reinterpret_cast<const unsigned char*>("\"]"), 2) == yajl_status_error);
        CHECK(p.rec.ends == 0);
    }
    {
        // valid surrogate pair split across chunks
        Parser p(true);
        CHECK(yajl_parse(p.h, reinterpret_cast<const unsigned char*>("[\"\\uD834"), 8) == yajl_status_ok);
        CHECK(yajl_parse(p.h, reinterpret_cast<const unsigned char*>("\\uDD1E\"]"), 8) == yajl_status_ok);
        CHECK(yajl_complete_parse(p.h) == yajl_status_ok);
        CHECK(p.rec.strings == std::vector<std::string>({"\xF0\x9D\x84\x9E"}));
    }

    // invalid UTF-8 in a streamed value is always an error
    {
        const std::string docs[] = {
            "[\"\xC0\x80\"]",        // overlong
            "[\"\xED\xA0\x80\"]",    // UTF-8 encoded surrogate
            "[\"\xF4\x90\x80\x80\"]",// beyond U+10FFFF
            "[\"\x80\"]",            // stray continuation byte
            "[\"\xE4\xB8\"]"         // truncated sequence, then quote
        };
        for (const std::string& doc : docs) {
            Parser p(true);
            CHECK(feed(p.h, doc, doc.size()) == yajl_status_error);
            CHECK(p.rec.ends == 0);
            unsigned char* err = yajl_get_error(p.h, 0, reinterpret_cast<const unsigned char*>(doc.data()), doc.size());
            CHECK(err != nullptr && std::strstr(reinterpret_cast<char*>(err), "UTF8") != nullptr);
            yajl_free_error(p.h, err);
        }
    }
    {
        // a multi-byte character split across chunks is fine
        Parser p(true);
        CHECK(yajl_parse(p.h, reinterpret_cast<const unsigned char*>("[\"\xE4"), 3) == yajl_status_ok);
        CHECK(yajl_parse(p.h, reinterpret_cast<const unsigned char*>("\xB8\xAD\"]"), 4) == yajl_status_ok);
        CHECK(yajl_complete_parse(p.h) == yajl_status_ok);
        CHECK(p.rec.strings == std::vector<std::string>({"\xE4\xB8\xAD"}));
    }
    {
        // dont_validate_strings does not relax streamed values
        Parser p(true);
        CHECK(yajl_config(p.h, yajl_dont_validate_strings, 1) == 1);
        const std::string doc = "[\"\xFF\"]";
        CHECK(feed(p.h, doc, doc.size()) == yajl_status_error);
        CHECK(p.rec.ends == 0);
    }

    // unclosed string at complete_parse: delivered prefix is fine, but
    // no end and no container close, and the error sticks
    {
        Parser p(true);
        CHECK(yajl_parse(p.h, reinterpret_cast<const unsigned char*>("[\"abc"), 5) == yajl_status_ok);
        CHECK(p.rec.streamed == "abc");
        CHECK(yajl_complete_parse(p.h) == yajl_status_error);
        CHECK(p.rec.ends == 0);
        CHECK(std::find(p.rec.events.begin(), p.rec.events.end(), "]") == p.rec.events.end());
        unsigned char* err = yajl_get_error(p.h, 0, reinterpret_cast<const unsigned char*>(""), 0);
        CHECK(err != nullptr && std::strstr(reinterpret_cast<char*>(err), "premature EOF") != nullptr);
        yajl_free_error(p.h, err);
        const size_t nevents = p.rec.events.size();
        CHECK(yajl_parse(p.h, reinterpret_cast<const unsigned char*>("\"]"), 2) == yajl_status_error);
        CHECK(p.rec.events.size() == nevents);
    }

    // garbage after a completed streamed string: the string's end was
    // delivered, but no container close follows the error
    {
        Parser p(true);
        const std::string doc = "[\"abc\" x]";
        CHECK(feed(p.h, doc, doc.size()) == yajl_status_error);
        CHECK(p.rec.ends == 1);
        CHECK(std::find(p.rec.events.begin(), p.rec.events.end(), "]") == p.rec.events.end());
    }

    // client cancelation: nothing further is delivered, no end is faked
    {
        Parser p(true);
        p.rec.cancelBegin = true;
        const std::string doc = "[\"abc\"]";
        CHECK(feed(p.h, doc, doc.size()) == yajl_status_client_canceled);
        CHECK(p.rec.events == std::vector<std::string>({"[", "B"}));
        CHECK(yajl_parse(p.h, reinterpret_cast<const unsigned char*>(" "), 1) == yajl_status_error);
        CHECK(p.rec.events == std::vector<std::string>({"[", "B"}));
    }
    {
        Parser p(true);
        p.rec.cancelFragment = 0;
        const std::string doc = "[\"abc\"]";
        CHECK(feed(p.h, doc, doc.size()) == yajl_status_client_canceled);
        CHECK(p.rec.events == std::vector<std::string>({"[", "B", "f"}));
        CHECK(p.rec.ends == 0);   // no end after cancelation
        CHECK(yajl_parse(p.h, reinterpret_cast<const unsigned char*>(" "), 1) == yajl_status_error);
        CHECK(p.rec.events == std::vector<std::string>({"[", "B", "f"}));
    }
    {
        Parser p(true);
        p.rec.cancelEnd = true;
        const std::string doc = "[\"abc\"]";
        CHECK(feed(p.h, doc, doc.size()) == yajl_status_client_canceled);
        CHECK(p.rec.events == std::vector<std::string>({"[", "B", "f", "E"}));
        CHECK(yajl_parse(p.h, reinterpret_cast<const unsigned char*>(" "), 1) == yajl_status_error);
        CHECK(p.rec.events == std::vector<std::string>({"[", "B", "f", "E"}));
    }
}

CASE(bounded_storage) {
    Track t;
    yajl_alloc_funcs afs;
    afs.malloc = track_malloc;
    afs.realloc = track_realloc;
    afs.free = track_free;
    afs.ctx = &t;

    // build a string value of at least 8MiB mixing long plain runs with
    // real \uXXXX escapes (2 decoded bytes) and surrogate pairs (4
    // decoded bytes), so an escape-only fast path cannot pass
    std::string doc = "[\"";
    size_t xCount = 0, e9Count = 0, spCount = 0;
    size_t decoded = 0;
    const size_t target = 8 * 1024 * 1024;
    while (decoded < target) {
        doc.append(7000, 'x');
        xCount += 7000;
        decoded += 7000;
        if ((xCount / 7000) % 5 == 0) {
            doc += "\\uD834\\uDD1E";
            spCount++;
            decoded += 4;
        } else {
            doc += "\\u00E9";
            e9Count++;
            decoded += 2;
        }
    }
    doc += "\"]";
    const size_t expectedLen = decoded;
    const unsigned long long expectedSum =
        static_cast<unsigned long long>(xCount) * 0x78 +
        static_cast<unsigned long long>(e9Count) * (0xC3 + 0xA9) +
        static_cast<unsigned long long>(spCount) * (0xF0 + 0x9D + 0x84 + 0x9E);

    Big big;
    yajl_callbacks cbs;
    std::memset(&cbs, 0, sizeof(cbs));
    cbs.yajl_string = big_string;   // must never fire
    yajl_handle h = yajl_alloc(&cbs, &afs, &big);
    CHECK(h != nullptr);
    yajl_string_stream_callbacks sc;
    sc.begin = big_begin;
    sc.fragment = big_fragment;
    sc.end = big_end;
    CHECK(yajl_set_string_stream(h, &sc) == 1);

    // feed everything but the final chunk (which holds the closing
    // quote) in 64KiB pieces
    const size_t chunk = 64 * 1024;
    const size_t lastStart = doc.size() - chunk;
    size_t pos = 0;
    while (pos < lastStart) {
        const size_t n = std::min(chunk, lastStart - pos);
        CHECK(yajl_parse(h, reinterpret_cast<const unsigned char*>(doc.data()) + pos, n) == yajl_status_ok);
        pos += n;
    }
    // fragments must have been delivered before the closing quote
    // arrives, and the value must not be finished yet
    CHECK(big.begins == 1);
    CHECK(big.fragments > 0);
    CHECK(big.ends == 0);

    CHECK(yajl_parse(h, reinterpret_cast<const unsigned char*>(doc.data()) + pos,
                     doc.size() - pos) == yajl_status_ok);
    CHECK(yajl_complete_parse(h) == yajl_status_ok);

    CHECK(big.begins == 1);
    CHECK(big.ends == 1);
    CHECK(big.stringCalls == 0);
    CHECK(big.bytes == expectedLen);
    CHECK(big.sum == expectedSum);

    // the handle's own unreleased allocations stayed bounded
    CHECK(t.peak < 256 * 1024);

    yajl_free(h);
    CHECK(t.current == 0);
}
