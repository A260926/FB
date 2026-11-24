这两段素材为自生成的64×64、3帧16位逐行画面，不含外部视频内容。
mono16.h265 使用 Monochrome 16；44416.h265 使用 Main 4:4:4 16 Intra。
对应 .yuv 是独立HM解码器输出，逐帧Y（单色）或Y/U/V（444），每样本16位小端，无行填充。

生成参照：官方 https://vcgit.hhi.fraunhofer.de/jvet/HM.git ，HM-16.20，commit 22178e370178133438c0339f57b3b3a29f112909。
HM源码使用随附 HM-COPYING；没有HM代码或可执行程序参与运行入口。
生成像素的C++源见 make-pixels.cc。编码参数见 provenance.json；输入和参照的SHA256也记录于该文件。
独立解码YUV与HM编码器的重建文件经过cmp逐字节比较。

扩展精度的规范语义参考 ITU-T H.265 (07/2024)，7.4.3.2.2、8.6和9.3.3：
https://www.itu.int/rec/dologin_pub.asp?id=T-REC-H.265-202407-I!!PDF-E&lang=e&type=items
