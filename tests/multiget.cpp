// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

// Functional and routing tests for DBImpl::MultiGet().  Split into four
// groups selected by multiget_case(): versions, routing, blocks, errors.

#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "cases.h"
#include "db/db_impl.h"
#include "db/filename.h"
#include "leveldb/cache.h"
#include "leveldb/db.h"
#include "leveldb/env.h"
#include "leveldb/filter_policy.h"
#include "leveldb/options.h"
#include "leveldb/slice.h"
#include "leveldb/status.h"
#include "routing_fixture.h"

namespace {

using namespace leveldb;

// CHECK_CHECK returns -1, which is only valid from the case functions.  For
// void helpers use this variant, which simply returns on failure.
#define CHECK_VOID(x) \
  do { if (!unit_check(!!(x), #x, __FILE__, __LINE__)) return; } while (0)

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static std::string WorkDir(const char* group) {
  return std::string("multiget_work/") + group;
}

static void EnsureWorkDir() {
  // CreateDir is not recursive; make sure the common parent exists.
  Env::Default()->CreateDir("multiget_work");
}

static void Cleanup(const std::string& dir, const Options& o) {
  DestroyDB(dir, o);
}

// Reverse-order comparator: exercises a non-default user comparator.
class ReverseComparator : public Comparator {
 public:
  const char* Name() const override { return "leveldb.ReverseTestComparator"; }
  int Compare(const Slice& a, const Slice& b) const override {
    return -a.compare(b);
  }
  void FindShortestSeparator(std::string*, const Slice&) const override {}
  void FindShortSuccessor(std::string*) const override {}
};

// 4-byte big-endian binary keys with embedded NUL / high bytes.
static std::string BinKey(unsigned n) {
  std::string s(4, '\0');
  for (int i = 3; i >= 0; --i) {
    s[i] = static_cast<char>(n & 0xff);
    n >>= 8;
  }
  return s;
}

static bool IsTableFile(const std::string& path) {
  size_t slash = path.find_last_of('/');
  std::string name = (slash == std::string::npos) ? path
                                                   : path.substr(slash + 1);
  return name.size() > 4 &&
         (name.compare(name.size() - 4, 4, ".sst") == 0 ||
          name.compare(name.size() - 4, 4, ".ldb") == 0);
}

// Run MultiGet and return its status vector for easy inspection.
static std::vector<Status> RunMultiGet(
    DB* db, const std::vector<Slice>& keys,
    std::vector<std::string>* values,
    const Snapshot* snapshot = NULL,
    bool verify_checksums = true,
    bool fill_cache = false) {
  ReadOptions ro;
  ro.snapshot = snapshot;
  ro.verify_checksums = verify_checksums;
  ro.fill_cache = fill_cache;
  return db->MultiGet(ro, keys, values);
}

static Status OneGet(DB* db, const Slice& key, std::string* value,
                     const Snapshot* snapshot = NULL) {
  ReadOptions ro;
  ro.snapshot = snapshot;
  ro.verify_checksums = true;
  ro.fill_cache = false;
  return db->Get(ro, key, value);
}

// ---------------------------------------------------------------------------
// Counting Env: counts RandomAccessFile::Read calls / bytes / unique offsets.
// ---------------------------------------------------------------------------

class ReadCountingEnv;
class ReadCountFile;

class ReadCountingEnv : public EnvWrapper {
 public:
  mutable std::atomic<unsigned long long> reads;
  mutable std::atomic<unsigned long long> bytes;
  mutable std::mutex mu;
  mutable std::set<std::pair<std::string, uint64_t> > offsets;

  ReadCountingEnv() : EnvWrapper(Env::Default()), reads(0), bytes(0) {}

  void RecordRead(const std::string& p, uint64_t off, size_t n) const {
    reads++;
    bytes += n;
    std::lock_guard<std::mutex> l(mu);
    offsets.insert(std::make_pair(p, off));
  }
  void Reset() {
    reads = 0;
    bytes = 0;
    std::lock_guard<std::mutex> l(mu);
    offsets.clear();
  }
  Status NewRandomAccessFile(const std::string& p,
                             RandomAccessFile** out) override;
};

class ReadCountFile : public RandomAccessFile {
 public:
  ReadCountFile(RandomAccessFile* f, Env* base, const std::string& p)
      : file_(f), base_(base), path_(p) {}
  ~ReadCountFile() override { delete file_; }
  Status Read(uint64_t off, size_t n, Slice* out, char* scratch)
      const override {
    ReadCountingEnv* env = reinterpret_cast<ReadCountingEnv*>(base_);
    env->RecordRead(path_, off, n);
    return file_->Read(off, n, out, scratch);
  }

 private:
  RandomAccessFile* file_;
  Env* base_;
  std::string path_;
};

Status ReadCountingEnv::NewRandomAccessFile(const std::string& p,
                                            RandomAccessFile** out) {
  RandomAccessFile* f = NULL;
  Status s = target()->NewRandomAccessFile(p, &f);
  *out = s.ok() ? new ReadCountFile(f, this, p) : NULL;
  return s;
}

// ---------------------------------------------------------------------------
// Barrier Env: block table-file opens until released, to freeze a flush with
// an immutable memtable in flight.
// ---------------------------------------------------------------------------

class BarrierEnv : public EnvWrapper {
 public:
  std::mutex mu;
  std::condition_variable cv;
  bool gate_open;
  unsigned blocked;

  explicit BarrierEnv(Env* base)
      : EnvWrapper(base), gate_open(true), blocked(0) {}

  void CloseGate() {
    std::unique_lock<std::mutex> l(mu);
    gate_open = false;
  }
  void OpenGate() {
    std::unique_lock<std::mutex> l(mu);
    gate_open = true;
    cv.notify_all();
  }
  bool WaitUntilBlocked(unsigned want) {
    std::unique_lock<std::mutex> l(mu);
    cv.wait(l, [&] { return blocked >= want; });
    return blocked >= want;
  }

  Status NewRandomAccessFile(const std::string& p,
                             RandomAccessFile** out) override {
    if (IsTableFile(p)) {
      std::unique_lock<std::mutex> l(mu);
      blocked++;
      cv.notify_all();
      cv.wait(l, [&] { return gate_open; });
    }
    return target()->NewRandomAccessFile(p, out);
  }
};

// ---------------------------------------------------------------------------
// Fault Env: the next N table-file Read() calls fail once, then recover.
// ---------------------------------------------------------------------------

class FaultFile : public RandomAccessFile {
 public:
  FaultFile(RandomAccessFile* f, std::atomic<int>* remaining)
      : file_(f), remaining_(remaining) {}
  ~FaultFile() override { delete file_; }
  Status Read(uint64_t off, size_t n, Slice* out, char* scratch)
      const override {
    if (remaining_->load() > 0) {
      remaining_->fetch_sub(1);
      return Status::IOError("injected transient read error", "fault");
    }
    return file_->Read(off, n, out, scratch);
  }

 private:
  RandomAccessFile* file_;
  std::atomic<int>* remaining_;
};

class FaultEnv : public EnvWrapper {
 public:
  std::atomic<int> fail_remaining;
  FaultEnv() : EnvWrapper(Env::Default()), fail_remaining(0) {}
  void Arm(int count) { fail_remaining = count; }
  Status NewRandomAccessFile(const std::string& p,
                             RandomAccessFile** out) override {
    RandomAccessFile* f = NULL;
    Status s = target()->NewRandomAccessFile(p, &f);
    *out = (s.ok() && IsTableFile(p)) ? new FaultFile(f, &fail_remaining) : f;
    return s;
  }
};

}  // namespace

// ===========================================================================
// versions group: mem/imm coverage, snapshots, L0 deletes, ordering/dups,
// binary keys, reverse comparator, and the read-barrier consistency check.
// ===========================================================================

static int VersionsGroup() {
  int cases = 0;
  const std::string dir = WorkDir("versions");
  Options o;
  o.create_if_missing = true;
  o.compression = kNoCompression;
  DB* db = NULL;
  CHECK_CHECK(DB::Open(o, dir, &db).ok());
  DBImpl* impl = reinterpret_cast<DBImpl*>(db);

  // Memtable coverage with shuffled and duplicated keys.
  {
    const char* keys[] = {"alpha", "beta", "gamma", "delta", "beta", "alpha"};
    std::vector<std::string> storage;
    std::vector<Slice> sl;
    for (const char* k : keys) {
      storage.push_back(k);
      CHECK_CHECK(db->Put(WriteOptions(), k,
                          std::string("v:") + k).ok());
    }
    for (auto& s : storage) sl.push_back(s);
    std::vector<std::string> vals;
    std::vector<Status> st = RunMultiGet(db, sl, &vals);
    CHECK_CHECK(st.size() == sl.size());
    for (size_t i = 0; i < sl.size(); ++i) {
      CHECK_CHECK(st[i].ok());
      CHECK_CHECK(vals[i] == std::string("v:") + sl[i].ToString());
      ++cases;
    }
  }

  // Immutable memtable coverage via a flush frozen behind a barrier.
  {
    BarrierEnv env(Env::Default());
    const std::string d2 = dir + "/imm";
    Options bo;
    bo.create_if_missing = true;
    bo.env = &env;
    bo.compression = kNoCompression;
    DB* bdb = NULL;
    CHECK_CHECK(DB::Open(bo, d2, &bdb).ok());
    DBImpl* bimpl = reinterpret_cast<DBImpl*>(bdb);

    // Seed an L2-resident key and warm its table handle.
    CHECK_CHECK(bdb->Put(WriteOptions(), "base", "old-base").ok());
    CHECK_CHECK(bimpl->TEST_CompactMemTable().ok());
    std::string warm;
    CHECK_CHECK(OneGet(bdb, "base", &warm).ok() && warm == "old-base");

    // Overwrites land in the current memtable and are frozen into imm.
    for (int i = 0; i < 8; ++i) {
      char k[16], v[32];
      snprintf(k, sizeof k, "imm%02d", i);
      snprintf(v, sizeof v, "imm-val-%02d", i);
      CHECK_CHECK(bdb->Put(WriteOptions(), k, v).ok());
    }

    env.CloseGate();
    Status flush_status;
    std::thread flusher([&] {
      flush_status = bimpl->TEST_CompactMemTable();
    });
    CHECK_CHECK(env.WaitUntilBlocked(1));  // Flush stalled with imm present.

    // Reads proceed without the global write lock and without opening the
    // gated table file; imm and warm L2 data are seen from one view.
    std::vector<std::string> sk;
    std::vector<Slice> sl;
    sk.push_back("base");
    for (int i = 0; i < 8; ++i) {
      char k[16];
      snprintf(k, sizeof k, "imm%02d", i);
      sk.push_back(k);
    }
    for (auto& s : sk) sl.push_back(s);
    std::vector<std::string> vals;
    std::vector<Status> st = RunMultiGet(bdb, sl, &vals);
    CHECK_CHECK(st.size() == sl.size());
    CHECK_CHECK(st[0].ok() && vals[0] == "old-base");
    for (int i = 0; i < 8; ++i) {
      char v[32];
      snprintf(v, sizeof v, "imm-val-%02d", i);
      CHECK_CHECK(st[i + 1].ok() && vals[i + 1] == v);
      ++cases;
    }

    env.OpenGate();
    flusher.join();
    CHECK_CHECK(flush_status.ok());

    // After the flush completes, the same batch gives identical answers.
    std::vector<std::string> vals2;
    std::vector<Status> st2 = RunMultiGet(bdb, sl, &vals2);
    for (size_t i = 0; i < st2.size(); ++i) {
      CHECK_CHECK(st2[i].ok() && vals2[i] == vals[i]);
      ++cases;
    }
    delete bdb;
    Cleanup(d2, bo);
  }

  // L0 new delete vs. an older snapshot, plus snapshot vs. current values.
  {
    CHECK_CHECK(db->Put(WriteOptions(), "l0k", "v1").ok());
    CHECK_CHECK(impl->TEST_CompactMemTable().ok());
    const Snapshot* s1 = db->GetSnapshot();
    CHECK_CHECK(db->Put(WriteOptions(), "l0k", "v2").ok());
    CHECK_CHECK(impl->TEST_CompactMemTable().ok());  // Overlapping => L0.
    const Snapshot* s2 = db->GetSnapshot();
    CHECK_CHECK(db->Delete(WriteOptions(), "l0k").ok());
    CHECK_CHECK(impl->TEST_CompactMemTable().ok());  // Newest L0 tombstone.

    std::vector<Slice> sl;
    std::string k = "l0k";
    sl.push_back(k);
    std::vector<std::string> v;

    std::vector<Status> cur = RunMultiGet(db, sl, &v);
    CHECK_CHECK(cur[0].IsNotFound() && v[0].empty());
    ++cases;

    std::vector<Status> at2 = RunMultiGet(db, sl, &v, s2);
    CHECK_CHECK(at2[0].ok() && v[0] == "v2");
    ++cases;

    std::vector<Status> at1 = RunMultiGet(db, sl, &v, s1);
    CHECK_CHECK(at1[0].ok() && v[0] == "v1");
    ++cases;

    // Single-key Get must report the same snapshot semantics.
    std::string g;
    CHECK_CHECK(OneGet(db, k, &g, s1).ok() && g == "v1");
    CHECK_CHECK(OneGet(db, k, &g, s2).ok() && g == "v2");
    CHECK_CHECK(OneGet(db, k, &g).IsNotFound());
    db->ReleaseSnapshot(s1);
    db->ReleaseSnapshot(s2);
  }

  // Binary keys and a reverse comparator.
  {
    const std::string d3 = dir + "/reverse";
    ReverseComparator rcmp;
    Options ro;
    ro.create_if_missing = true;
    ro.comparator = &rcmp;
    ro.compression = kNoCompression;
    DB* rdb = NULL;
    CHECK_CHECK(DB::Open(ro, d3, &rdb).ok());
    std::vector<std::string> storage;
    for (unsigned i = 0; i < 64; ++i) {
      std::string bk = BinKey(i);
      storage.push_back(bk);
      CHECK_CHECK(rdb->Put(WriteOptions(), bk,
                           std::string("val-") + std::to_string(i)).ok());
    }
    // Supply keys in shuffled order, including binary NUL and 0xff bytes.
    std::vector<unsigned> order;
    for (unsigned i = 0; i < 64; ++i) order.push_back(i);
    std::reverse(order.begin(), order.end());
    std::vector<Slice> sl;
    for (unsigned idx : order) sl.push_back(storage[idx]);
    std::vector<std::string> vals;
    std::vector<Status> st = RunMultiGet(rdb, sl, &vals);
    for (size_t i = 0; i < order.size(); ++i) {
      CHECK_CHECK(st[i].ok());
      CHECK_CHECK(vals[i] ==
                  std::string("val-") + std::to_string(order[i]));
      ++cases;
    }
    // A missing binary key yields NotFound and an empty value.
    std::string absent = BinKey(100000);
    std::vector<Slice> asl;
    asl.push_back(absent);
    std::vector<std::string> av;
    std::vector<Status> ast = RunMultiGet(rdb, asl, &av);
    CHECK_CHECK(ast[0].IsNotFound() && av[0].empty());
    ++cases;
    delete rdb;
    Cleanup(d3, ro);
  }

  // Empty / NULL argument edge cases.
  {
    std::vector<Slice> empty;
    std::vector<std::string> vals;
    std::vector<Status> st = RunMultiGet(db, empty, &vals);
    CHECK_CHECK(st.empty() && vals.empty());
    ++cases;
    std::vector<Slice> one;
    std::string k = "alpha";
    one.push_back(k);
    std::vector<Status> null_st = db->MultiGet(ReadOptions(), one, NULL);
    CHECK_CHECK(null_st.size() == 1 && null_st[0].IsInvalidArgument());
    ++cases;
  }

  delete db;
  Cleanup(dir, o);
  CHECK_CHECK(cases > 0);
  return cases;
}

// ===========================================================================
// routing group: the four routing_fixture materials against real counters.
// ===========================================================================

static int RoutingGroup() {
  int cases = 0;
  const std::string root = WorkDir("routing");
  Env::Default()->CreateDir(root);

  struct Shape {
    bool files;
    bool sparse;
    double max_cmp_ratio;
  };
  Shape shapes[4] = {
      {true, false, 0.70}, {true, true, 0.70},
      {false, false, 0.65}, {false, true, 0.80}};

  for (int s = 0; s < 4; ++s) {
    const bool files = shapes[s].files;
    const bool sparse = shapes[s].sparse;
    const std::string path = root + (files ? (sparse ? "/fs" : "/fd")
                                           : (sparse ? "/bs" : "/bd"));
    DestroyDB(path, Options());  // Ensure a clean, repeatable fixture.
    devcheck::RoutingFixture f(path, files);
    if (sparse) {
      for (unsigned i = 0; i < 512; ++i) {
        f.keys[i] = devcheck::RoutingKey(i * (files ? 2u : 16u));
      }
      f.slices.clear();
      for (auto& k : f.keys) f.slices.push_back(k);
    }

    // Reference single-key path.
    f.Reset();
    f.PointReference();
    devcheck::Counters base = f.Count();

    // Batched path on the same data/snapshot.
    f.Reset();
    std::vector<std::string> values;
    std::vector<Status> st =
        RunMultiGet(f.db(), f.slices, &values);
    devcheck::Counters bat = f.Count();

    // Content parity with the expected per-rank values.
    for (unsigned i = 0; i < 512; ++i) {
      unsigned rank = 0;
      for (int b = 0; b < 4; ++b) {
        rank = (rank << 8) |
               (static_cast<unsigned char>(f.keys[i][b]) ^ 0xa5u);
      }
      CHECK_CHECK(st[i].ok());
      CHECK_CHECK(values[i] == devcheck::RoutingValue(rank));
      ++cases;
    }

    const double cmp_ratio = bat.comparisons / (double)base.comparisons;
    const double byte_ratio = bat.bytes / (double)base.bytes;
    CHECK_CHECK(cmp_ratio <= shapes[s].max_cmp_ratio);
    CHECK_CHECK(bat.reads <= 512);
    CHECK_CHECK(bat.unique_reads <= 512);
    CHECK_CHECK(byte_ratio <= 1.10);
    ++cases;
  }
  return cases;
}

// ===========================================================================
// blocks group: same-block sharing, filter rejection, and block independence.
// ===========================================================================

// Build a one-file table whose keys live in many distinct small blocks.
static void BuildBlockDB(const std::string& dir, ReadCountingEnv* env,
                         Cache* block_cache, DB** out,
                         const FilterPolicy* filter = NULL) {
  Options o;
  o.create_if_missing = true;
  o.env = env;
  o.compression = kNoCompression;
  o.block_size = 64;
  o.block_cache = block_cache;
  o.write_buffer_size = 16 << 20;
  o.max_file_size = 16 << 20;
  o.filter_policy = filter;
  DB* db = NULL;
  CHECK_VOID(DB::Open(o, dir, &db).ok());
  DBImpl* impl = reinterpret_cast<DBImpl*>(db);
  for (int i = 0; i < 400; ++i) {
    char k[16], v[64];
    snprintf(k, sizeof k, "key-%04d", i);
    snprintf(v, sizeof v, "value-%04d-%s", i,
             "paddingpaddingpaddingpadding");
    CHECK_VOID(db->Put(WriteOptions(), k, v).ok());
  }
  CHECK_VOID(impl->TEST_CompactMemTable().ok());
  *out = db;
}

// Build a table with large blocks so several adjacent keys share one physical
// data block, while the key range still spans many blocks.
static void BuildSharedBlockDB(const std::string& dir, ReadCountingEnv* env,
                               Cache* block_cache, DB** out) {
  Options o;
  o.create_if_missing = true;
  o.env = env;
  o.compression = kNoCompression;
  o.block_size = 512;  // Multiple small entries per block.
  o.block_cache = block_cache;
  o.write_buffer_size = 16 << 20;
  o.max_file_size = 16 << 20;
  DB* db = NULL;
  CHECK_VOID(DB::Open(o, dir, &db).ok());
  DBImpl* impl = reinterpret_cast<DBImpl*>(db);
  for (int i = 0; i < 2000; ++i) {
    char k[12], v[8];
    snprintf(k, sizeof k, "k%05d", i);
    snprintf(v, sizeof v, "v%05d", i);
    CHECK_VOID(db->Put(WriteOptions(), k, v).ok());
  }
  CHECK_VOID(impl->TEST_CompactMemTable().ok());
  *out = db;
}

static std::string SmallKey(int i) {
  char k[12];
  snprintf(k, sizeof k, "k%05d", i);
  return std::string(k);
}
static std::string SmallValue(int i) {
  char v[8];
  snprintf(v, sizeof v, "v%05d", i);
  return std::string(v);
}

static int BlocksGroup() {
  int cases = 0;
  const std::string dir = WorkDir("blocks");
  Env::Default()->CreateDir(dir);
  Cache* zero_cache = NewLRUCache(0);  // No permanent data-block cache.

  // ---- Same-block sharing ------------------------------------------------
  {
    ReadCountingEnv env;
    DB* db = NULL;
    BuildSharedBlockDB(dir + "/share", &env, zero_cache, &db);

    // Warm table metadata (index/filter) only, never caching data blocks.
    {
      std::string w;
      for (int i = 0; i < 2000; ++i) {
        CHECK_CHECK(OneGet(db, SmallKey(i), &w).ok());
      }
    }

    // Pick two adjacent keys and prove they occupy one physical block.
    env.Reset();
    std::string a = SmallKey(10), b = SmallKey(11);
    {
      std::string v;
      CHECK_CHECK(OneGet(db, a, &v).ok());
      CHECK_CHECK(OneGet(db, b, &v).ok());
      unsigned long long two_key_reads = env.reads.load();
      std::set<std::pair<std::string, uint64_t> > two_key_offsets;
      {
        std::lock_guard<std::mutex> l(env.mu);
        two_key_offsets = env.offsets;
      }
      CHECK_CHECK(two_key_reads == 2);  // Separate Get() => separate reads.
      CHECK_CHECK(two_key_offsets.size() == 1);  // Same block offset.
      ++cases;

      // One MultiGet for the same two keys (plus repeats) loads it once.
      env.Reset();
      std::vector<std::string> ks;
      ks.push_back(a);
      ks.push_back(b);
      ks.push_back(a);  // Duplicate logical key reuses the same block.
      ks.push_back(b);
      std::vector<Slice> sl;
      for (auto& k : ks) sl.push_back(k);
      std::vector<std::string> vals;
      std::vector<Status> st = RunMultiGet(db, sl, &vals);
      for (auto& s : st) CHECK_CHECK(s.ok());
      CHECK_CHECK(vals[0] == SmallValue(10));
      CHECK_CHECK(vals[1] == SmallValue(11));
      CHECK_CHECK(vals[2] == SmallValue(10));
      CHECK_CHECK(vals[3] == SmallValue(11));
      CHECK_CHECK(env.reads.load() == 1);  // Single block, single load.
      {
        std::lock_guard<std::mutex> l(env.mu);
        CHECK_CHECK(env.offsets.size() == 1);
      }
      ++cases;
    }

    // Scattered keys in distinct blocks stay independent: one read each.
    {
      env.Reset();
      std::vector<std::string> ks;
      for (int i = 0; i < 2000; i += 137) ks.push_back(SmallKey(i));
      std::vector<Slice> sl;
      for (auto& k : ks) sl.push_back(k);
      std::vector<std::string> vals;
      std::vector<Status> st = RunMultiGet(db, sl, &vals);
      CHECK_CHECK(st.size() == sl.size());
      for (auto& s : st) CHECK_CHECK(s.ok());
      CHECK_CHECK(env.reads.load() == sl.size());
      ++cases;
    }
    delete db;
    Cleanup(dir + "/share", Options());
  }

  // ---- Filter rejection --------------------------------------------------
  {
    const FilterPolicy* bloom = NewBloomFilterPolicy(10);
    ReadCountingEnv fenv;
    DB* fdb = NULL;
    BuildBlockDB(dir + "/filter", &fenv, zero_cache, &fdb, bloom);
    std::string w;
    for (int i = 0; i < 400; ++i) {
      char k[16];
      snprintf(k, sizeof k, "key-%04d", i);
      CHECK_CHECK(OneGet(fdb, k, &w).ok());
    }
    fenv.Reset();
    std::vector<std::string> absent;
    for (int i = 0; i < 16; ++i) {
      char k[32];
      snprintf(k, sizeof k, "definitely-absent-%04d-xxxx", i);
      absent.push_back(k);
    }
    std::vector<Slice> sl;
    for (auto& k : absent) sl.push_back(k);
    std::vector<std::string> vals;
    std::vector<Status> st = RunMultiGet(fdb, sl, &vals);
    for (auto& s : st) CHECK_CHECK(s.IsNotFound());
    for (auto& v : vals) CHECK_CHECK(v.empty());
    CHECK_CHECK(fenv.reads.load() == 0);  // Filter rejected every lookup.
    ++cases;

    // A present key mixed with rejected keys still resolves correctly.
    fenv.Reset();
    std::vector<std::string> mixed;
    mixed.push_back("key-0010");
    mixed.push_back("nope-nope-nope");
    std::vector<Slice> msl;
    for (auto& k : mixed) msl.push_back(k);
    std::vector<std::string> mv;
    std::vector<Status> mst = RunMultiGet(fdb, msl, &mv);
    CHECK_CHECK(mst[0].ok() && mv[0].find("value-0010") == 0);
    CHECK_CHECK(mst[1].IsNotFound() && mv[1].empty());
    ++cases;
    delete fdb;
    delete bloom;
    Cleanup(dir + "/filter", Options());
  }

  delete zero_cache;
  CHECK_CHECK(cases > 0);
  return cases;
}

// ===========================================================================
// errors group: corrupt block, filter/IO errors, partial batch failure, and
// recovery after a transient error.
// ===========================================================================

// Overwrite a byte inside the first data block while leaving the footer and
// index block intact, forcing a checksum mismatch under verify_checksums.
static Status CorruptFirstDataBlock(const std::string& dbname) {
  Env* env = Env::Default();
  std::vector<std::string> files;
  Status s = env->GetChildren(dbname, &files);
  if (!s.ok()) return s;
  std::string target;
  for (auto& name : files) {
    if (IsTableFile(name)) { target = dbname + "/" + name; break; }
  }
  if (target.empty()) {
    return Status::Corruption("no table file found");
  }
  std::string contents;
  s = ReadFileToString(env, target, &contents);
  if (!s.ok()) return s;
  if (contents.size() <= 32) {
    return Status::Corruption("table file too small");
  }
  contents[10] = static_cast<char>(contents[10] ^ 0xff);
  return WriteStringToFile(env, contents, target);
}

static int ErrorsGroup() {
  int cases = 0;
  const std::string dir = WorkDir("errors");
  Env::Default()->CreateDir(dir);

  // Corrupt block: one key errors while the rest of the batch succeeds.
  {
    ReadCountingEnv env;
    Cache* zero = NewLRUCache(0);
    const std::string d = dir + "/corrupt";
    DB* db = NULL;
    BuildBlockDB(d, &env, zero, &db);
    std::string w;
    for (int i = 0; i < 400; ++i) {
      char k[16];
      snprintf(k, sizeof k, "key-%04d", i);
      CHECK_CHECK(OneGet(db, k, &w).ok());
    }
    delete db;  // Release handles before mutating the file.

    CHECK_CHECK(CorruptFirstDataBlock(d).ok());

    Options ro;
    ro.create_if_missing = false;
    ro.env = &env;
    ro.block_cache = zero;
    ro.compression = kNoCompression;
    DB* reopened = NULL;
    CHECK_CHECK(DB::Open(ro, d, &reopened).ok());

    std::vector<std::string> ks;
    ks.push_back("key-0000");  // Lives in the corrupted first block.
    ks.push_back("key-0100");
    ks.push_back("key-0300");
    std::vector<Slice> sl;
    for (auto& k : ks) sl.push_back(k);
    std::vector<std::string> vals;
    std::vector<Status> st = RunMultiGet(reopened, sl, &vals, NULL, true);
    CHECK_CHECK(!st[0].ok() && st[0].IsCorruption());
    CHECK_CHECK(vals[0].empty());
    CHECK_CHECK(st[1].ok() && vals[1].find("value-0100") == 0);
    CHECK_CHECK(st[2].ok() && vals[2].find("value-0300") == 0);
    ++cases;

    // The corruption must not be treated as a miss and fall through: there
    // is no older value to surface, so it stays an error on retry.
    std::vector<std::string> v2;
    std::vector<Status> st2 = RunMultiGet(reopened, sl, &v2, NULL, true);
    CHECK_CHECK(st2[0].IsCorruption());
    ++cases;
    delete reopened;
    Cleanup(d, ro);
    delete zero;
  }

  // Recoverable I/O error: the failed key reports an IOError, the other keys
  // succeed, and a later retry after recovery succeeds.
  {
    FaultEnv env;
    Cache* zero = NewLRUCache(0);
    const std::string d = dir + "/io";
    Options o;
    o.create_if_missing = true;
    o.env = &env;
    o.block_cache = zero;
    o.compression = kNoCompression;
    o.block_size = 64;
    DB* db = NULL;
    CHECK_CHECK(DB::Open(o, d, &db).ok());
    DBImpl* impl = reinterpret_cast<DBImpl*>(db);
    for (int i = 0; i < 200; ++i) {
      char k[16], v[48];
      snprintf(k, sizeof k, "io-%04d", i);
      snprintf(v, sizeof v, "iov-%04d-paddingpadding", i);
      CHECK_CHECK(db->Put(WriteOptions(), k, v).ok());
    }
    CHECK_CHECK(impl->TEST_CompactMemTable().ok());
    std::string w;
    for (int i = 0; i < 200; ++i) {
      char k[16];
      snprintf(k, sizeof k, "io-%04d", i);
      CHECK_CHECK(OneGet(db, k, &w).ok());
    }

    std::vector<std::string> ks;
    for (int i = 0; i < 200; i += 25) {
      char k[16];
      snprintf(k, sizeof k, "io-%04d", i);
      ks.push_back(k);
    }
    std::vector<Slice> sl;
    for (auto& k : ks) sl.push_back(k);

    env.Arm(1);  // The first data-block read fails once, then recovers.
    std::vector<std::string> vals;
    std::vector<Status> st = RunMultiGet(db, sl, &vals, NULL, true);
    // The failed block serves one or more keys; all of them see the I/O
    // error with an empty value, while keys in other blocks still resolve.
    int err_count = 0;
    int ok_count = 0;
    for (size_t i = 0; i < st.size(); ++i) {
      if (st[i].ok()) {
        char v[48];
        snprintf(v, sizeof v, "iov-%04d-paddingpadding",
                 static_cast<int>(i) * 25);
        CHECK_CHECK(vals[i] == v);
        ++ok_count;
      } else {
        CHECK_CHECK(st[i].IsIOError());
        CHECK_CHECK(vals[i].empty());
        ++err_count;
      }
    }
    CHECK_CHECK(err_count >= 1);                 // The fault was reported.
    CHECK_CHECK(ok_count >= 1);                  // Other keys kept working.
    CHECK_CHECK(err_count + ok_count == static_cast<int>(st.size()));
    ++cases;

    // After the fault is exhausted, a retry reads every key successfully.
    std::vector<std::string> vals2;
    std::vector<Status> st2 = RunMultiGet(db, sl, &vals2, NULL, true);
    for (auto& s : st2) CHECK_CHECK(s.ok());
    for (size_t i = 0; i < ks.size(); ++i) {
      char v[48];
      snprintf(v, sizeof v, "iov-%04d-paddingpadding",
               static_cast<int>(i) * 25);
      CHECK_CHECK(vals2[i] == v);
      ++cases;
    }
    delete db;
    Cleanup(d, o);
    delete zero;
  }

  CHECK_CHECK(cases > 0);
  return cases;
}

// ===========================================================================
// Entry point invoked by support/target_main.cpp.
// ===========================================================================

int multiget_case(const char* group) {
  EnsureWorkDir();
  std::string g = group == NULL ? "" : group;
  if (g == "versions") return VersionsGroup();
  if (g == "routing") return RoutingGroup();
  if (g == "blocks") return BlocksGroup();
  if (g == "errors") return ErrorsGroup();
  return -1;
}
