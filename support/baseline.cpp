#include "leveldb/db.h"
#include <cassert>
#include <iostream>
#include <memory>
int main(int n,char **a){
 assert(n==2);leveldb::Options o;o.create_if_missing=true;o.compression=leveldb::kNoCompression;o.block_size=256;
 leveldb::DB *p;assert(leveldb::DB::Open(o,a[1],&p).ok());std::unique_ptr<leveldb::DB> db(p);
 assert(db->Put(leveldb::WriteOptions(),"key","before").ok());
 auto snap=db->GetSnapshot();
 assert(db->Put(leveldb::WriteOptions(),"key","after").ok());assert(db->Put(leveldb::WriteOptions(),"gone","old").ok());assert(db->Delete(leveldb::WriteOptions(),"gone").ok());
 leveldb::ReadOptions r;r.snapshot=snap;std::string v;assert(db->Get(r,"key",&v).ok() && v=="before");
 db->ReleaseSnapshot(snap);db->CompactRange(nullptr,nullptr);db.reset();
 assert(leveldb::DB::Open(o,a[1],&p).ok());db.reset(p);r.snapshot=nullptr;
 assert(db->Get(r,"key",&v).ok() && v=="after");assert(db->Get(r,"gone",&v).IsNotFound());
 std::cout<<"SNAPSHOT=before REOPEN=after TOMBSTONE=NotFound\n";
}
