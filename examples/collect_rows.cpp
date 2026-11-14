// collect_rows.cpp
//
// A real collection pipeline built on ondemand::value::fork().
//
// A local JSON response contains a "rows" array. Every row has a product id, a
// nested "price" object and a multilingual "desc" object. Two independent
// extraction modules consume each row through their own fork:
//
//   - the price module reads price.amount / price.currency,
//   - the description module reads desc.en / desc.zh.
//
// The parent path only reads the id and immediately advances to the next row
// while both forks are still live (three-way interleaving). The modules own
// independent cursors and decode buffers, so an unescaped string produced by one
// is not overwritten by the other or by the parent's continued iteration.
//
// Rows are merged by product id:
//   - the first complete occurrence of an id is published, in first-seen order;
//   - a later row whose extracted fields are identical is deduplicated (the id
//     is still output exactly once);
//   - a later row whose fields differ produces a conflict record carrying the
//     first and the conflicting line numbers and the offending field;
//   - a row where either module fails (missing field, wrong type) is dropped: a
//     half-extracted record is never published and the failure cannot pollute
//     any other row.
//
// Output is JSON Lines written to the file given as argv[1] (default
// collect_rows.jsonl). All parsing, unescaping, cleaning and deduplication goes
// through the fork API; nothing here assumes a pre-organized input.
//
// The pipeline function is reusable: define COLLECT_ROWS_NO_MAIN before
// including this translation unit to suppress its main().

#include "simdjson.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace collect_rows_example {

using namespace simdjson;

// ---------------------------------------------------------------------------
// The local, untrusted collection response. It deliberately contains direct
// UTF-8, backslash and \u escapes, an exact duplicate, a conflicting duplicate
// and rows that fail one of the two modules.
// ---------------------------------------------------------------------------
padded_string sample_response() {
  return R"({
  "rows": [
    {"id":"p1","price":{"amount":12.5,"currency":"EUR"},"desc":{"en":"tea\ncup","zh":"茶"}},
    {"id":"p2","price":{"amount":99.0,"currency":"USD"},"desc":{"en":"café","zh":"咖啡"}},
    {"id":"p1","price":{"amount":12.5,"currency":"EUR"},"desc":{"en":"tea\ncup","zh":"茶"}},
    {"id":"p3","price":{"amount":7.0,"currency":"JPY"},"desc":{"en":"only-english"}},
    {"id":"p4","price":{"amount":"not-a-number","currency":"GBP"},"desc":{"en":"x","zh":"y"}},
    {"id":"p2","price":{"amount":100.0,"currency":"USD"},"desc":{"en":"café","zh":"咖啡"}},
    {"id":"p5","price":{"amount":1.0,"currency":"USD"},"desc":{"en":"a\\b\"c","zh":"斜線"}}
  ],
  "total": 7
})"_padded;
}

struct PriceInfo {
  double amount{0.0};
  std::string currency;
};

struct DescInfo {
  std::string en;
  std::string zh;
};

struct MergedRecord {
  std::size_t line{0};
  std::string id;
  PriceInfo price;
  DescInfo desc;
};

struct ConflictRecord {
  std::string id;
  std::size_t first_line{0};
  std::size_t conflict_line{0};
  std::string field;
};

// A single output line is either a merged record or a conflict record.
struct OutputLine {
  bool is_conflict{false};
  MergedRecord record;
  ConflictRecord conflict;
};

struct PipelineCounters {
  std::size_t rows_seen{0};
  std::size_t rows_published{0};
  std::size_t duplicates_dropped{0};
  std::size_t conflicts{0};
  std::size_t rows_rejected{0}; // missing field / wrong type in one module
};

// Module A: extract the nested price through its own fork. A failure (missing
// field or wrong type) is returned as an error and leaves the fork's cursor as
// the only damaged state.
simdjson_warn_unused error_code
extract_price(ondemand::forked_value &fork, PriceInfo &out) noexcept {
  ondemand::value root;
  SIMDJSON_TRY(fork.get_value().get(root));
  SIMDJSON_TRY(root["price"]["amount"].get_double().get(out.amount));
  std::string_view currency;
  SIMDJSON_TRY(root["price"]["currency"].get_string().get(currency));
  out.currency.assign(currency.data(), currency.size());
  return SUCCESS;
}

// Module B: extract the multilingual description through a second fork.
simdjson_warn_unused error_code
extract_description(ondemand::forked_value &fork, DescInfo &out) noexcept {
  ondemand::value root;
  SIMDJSON_TRY(fork.get_value().get(root));
  std::string_view en, zh;
  SIMDJSON_TRY(root["desc"]["en"].get_string().get(en));
  SIMDJSON_TRY(root["desc"]["zh"].get_string().get(zh));
  out.en.assign(en.data(), en.size());
  out.zh.assign(zh.data(), zh.size());
  return SUCCESS;
}

// Minimal JSON string escaper for the values we serialize. UTF-8 bytes are
// passed through unchanged; only the JSON-mandated control characters and
// quotes/backslashes are escaped.
void append_json_escaped(std::string &out, std::string_view in) {
  out.push_back('"');
  for (char ch : in) {
    const unsigned char c = static_cast<unsigned char>(ch);
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out.push_back(ch);
        }
    }
  }
  out.push_back('"');
}

