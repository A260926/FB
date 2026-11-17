/* Copyright (c) 2018–2021 SplineLib

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
the Software, and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE. */

#include "BSplineLib/Splines/shared_refinement.hpp"

#include <algorithm>
#include <utility>

namespace bsplinelib::splines {

struct SharedRefinementRegistry::Entry {
  std::weak_ptr<void> observer;
  SharedPointer<vector_spaces::VectorSpace> net;
  Callback callback;
};

SharedRefinementRegistry::SharedRefinementRegistry() = default;

SharedRefinementRegistry::~SharedRefinementRegistry() = default;

std::shared_ptr<void>
SharedRefinementRegistry::RegisterNet(
    SharedPointer<vector_spaces::VectorSpace> net,
    Callback callback) {
  std::lock_guard<std::mutex> const lock(mutex_);
  // Keeping the registry alive for as long as an observer token exists makes
  // the unregistration deleter safe even if all other references are gone.
  auto self = shared_from_this();
  auto token = std::shared_ptr<void>(
      new char,
      [this, self](void* raw) {
        this->UnregisterNet(raw);
        delete static_cast<char*>(raw);
      });
  entries_.push_back(
      Entry{std::weak_ptr<void>{token}, std::move(net), std::move(callback)});
  return token;
}

void SharedRefinementRegistry::DispatchEdit(
    Edit const& edit,
    void* initiating_net,
    Callback const& initiating_callback) {
  std::lock_guard<std::mutex> const lock(mutex_);
  initiating_callback(edit);
  VisitNets(edit, initiating_net);
}

std::size_t SharedRefinementRegistry::NumberOfNets() {
  std::lock_guard<std::mutex> const lock(mutex_);
  CleanupExpired();
  // Several wrappers may alias the same control net, so count distinct nets.
  std::unordered_set<void*> distinct;
  for (Entry const& entry : entries_) {
    if (entry.net != nullptr) {
      distinct.insert(static_cast<void*>(entry.net.get()));
    }
  }
  return distinct.size();
}

void SharedRefinementRegistry::UnregisterNet(void* key) {
  std::lock_guard<std::mutex> const lock(mutex_);
  entries_.erase(
      std::remove_if(entries_.begin(),
                     entries_.end(),
                     [&](Entry const& entry) {
                       return TokenKey(entry.observer) == key;
                     }),
      entries_.end());
}

void SharedRefinementRegistry::CleanupExpired() {
  entries_.erase(std::remove_if(entries_.begin(),
                                entries_.end(),
                                [](Entry const& entry) {
                                  return entry.observer.expired()
                                         || entry.net == nullptr;
                                }),
                 entries_.end());
}

void SharedRefinementRegistry::VisitNets(Edit const& edit,
                                         void* initiating_net) {
  CleanupExpired();
  std::unordered_set<void*> visited;
  // The initiating net has already been refined by initiating_callback.
  if (initiating_net != nullptr) {
    visited.insert(initiating_net);
  }
  // Each net address must be refined at most once: aliased wrappers and the
  // homogeneous BSpline of a Nurbs share the very same VectorSpace.
  for (Entry& entry : entries_) {
    if (entry.net == nullptr
        || !visited.insert(static_cast<void*>(entry.net.get())).second) {
      continue;
    }
    entry.callback(edit);
  }
}

void* SharedRefinementRegistry::TokenKey(
    std::weak_ptr<void> const& observer) {
  if (std::shared_ptr<void> const locked = observer.lock()) {
    return locked.get();
  }
  return nullptr;
}

} // namespace bsplinelib::splines
