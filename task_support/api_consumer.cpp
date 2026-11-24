#include <matroska/KaxOrderedReader.h>
#include "fixture_io.h"
#include <sstream>
using R=libmatroska::KaxOrderedReader;
static int verify(const char*path){std::ifstream f(path);std::string text((std::istreambuf_iterator<char>(f)),{});std::ostringstream expected;
 expected<<"C 0 0 7 0 0 "<<ordered_fixture::bytes("test/fixtures/codec-private.bin").size()<<"\n"
 <<"F 0 0 7 0 0 1000000 0 1000000 36\n"
 <<"F 0 0 7 1000000 1000000 1000000 1000000 1000000 13\n";
 if(!f||text!=expected.str())return 1;std::cout<<"example_manifest=matched\n";return 0;}
int main(int argc,char**argv){try{if(argc==3&&std::string(argv[1])=="--verify")return verify(argv[2]);ordered_fixture::write(".task-build/ordered-input.mkv");
 std::vector<std::shared_ptr<libebml::IOCallback>> sources;sources.push_back(std::make_shared<libebml::StdIOCallback>(".task-build/ordered-input.mkv",MODE_READ));std::unique_ptr<R> r;
 if(!R::Open(sources,0,91,71,r)||!r||r->DurationNS()!=2000000)return 2;R::Packet p;
 if(r->ReadNext(p)!=R::Status::Ok||p.kind!=R::Kind::Config||p.data!=ordered_fixture::bytes("test/fixtures/codec-private.bin")||p.track_number!=7)return 3;
 for(unsigned i=0;i<2;i++){if(r->ReadNext(p)!=R::Status::Ok||p.kind!=R::Kind::Frame||p.data!=ordered_fixture::bytes(i?"test/fixtures/frame1.packet":"test/fixtures/frame0.packet")||p.source_ns!=i*1000000||p.program_ns!=i*1000000||p.duration_ns!=1000000||p.visible_begin_ns!=i*1000000||p.visible_duration_ns!=1000000)return 4;}
 if(r->ReadNext(p)!=R::Status::End)return 5;
 if(!r->Seek(1500000)||r->ReadNext(p)!=R::Status::Ok||p.kind!=R::Kind::Config)return 6;
 if(r->ReadNext(p)!=R::Status::Ok||p.kind!=R::Kind::Frame||p.source_ns!=0||p.visible_begin_ns||p.visible_duration_ns)return 7;
 if(r->ReadNext(p)!=R::Status::Ok||p.source_ns!=1000000||p.visible_begin_ns!=1500000||p.visible_duration_ns!=500000)return 8;
 std::cout<<"consumer frames=2 duration_ns=2000000 seek_preroll=matched\n";return 0;
 }catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 9;}}
