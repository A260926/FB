#include <aribcaption/context.hpp>
#include <aribcaption/decoder.hpp>
#include "fixture.hpp"
#include <iostream>
#include <cstdlib>
#define CHECK(x) do{if(!(x)){std::cerr<<"baseline line "<<__LINE__<<"\n";return 1;}}while(0)
int main(){using namespace aribcaption;Context ctx;Decoder d(ctx);DecodeResult r;
 CHECK(d.Initialize(EncodingScheme::kARIB_STD_B24_JIS));
 CHECK(d.Decode(management,sizeof management,1000,r)==DecodeStatus::kNoCaption);
 CHECK(d.QueryISO6392LanguageCode(LanguageId::kFirst)==0x6a706e);
 CHECK(d.Decode(statement,sizeof statement,1200,r)==DecodeStatus::kGotCaption);
 CHECK(r.caption);auto &c=*r.caption;CHECK(c.pts==1200);CHECK(c.plane_width==960&&c.plane_height==540);
 CHECK(c.text=="\xef\xbc\xa1\xef\xbc\xa2\xe6\x97\xa5");
 size_t n=0;uint32_t cps[]={0xff21,0xff22,0x65e5};
 for(const auto &reg:c.regions)for(const auto &ch:reg.chars){CHECK(n<3);CHECK(ch.codepoint==cps[n]);CHECK(ch.x==int(n*40));CHECK(ch.y==0);n++;}
 CHECK(n==3);CHECK(c.iso6392_language_code==0x6a706e);
 std::cout<<"BASELINE JIS chars=3 positions=0,40,80 plane=960x540 pts=1200 language=jpn OK\n";return 0;}
