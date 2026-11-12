// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/db_batch.h"

#include <vector>
#include "db/memtable.h"
#include "db/table_cache.h"
#include "leveldb/table.h"
#include "table/batch_table.h"

namespace leveldb {

void BatchSaver::SaveValue(void* arg, const Slice& ikey, const Slice& v) {
  BatchSaver* s = reinterpret_cast<BatchSaver*>(arg);
  ParsedInternalKey parsed_key;
  if (!ParseInternalKey(ikey, &parsed_key)) {
    s->state = kCorrupt;
  } else if (s->ucmp->Compare(parsed_key.user_key, s->user_key) == 0) {
    s->state = (parsed_key.type == kTypeValue) ? kFound : kDeleted;
    if (s->state == kFound) {
      s->value->assign(v.data(), v.size());
    }
  }
}

void MemTableBatchGet(MemTable* mem, std::vector<BatchGetState*>* states) {
  for (size_t i = 0; i < states->size(); i++) {
    BatchGetState* st = (*states)[i];
    if (st->done) {
      continue;
    }
    Status s;
    if (mem->Get(*st->lkey, st->saver.value, &s)) {
      // The memtable contains either a value or a deletion marker, both of
      // which terminate the search for this user key.
      st->done = true;
      st->status = s;
      st->saver.state = s.IsNotFound() ? BatchSaver::kDeleted
                                       : BatchSaver::kFound;
    }
  }
}

Status BatchTableReader::LookupGroup(
    uint64_t file_number,
    uint64_t file_size,
    std::vector<BatchGetState*>* group) {
  Table* table = NULL;
  Cache::Handle* handle = NULL;
  Status open = table_cache_->GetTable(file_number, file_size,
                                       &table, &handle);
  if (!open.ok()) {
    // The file itself cannot be opened: every key that still needs it gets
    // the error; this must not look like a miss.
    for (size_t i = 0; i < group->size(); i++) {
      BatchGetState* st = (*group)[i];
      if (!st->done) {
        st->status = open;
        st->done = true;
      }
    }
    return open;
  }

  std::vector<TableBatchItem> items;
  std::vector<BatchGetState*> active;
  items.reserve(group->size());
  active.reserve(group->size());
  for (size_t i = 0; i < group->size(); i++) {
    BatchGetState* st = (*group)[i];
    if (st->done) {
      continue;
    }
    TableBatchItem item;
    item.internal_key = st->lkey->internal_key();
    item.arg = &st->saver;
    item.handle_result = &BatchSaver::SaveValue;
    items.push_back(item);
    active.push_back(st);
  }

  Status table_status = table->InternalMultiGet(options_, &items);
  for (size_t i = 0; i < active.size(); i++) {
    BatchGetState* st = active[i];
    if (!items[i].status.ok()) {
      st->status = items[i].status;
      st->done = true;
      continue;
    }
    if (!table_status.ok()) {
      st->status = table_status;
      st->done = true;
      continue;
    }
    switch (st->saver.state) {
      case BatchSaver::kNotFound:
        // Absent from this file: keep searching older layers.
        st->done = false;
        break;
      case BatchSaver::kFound:
        st->status = Status::OK();
        st->done = true;
        break;
      case BatchSaver::kDeleted:
        // A tombstone terminates the search exactly like Version::Get().
        st->status = Status::NotFound(Slice());
        st->done = true;
        break;
      case BatchSaver::kCorrupt:
        st->status = Status::Corruption("corrupted key for ",
                                        st->saver.user_key);
        st->done = true;
        break;
    }
  }

  table_cache_->ReleaseTable(handle);
  return table_status;
}

}  // namespace leveldb
