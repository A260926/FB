构建使用此提交内的 simplecpp（0BSD）、TinyXML2 11.0.0（Zlib）、picojson（BSD-2-Clause）；deps.sha256 固定全部这些内附源码与许可证字节，build.sh 先核对。系统依赖为 C++11 编译器、CMake >= 3.13、线程和标准库、Bash、POSIX shell、shasum、xmllint。不安装网络依赖，不使用系统 Boost、PCRE 或 Qt，TinyXML2 固定使用内附版本。CMake 会发现 Python 并复制上游 addons，但 USE_MATCHCOMPILER=Off、DISABLE_DMAKE=ON，不执行 Python 生成器或 addons。任务实现、新增测试和示例均使用 C++、C、Shell。

.build 是本目录的本地构建产物，可删除重建。CHECK_FOCUSED_TESTS 只缩小 testrunner 的编译注册范围，包含原 TestBufferOverrun、TestNullPointer、TestUninitVar；选项未启用时上游构建不变。run-baseline.sh 是起点行为记录，扩展能力后它会按预期不再成立，请用 run-target.sh 检查新行为，不要篡改起点记录。
