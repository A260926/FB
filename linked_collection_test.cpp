// Tests for 1xx informational notifications and linked-resource collection.
//
// Every scenario is driven by a small raw HTTP server on the loopback
// interface. Ordering is established with mutexes/condition variables rather
// than timers, so the assertions do not depend on scheduling delays.

#include "linked_collection.h"
#include "task_support/check.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Minimal raw HTTP/1.1 server over a loopback TCP socket.
// ---------------------------------------------------------------------------

using Handler = std::function<void(int fd, const std::string &request)>;

void write_all(int fd, const std::string &data) {
  size_t off = 0;
  while (off < data.size()) {
    auto n = ::send(fd, data.data() + off, data.size() - off, 0);
    if (n <= 0) { break; }
    off += static_cast<size_t>(n);
  }
}

std::string read_until_head(int fd) {
  std::string head;
  char buf[2048];
  while (head.find("\r\n\r\n") == std::string::npos) {
    auto n = ::recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) { break; }
    head.append(buf, static_cast<size_t>(n));
  }
  return head;
}

std::string request_path(const std::string &request) {
  auto sp1 = request.find(' ');
  auto sp2 = request.find(' ', sp1 + 1);
  auto target = request.substr(sp1 + 1, sp2 - sp1 - 1);
  auto q = target.find('?');
  return q == std::string::npos ? target : target.substr(0, q);
}

std::string header_value(const std::string &request, const std::string &name) {
  auto key = name + ":";
  auto pos = request.find(key);
  if (pos == std::string::npos) { return std::string(); }
  auto eol = request.find("\r\n", pos);
  auto val = request.substr(pos + key.size(), eol - pos - key.size());
  auto begin = val.find_first_not_of(" \t");
  if (begin == std::string::npos) { return std::string(); }
  auto end = val.find_last_not_of(" \t");
  return val.substr(begin, end - begin + 1);
}

std::string ok_response(const std::string &body) {
  return "HTTP/1.1 200 OK\r\nContent-Length: " +
         std::to_string(body.size()) +
         "\r\nConnection: close\r\n\r\n" + body;
}

std::string redirect_response(const std::string &location) {
  return "HTTP/1.1 302 Found\r\nContent-Length: 0\r\nLocation: " + location +
         "\r\nConnection: close\r\n\r\n";
}

class RawServer {
public:
  std::map<std::string, Handler> routes;

  RawServer() {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(listen_fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) ||
        ::listen(listen_fd_, 32)) {
      throw std::runtime_error("failed to bind raw test server");
    }
    socklen_t len = sizeof(addr);
    if (::getsockname(listen_fd_, reinterpret_cast<sockaddr *>(&addr), &len)) {
      throw std::runtime_error("getsockname failed");
    }
    port_ = ntohs(addr.sin_port);

    acceptor_ = std::thread([this] { accept_loop(); });
  }

  ~RawServer() {
    stopping_ = true;
    ::shutdown(listen_fd_, SHUT_RDWR);
    ::close(listen_fd_);
    if (acceptor_.joinable()) { acceptor_.join(); }
    std::lock_guard<std::mutex> lk(conns_mutex_);
    for (auto &t : conns_) {
      if (t.joinable()) { t.join(); }
    }
  }

  int port() const { return port_; }
  std::string base() const {
    return "http://127.0.0.1:" + std::to_string(port_);
  }

private:
  void accept_loop() {
    while (!stopping_) {
      auto fd = ::accept(listen_fd_, nullptr, nullptr);
      if (fd < 0) { return; }
      std::lock_guard<std::mutex> lk(conns_mutex_);
      conns_.emplace_back([this, fd] {
        auto request = read_until_head(fd);
        auto path = request_path(request);
        auto it = routes.find(path);
        if (it == routes.end()) {
          write_all(fd, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n"
                        "Connection: close\r\n\r\n");
        } else {
          it->second(fd, request);
        }
        ::shutdown(fd, SHUT_RDWR);
        ::close(fd);
      });
    }
  }

  int listen_fd_ = -1;
  int port_ = 0;
  std::atomic<bool> stopping_{false};
  std::thread acceptor_;
  std::mutex conns_mutex_;
  std::vector<std::thread> conns_;
};

