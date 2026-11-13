#include "linked_collection.h"

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_set>

namespace httplib {

namespace {

// ---------------------------------------------------------------------------
// Small string helpers
// ---------------------------------------------------------------------------

std::string trim(const std::string &s) {
  auto begin = s.find_first_not_of(" \t");
  if (begin == std::string::npos) { return std::string(); }
  auto end = s.find_last_not_of(" \t");
  return s.substr(begin, end - begin + 1);
}

std::string to_lower(std::string s) {
  for (auto &c : s) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
  return s;
}

bool iequals(const std::string &a, const std::string &b) {
  if (a.size() != b.size()) { return false; }
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Canonical http URLs
// ---------------------------------------------------------------------------

struct CanonUrl {
  std::string scheme; // always "http"
  std::string host;   // lowercased
  int port = 80;
  std::string path;   // begins with '/', dot segments removed
  std::string query;  // includes the leading '?', or empty
  std::string origin;
  std::string canonical;
};

std::string build_origin(const std::string &scheme, const std::string &host,
                         int port) {
  return scheme + "://" + host + ":" + std::to_string(port);
}

std::string build_canonical(const std::string &scheme, const std::string &host,
                            int port, const std::string &path,
                            const std::string &query) {
  auto canonical = scheme + "://" + host;
  if (port != 80) { canonical += ":" + std::to_string(port); }
  canonical += path;
  canonical += query;
  return canonical;
}

// Parses an absolute http URL without userinfo into a CanonUrl.
bool parse_absolute_http(const std::string &url, CanonUrl &out) {
  detail::UrlComponents uc;
  if (!detail::parse_url(url, uc)) { return false; }
  if (uc.scheme != "http" || uc.host.empty()) { return false; }

  auto port = 80;
  if (!uc.port.empty() && !detail::parse_port(uc.port, port)) { return false; }

  auto path = uc.path.empty() ? std::string("/") : uc.path;
  if (path[0] != '/') { return false; }
  path = detail::remove_dot_segments(path);

  out.scheme = "http";
  out.host = to_lower(uc.host);
  out.port = port;
  out.path = path;
  out.query = uc.query; // already carries the leading '?'
  out.origin = build_origin(out.scheme, out.host, out.port);
  out.canonical =
      build_canonical(out.scheme, out.host, out.port, out.path, out.query);
  return true;
}

// Resolves a Link reference against the URL of the request that produced the
// header (RFC 3986 section 5). Returns the canonical same-origin URL, or an
// empty string when the reference is malformed, not http, or cross-origin.
std::string resolve(const CanonUrl &base, const std::string &raw_ref) {
  auto ref = trim(raw_ref);
  if (ref.empty()) { return std::string(); }

  // The fragment is never part of the resource identity.
  ref = ref.substr(0, ref.find('#'));
  if (ref.empty()) { return base.canonical; }

  // Absolute URI.
  if (ref.find("://") != std::string::npos) {
    CanonUrl abs;
    if (!parse_absolute_http(ref, abs) || abs.origin != base.origin) {
      return std::string();
    }
    return abs.canonical;
  }

  // Network-path reference ("//host/path"); the scheme comes from the base.
  if (ref.compare(0, 2, "//") == 0) {
    CanonUrl net;
    if (!parse_absolute_http(base.scheme + ":" + ref, net) ||
        net.origin != base.origin) {
      return std::string();
    }
    return net.canonical;
  }

  // A ':' in the first segment names another scheme, which http cannot follow.
  if (ref.find(':') < ref.find_first_of("/?#")) { return std::string(); }

  std::string path;
  std::string query;
  auto qpos = ref.find('?');
  auto ref_path = qpos == std::string::npos ? ref : ref.substr(0, qpos);
  auto ref_query = qpos == std::string::npos ? std::string()
                                              : ref.substr(qpos);

  if (ref_path.empty()) {
    path = base.path;
    query = ref_query.empty() ? base.query : ref_query;
  } else if (ref_path[0] == '/') {
    path = detail::remove_dot_segments(ref_path);
    query = ref_query;
  } else {
    auto dir = base.path.substr(0, base.path.rfind('/') + 1);
    path = detail::remove_dot_segments(dir + ref_path);
    query = ref_query;
  }

  return build_canonical(base.scheme, base.host, base.port, path, query);
}

// ---------------------------------------------------------------------------
// Link header parsing (RFC 8288)
// ---------------------------------------------------------------------------

// Splits a Link field value on top-level commas. A comma inside the angle
// bracket URI-Reference or inside a quoted string is not a separator.
std::vector<std::string> split_link_entries(const std::string &value) {
  std::vector<std::string> entries;
  std::string cur;
  bool in_angle = false;
  bool in_quote = false;

  for (size_t i = 0; i < value.size(); ++i) {
    auto c = value[i];
    if (in_quote) {
      cur += c;
      if (c == '\\' && i + 1 < value.size()) {
        cur += value[++i]; // quoted-pair: keep the escaped char verbatim
      } else if (c == '"') {
        in_quote = false;
      }
      continue;
    }
    if (c == '"') {
      in_quote = true;
      cur += c;
    } else if (c == '<') {
      in_angle = true;
      cur += c;
    } else if (c == '>') {
      in_angle = false;
      cur += c;
    } else if (c == ',' && !in_angle) {
      entries.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  entries.push_back(cur);
  return entries;
}

// Removes surrounding quotes and resolves quoted-pair escapes. The caller
// guarantees the value starts with '"'; returns false if the quote never
// closes.
bool dequote(const std::string &value, std::string &out) {
  // value already has leading '"'; find the closing quote, honoring escapes.
  std::string result;
  for (size_t i = 1; i < value.size(); ++i) {
    auto c = value[i];
    if (c == '\\' && i + 1 < value.size()) {
      result += value[++i];
    } else if (c == '"') {
      out = std::move(result);
      return true;
    } else {
      result += c;
    }
  }
  return false;
}

// Splits the parameter section (everything after '>') on semicolons that are
// outside quoted strings.
std::vector<std::string> split_params(const std::string &s) {
  std::vector<std::string> parts;
  std::string cur;
  bool in_quote = false;
  for (size_t i = 0; i < s.size(); ++i) {
    auto c = s[i];
    if (in_quote) {
      cur += c;
      if (c == '\\' && i + 1 < s.size()) {
        cur += s[++i];
      } else if (c == '"') {
        in_quote = false;
      }
      continue;
    }
    if (c == '"') {
      in_quote = true;
      cur += c;
    } else if (c == ';') {
      parts.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  parts.push_back(cur);
  return parts;
}

// Extracts the URI-Reference of a preload/fetch link from one link-value.
// Returns false for entries without a bracketed URI or with an unclosed
// quoted parameter, which the caller must ignore entirely.
bool parse_preload_fetch_entry(std::string entry, std::string &uri) {
  entry = trim(entry);
  if (entry.empty() || entry[0] != '<') { return false; }

  auto close = entry.find('>');
  if (close == std::string::npos) { return false; }
  uri = entry.substr(1, close - 1);

  std::string rel;
  std::string as;
  auto have_rel = false;
  auto have_as = false;

  for (auto &part : split_params(entry.substr(close + 1))) {
    auto token = trim(part);
    if (token.empty()) { continue; }
    auto eq = token.find('=');
    if (eq == std::string::npos) { continue; } // valueless parameter

    auto name = to_lower(trim(token.substr(0, eq)));
    auto value = trim(token.substr(eq + 1));
    if (value.empty()) { continue; }

    if (value[0] == '"') {
      std::string unquoted;
      if (!dequote(value, unquoted)) { return false; } // unclosed quote
      value = std::move(unquoted);
    }

    if (name == "rel") {
      rel = value;
      have_rel = true;
    } else if (name == "as") {
      as = value;
      have_as = true;
    }
  }

  if (!have_rel || !have_as) { return false; }

  auto rel_matches = false;
  size_t i = 0;
  while (i < rel.size()) {
    while (i < rel.size() &&
           std::isspace(static_cast<unsigned char>(rel[i]))) {
      ++i;
    }
    auto begin = i;
    while (i < rel.size() &&
           !std::isspace(static_cast<unsigned char>(rel[i]))) {
      ++i;
    }
    if (i > begin && iequals(rel.substr(begin, i - begin), "preload")) {
      rel_matches = true;
      break;
    }
  }

  return rel_matches && iequals(trim(as), "fetch");
}

// Collects every preload/fetch URI-Reference from all Link header fields, in
// wire order.
std::vector<std::string> extract_link_refs(const Headers &headers) {
  std::vector<std::string> refs;
  auto range = headers.equal_range("Link");
  for (auto it = range.first; it != range.second; ++it) {
    for (auto &entry : split_link_entries(it->second)) {
      std::string uri;
      if (parse_preload_fetch_entry(entry, uri)) { refs.push_back(uri); }
    }
  }
  return refs;
}

// ---------------------------------------------------------------------------
// Collection engine
// ---------------------------------------------------------------------------

constexpr size_t kMaxRedirects = 8;

// Builds the client (and therefore the connection) used for a single hop. A
// separate client per request keeps associated fetches from waiting on the
// primary request's connection lock.
std::unique_ptr<ClientImpl> make_client(const std::string &host, int port) {
  return std::unique_ptr<ClientImpl>(new ClientImpl(host, port));
}

class Collector {
public:
  Collector(CanonUrl primary, size_t concurrency)
      : primary_(std::move(primary)), concurrency_(concurrency) {
    seen_.insert(primary_.canonical);
    result_.primary.url = primary_.canonical;
  }

  LinkedCollection run() {
    // Workers own their own connections; at most `concurrency_` associated
    // requests are ever in flight, independently of the primary connection.
    std::vector<std::thread> workers;
    workers.reserve(concurrency_);
    for (size_t i = 0; i < concurrency_; ++i) {
      workers.emplace_back([this] { worker_loop(); });
    }

    run_primary();

    {
      std::lock_guard<std::mutex> lock(mutex_);
      primary_done_ = true;
    }
    cv_.notify_all();

    for (auto &t : workers) { t.join(); }

    result_.resources = std::move(resources_);
    return std::move(result_);
  }

private:
  // Adds a discovered reference, resolving it against the hop that produced
  // the header. New canonical URLs enter the discovery order exactly once.
  void schedule(const CanonUrl &base, const std::string &ref) {
    auto canonical = resolve(base, ref);
    if (canonical.empty()) { return; }

    std::unique_lock<std::mutex> lock(mutex_);
    if (!seen_.insert(canonical).second) { return; } // duplicate or primary

    LinkedResource slot;
    slot.url = canonical;
    resources_.push_back(std::move(slot));
    queue_.push_back(Work{canonical, resources_.size() - 1});
    cv_.notify_one();
  }

  void worker_loop() {
    for (;;) {
      Work work;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&] { return !queue_.empty() || primary_done_; });
        if (queue_.empty()) { return; }
        work = queue_.front();
        queue_.pop_front();
      }

      auto resource = fetch(work.url);
      {
        std::lock_guard<std::mutex> lock(mutex_);
        resources_[work.index] = std::move(resource);
      }
    }
  }

  // GETs one associated resource, following at most eight same-origin
  // redirects. The stored URL is always the originally discovered one.
  LinkedResource fetch(const std::string &original_canonical) {
    LinkedResource resource;
    resource.url = original_canonical;

    std::unordered_set<std::string> visited;
    CanonUrl current;
    if (!parse_absolute_http(original_canonical, current)) {
      resource.error = Error::Connection;
      return resource;
    }

    size_t redirects = 0;
    for (;;) {
      if (!visited.insert(current.canonical).second ||
          current.origin != primary_.origin) {
        resource.error = Error::ExceedRedirectCount; // loop / cross-origin
        return resource;
      }

      auto client = make_client(current.host, current.port);
      client->set_connection_timeout(10, 0);
      client->set_read_timeout(10, 0);

      auto res = client->Get(current.path + current.query);
      if (!res) {
        resource.error = res.error();
        return resource;
      }

      if (detail::is_redirect(*res)) {
        if (++redirects > kMaxRedirects) {
          resource.error = Error::ExceedRedirectCount;
          return resource;
        }
        auto next = resolve(current, res->get_header_value("Location"));
        if (next.empty()) { // cross-origin or unusable Location
          resource.error = Error::ExceedRedirectCount;
          return resource;
        }
        if (!parse_absolute_http(next, current)) {
          resource.error = Error::ExceedRedirectCount;
          return resource;
        }
        continue;
      }

      resource.status = res->status;
      resource.body = res->body;
      resource.error = Error::Success;
      return resource;
    }
  }

  // Drives the primary request, following same-origin redirects itself so a
  // 103 from any hop can be resolved against that hop's URL.
  void run_primary() {
    CanonUrl current = primary_;
    std::unordered_set<std::string> visited;
    size_t redirects = 0;

    for (;;) {
      if (!visited.insert(current.canonical).second ||
          current.origin != primary_.origin) {
        result_.primary.error = Error::ExceedRedirectCount;
        return;
      }

      const CanonUrl base = current;
      auto client = make_client(current.host, current.port);
      client->set_connection_timeout(10, 0);
      client->set_read_timeout(10, 0);

      Request req;
      req.method = "GET";
      req.path = current.path + current.query;
      req.informational_response_handler =
          [this, &base](const Response &interim) {
            if (interim.status != StatusCode::EarlyHints_103) { return; }
            for (const auto &ref : extract_link_refs(interim.headers)) {
              schedule(base, ref);
            }
          };

      Response res;
      Error error = Error::Success;
      auto ok = client->send(req, res, error);

      if (!ok) {
        result_.primary.error = error;
        return;
      }

      if (detail::is_redirect(res)) {
        if (++redirects > kMaxRedirects) {
          result_.primary.error = Error::ExceedRedirectCount;
          return;
        }
        auto next = resolve(current, res.get_header_value("Location"));
        if (next.empty()) {
          result_.primary.error = Error::ExceedRedirectCount;
          return;
        }
        if (!parse_absolute_http(next, current)) {
          result_.primary.error = Error::ExceedRedirectCount;
          return;
        }
        continue;
      }

      // The final response's Link entries participate too. Linked resources'
      // own Link headers are deliberately never expanded (see fetch()).
      for (const auto &ref : extract_link_refs(res.headers)) {
        schedule(base, ref);
      }

      result_.primary.status = res.status;
      result_.primary.body = res.body;
      result_.primary.error = Error::Success;
      return;
    }
  }

  struct Work {
    std::string url;
    size_t index = 0;
  };

  CanonUrl primary_;
  size_t concurrency_;

  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<Work> queue_;
  std::vector<LinkedResource> resources_;
  std::unordered_set<std::string> seen_;
  bool primary_done_ = false;

  LinkedCollection result_;
};

} // namespace

LinkedCollection collect_linked(const std::string &url, size_t concurrency) {
  if (concurrency == 0) {
    throw std::invalid_argument("concurrency must be greater than zero");
  }

  CanonUrl primary;
  if (!parse_absolute_http(url, primary)) {
    throw std::invalid_argument(
        "only http URLs without userinfo are supported");
  }

  Collector collector(std::move(primary), concurrency);
  return collector.run();
}

} // namespace httplib
