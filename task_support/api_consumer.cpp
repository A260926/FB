#include "HLSSegmenter.hh"
#include "HLSPartPublisher.hh"
#include "BasicUsageEnvironment.hh"
static void part(void*,const HLSPartInfo&){}
int main(){
 TaskScheduler* s=BasicTaskScheduler::createNew();UsageEnvironment* e=BasicUsageEnvironment::createNew(*s);
 HLSSegmenter* h=HLSSegmenter::createNew(*e,2,".task-build/api-probe-");
 if(h->enablePartialSegments(0,part,nullptr)) return 20;
 if(!h->enablePartialSegments(500000,part,nullptr)) return 21;
 if(h->enablePartialSegments(500000,part,nullptr)) return 22;
 Medium::close(h);e->reclaim();delete s;return 0;
}
