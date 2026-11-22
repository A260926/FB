#include <opentimelineio/timeline.h>
#include <opentimelineio/track.h>
#include <opentimelineio/stack.h>
#include <opentimelineio/clip.h>
#include <opentimelineio/externalReference.h>
#include <opentimelineio/transition.h>
#include <iostream>
namespace o=opentimelineio::OPENTIMELINEIO_VERSION_NS;
using o::RationalTime;using o::TimeRange;
#define CHECK(x) do {if(!(x)){std::cerr<<"baseline line "<<__LINE__<<"\n";return 1;}} while(0)
int main(){
 o::ErrorStatus e;
 auto r=[](double a,double n){return TimeRange(RationalTime(a,48000),RationalTime(n,48000));};
 o::SerializableObject::Retainer<o::Timeline> scene(new o::Timeline("mix"));
 auto track=new o::Track("audio",std::nullopt,o::Track::Kind::audio);
 auto a=new o::Clip("A",new o::ExternalReference("a.wav",r(0,16)),r(2,4));
 auto b=new o::Clip("B",new o::ExternalReference("b.wav",r(0,16)),r(5,4));
 auto dissolve=new o::Transition("crossfade",o::Transition::Type::SMPTE_Dissolve,RationalTime(1,48000),RationalTime(1,48000));
 track->append_child(a,&e);track->append_child(dissolve,&e);track->append_child(b,&e);
 scene->tracks()->append_child(track,&e);CHECK(!o::is_error(e));
 CHECK(scene->duration(&e)==RationalTime(8,48000));
 CHECK(dissolve->range_in_parent(&e).value()==r(3,2));
 CHECK(a->visible_range(&e)==r(2,5));CHECK(b->visible_range(&e)==r(4,5));
 o::SerializableObject::Retainer<o::Track> retained(track);
 auto nest=new o::Stack("crop",r(1,6));scene->tracks()->remove_child(0,&e);nest->append_child(track,&e);scene->tracks()->append_child(nest,&e);
 CHECK(a->transformed_time(RationalTime(5,48000),nest,&e)==RationalTime(3,48000));
 auto json=scene->to_json_string(&e);CHECK(!o::is_error(e));
 o::SerializableObject::Retainer<o::SerializableObject> copy(o::SerializableObject::from_json_string(json,&e));
 CHECK(copy&&!o::is_error(e));CHECK(dynamic_cast<o::Timeline*>(copy.value)->duration(&e)==RationalTime(6,48000));
 std::cout<<"OTIO_AUDIO_GRAPH_OK duration=8 transition=3..5 handles=A:2..7,B:4..9 nested=6 json=ok\n";
}