// A barrier-like gate used to order concurrent requests without sleeping.
struct Gate {
  std::mutex mutex;
  std::condition_variable cv;
  bool open = false;

  void wait() {
    std::unique_lock<std::mutex> lk(mutex);
    cv.wait(lk, [&] { return open; });
  }
  void release() {
    {
      std::lock_guard<std::mutex> lk(mutex);
      open = true;
    }
    cv.notify_all();
  }
};

} // namespace

// ---------------------------------------------------------------------------
// overlap: linked requests start before the primary body, and the number of
// simultaneously open associated connections never exceeds `concurrency`.
// ---------------------------------------------------------------------------
void test_overlap(Check &check) {
  constexpr size_t kConcurrency = 3;
  constexpr int kLinks = 7;

  RawServer server;
  Gate gate;

  std::mutex state_mutex;
  std::condition_variable state_cv;
  int inflight = 0;
  int max_inflight = 0;
  int arrivals = 0;
  int completed = 0;
  std::vector<std::string> events;

  server.routes["/start"] = [&](int fd, const std::string &) {
    std::string link;
    for (int i = 0; i < kLinks; ++i) {
      if (i) { link += ", "; }
      link += "</r" + std::to_string(i) +
              ">; rel=preload; as=fetch";
    }
    write_all(fd, "HTTP/1.1 103 Early Hints\r\nLink: " + link + "\r\n\r\n");

    // Do not send the final body until the first `kConcurrency` associated
    // requests are already connected and waiting.
    {
      std::unique_lock<std::mutex> lk(state_mutex);
      state_cv.wait(lk, [&] { return arrivals >=
                                  static_cast<int>(kConcurrency); });
      gate.release();
      state_cv.wait(lk, [&] { return completed == kLinks; });
      events.push_back("primary-body");
    }
    write_all(fd, ok_response("PRIMARY"));
  };

  for (int i = 0; i < kLinks; ++i) {
    auto path = "/r" + std::to_string(i);
    server.routes[path] = [&, path](int fd, const std::string &) {
      {
        std::lock_guard<std::mutex> lk(state_mutex);
        ++inflight;
        if (inflight > max_inflight) { max_inflight = inflight; }
        ++arrivals;
        events.push_back("arrive:" + path);
        state_cv.notify_all();
      }
      gate.wait(); // block while holding no server-wide lock
      {
        std::lock_guard<std::mutex> lk(state_mutex);
        --inflight;
        ++completed;
        state_cv.notify_all();
      }
      write_all(fd, ok_response("body-" + path.substr(2)));
    };
  }

  auto collection =
      httplib::collect_linked(server.base() + "/start", kConcurrency);

  check(collection.primary.error == httplib::Error::Success,
        "primary request succeeds");
  check(collection.primary.status == 200 &&
            collection.primary.body == "PRIMARY",
        "primary body delivered");
  check(collection.resources.size() == kLinks,
        "every linked resource collected exactly once");
  check(max_inflight == static_cast<int>(kConcurrency),
        "concurrency limit is reached but never exceeded");

  auto body_event = std::find(events.begin(), events.end(), "primary-body");
  check(body_event != events.end(), "primary body event recorded");
  auto arrivals_before_body = 0;
  for (auto it = events.begin(); it != body_event; ++it) {
    if (it->rfind("arrive:", 0) == 0) { ++arrivals_before_body; }
  }
  check(arrivals_before_body == kLinks,
        "every associated request arrived before the primary body");

  for (const auto &resource : collection.resources) {
    check(resource.error == httplib::Error::Success &&
              resource.status == 200,
          "linked resource fetched successfully");
  }
}

