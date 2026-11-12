// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "leveldb/table.h"

#include <map>
#include "leveldb/cache.h"
#include "leveldb/comparator.h"
#include "leveldb/env.h"
#include "leveldb/filter_policy.h"
#include "leveldb/options.h"
#include "table/batch_table.h"
#include "table/block.h"
#include "table/filter_block.h"
#include "table/format.h"
#include "table/two_level_iterator.h"
#include "util/coding.h"

namespace leveldb {

struct Table::Rep {
  ~Rep() {
    delete filter;
    delete [] filter_data;
    delete index_block;
  }

  Options options;
  Status status;
  RandomAccessFile* file;
  uint64_t cache_id;
  FilterBlockReader* filter;
  const char* filter_data;

  BlockHandle metaindex_handle;  // Handle to metaindex_block: saved from footer
  Block* index_block;
};

Status Table::Open(const Options& options,
                   RandomAccessFile* file,
                   uint64_t size,
                   Table** table) {
  *table = NULL;
  if (size < Footer::kEncodedLength) {
    return Status::Corruption("file is too short to be an sstable");
  }

  char footer_space[Footer::kEncodedLength];
  Slice footer_input;
  Status s = file->Read(size - Footer::kEncodedLength, Footer::kEncodedLength,
                        &footer_input, footer_space);
  if (!s.ok()) return s;

  Footer footer;
  s = footer.DecodeFrom(&footer_input);
  if (!s.ok()) return s;

  // Read the index block
  BlockContents contents;
  Block* index_block = NULL;
  if (s.ok()) {
    ReadOptions opt;
    if (options.paranoid_checks) {
      opt.verify_checksums = true;
    }
    s = ReadBlock(file, opt, footer.index_handle(), &contents);
    if (s.ok()) {
      index_block = new Block(contents);
    }
  }

  if (s.ok()) {
    // We've successfully read the footer and the index block: we're
    // ready to serve requests.
    Rep* rep = new Table::Rep;
    rep->options = options;
    rep->file = file;
    rep->metaindex_handle = footer.metaindex_handle();
    rep->index_block = index_block;
    rep->cache_id = (options.block_cache ? options.block_cache->NewId() : 0);
    rep->filter_data = NULL;
    rep->filter = NULL;
    *table = new Table(rep);
    (*table)->ReadMeta(footer);
  } else {
    delete index_block;
  }

  return s;
}

void Table::ReadMeta(const Footer& footer) {
  if (rep_->options.filter_policy == NULL) {
    return;  // Do not need any metadata
  }

  // TODO(sanjay): Skip this if footer.metaindex_handle() size indicates
  // it is an empty block.
  ReadOptions opt;
  if (rep_->options.paranoid_checks) {
    opt.verify_checksums = true;
  }
  BlockContents contents;
  if (!ReadBlock(rep_->file, opt, footer.metaindex_handle(), &contents).ok()) {
    // Do not propagate errors since meta info is not needed for operation
    return;
  }
  Block* meta = new Block(contents);

  Iterator* iter = meta->NewIterator(BytewiseComparator());
  std::string key = "filter.";
  key.append(rep_->options.filter_policy->Name());
  iter->Seek(key);
  if (iter->Valid() && iter->key() == Slice(key)) {
    ReadFilter(iter->value());
  }
  delete iter;
  delete meta;
}

void Table::ReadFilter(const Slice& filter_handle_value) {
  Slice v = filter_handle_value;
  BlockHandle filter_handle;
  if (!filter_handle.DecodeFrom(&v).ok()) {
    return;
  }

  // We might want to unify with ReadBlock() if we start
  // requiring checksum verification in Table::Open.
  ReadOptions opt;
  if (rep_->options.paranoid_checks) {
    opt.verify_checksums = true;
  }
  BlockContents block;
  if (!ReadBlock(rep_->file, opt, filter_handle, &block).ok()) {
    return;
  }
  if (block.heap_allocated) {
    rep_->filter_data = block.data.data();     // Will need to delete later
  }
  rep_->filter = new FilterBlockReader(rep_->options.filter_policy, block.data);
}

Table::~Table() {
  delete rep_;
}

static void DeleteBlock(void* arg, void* ignored) {
  delete reinterpret_cast<Block*>(arg);
}

static void DeleteCachedBlock(const Slice& key, void* value) {
  Block* block = reinterpret_cast<Block*>(value);
  delete block;
}

static void ReleaseBlock(void* arg, void* h) {
  Cache* cache = reinterpret_cast<Cache*>(arg);
  Cache::Handle* handle = reinterpret_cast<Cache::Handle*>(h);
  cache->Release(handle);
}

// Convert an index iterator value (i.e., an encoded BlockHandle)
// into an iterator over the contents of the corresponding block.
Iterator* Table::BlockReader(void* arg,
                             const ReadOptions& options,
                             const Slice& index_value) {
  Table* table = reinterpret_cast<Table*>(arg);
  Cache* block_cache = table->rep_->options.block_cache;
  Block* block = NULL;
  Cache::Handle* cache_handle = NULL;

  BlockHandle handle;
  Slice input = index_value;
  Status s = handle.DecodeFrom(&input);
  // We intentionally allow extra stuff in index_value so that we
  // can add more features in the future.

  if (s.ok()) {
    BlockContents contents;
    if (block_cache != NULL) {
      char cache_key_buffer[16];
      EncodeFixed64(cache_key_buffer, table->rep_->cache_id);
      EncodeFixed64(cache_key_buffer+8, handle.offset());
      Slice key(cache_key_buffer, sizeof(cache_key_buffer));
      cache_handle = block_cache->Lookup(key);
      if (cache_handle != NULL) {
        block = reinterpret_cast<Block*>(block_cache->Value(cache_handle));
      } else {
        s = ReadBlock(table->rep_->file, options, handle, &contents);
        if (s.ok()) {
          block = new Block(contents);
          if (contents.cachable && options.fill_cache) {
            cache_handle = block_cache->Insert(
                key, block, block->size(), &DeleteCachedBlock);
          }
        }
      }
    } else {
      s = ReadBlock(table->rep_->file, options, handle, &contents);
      if (s.ok()) {
        block = new Block(contents);
      }
    }
  }

  Iterator* iter;
  if (block != NULL) {
    iter = block->NewIterator(table->rep_->options.comparator);
    if (cache_handle == NULL) {
      iter->RegisterCleanup(&DeleteBlock, block, NULL);
    } else {
      iter->RegisterCleanup(&ReleaseBlock, block_cache, cache_handle);
    }
  } else {
    iter = NewErrorIterator(s);
  }
  return iter;
}

Iterator* Table::NewIterator(const ReadOptions& options) const {
  return NewTwoLevelIterator(
      rep_->index_block->NewIterator(rep_->options.comparator),
      &Table::BlockReader, const_cast<Table*>(this), options);
}

Status Table::InternalGet(const ReadOptions& options, const Slice& k,
                          void* arg,
                          void (*saver)(void*, const Slice&, const Slice&)) {
  Status s;
  Iterator* iiter = rep_->index_block->NewIterator(rep_->options.comparator);
  iiter->Seek(k);
  if (iiter->Valid()) {
    Slice handle_value = iiter->value();
    FilterBlockReader* filter = rep_->filter;
    BlockHandle handle;
    if (filter != NULL &&
        handle.DecodeFrom(&handle_value).ok() &&
        !filter->KeyMayMatch(handle.offset(), k)) {
      // Not found
    } else {
      Iterator* block_iter = BlockReader(this, options, iiter->value());
      block_iter->Seek(k);
      if (block_iter->Valid()) {
        (*saver)(arg, block_iter->key(), block_iter->value());
      }
      s = block_iter->status();
      delete block_iter;
    }
  }
  if (s.ok()) {
    s = iiter->status();
  }
  delete iiter;
  return s;
}

Status Table::InternalMultiGet(const ReadOptions& options,
                               std::vector<TableBatchItem>* items) {
  const Comparator* cmp = rep_->options.comparator;
  Cache* block_cache = rep_->options.block_cache;
  Iterator* iiter = rep_->index_block->NewIterator(cmp);

  // Call-scoped sharing of data blocks.  A block needed by several lookup
  // items is read and checksum-verified at most once, independently of the
  // permanent block cache capacity and of fill_cache.  Everything is
  // released before this method returns.
  std::vector<Block*> owned_blocks;
  std::vector<Cache::Handle*> pinned_handles;
  std::map<uint64_t, Block*> shared_blocks;  // keyed by block offset

  size_t i = 0;
  const size_t n = items->size();
  while (i < n) {
    TableBatchItem& first = (*items)[i];
    if (first.done) {
      ++i;
      continue;
    }

    // Gallop the index iterator forward to the block covering first.
    iiter->SeekForward(first.internal_key);
    if (!iiter->Valid()) {
      // No index entry at or past this key: it cannot be in this table.
      first.done = true;
      ++i;
      continue;
    }

    // Every still-pending item not greater than the current index separator
    // routes to the same data block; gather them as one group.
    const Slice separator = iiter->key();
    size_t j = i + 1;
    while (j < n && !(*items)[j].done &&
           cmp->Compare(separator, (*items)[j].internal_key) >= 0) {
      ++j;
    }

    Slice handle_value = iiter->value();
    BlockHandle handle;
    bool handle_ok = handle.DecodeFrom(&handle_value).ok();
    FilterBlockReader* filter = rep_->filter;

    // Filter membership is per key, even though the items share one block.
    // Only skip the block read when every item in the group is rejected;
    // otherwise load it once and honor per-item rejections individually.
    bool all_filter_rejected = true;
    if (filter == NULL || !handle_ok) {
      all_filter_rejected = false;
    } else {
      for (size_t k = i; k < j; k++) {
        if (filter->KeyMayMatch(handle.offset(),
                                (*items)[k].internal_key)) {
          all_filter_rejected = false;
          break;
        }
      }
    }

    if (all_filter_rejected) {
      for (size_t k = i; k < j; k++) {
        (*items)[k].done = true;
      }
    } else if (!handle_ok) {
      Status err = Status::Corruption("bad block handle");
      for (size_t k = i; k < j; k++) {
        (*items)[k].status = err;
        (*items)[k].done = true;
      }
    } else {
      Block* block = NULL;
      std::map<uint64_t, Block*>::iterator found =
          shared_blocks.find(handle.offset());
      if (found != shared_blocks.end()) {
        block = found->second;
      } else {
        // Mirror Table::BlockReader's permanent-cache/fill_cache behavior,
        // then keep the block in the call-scoped map as well.
        char cache_key_buffer[16];
        Slice cache_key;
        Cache::Handle* cache_handle = NULL;
        if (block_cache != NULL) {
          EncodeFixed64(cache_key_buffer, rep_->cache_id);
          EncodeFixed64(cache_key_buffer + 8, handle.offset());
          cache_key = Slice(cache_key_buffer, sizeof(cache_key_buffer));
          cache_handle = block_cache->Lookup(cache_key);
          if (cache_handle != NULL) {
            block = reinterpret_cast<Block*>(block_cache->Value(cache_handle));
            pinned_handles.push_back(cache_handle);
          }
        }
        if (block == NULL) {
          BlockContents contents;
          Status read_status =
              ReadBlock(rep_->file, options, handle, &contents);
          if (!read_status.ok()) {
            // A read/checksum failure is an error for these keys, not a miss
            // that would allow older versions to surface.
            for (size_t k = i; k < j; k++) {
              (*items)[k].status = read_status;
              (*items)[k].done = true;
            }
            i = j;
            continue;
          }
          block = new Block(contents);
          if (block_cache != NULL && contents.cachable &&
              options.fill_cache) {
            cache_handle = block_cache->Insert(
                cache_key, block, block->size(), &DeleteCachedBlock);
            pinned_handles.push_back(cache_handle);
          } else {
            owned_blocks.push_back(block);
          }
        }
        shared_blocks[handle.offset()] = block;
      }

      Iterator* block_iter = block->NewIterator(cmp);
      for (size_t k = i; k < j; k++) {
        TableBatchItem& item = (*items)[k];
        if (filter != NULL &&
            !filter->KeyMayMatch(handle.offset(), item.internal_key)) {
          // Definitively absent from this block; leave the saver untouched
          // so the version layer continues into older files.  Do not move
          // the block iterator, preserving the forward ordering.
          item.done = true;
          continue;
        }
        block_iter->SeekForward(item.internal_key);
        if (block_iter->Valid()) {
          (*item.handle_result)(item.arg, block_iter->key(),
                                block_iter->value());
        }
        Status bs = block_iter->status();
        if (!bs.ok()) {
          item.status = bs;
        }
        item.done = true;
      }
      delete block_iter;
    }
    i = j;
  }

  Status s = iiter->status();
  delete iiter;
  for (size_t k = 0; k < owned_blocks.size(); k++) {
    delete owned_blocks[k];
  }
  for (size_t k = 0; k < pinned_handles.size(); k++) {
    block_cache->Release(pinned_handles[k]);
  }
  return s;
}


uint64_t Table::ApproximateOffsetOf(const Slice& key) const {
  Iterator* index_iter =
      rep_->index_block->NewIterator(rep_->options.comparator);
  index_iter->Seek(key);
  uint64_t result;
  if (index_iter->Valid()) {
    BlockHandle handle;
    Slice input = index_iter->value();
    Status s = handle.DecodeFrom(&input);
    if (s.ok()) {
      result = handle.offset();
    } else {
      // Strange: we can't decode the block handle in the index block.
      // We'll just return the offset of the metaindex block, which is
      // close to the whole file size for this case.
      result = rep_->metaindex_handle.offset();
    }
  } else {
    // key is past the last key in the file.  Approximate the offset
    // by returning the offset of the metaindex block (which is
    // right near the end of the file).
    result = rep_->metaindex_handle.offset();
  }
  delete index_iter;
  return result;
}

}  // namespace leveldb
