# Demux跨层生命周期回归

本目标直接编译生产Demux、MP4解复用器并链接真实RtspClient，不链接AX硬件库。
TCP服务端为测试替身；参数集来自已有sample.h264，标记IDR只用于包身份测试，不作图像质量验收。

```sh
cmake -S tests/demux_reconnect -B /tmp/axsdk-demux-tests -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/axsdk-demux-tests -j4
ctest --test-dir /tmp/axsdk-demux-tests --output-on-failure
```

可通过`-DAXSDK_TEST_MP4=/absolute/path/to/fixture.mp4`增加MP4切换/Reset回归。
正式本轮Host验证使用FFmpeg生成的32×32、5FPS、5帧H.264 MP4，非板端视频样本。
ASan/UBSan或TSan通过独立build的CMAKE_CXX_FLAGS启用，不能将两类消毒器混合。

所有CHECK在Release下执行。重连测试连续三次关闭服务端连接，再验证新会话的独立载荷标记；
Interrupt用例等服务端收到第二次DESCRIBE后取消，避免只覆盖“重连尚未开始”。
每项CTest有20秒进程超时，不能把超时当作通过。
