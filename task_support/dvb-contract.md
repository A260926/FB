接口及产品边界

构造时width/height为1..2048，固定整个实例画幅；不处理DDS窗口。pageID只选一个页面，其他page忽略。本子集仅region_depth=3（8bit）、object_type/provider均0、object_coding_method=0、full_range_flag=1；遇到选中页面不支持的编码返回error。输入必须为data_identifier=0x20、subtitle_stream_id=0，加若干segment，允许末尾0xff。每个segment头为0x0f,type,pageID(u16),length(u16)，多字节大端；type 0x10/11/12/13/80分别是PCS/RCS/CLUT/ODS/EODS。保留位忽略。段不能跨PES截断，显示集可以跨PES，直到EODS才提交。

二进制字段速记（位从高到低，uN为N位无符号）：PCS为timeout(u8),version(4)/state(2)/保留(2)，随后重复regionID(u8),保留(u8),x/y(u16)。RCS为id(u8),version(4)/fill(1)/保留(3),width/height(u16),compatibility(3)/depth(3)/保留(2),CLUTid(u8),background8(u8),background4(4)/background2(2)/保留(2)，随后重复objectID(u16),type(2)/provider(2)/x(12),保留(4)/y(12)。CLUT为id(u8),version(4)/保留(4)，随后entryID(u8),2bit/4bit/8bit适用标志(各1)/保留(4)/full_range(1),Y/Cr/Cb/T(各u8)。ODS为id(u16),version(4)/method(2)/nonmod(1)/保留(1),topLen/bottomLen(u16)，再跟两场的字节串；可有一个0填充字节。EODS负载为空。

像素子块：0x12引入8bit串，非零字节是一个该色像素；零后接一个控制字节，最高位0表示低7位个色0，最高位1表示低7位个后续色；全零控制字节结束串。0xf0换行，x回0，y加2；上场起y=0，下场起y=1，bottomLen=0时下场复用上场。nonmod下色码1不写索引。只支持这两种子块标记，其他明确报错。标准参考为ETSI EN 300 743 V1.6.1第7、11节，链接见README。以下是本接口专门约定，不要求实现标准外的功能。

显示集提交约定

Push的at是非负、非递减的相对时间；一个未结束显示集内各PES的at必须相同。页/region/CLUT/object各自按ID和4bit版本缓存，相同版本重传忽略，编号只比较是否不同（包含15到0），不比较大小。一个显示集至多含每类每ID的一个新版本；可先收到对象再收到其region。新版本PCS的state=1或2开始新epoch，清除此前region、object和CLUT，当前显示集内容按新epoch建立；PCS应当在该显示集的第一条选中段，其后才是资源段。state=0保留所有缓存。忽略同版本PCS，不因此清缓存。state=3拒绝。

每个region拥有独立的width×height索引平面，首次出现初始化background8。同epoch内width/height/depth/CLUTid不变；新的RCS版本替换对象引用列表，fill置位就先清背景，不置位保留旧像素，再按列表顺序画缓存对象。未收到RCS但某个被引用对象版本变化时，也要在原引用位置重画该对象；多个对象同时变化按原列表顺序覆盖。对象坐标以region左上为原点，超出region部分裁去；未写的像素不透明化、不清零。所有引用在EODS处必须可解析，缺对象/region/CLUT为错误，不发布半成品。

CLUT新版本将所列entry覆盖到原表，entry0缺省全透明；其他被用到的entry必须存在。保留索引直到输出阶段：改变CLUT要重着色旧像素。RGB用Go标准库color.YCbCrToRGB(Y,Cb,Cr)，alpha=255-T，Y=0强制透明，透明RGB归零。每次EODS按当前PCS列出的region顺序在透明整帧上source-over，region定位用PCS坐标，越出画幅裁掉。Go image/draw的NRGBA source-over结果作为字节取整参照。空region列表是清屏，不发布空白图像区间。

当前可见帧从EODS的at开始，下一次EODS以at结束它，或在其PCS.timeout秒后结束，以较早者为准；timeout=0表示不自动过期。返回已关闭的非空、正时长BitmapSubtitle。即使图像相同也不用合并区间，重传EODS可以产生新的区间。Flush(at)只封闭当前帧，不强行提交未结束显示集；有未结束显示集则报错。重复Flush无输出；Flush后允许继续Push，时码继续非递减。超时后重显只发生在新的EODS。返回的图像及Pix不得与解码器或别的结果共享可变存储。

格式错误或倒退时间必须返回错误且不修改调用前的状态（包括未提交显示集）；合法前序Push的缓存仍可用。一次Push可含多个同at的显示集，错误时整次回滚即可；不用保留历次已交付的帧。该约定是编辑预览API的失败恢复要求。

TS及文件示例

ReadFromDVBSubtitles只使用o.PID的private_stream_1且有PTS的PES；用PTS.Base的90k时基换成相对纳秒，整数计算截断，不引入PCR回退或33bit回绕。返回Push和Flush结果的串联；o.EndAt传给Flush。错误不返回部分结果。原Open的.ts仍按Teletext走，不自动探测位图。

examples/dvbsub/main.go读取上述固定选项。index.json是JSON数组，各项{"file":"frame-0001.png","start_at":0,"end_at":2000000000}，时间是整数纳秒；PNG为整幅8×8直通RGBA效果图。调用前目录由调用者创建，示例需在目录中创建全新命名文件或覆盖自己同名输出，任何写出错误非零退出。target在新临时目录执行，不能用残留文件通过。至少输出一个非空且有可见像素的图像区间。PNG不包含字体或外部素材。
