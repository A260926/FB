// Fixed minimum normal consumer, not a TimeMappedStack implementation.
#include <opentimelineio/timeMappedStack.h>
#include <opentimelineio/track.h>
#include <opentimelineio/clip.h>
#include <iostream>
namespace o=opentimelineio::OPENTIMELINEIO_VERSION_NS;
#define REQUIRE(c) do {if(!(c)){std::cerr<<"TIME_MAP_BINDING_FAIL "<<__LINE__<<"\n";return 1;}}while(0)
int main(){
 o::ErrorStatus e;o::SerializableObject::Retainer<o::TimeMappedStack> m(new o::TimeMappedStack());
 auto tr=new o::Track();auto c=new o::Clip("A",nullptr,o::TimeRange(o::RationalTime(100,24),o::RationalTime(24,24)));
 REQUIRE(tr->append_child(c,&e));REQUIRE(m->append_child(tr,&e));
 REQUIRE(m->set_time_map({{o::RationalTime(0,24),o::RationalTime(0,24)},{o::RationalTime(24,24),o::RationalTime(24,24)}},&e));
 REQUIRE(!o::is_error(e)&&m->duration(&e)==o::RationalTime(24,24));
 auto v=m->sample_at(o::RationalTime(6,24),&e);
 REQUIRE(!o::is_error(e)&&v.size()==1&&v[0].clip==c&&v[0].source_time==o::RationalTime(106,24));
 std::cout<<"TIME_MAP_API_BINDING_OK\n";
}
