#include <opentimelineio/clip.h>
#include <opentimelineio/track.h>
#include <opentimelineio/linearTimeWarp.h>
#include <opentimelineio/trackAlgorithm.h>
#include <iostream>
namespace o=opentimelineio::OPENTIMELINEIO_VERSION_NS;
using o::RationalTime;using o::TimeRange;
#define REQUIRE(c) do {if(!(c)){std::cerr<<"BASELINE_FAIL "<<__LINE__<<"\n";return 1;}}while(0)
int main(){o::ErrorStatus e;
 auto r=[](int a,int n){return TimeRange(RationalTime(a,24),RationalTime(n,24));};
 o::SerializableObject::Retainer<o::Track> t(new o::Track("source"));
 auto c=new o::Clip("A",nullptr,r(100,24));t->append_child(c,&e);
 c->effects().push_back(new o::LinearTimeWarp("speed","",2));
 REQUIRE(!o::is_error(e));REQUIRE(t->duration(&e)==RationalTime(24,24));
 REQUIRE(c->transformed_time(RationalTime(112,24),t,&e)==RationalTime(12,24));
 o::SerializableObject::Retainer<o::Track> cut(o::track_trimmed_to_range(t,r(6,8),&e));
 REQUIRE(cut&&!o::is_error(e));auto cc=dynamic_cast<o::Clip*>(cut->children()[0].value);
 REQUIRE(cc&&cc->source_range()==r(106,8));REQUIRE(cut->duration(&e)==RationalTime(8,24));
 std::cout<<"OTIO_BASELINE duration=24 source112=output12 trim_source=106 length=8 effect_scalar=2\n";
}
