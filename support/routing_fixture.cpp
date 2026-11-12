#include "routing_fixture.h"
#include <cassert>
#include <cstdio>
#include "db/db_impl.h"
#include "leveldb/cache.h"
namespace devcheck {
namespace {
unsigned Rank(const leveldb::Slice& s) {
  assert(s.size()==4);
  unsigned n=0;
  for(int i=0;i<4;++i) n=(n<<8)|(static_cast<unsigned char>(s[i])^0xa5u);
  return n;
}
class CountedFile : public leveldb::RandomAccessFile {
  leveldb::RandomAccessFile* file_;
  CountingEnv* env_;
  std::string path_;
 public:
  CountedFile(leveldb::RandomAccessFile* f,CountingEnv* e,const std::string& p):file_(f),env_(e),path_(p){}
  ~CountedFile() override { delete file_; }
  leveldb::Status Read(uint64_t off,size_t n,leveldb::Slice* out,char* scratch) const override {
    env_->Record(path_,off,n); return file_->Read(off,n,out,scratch);
  }
};
}
std::string RoutingKey(unsigned rank) {
  std::string s(4,'\0');
  for(int i=3;i>=0;--i){s[i]=static_cast<char>((rank&255)^0xa5u);rank>>=8;}
  return s;
}
std::string RoutingValue(unsigned rank) { return std::string(1100,'v')+std::to_string(rank); }
int CountingComparator::Compare(const leveldb::Slice& a,const leveldb::Slice& b) const {
  ++comparisons; unsigned x=Rank(a),y=Rank(b); return x<y?-1:x>y?1:0;
}
leveldb::Status CountingEnv::NewRandomAccessFile(const std::string& path,leveldb::RandomAccessFile** out){
  leveldb::RandomAccessFile* f=nullptr;
  auto s=target()->NewRandomAccessFile(path,&f);
  *out=s.ok()?new CountedFile(f,this,path):nullptr;return s;
}
void CountingEnv::Record(const std::string& p,uint64_t off,size_t n)const{
 ++reads;bytes+=n;std::lock_guard<std::mutex> lock(mutex);offsets.insert({p,off});
}
RoutingFixture::RoutingFixture(const std::string& root,bool files):cache_(leveldb::NewLRUCache(0)){
  leveldb::Options o;o.create_if_missing=true;o.comparator=&comparator;o.env=&env;
  o.compression=leveldb::kNoCompression;o.block_cache=cache_;o.block_size=64;
  o.write_buffer_size=16<<20;o.max_file_size=16<<20;o.max_open_files=2048;
  leveldb::DB* raw=nullptr;assert(leveldb::DB::Open(o,root,&raw).ok());db_.reset(raw);
  unsigned total=files?1024:8192;
  auto impl=static_cast<leveldb::DBImpl*>(raw);
  for(unsigned i=0;i<total;++i){
    assert(raw->Put(leveldb::WriteOptions(),RoutingKey(i),RoutingValue(i)).ok());
    if(files) assert(impl->TEST_CompactMemTable().ok());
  }
  if(!files) assert(impl->TEST_CompactMemTable().ok());
  // No overlapping user ranges: flush output must sit at L2, below size triggers.
  std::string nf;assert(raw->GetProperty("leveldb.num-files-at-level2",&nf));
  assert(nf==(files?"1024":"1"));
  // Both dense and sparse batches have 512 distinct keys in 512 distinct blocks.
  // Dense starts away from zero; sparse spans the full index/file set.
  for(unsigned i=0;i<512;++i) keys.push_back(RoutingKey(files?256+i:2048+i));
  for(auto& k:keys) slices.push_back(leveldb::Slice(k));
  // Warm table headers/indexes only: read each file once, retaining zero data cache.
  leveldb::ReadOptions ro;ro.fill_cache=false;ro.verify_checksums=true;
  for(unsigned i=0;i<(files?total:1);++i){std::string v;assert(raw->Get(ro,RoutingKey(i),&v).ok());}
  Reset();
}
RoutingFixture::~RoutingFixture(){db_.reset();delete cache_;}
void RoutingFixture::Reset(){comparator.comparisons=0;env.reads=0;env.bytes=0;std::lock_guard<std::mutex> lock(env.mutex);env.offsets.clear();}
Counters RoutingFixture::Count()const{std::lock_guard<std::mutex> lock(env.mutex);return {comparator.comparisons.load(),env.reads.load(),env.bytes.load(),env.offsets.size()};}
void RoutingFixture::PointReference(){
  leveldb::ReadOptions ro;ro.fill_cache=false;ro.verify_checksums=true;
  for(auto& k:keys){std::string v;assert(db_->Get(ro,k,&v).ok());assert(v==RoutingValue(Rank(k)));}
}
}
