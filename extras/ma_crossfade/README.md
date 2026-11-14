# ma_crossfade

把若干条已经裁切好、可寻址的 f32 数据来源（`ma_data_source`）接成一个可寻址的数据流，
相邻两段之间支持等功率（equal-power）交叉淡化。它本身是一个标准 data source，实现了
read / seek / get_data_format / get_cursor / get_length，可以直接挂到
`ma_data_source_node` 上，由离线 node graph 读取，不依赖任何音频设备。

miniaudio 自带的 data source chaining 只能按顺序在来源之间切换，无法同时读取两个来源并
保持共同的输出位置；本扩展补上这一能力，且不改变既有 chaining 的语义。

## 时间线几何

每一段（clip）描述：借用的来源指针、素材起始帧、段长度 `L[i]`、与下一段的重叠帧数
`N[i]`（最后一段必须为 0）。

- 第 0 段从输出帧 0 开始；第 i+1 段起点 = 第 i 段起点 + `L[i] - N[i]`。
- 成片总长 = `sum(L) - sum(N)`，且非零。
- 任一段前后两个重叠之和不得超过它自身长度，因此任意时刻最多只有两段同时发声。
- `N == 0` 是硬切。重叠内第 k 帧（k 从 0 到 N-1）按

      theta = pi * (k + 0.5) / (2 * N)
      out   = old * cos(theta) + new * sin(theta)

  逐声道独立计算，不做限幅。其余位置原样透传当前段的样本。
- 裁切窗口之外的素材永远不会被读到；旧段尾部与新段头部都只消费一次。

所有几何计算都带 64 位溢出检查，溢出时初始化返回 `MA_INVALID_ARGS`。

## 所有权

- 构造时**深拷贝剪辑表**；调用方在 `ma_crossfade_init` 返回后即可释放自己那份表。
- 来源指针是**借用**的：组合源在存活期间独占这些来源的读取与寻址，但
  `ma_crossfade_uninit` **不会**释放任何来源，来源的生命周期由调用方管理，且必须晚于
  组合源。
- 同一个来源指针在一张表里只能出现一次。
- 所有来源必须报告相同的 `ma_format_f32`、相同声道数（1 或 2）与相同采样率，并支持寻址。
- `read` 路径不做任何堆分配；只有初始化会分配剪辑表副本和定长工作区。未配对样本的缓存
  容量由固定处理块（`MA_CROSSFADE_BLOCK_FRAMES`）和声道数决定，不随录音时长增长。

## 暂缺数据（MA_BUSY）、短读与提前结束

来源可能短读，也可能在已交付部分帧后返回 `MA_BUSY`。组合源遵守下列约定：

- 重叠区只提交两侧都已就绪的**配对前缀**；另一侧先拿到的 PCM 会保留在定长缓存里等待
  重试，不会重新拉取、跳过，也不会提前混入后面的帧。
- 没有任何可提交数据时立即返回 `MA_BUSY`（帧数为 0），不会忙循环；已经有配对前缀时，
  同时返回该前缀帧数和 `MA_BUSY`。
- 来源在素材裁切范围用完之前就结束（例如返回 `MA_AT_END` 或非 BUSY 的短读）属于
  **数据错误**，组合源返回 `MA_ERROR`；此前已经提交的前缀仍然有效，截断的素材不会被当作
  正常的流结束。
- 正常抵达成片总长后，下一次读取以 0 帧返回 `MA_AT_END`。
- `pFramesOut == NULL` 的向前丢弃，在 `frameCount` 不超过剩余成片帧数时，与真实读掉同样
  多的帧等价；要读超过剩余长度、或要确认末端时，请使用非 NULL 输出缓冲。
- 零长度读取遵循库约定，返回 `MA_INVALID_ARGS`。

寻址使用成片帧号，闭区间 `[0, 总长]`。越界寻址在触碰任何来源之前就被拒绝
（`MA_INVALID_OPERATION`），组合源游标与缓存都不变。落在重叠中间时，两侧来源都会定位到
各自正确的素材帧，因此会从转场中间继续，而不是从转场起点重新淡入。

## 离线预览示例（无音频设备）

