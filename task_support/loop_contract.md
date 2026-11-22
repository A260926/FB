公共函数加入include/SDL3_sound/SDL_sound.h，内部头须自包含且可被编译入口强制include：
```c
typedef struct { Uint64 logical_frame,source_frame,output_frame,remaining_repeats; } Sound_LoopPosition;
int Sound_SetLoop(Sound_Sample*,Uint64 start,Uint64 end,Uint32 additional_repeats);
int Sound_SeekLoopFrame(Sound_Sample*,Uint64 logical_frame);
int Sound_GetLoopPosition(Sound_Sample*,Sound_LoopPosition*);
```
成功1、失败0，设置Sound_GetError说明。SetLoop仅接受可定位的题面WAV；0<=start<end<=N，总长N+additional_repeats*(end-start)在Uint64内。设置成功从逻辑0开始；repeats=0仍可用查询和新seek。已开始播放后不能换配置，Rewind后可换。未配置时Get/SeekLoopFrame拒绝；旧路径不受影响。显式SeekLoopFrame成功后output_frame归0，逻辑原点为目标，Sound_GetLoopPosition立即返回该逻辑帧对应物理位置。
逻辑q在[0,end)映射q；在[end,end+repeats*L)映射start+(q-end)%L；此后映射q-repeats*L，L=end-start。逻辑末尾映射N。remaining_repeats指尚未完成的额外循环次数：q<end时为repeats；循环段内为repeats-floor((q-end)/L)；之后为0。查询报告的是交付时钟而不是底层IO已读位置；显式定位不改变原始additional_repeats配置。
示例产物固定为本次生成的48000Hz双声道交错f32，至少64帧，含有限非零样本；文件由示例创建，不能复制种子素材充数。新增源/头及示例在target均须非空、编译并实际运行；必需头通过-include参与生产编译。
