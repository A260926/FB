#include <opentimelineio/track.h>
#include <opentimelineio/clip.h>
#include <opentimelineio/gap.h>
#include <opentimelineio/trackAlgorithm.h>
#include <iostream>
#include <cstdlib>
using namespace OTIO_NS;
#define CHECK(x) do {if(!(x)){std::cerr<<"check failed line "<<__LINE__<<"\n";std::exit(1);}}while(0)
int main(){
 using R=RationalTime; using T=TimeRange;
 OTIO_NS::ErrorStatus e;
 SerializableObject::Retainer<Track> tr=new Track("video");
 CHECK(tr->append_child(new Clip("A",nullptr,T(R(100,24),R(20,24))),&e));
 CHECK(tr->append_child(new Gap(T(R(0,24),R(10,24))),&e));
 CHECK(tr->append_child(new Clip("B",nullptr,T(R(200,24),R(30,24))),&e));
 const auto before=tr->to_json_string(&e);CHECK(!is_error(e));
 SerializableObject::Retainer<Track> out=track_trimmed_to_range(tr,T(R(5,24),R(35,24)),&e);
 CHECK(out && !is_error(e)); CHECK(out->children().size()==3);
 auto a=dynamic_cast<Clip*>(out->children()[0].value);auto b=dynamic_cast<Clip*>(out->children()[2].value);
 CHECK(a && b);CHECK(a->trimmed_range().start_time()==R(105,24));
 CHECK(a->duration()==R(15,24));CHECK(b->duration()==R(10,24));
 CHECK(out->duration()==R(35,24));CHECK(tr->to_json_string(&e)==before);
 SerializableObject::Retainer<SerializableObject> reread=SerializableObject::from_json_string(out->to_json_string(&e),&e);
 CHECK(reread && !is_error(e) && reread->is_equivalent_to(*out));
 std::cout<<"OTIO_BASELINE_OK children=3 A=105+15 gap=10 B=200+10 total=35 original=60 roundtrip=ok\n";
}
