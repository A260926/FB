逐帧入口加入dr_mp3.h：
```c
/* 返回每声道帧数，沿用info.frame_bytes消费量。仅DR_MP3_FLOAT_OUTPUT构建使用。
 * cuts非NULL，两项分别为当前MPEG-1帧的两个granule的low子带数0..32。
 * 输出各有DRMP3_MAX_SAMPLES_PER_FRAME个float容量。 */
int drmp3dec_decode_frame_stems(drmp3dec*,const drmp3_uint8*,int,float *low,float *high,drmp3dec_frame_info*,const unsigned cuts[2]);
```
非法cuts/NULL参数返回0，清info（若非NULL），不消费输入、不改解码现场。完整但reservoir不足的帧沿用原入口消费及历史规则，不能捏造声音。原生与stems模式切换前调用drmp3dec_init，后者须初始化新历史。
高层接口加入dr_mp3_stems.h，使用如下类型、字段和签名：
```c
#include "dr_mp3.h"
typedef struct drmp3_stems drmp3_stems;
typedef struct { drmp3_uint64 granule; unsigned low_subbands; } drmp3_stem_point;
drmp3_stems *drmp3_stems_open_memory(const void*,size_t,const drmp3_stem_point*,size_t);
void drmp3_stems_close(drmp3_stems*);
drmp3_uint64 drmp3_stems_read(drmp3_stems*,drmp3_uint64 frames,float *low,float *high);
int drmp3_stems_seek(drmp3_stems*,drmp3_uint64 frame);
drmp3_uint64 drmp3_stems_tell(const drmp3_stems*);
drmp3_uint64 drmp3_stems_length(const drmp3_stems*);
unsigned drmp3_stems_channels(const drmp3_stems*);
unsigned drmp3_stems_sample_rate(const drmp3_stems*);
```
open失败返回NULL；数据借用至close，points复制。日程必须非空、首granule=0、严格递增、cut<=32；拒绝非法输入，不能修改传入内存。seek成功1失败0；read返回实际帧数，非零容量要求两个输出非NULL。zero read允许NULL且不改变现场。默认不做额外尾零、增益或削波；从原生浮点解码得到的值直接分离。
