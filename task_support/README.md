Fixture来源：本目录makefixture/main.go手写有限DVB段，经固定astits muxer封装，不含解码答案。basic.ts的PID=256/page=1，四个PES绝对PTS=1,1,3,4秒，对应相对0,0,2,3秒。pes0/1组成同一显示集；pes2只更新CLUT；pes3清屏。region位于(1,1)，大小4×4；object top场含两种索引，bottom缺省复制。

规范来源：[ETSI EN 300 743 V1.6.1](https://www.etsi.org/deliver/etsi_EN/300700_300799/300743/01.06.01_60/en_300743v010601p.pdf)。只实现dvb-contract.md明示子集及API约定，运行不读取外部文档。
