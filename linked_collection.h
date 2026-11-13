#ifndef CPPHTTPLIB_LINKED_COLLECTION_H
#define CPPHTTPLIB_LINKED_COLLECTION_H

#include "httplib.h"

#include <cstddef>
#include <string>
#include <vector>

namespace httplib {

// One resource fetched as part of a linked collection.
//
// `url` is the canonical URL at which the resource was first discovered. A
// resource that produced an HTTP response keeps its real `status` and `body`
// (even non-2xx ones) and `error == Error::Success`; a transport or protocol
// failure keeps the httplib `Error`, with `status == 0` and whatever body had
// arrived.
struct LinkedResource {
  std::string url;
  int status = 0;
  std::string body;
  Error error = Error::Success;
};

// The primary response plus every linked resource collected alongside it.
// `resources` is ordered by first discovery of each canonical URL and never
// contains the primary URL itself.
struct LinkedCollection {
  LinkedResource primary;
  std::vector<LinkedResource> resources;
};

// GETs `url` and, in parallel, the resources announced through
// `Link: <...>; rel=preload; as=fetch` entries in 103 Early Hints (and the
// final response) heads. Only http URLs without userinfo are supported;
// `concurrency` must be greater than zero, otherwise std::invalid_argument is
// thrown.
LinkedCollection collect_linked(const std::string &url, size_t concurrency);

} // namespace httplib

#endif