// ---------------------------------------------------------------------------
// links: Link header parsing, canonical resolution, discovery order and
// de-duplication.
// ---------------------------------------------------------------------------
void test_links(Check &check) {
  RawServer server;

  server.routes["/catalog"] = [&](int fd, const std::string &) {
    write_all(fd,
              "HTTP/1.1 103 Early Hints\r\n"
              // case-insensitive parameter names and quoted values; two
              // entries separated by a top-level comma.
              "Link: </a>; rel=preload; as=fetch, "
              "</b>; REL=\"PreLoad\"; As=\"FETCH\"\r\n"
              // rel may carry several whitespace-separated tokens.
              "Link: </c>; rel=\"next preload\"; as=\"fetch\"\r\n"
              // wrong `as` and wrong `rel` are both rejected.
              "Link: </d>; rel=preload; as=style, "
              "</e>; rel=prefetch; as=fetch\r\n"
              // a comma inside the angle brackets is not a separator; a
              // quoted rel containing an escaped quote and an internal comma
              // still lists `preload` as one of its whitespace tokens.
              "Link: </e,comma>; rel=\"preload \\\"a,b\\\"\"; as=fetch\r\n"
              // missing closing angle bracket, and an unclosed quoted
              // parameter, are both ignored; the valid sibling is kept.
              "Link: <f-relative>; rel=preload; as=fetch, "
              "</g>; rel=preload; as=\"fetch\r\n"
              // fragment dropped, dot segments removed, query and percent
              // escaping preserved.
              "Link: </dir/../data%2Fx?a=1%202#frag>; rel=preload; as=fetch\r\n"
              "\r\n");
    // The final response participates too; the duplicate </a> must not be
    // fetched again, and </h> joins after the early-hint resources.
    write_all(fd,
              "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n"
              "Link: </h>; rel=preload; as=fetch, "
              "</a>; rel=preload; as=fetch\r\n"
              "Connection: close\r\n\r\nmain");
  };

  const std::vector<std::string> routed = {
      "/a", "/b", "/c", "/e,comma", "/f-relative", "/data%2Fx", "/h"};
  for (const auto &path : routed) {
    auto handler = [path](int fd, const std::string &) {
      write_all(fd, ok_response("body:" + path));
    };
    server.routes[path] = handler;
    // The client percent-encodes a comma on the wire; answer that alias too.
    if (path.find(',') != std::string::npos) {
      std::string encoded;
      for (auto ch : path) {
        if (ch == ',') { encoded += "%2C"; }
        else { encoded += ch; }
      }
      server.routes[encoded] = handler;
    }
  }

  auto collection =
      httplib::collect_linked(server.base() + "/catalog", 4);

  check(collection.primary.status == 200 &&
            collection.primary.body == "main",
        "primary response kept");

  std::vector<std::string> urls;
  for (const auto &resource : collection.resources) {
    urls.push_back(resource.url);
  }

  const auto b = server.base();
  const std::vector<std::string> expected = {
      b + "/a",
      b + "/b",
      b + "/c",
      b + "/e,comma",
      b + "/f-relative",
      b + "/data%2Fx?a=1%202",
      b + "/h",
  };
  check(urls == expected, "discovery order, filtering and canonicalization");

  check(std::none_of(urls.begin(), urls.end(),
                     [&](const std::string &u) {
                       return u == collection.primary.url;
                     }),
        "the primary URL is never a linked resource");
  check(std::none_of(urls.begin(), urls.end(),
                     [&](const std::string &u) {
                       return u == b + "/d" || u == b + "/e" ||
                              u == b + "/g";
                     }),
        "non-matching and malformed link entries are ignored");

  const auto find = [&](const std::string &url) -> const httplib::LinkedResource & {
    for (const auto &resource : collection.resources) {
      if (resource.url == url) { return resource; }
    }
    throw std::runtime_error("missing resource " + url);
  };

  check(find(b + "/data%2Fx?a=1%202").url.find('#') == std::string::npos,
        "fragments are removed");
  check(find(b + "/a").body == "body:/a" &&
            find(b + "/h").body == "body:/h",
        "early and final-link resources carry their bodies");
  check(collection.resources.size() == expected.size(),
        "duplicate final header link is not refetched");
}

