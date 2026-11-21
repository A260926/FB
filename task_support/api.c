#include <embree4/rtcore_photon_beam.h>
void task_c_api_probe(RTCDevice d) { RTCPhotonBeamMap m=rtcNewPhotonBeamMap(d,0,0); rtcReleasePhotonBeamMap(m); }
