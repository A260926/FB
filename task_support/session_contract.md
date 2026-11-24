此文件是新接口的数据与行为契约，不含生产实现。

新增类型置于fuzztest::internal命名空间，位于centipede/session_feedback.h。enum class SessionEventKind : uint8_t { Begin=1, Step=2, Finish=3, Cancel=4, Timeout=5 }; struct SessionEdge { uint32_t from=0, action=0, to=0; }; struct SessionModel { uint32_t initial=0; std::vector<SessionEdge> edges; }; struct SessionEvent { SessionEventKind kind=SessionEventKind::Begin; uint64_t session=0, tick=0, deadline=0; uint32_t sequence=0, action=0; }; 保持聚合初始化字段顺序，SessionEvent提供逐字段bool operator==(const SessionEvent&) const，可用C++17。runner_result.h必须使这些类型对调用者可见。ExecutionResult::clear及BatchResult::ClearAndResize也要清新增轨迹。

模型initial/from/to/action均在0..255，最多4096条边；同(from,action)不允许重复或冲突；空边模型合法。状态集合为initial及全部from/to。模型声明顺序不改变特征，未从initial到达的边不能凭声明产生覆盖。Begin的session非零、sequence/action为0且deadline>tick；同输入最多64个会话、每会话最多4096个Step，总事件不超过65536；session身份在同输入不可重开，下一输入可重用。Begin不产生覆盖。Step的deadline为0，sequence从1严格连续，action是模型动作；每会话tick不递减且必须小于deadline，按当前状态/动作找边，走边后更新状态。Finish/Cancel/Timeout的deadline/action为0，sequence等于最后Step序号（可为0），tick不倒退；Finish和Cancel必须早于deadline，Timeout必须不早于deadline。Timeout发生前不能接收deadline及以后的Step；三种终止只允许一次，关闭后不能再接收任何事件。不同session可交错，彼此逻辑tick独立，不对不同session按时间重排。相同tick的Step按sequence和流顺序执行，终止在已记录前驱之后执行；不推测真实时钟或补造没有出现的Step。

边特征为feature_domains::kUserDomains[14].ConvertToMe(from*256+action)，终止特征为kUserDomains[15].ConvertToMe(current_state*3+K)，K=0/1/2分别代表Finish/Cancel/Timeout；不产生Begin特征。两域明确保留给此可选模式，普通feature若落在这两域内，新模式须拒绝而非混淆来源。重复走边只贡献一个特征；输出features为旧普通features与新特征的排序去重并集，各输入单独保存，不能把上一输入覆盖带到下一输入。ExecutionResult::session_events保存该完整输入原始事件流顺序。

新blob tag固定为9，不重用现有0..8；payload恰为40字节：kind在0字节；1..7保留字节全0；session/tick/deadline分别在8/16/24起，以8字节无符号小端表示；sequence/action分别在32/36起，以4字节小端表示。WriteSessionEvent只验证单条形状（kind/非零session、Begin或其他kind的零字段要求、Begin的deadline、action界限），不维持模型或会话状态；非法或容量不足返回false且BlobSequence.offset和原字节不变。不能用memcpy直接保存C++结构体。WriteOneFeatureVec/Metadata/Stats/InputBegin/InputEnd沿用原格式。

BatchResult::ReadSessionBatch按ClearAndResize(N)设定的最多N个输入读取。新模式仅接受旧InputBegin/Features/Stats/Metadata/InputEnd及新tag9；未知tag、输入嵌套、没有Begin的数据、额外End、输入数超限、features字节非feature_t整数倍、stats长度错误、同输入重复Features/Stats/Metadata、事件非法或模型非法均返回false，原BatchResult所有字段（含新增轨迹）保持原值；读取游标可以前进，不要求恢复流。缺少Features/Stats/Metadata允许，默认为空/零。Metadata字节沿用旧模式原样保存，不重新解释CMP内容。完整InputEnd要求所有已Begin会话已关闭；没有会话的普通输入也合法。成功时只发布完整输入前缀，num_outputs_read为完整数量；最后只有一个已Begin但缺End、其已有事件均合法的输入可以成功，但尾部输入的features/metadata/stats/事件全部清空，剩余未执行槽也清空；空流成功且所有槽清空。非会话普通Read接口保持旧行为。

BlobSequence::read_error() const返回本次Reset后最近的读取阶段是否遇到损坏封装；Reset清false。正常全零终止头或写完数据恰好到边界不报错；读到非零残缺头、tag=0但size非0、payload越界时设true且Read仍返回无效Blob，保留旧Read的返回契约。ReadSessionBatch遇read_error=true整次失败，不能把损坏封装当合法半条输入。头部与payload的原生tag/size封装不改，只新增事件payload固定小端格式；本地同架构进程间使用。

示例用至少两个子进程经真实BlobSequence产生普通覆盖及会话事件，一份包含正常完成、另一份包含取消或超时及合法未完成尾部；父进程读取后按稳定输入编号归并已完成覆盖并核内容。不能让子进程仅打印预置答案。用管道握手或共享内存控制先后，不依靠sleep；输出目录每次全新，transcript.txt须由本次运行写出。
