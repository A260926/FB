// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

// Call-scoped machinery behind DBImpl::MultiGet().  All routing state is
// built per call, used to resolve the whole batch, and released when the
// call returns; the on-disk format is unchanged.

#ifndef STORAGE_LEVELDB_DB_DB_BATCH_H_
#define STORAGE_LEVELDB_DB_DB_BATCH_H_

#include <string>
#include <vector>
#include "db/dbformat.h"
#include "db/version_set.h"
#include "leveldb/options.h"
#include "leveldb/status.h"

namespace leveldb {

class MemTable;
class TableCache;

// Per-key outcome of probing one table/memtable, mirroring the Saver states
// used by the single-key path so deletions and corruptions behave identically.
struct BatchSaver {
  enum State {
    kNotFound,
    kFound,
    kDeleted,
    kCorrupt
  };
  State state;
  const Comparator* ucmp;
  Slice user_key;
  std::string* value;

  BatchSaver() : state(kNotFound), ucmp(NULL), value(NULL) { }

  // Table::InternalGet-style callback.
  static void SaveValue(void* arg, const Slice& ikey, const Slice& v);
};

// Routing state for one distinct user key in the batch.
struct BatchGetState {
  // Takes ownership of "key".
  BatchGetState(LookupKey* key, const Comparator* cmp)
      : lkey(key),
        done(false),
        seek_file(NULL),
        seek_file_level(-1),
        last_file_read(NULL),
        last_file_read_level(-1),
        files_touched(0) {
    saver.ucmp = cmp;
    saver.user_key = key->user_key();
    saver.value = &result_storage;
  }

  ~BatchGetState() { delete lkey; }

  LookupKey* const lkey;
  BatchSaver saver;
  // Holds the found value until it is copied back to the caller's slots.
  std::string result_storage;
  // Set when the key has been resolved (found/deleted/corrupt) or a file or
  // block error makes further lookup pointless for this key.
  bool done;
  // Non-OK when a file/block operation failed for this key.
  Status status;
  // Seek compaction bookkeeping, mirroring Version::Get().
  FileMetaData* seek_file;
  int seek_file_level;
  FileMetaData* last_file_read;
  int last_file_read_level;
  int files_touched;

  // No copying allowed
  BatchGetState(const BatchGetState&);
  void operator=(const BatchGetState&);
};

// Probe a memtable for the comparator-sorted, still-unresolved "states".
// Resolved states are marked done; the others are left for the next layer.
extern void MemTableBatchGet(MemTable* mem,
                             std::vector<BatchGetState*>* states);

// Pins table handles for the duration of one MultiGet() call and groups the
// per-table work so that a data block touched by several keys is loaded and
// checksum-verified at most once.  All state is released on destruction.
class BatchTableReader {
 public:
  BatchTableReader(TableCache* table_cache, const ReadOptions& options)
      : table_cache_(table_cache),
        options_(options) {
  }

  const ReadOptions& options() const { return options_; }

  // Route one sorted group of lookups that all land in the same table file.
  // "group" must be ordered by internal key.  Per-query failures are recorded
  // on the states themselves so that other keys keep being processed.  A
  // returned non-OK status denotes a failure while opening the table.
  Status LookupGroup(uint64_t file_number,
                     uint64_t file_size,
                     std::vector<BatchGetState*>* group);

 private:
  TableCache* table_cache_;
  ReadOptions options_;

  // No copying allowed
  BatchTableReader(const BatchTableReader&);
  void operator=(const BatchTableReader&);
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_DB_DB_BATCH_H_