下面的示例用两段内存素材构造一条带重叠转场的时间线，再通过 node graph 离线把成片渲染到
浮点缓冲，全程不打开设备。

```c
#include "miniaudio.h"
#include "extras/ma_crossfade/ma_crossfade.h"

/* 假定这两个缓冲是 f32、双声道、同一采样率。 */
extern float g_clip_a_pcm[];   /* 至少 20 帧 */
extern float g_clip_b_pcm[];   /* 至少 16 帧 */

int render_preview(float* pOut, ma_uint64 outCapacityInFrames)
{
    ma_audio_buffer_ref sourceA, sourceB;
    ma_crossfade* pCrossfade = NULL;
    ma_node_graph graph;
    ma_node_graph_config graphConfig;
    ma_data_source_node node;
    ma_data_source_node_config nodeConfig;
    ma_crossfade_clip clips[2];
    ma_uint64 totalLength = 0, done = 0;
    ma_result result;

    ma_audio_buffer_ref_init(ma_format_f32, 2, g_clip_a_pcm, 20, &sourceA);
    ma_audio_buffer_ref_init(ma_format_f32, 2, g_clip_b_pcm, 16, &sourceB);

    /* 段 A：取素材帧 2 起的 12 帧；段 B：取素材帧 1 起的 10 帧；重叠 5 帧，硬切结尾。 */
    clips[0].pDataSource       = &sourceA;
    clips[0].clipStartInFrames = 2;
    clips[0].lengthInFrames    = 12;
    clips[0].overlapInFrames   = 5;
    clips[1].pDataSource       = &sourceB;
    clips[1].clipStartInFrames = 1;
    clips[1].lengthInFrames    = 10;
    clips[1].overlapInFrames   = 0;

    /* 剪辑表被复制；NULL 表示沿用来源报告的声道数与采样率。 */
    result = ma_crossfade_init(NULL, clips, 2, &pCrossfade);
    if (result != MA_SUCCESS) {
        goto cleanup;
    }

    ma_data_source_get_length_in_pcm_frames(&pCrossfade->base, &totalLength);
    if (totalLength > outCapacityInFrames) {
        result = MA_INVALID_ARGS;   /* 输出缓冲太小。 */
        goto cleanup;
    }

    /* 离线 node graph：不初始化任何设备。 */
    graphConfig = ma_node_graph_config_init(2);
    result = ma_node_graph_init(&graphConfig, NULL, &graph);
    if (result != MA_SUCCESS) {
        goto cleanup;
    }

    nodeConfig = ma_data_source_node_config_init(&pCrossfade->base);
    result = ma_data_source_node_init(&graph, &nodeConfig, NULL, &node);
    if (result != MA_SUCCESS) {
        ma_node_graph_uninit(&graph, NULL);
        goto cleanup;
    }
    ma_node_attach_output_bus(&node, 0, ma_node_graph_get_endpoint(&graph), 0);

    while (done < totalLength) {
        ma_uint64 framesRead = 0;
        result = ma_node_graph_read_pcm_frames(&graph, pOut + done * 2, totalLength - done, &framesRead);
        if (result == MA_BUSY) {
            /* 来源暂缺数据：已交付的前缀已写入 pOut，直接重试即可，不要改动请求位置。 */
            if (framesRead == 0) {
                continue;
            }
        } else if (result != MA_SUCCESS) {
            break;   /* 包括来源提前结束导致的 MA_ERROR。 */
        }
        done += framesRead;
    }

    ma_data_source_node_uninit(&node, NULL);
    ma_node_graph_uninit(&graph, NULL);

cleanup:
    /* 组合源不释放借用来源，来源由这里自行回收。 */
    if (pCrossfade != NULL) {
        ma_crossfade_uninit(pCrossfade);
    }
    ma_audio_buffer_ref_uninit(&sourceA);
    ma_audio_buffer_ref_uninit(&sourceB);
    return (int)result;
}
```

也可以不经 node graph，直接把 `&pCrossfade->base` 当作普通 `ma_data_source` 调用
`ma_data_source_read_pcm_frames` / `ma_data_source_seek_to_pcm_frame`，语义完全一致。
