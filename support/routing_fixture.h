#ifndef CHECK_ROUTING_FIXTURE_H
#define CHECK_ROUTING_FIXTURE_H
#include <atomic>
#include <memory>
#include <mutex>
#include <set>
#include <utility>
#include <string>
#include <vector>
#include "leveldb/comparator.h"
#include "leveldb/db.h"
#include "leveldb/env.h"
namespace devcheck {
// Transparent diagnostics: no query results or routing implementation here.
class CountingComparator : public leveldb::Comparator {
 public:
  mutable std::atomic<unsigned long long> comparisons;
  CountingComparator() : comparisons(0) {}
  const char* Name() const override { return "devcheck.xor-order.v1"; }
  int Compare(const leveldb::Slice&, const leveldb::Slice&) const override;
  void FindShortestSeparator(std::string*, const leveldb::Slice&) const override {}
  void FindShortSuccessor(std::string*) const override {}
};
struct Counters { unsigned long long comparisons, reads, bytes, unique_reads; };
class CountingEnv : public leveldb::EnvWrapper {
 public:
  mutable std::atomic<unsigned long long> reads, bytes;
  mutable std::mutex mutex;
  mutable std::set<std::pair<std::string,uint64_t>> offsets;
  CountingEnv() : EnvWrapper(leveldb::Env::Default()), reads(0), bytes(0) {}
  void Record(const std::string&, uint64_t, size_t) const;
  leveldb::Status NewRandomAccessFile(const std::string&, leveldb::RandomAccessFile**) override;
};
class RoutingFixture {
 public:
  CountingComparator comparator;
  CountingEnv env;
  std::vector<std::string> keys;
  std::vector<leveldb::Slice> slices;
  explicit RoutingFixture(const std::string& root, bool files);
  ~RoutingFixture();
  leveldb::DB* db() { return db_.get(); }
  void Reset();
  Counters Count() const;
  void PointReference();
 private:
  leveldb::Cache* cache_;
  std::unique_ptr<leveldb::DB> db_;
};
std::string RoutingKey(unsigned rank);
std::string RoutingValue(unsigned rank);
}
#endif
