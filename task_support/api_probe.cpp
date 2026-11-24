// Fixed consumer: this is not a timecode implementation or a substitute for the new tests.
#include <mp4v2/mp4v2.h>
#include <mp4v2/timecode.h>
#include <cstring>
int main(){
 MP4FileHandle f=MP4Read(".task-build/simple.mov");if(f==MP4_INVALID_FILE_HANDLE)return 10;
 MP4VideoTimeCode tc{};
 bool ok=MP4GetVideoTimeCode(f,1,80,&tc);MP4Close(f);
 return ok && tc.videoSampleId==3 && tc.timeCodeTrackId==2 && tc.timeCodeSampleId==1 && tc.frameNumber==90002 && !std::strcmp(tc.text,"01:00:00:02") ? 0 : 11;
}
