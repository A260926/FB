# 双轨关联与编辑契约

API 返回模型，失败抛 Error（不得改输入）；fromJSON 接收 JSON.parse 后的对象，toJSON 返回可JSON.stringify的对象。groups 为按消费顺序排列的 {left:[key], right:[key], locked:boolean}；允许的长度对只有(1,1),(1,2),(2,1),(1,0),(0,1)。每侧每个输入条目恰好被消费一次，保持输入序，空轨合法。输入cue和key序列可由模型持有，但align/realign不能改原数组、cue或调用方options。外部可改cue，再realign；不支持直接改groups。

key为非空字符串，仅轨内唯一；原cue.id允许重复。cue是本库VTTCue，每轨不同条目不能共用同一cue，两轨也不能共用cue。时间必须有限、非负且有毫秒精度：秒数乘1000与最近整数之差<=1e-6，转整数后在[0,1e9]且end>start，各轨start非递减，允许重叠。options为可选对象：gapPenalty默认2000，mergePenalty默认500，均为[0,1e6]整数。每轨至多200条。未知options字段可忽略；已知字段错误则拒绝。

匹配块包络取成员start最小值和end最大值。匹配代价为两侧包络start差绝对值加end差绝对值，再加mergePenalty*(左条数+右条数-2)；单边跳过每条花gapPenalty。不做文本相似度、翻译或时钟回绕。整条消费路径取最低总代价；并列时按完整转移序列的字典序选择，转移优先级固定11<12<21<10<01。group cost相加为模型cost；cost读取当前cue值和当前groups重新计算，不信任持久化的cost。

options.locks默认[]，每项{left:[keys],right:[keys]}只能是11/12/21之一；两侧各自必须引用连续输入条目，锁列表按两轨索引严格递增且不交叠。无序、交叉、重叠、不存在、重复key均拒绝，不自动排序。锁定块必须作为单个locked:true的转移出现，前后分别全局求最优，其他组locked:false；锁块自身也计上述代价。realign保留锁，并按当前cue时间重新求解，失败保留当前groups。

conform(index,ripple=false)只接受双边非空的组。左包络[L0,L1]、旧右包络[R0,R1]，选中组每个右cue的端点x映射为 L0+floor((x-R0)*(L1-L0)/(R1-R0)) 毫秒；从原端点一次算出，不逐步累积。涉及的乘法可能超过安全整数范围，需用BigInt或等价精确整数方法，输出秒数写回原cue setter。ripple=true再将该组最后右条目之后的全部右轨条目平移(L1-R1)，包含跳过块。其余对象不动，左轨不改，groups和locks不改，cost随现值变化。数字组索引须为有效整数，ripple须bool。

提交前检查所有新时间仍满足输入时间范围、正时长和每轨start顺序。只要任何受影响cue.text含合法WebVTT内联时间标签（<mm:ss.mmm>或<hh:mm:ss.mmm>）就拒绝该次编辑；格式标签/实体/说话人及正文保持原文，不处理逐词重定时。当前整模型输入不合法也拒绝。失败不得改任何cue时间、文字、hasBeenReset、displayState、groups或锁；成功通过现有setter使受影响cue.hasBeenReset=true，保留displayState供原processCues依据reset失效。不要清除/重写没有改动的左cue或后继cue状态。

toJSON输出带版本的文档、两轨key/cue值、region引用表、locks和groups；格式自行定义并记录。fromJSON验证版本、key、时间、锁、完整合法路径与引用，锁位置须与groups吻合；不要求当前groups仍是最优，因为conform不自动realign。缓存字段不保存；恢复的新cue/region都经原生构造器和setter创建，同一region对象共享按身份保留，不按属性值去重。独立恢复两次不共享对象，原文件cue id、pauseOnExit、text及所有原生布局属性保留。拒绝未知版本、缺字段、坏region引用及非法/不完整路径，无部分模型返回。

examples/bilingual.cjs align LEFT.vtt RIGHT.vtt OUT.json 用生产Parser（TextDecoder支持UTF-8）逐文件读取，按回调顺序分配l1/r1等key，默认参数生成模型；examples/bilingual.cjs conform IN.json GROUP OUT.json [--ripple] 在另一个进程恢复后编辑并保存。文件只在全部解析/求解/序列化成功后替换，错误非零且已有OUT字节保持。首条字幕正文有中文和重复原id也须正确保留。实现不使用npm安装、外部服务或Python。
