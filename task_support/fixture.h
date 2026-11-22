#pragma once
#include "matroska/KaxCluster.h"
#include "matroska/KaxSegment.h"
#include <ebml/MemIOCallback.h>
#include <memory>
#include <vector>
using namespace libebml;
using namespace libmatroska;
inline int released=0;
inline bool release_count(const DataBuffer&) { ++released; return true; }
inline void fill(KaxTrackEntry& track,KaxCluster& cluster) {
 track.SetGlobalTimestampScale(1000000);
 GetChild<KaxTrackNumber>(track).SetValue(1);
 track.EnableLacing(true);
 cluster.SetPreviousTimestamp(0,1000000);
 GetChild<KaxClusterTimestamp>(cluster).SetValue(1);
 KaxBlockGroup *g=nullptr;
 binary a[5]={1,2,3,4,5};
 std::vector<binary> b(300,0x42);
 cluster.AddFrame(track,1000000,*new DataBuffer(a,5,release_count,true),g,LACING_XIPH);
 cluster.AddFrame(track,2000000,*new DataBuffer(b.data(),300,release_count,true),g,LACING_XIPH);
}
inline std::vector<binary> render(KaxCluster& cluster) {
 MemIOCallback output; KaxSegment segment; KaxCues cues;
 cues.SetGlobalTimestampScale(1000000);
 segment.WriteHead(output,5); cluster.SetParent(segment);
 cluster.Render(output,cues);
 return {output.GetDataBuffer(),output.GetDataBuffer()+output.getFilePointer()};
}
