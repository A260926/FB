// Example: resolving a batch of fact keys with DB::MultiGet().
//
// MultiGet() looks up many keys against one snapshot and shares the routing
// work (level/file selection, table indexes and data-block reads) across the
// whole batch.  It is a public-API example: it only uses <leveldb/db.h>.
//
// Build against an installed or locally built leveldb, e.g.:
//   c++ -std=c++11 -Iinclude multiget_example.cc -L. -lleveldb -lpthread \
//       -o multiget_example
//
// The program creates a temporary database, populates it, and resolves a
// shuffled batch that contains present keys, repeated keys and a missing key.

#include <cstdio>
#include <string>
#include <vector>

#include "leveldb/db.h"

int main(int argc, char** argv) {
  const std::string dbname =
      (argc > 1) ? argv[1] : "multiget_example_db";

  leveldb::Options options;
  options.create_if_missing = true;

  leveldb::DB* db = NULL;
  leveldb::Status status = leveldb::DB::Open(options, dbname, &db);
  if (!status.ok()) {
    fprintf(stderr, "open failed: %s\n", status.ToString().c_str());
    return 1;
  }

  // Populate some dimension rows.
  const int kNum = 100;
  for (int i = 0; i < kNum; ++i) {
    char key[16], value[32];
    snprintf(key, sizeof(key), "fact:%04d", i);
    snprintf(value, sizeof(value), "dimension:%04d", i);
    status = db->Put(leveldb::WriteOptions(), key, value);
    if (!status.ok()) {
      fprintf(stderr, "put failed: %s\n", status.ToString().c_str());
      return 1;
    }
  }

  // Build an out-of-order batch with duplicates and one absent key.  Keys may
  // be shuffled and repeated; ordering and equality follow the comparator.
  std::vector<std::string> key_storage;
  const int wanted[] = {73, 4, 4, 99, 0, 73, 200 /* absent */, 41};
  for (size_t i = 0; i < sizeof(wanted) / sizeof(wanted[0]); ++i) {
    char key[16];
    snprintf(key, sizeof(key), "fact:%04d", wanted[i]);
    key_storage.push_back(key);
  }

  std::vector<leveldb::Slice> keys;
  for (size_t i = 0; i < key_storage.size(); ++i) {
    keys.push_back(key_storage[i]);
  }

  // One call resolves the whole batch on a single snapshot.
  std::vector<std::string> values;
  std::vector<leveldb::Status> statuses =
      db->MultiGet(leveldb::ReadOptions(), keys, &values);

  bool all_good = true;
  for (size_t i = 0; i < keys.size(); ++i) {
    if (statuses[i].ok()) {
      printf("%s -> %s\n", keys[i].ToString().c_str(),
             values[i].c_str());
    } else if (statuses[i].IsNotFound()) {
      printf("%s -> <not found>\n", keys[i].ToString().c_str());
    } else {
      // An error for one key never prevents the other keys from resolving.
      printf("%s -> ERROR %s\n", keys[i].ToString().c_str(),
             statuses[i].ToString().c_str());
      all_good = false;
    }
  }

  // Edge cases: NULL output and an empty batch.
  std::vector<leveldb::Status> null_statuses =
      db->MultiGet(leveldb::ReadOptions(), keys, NULL);
  if (null_statuses.size() != keys.size() ||
      !null_statuses[0].IsInvalidArgument()) {
    fprintf(stderr, "expected InvalidArgument for NULL values\n");
    all_good = false;
  }

  std::vector<leveldb::Slice> no_keys;
  std::vector<std::string> no_values;
  std::vector<leveldb::Status> no_statuses =
      db->MultiGet(leveldb::ReadOptions(), no_keys, &no_values);
  if (!no_statuses.empty() || !no_values.empty()) {
    fprintf(stderr, "expected empty results for an empty batch\n");
    all_good = false;
  }

  delete db;
  printf(all_good ? "EXAMPLE_OK\n" : "EXAMPLE_FAILED\n");
  return all_good ? 0 : 1;
}