// ---------------------------------------------------------------------------
// protocol: 100/103 notifications with isolated final headers, plus redirect,
// loop, cross-origin and failure semantics for collection.
// ---------------------------------------------------------------------------
void test_protocol(Check &check) {
  RawServer server;

  // --- 103/102 interim notifications and final header isolation -------------
  server.routes["/hints"] = [&](int fd, const std::string &) {
    write_all(fd,
              "HTTP/1.1 103 Early Hints\r\nLink: </early>; rel=preload\r\n\r\n"
              "HTTP/1.1 102 Processing\r\nX-Interim: 1\r\n\r\n"
              "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n"
              "X-Final: yes\r\nConnection: close\r\n\r\ndone");
  };

  {
    httplib::Client client("127.0.0.1", server.port());
    client.set_read_timeout(10, 0);
    httplib::Request request;
    request.method = "GET";
    request.path = "/hints";

    std::vector<int> interim_statuses;
    std::vector<std::string> interim_links;
    request.informational_response_handler =
        [&](const httplib::Response &interim) {
          interim_statuses.push_back(interim.status);
          interim_links.push_back(interim.get_header_value("Link"));
          check(interim.body.empty(), "interim response has no body");
        };

    httplib::Response response;
    httplib::Error error = httplib::Error::Success;
    auto ok = client.send(request, response, error);

    check(ok && error == httplib::Error::Success, "hints request succeeds");
    check((interim_statuses == std::vector<int>{103, 102}),
          "interim heads delivered in wire order");
    check(interim_links.at(0) == "</early>; rel=preload",
          "interim header visible to callback");
    check(response.status == 200 && response.body == "done",
          "final status and body");
    check(response.get_header_value_count("Link") == 0 &&
              response.get_header_value_count("X-Interim") == 0 &&
              response.get_header_value("X-Final") == "yes",
          "interim headers do not leak into the final response");
  }

  // --- 100 Continue notifies once and the body is sent exactly once ---------
  std::atomic<bool> body_complete{false};
  server.routes["/continue"] = [&](int fd, const std::string &request) {
    auto length = std::stoul(header_value(request, "Content-Length"));
    write_all(fd,
              "HTTP/1.1 103 Early Hints\r\nLink: </early>; rel=preload\r\n\r\n"
              "HTTP/1.1 100 Continue\r\n\r\n");
    std::string body;
    body.resize(length);
    size_t got = 0;
    while (got < length) {
      auto n = ::recv(fd, body.data() + got, length - got, 0);
      if (n <= 0) { break; }
      got += static_cast<size_t>(n);
    }
    // Recorded from the server thread; asserted back on the client thread.
    body_complete = (got == length);
    write_all(fd, ok_response("accepted"));
  };

  {
    httplib::Client client("127.0.0.1", server.port());
    client.set_read_timeout(10, 0);
    httplib::Request request;
    request.method = "POST";
    request.path = "/continue";
    request.body = std::string(2048, 'B');
    request.set_header("Expect", "100-continue");

    std::vector<int> interim_statuses;
    request.informational_response_handler =
        [&](const httplib::Response &interim) {
          interim_statuses.push_back(interim.status);
        };

    httplib::Response response;
    httplib::Error error = httplib::Error::Success;
    auto ok = client.send(request, response, error);

    check(ok && response.status == 200 && response.body == "accepted",
          "expect-continue exchange completes");
    check(body_complete.load(),
          "request body is sent exactly once after 100 Continue");
    check((interim_statuses == std::vector<int>{103, 100}),
          "103 then 100 are both reported");
    check(response.get_header_value_count("Link") == 0,
          "no interim header survives into the final head");
  }

  // --- collection redirect, loop, cross-origin and failure handling --------
  server.routes["/go"] = [&](int fd, const std::string &) {
    // This 103 belongs to the pre-redirect hop and must resolve against it.
    write_all(fd,
              "HTTP/1.1 103 Early Hints\r\n"
              "Link: </early.json>; rel=preload; as=fetch\r\n\r\n");
    write_all(fd, redirect_response("/dest"));
  };
  server.routes["/early.json"] = [&](int fd, const std::string &) {
    write_all(fd, ok_response("EARLY"));
  };
  server.routes["/dest"] = [&](int fd, const std::string &) {
    write_all(fd,
              "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n"
              "Link: </rel/../norm.json>; rel=preload; as=fetch, "
              "</loop.json>; rel=preload; as=fetch, "
              "</bad.json>; rel=preload; as=fetch, "
              "</chain0.json>; rel=preload; as=fetch, "
              "<http://other.example/x>; rel=preload; as=fetch\r\n"
              "Connection: close\r\n\r\nDONE");
  };
  server.routes["/norm.json"] = [&](int fd, const std::string &) {
    write_all(fd, ok_response("NORM"));
  };
  server.routes["/loop.json"] = [&](int fd, const std::string &) {
    write_all(fd, redirect_response("/loop2.json"));
  };
  server.routes["/loop2.json"] = [&](int fd, const std::string &) {
    write_all(fd,
              "HTTP/1.1 307 Temporary Redirect\r\nContent-Length: 0\r\n"
              "Location: /loop.json\r\nConnection: close\r\n\r\n");
  };
  server.routes["/bad.json"] = [&](int fd, const std::string &) {
    write_all(fd,
              "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 5\r\n"
              "Connection: close\r\n\r\nerror");
  };
  for (int i = 0; i < 9; ++i) {
    auto current = "/chain" + std::to_string(i) + ".json";
    auto next = "/chain" + std::to_string(i + 1) + ".json";
    server.routes[current] = [&, next](int fd, const std::string &) {
      write_all(fd, redirect_response(next));
    };
  }
  server.routes["/chain9.json"] = [&](int fd, const std::string &) {
    write_all(fd, ok_response("END"));
  };

  auto collection = httplib::collect_linked(server.base() + "/go", 3);

  const auto find = [&](const std::string &url) -> const httplib::LinkedResource & {
    for (const auto &resource : collection.resources) {
      if (resource.url == url) { return resource; }
    }
    throw std::runtime_error("missing resource " + url);
  };

  check(collection.primary.status == 200 &&
            collection.primary.body == "DONE",
        "primary follows the same-origin redirect");
  check(find(server.base() + "/early.json").body == "EARLY",
        "pre-redirect 103 resolved against the redirecting hop");
  check(find(server.base() + "/norm.json").body == "NORM",
        "relative dot-segment link normalized");
  check(find(server.base() + "/loop.json").error ==
            httplib::Error::ExceedRedirectCount,
        "redirect loop reported as an error on that resource only");
  check(find(server.base() + "/chain0.json").error ==
            httplib::Error::ExceedRedirectCount,
        "more than eight redirects reported as an error");
  check(find(server.base() + "/bad.json").status == 500 &&
            find(server.base() + "/bad.json").body == "error" &&
            find(server.base() + "/bad.json").error ==
                httplib::Error::Success,
        "non-2xx keeps its real status and body");

  auto has_cross_origin =
      std::any_of(collection.resources.begin(), collection.resources.end(),
                  [](const httplib::LinkedResource &resource) {
                    return resource.url.find("other.example") !=
                           std::string::npos;
                  });
  check(!has_cross_origin, "cross-origin links are not collected");

  // --- primary transport failure retains already discovered resources ------
  server.routes["/fail"] = [&](int fd, const std::string &) {
    write_all(fd,
              "HTTP/1.1 103 Early Hints\r\n"
              "Link: </keep.json>; rel=preload; as=fetch\r\n\r\n");
    // Abruptly close without a final response: the primary request fails, but
    // the linked resource uses its own connection and still completes.
    ::shutdown(fd, SHUT_RDWR);
  };
  server.routes["/keep.json"] = [&](int fd, const std::string &) {
    write_all(fd, ok_response("KEPT"));
  };

  auto failed = httplib::collect_linked(server.base() + "/fail", 2);
  check(failed.primary.error != httplib::Error::Success,
        "primary transport failure is reported");
  auto kept = std::find_if(failed.resources.begin(), failed.resources.end(),
                           [&](const httplib::LinkedResource &resource) {
                             return resource.url ==
                                    server.base() + "/keep.json";
                           });
  check(kept != failed.resources.end() && kept->status == 200 &&
            kept->body == "KEPT" &&
            kept->error == httplib::Error::Success,
        "resources collected before the primary failure are retained");

  // --- argument validation --------------------------------------------------
  auto throws_invalid = [&](const std::string &url, size_t concurrency) {
    try {
      httplib::collect_linked(url, concurrency);
      return false;
    } catch (const std::invalid_argument &) {
      return true;
    }
  };
  check(throws_invalid(server.base() + "/x", 0),
        "zero concurrency throws invalid_argument");
  check(throws_invalid("http://user:pass@127.0.0.1/x", 1),
        "userinfo URLs throw invalid_argument");
  check(throws_invalid("https://127.0.0.1/x", 1),
        "non-http URLs throw invalid_argument");
}