std::string serialize_record(const MergedRecord &r) {
  std::string out;
  out += "{\"id\":";
  append_json_escaped(out, r.id);
  out += ",\"price\":{\"amount\":";
  // The inputs use short, exactly representable decimal fractions.
  char num[64];
  std::snprintf(num, sizeof(num), "%.1f", r.price.amount);
  out += num;
  out += ",\"currency\":";
  append_json_escaped(out, r.price.currency);
  out += "},\"desc\":{\"en\":";
  append_json_escaped(out, r.desc.en);
  out += ",\"zh\":";
  append_json_escaped(out, r.desc.zh);
  out += "}}";
  return out;
}

std::string serialize_conflict(const ConflictRecord &c) {
  std::string out;
  out += "{\"conflict\":true,\"id\":";
  append_json_escaped(out, c.id);
  out += ",\"first_line\":";
  out += std::to_string(c.first_line);
  out += ",\"conflict_line\":";
  out += std::to_string(c.conflict_line);
  out += ",\"field\":";
  append_json_escaped(out, c.field);
  out += "}";
  return out;
}

// Compare a new, fully extracted row against the first record for its id and
// report the first differing field, or an empty string if they are identical.
std::string first_difference(const MergedRecord &first,
                             const MergedRecord &candidate) {
  if (first.price.amount != candidate.price.amount) { return "price.amount"; }
  if (first.price.currency != candidate.price.currency) { return "price.currency"; }
  if (first.desc.en != candidate.desc.en) { return "desc.en"; }
  if (first.desc.zh != candidate.desc.zh) { return "desc.zh"; }
  return {};
}

// Run the whole pipeline over `json` and append JSON Lines to `output`. Returns
// 0 on success and a non-zero code on a hard parse or I/O failure. Per-row
// module errors never fail the run.
int run_collect_rows(padded_string &json, std::vector<std::string> &output,
                     PipelineCounters &counters) {
  ondemand::parser parser;
  auto doc_r = parser.iterate(json);
  if (doc_r.error()) { return static_cast<int>(doc_r.error()); }
  ondemand::document &doc = doc_r.value_unsafe();

  ondemand::array rows;
  if (doc["rows"].get_array().get(rows)) { return 1; }

  // First-seen record for each product id, retaining the cleaned (owned) values.
  std::map<std::string, MergedRecord> first_by_id;

  std::size_t line = 0;
  for (auto row_result : rows) {
    ++line;
    counters.rows_seen++;
    ondemand::value row;
    if (row_result.get(row)) { counters.rows_rejected++; continue; }

    // Two independent forks of the still-unconsumed row value.
    auto price_fork_r = row.fork();
    auto desc_fork_r = row.fork();
    if (price_fork_r.error() || desc_fork_r.error()) {
      counters.rows_rejected++;
      continue;
    }
    ondemand::forked_value price_fork = std::move(price_fork_r.value_unsafe());
    ondemand::forked_value desc_fork = std::move(desc_fork_r.value_unsafe());

    // The parent path reads only the id and then advances to the next row while
    // both forks stay live: the defining three-way interleave.
    std::string_view id_view;
    if (row["id"].get_string().get(id_view)) { counters.rows_rejected++; continue; }

    PriceInfo price;
    DescInfo desc;
    // Each module is fully independent: a failure here cannot touch the other
    // fork, the parent cursor or any other row.
    const error_code price_err = extract_price(price_fork, price);
    const error_code desc_err = extract_description(desc_fork, desc);
    if (price_err || desc_err) {
      // Never publish a half record: require BOTH modules to succeed.
      counters.rows_rejected++;
      continue;
    }

    MergedRecord candidate;
    candidate.line = line;
    candidate.id.assign(id_view.data(), id_view.size());
    candidate.price = std::move(price);
    candidate.desc = std::move(desc);

    auto found = first_by_id.find(candidate.id);
    if (found == first_by_id.end()) {
      first_by_id.emplace(candidate.id, candidate);
      output.push_back(serialize_record(candidate));
      counters.rows_published++;
    } else {
      const std::string diff = first_difference(found->second, candidate);
      if (diff.empty()) {
        // Exact duplicate: the id stays published exactly once.
        counters.duplicates_dropped++;
      } else {
        ConflictRecord conflict;
        conflict.id = candidate.id;
        conflict.first_line = found->second.line;
        conflict.conflict_line = candidate.line;
        conflict.field = diff;
        output.push_back(serialize_conflict(conflict));
        counters.conflicts++;
      }
    }
  }
  return 0;
}

int run_to_file(padded_string &json, const std::string &output_path,
                PipelineCounters &counters) {
  std::vector<std::string> lines;
  const int rc = run_collect_rows(json, lines, counters);
  if (rc != 0) { return rc; }
  std::ofstream out(output_path, std::ios::binary | std::ios::trunc);
  if (!out) { return 2; }
  for (const std::string &line_text : lines) {
    out << line_text << '\n';
  }
  if (!out) { return 3; }
  return 0;
}

} // namespace collect_rows_example

#ifndef COLLECT_ROWS_NO_MAIN
int main(int argc, char **argv) {
  const std::string output_path =
      argc > 1 ? std::string(argv[1]) : std::string("collect_rows.jsonl");

  auto json = collect_rows_example::sample_response();
  collect_rows_example::PipelineCounters counters;
  const int rc = collect_rows_example::run_to_file(json, output_path, counters);
  if (rc != 0) {
    std::fprintf(stderr, "collect_rows failed with code %d\n", rc);
    return rc;
  }
  std::printf("collect_rows: seen=%zu published=%zu duplicates_dropped=%zu "
              "conflicts=%zu rejected=%zu -> %s\n",
              counters.rows_seen, counters.rows_published,
              counters.duplicates_dropped, counters.conflicts,
              counters.rows_rejected, output_path.c_str());
  return 0;
}
#endif // COLLECT_ROWS_NO_MAIN
