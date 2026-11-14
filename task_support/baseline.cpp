#include <cstdio>
#include <string>
#include "aribcaption/decoder.hpp"
#include "task_support/pes.hpp"
int main(){
 aribcaption::Context c;aribcaption::Decoder d(c);
 if(!d.Initialize(aribcaption::EncodingScheme::kARIB_STD_B24_JIS)) return 1;
 d.SetReplaceMSZFullWidthAlphanumeric(true);
 aribcaption::DecodeResult r;
 auto normal=task_support::pes({{0x89,0x0e,0x41,0x42}});
 auto st=d.Decode(normal.data(),normal.size(),1000,r);
 if(st!=aribcaption::DecodeStatus::kGotCaption||!r.caption||r.caption->text!="AB")return 2;
 std::printf("normal Decode: text=%s pts=%lld\n",r.caption->text.c_str(),(long long)r.caption->pts);
 d.Flush();
 auto custom=task_support::pes({{0x89,0x0e,0x58,0x95,0x40,0x21,0x89,0x0e,0x41,0x95,0x4f,0x59}});
 st=d.Decode(custom.data(),custom.size(),2000,r);
 if(st!=aribcaption::DecodeStatus::kGotCaption||!r.caption||r.caption->text!="X")return 3;
 std::printf("MACRO current Decode: text=%s; trailing Y was not reached\n",r.caption->text.c_str());
 return 0;
}
