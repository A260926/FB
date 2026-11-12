// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

// Internal argument type shared by the call-scoped batch read path
// (DBImpl::MultiGet).  A group of these targets a single table file; the
// table layer resolves them together, loading each data block at most once.

#ifndef STORAGE_LEVELDB_TABLE_BATCH_TABLE_H_
#define STORAGE_LEVELDB_TABLE_BATCH_TABLE_H_

#include <vector>
#include "leveldb/slice.h"
#include "leveldb/status.h"

namespace leveldb {

struct TableBatchItem {
  // Internal key (user key + sequence/type tag) to look up.
  Slice internal_key;
  // Saver callback, with the same contract as Table::InternalGet(): invoked
  // at most once, with the entry found at or just after the target key.
  void* arg;
  void (*handle_result)(void* arg, const Slice& k, const Slice& v);
  // Set by the table layer when the lookup for this item has finished.
  bool done;
  // Block/table level error specific to this item; OK otherwise.
  Status status;

  TableBatchItem() : arg(NULL), handle_result(NULL), done(false) { }
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_TABLE_BATCH_TABLE_H_
