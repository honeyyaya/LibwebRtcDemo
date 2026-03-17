WebRTC 入门连接

客户端如何连接服务端：

Demo 1: Qt 使用 WebEngineView 加载  http://192.168.50.68:3000/receiver.html 到本地

Receiver.html 原理 :

![img](https://pic2.zhimg.com/v2-84e5bf7a5487db5eedf5656d5b271fad_1440w.jpg)



详细解释如下:

根据上图，整体流程是：

1. 用户 A 和用户 B 都需要先连接到信令服务器；
2. 用户 A 和用户 B 都创建一个 PeerConnection（此时 WebRTC 会自动向 STUN/TURN 服务获取 candidate 信息, WebRTC 内置了 ICE）；
3. 用户 A 将本地音视频流添加到 PeerConnection 中（通过 getUserMedia 获取音视频流）；
4. 用户 A 作为发起方创建 offer（offer 中包含了 SDP 信息），并将获取的本地 SDP 信息添加到 PeerConnection 中（setLocalDescription），然后再通过信令服务器转发给用户 B;
5. 用户 B 接收到用户 A 的 offer 后，将其添加到 PeerConnection 中（setRemoteDescription）；
6. 用户 B 将本地音视频流添加到 PeerConnection 中（通过 getUserMedia 获取音视频流）；
7. 用户 B 创建一个 Answer，并添加到 PeerConnection 中（setLocalDescription）;
8. 用户 B 通过信令服务器将 answer 转发给用户 A；
9. 用户 A 接收到 answer 后将其添加到 PeerConnection 中；
10. 用户 A 和 用户 B 都接收到了 candidate 信息后，都通过信令服务器转发给对方并添加到 PeerConnection 中（addIceCandidate）；
11. 媒体信息和网络信息交换完毕后，WebRTC 开始尝试建立 P2P 连接；
12. 建立成功后，双方就可以通过 onTrack 获取数据并渲染到页面上。

**上图是以用户 A 为发起方，用户 B 为接收方。**





本地需求：不需要STUN 和 TURN 服务器



// 如何使用C++ 去连接信令服务器





遇到崩溃问题

``E/rtc     : #`
`E/rtc     : # Fatal error in: ../../media/engine/webrtc_voice_engine.cc, line 806`
`E/rtc     : # last system error: 0`
`E/rtc     : # Check failed: adm_`
`E/rtc     : #` `

已在 `android/AndroidManifest.xml` 中加入 WebRTC 所需的音频权限：

- RECORD_AUDIO：WebRTC 音频设备模块初始化需要
- MODIFY_AUDIO_SETTINGS：音频相关设置需要



1. 确认权限
   在连接前调用 `requestPermissionAndConnect()`，在 logcat 中确认 `VERIFY-1` 中 RECORD_AUDIO 为 0。
   1. 
2. 延迟初始化
   不要在应用启动时立刻调用 `LibWebRTC::Initialize()` / `CreateRTCPeerConnectionFactory()`，等 Activity 完全启动后（例如首次点击「连接接收」）再初始化。
3. 检查初始化时机
   你现在的流程是「收到 Offer 后才 initWebRTC」，理论上不会太早。如果错误仍出现，可尝试：
   - 在 `Component.onCompleted` 或首次显示主界面时延迟 1–2 秒后再允许用户点击连接；
   - 或在应用进入前台后再触发连接逻辑。
4. NDK / API 版本
   查看 libwebrtc 的编译配置与目标 API level，确保与当前设备（以及 Qt/Android 配置）一致，例如在 logcat 中看 `VERIFY-4` 的系统信息。
5. 编译时关闭音频（若仅需视频）
   若只需要视频，可用 `rtc_use_null_audio_device=true` 重新编译 libwebrtc，这样不会再创建真实 ADM，自然也不会触发该错误。
6. 设备/ROM 兼容
   某些定制 ROM 或低端设备对 OpenSL/AAudio 支持不完整，可尝试换一台设备或模拟器验证。



方案二：禁用掉webrtc_voice_engine加载后，动态库无法启动。**No**

方案一：**修改libwebrtc 的配置文件（build.gn） 添加宏    defines += [ "WEBRTC_DUMMY_AUDIO_BUILD" ]**

方案一 已经正常接受流信息，下一部做渲染

修改引入的未知：

​	Dummy Audio APIs 暂时规避策略 无法规避真正问题，需要重新思考解决方案。



测试延时效果一：240~250ms

​	其中终端日志显示：

> D/default : [VideoPerf] frame# 1020 | 线程队列: 28.878 ms | YUV转换: 28.839 ms | 缓冲拷贝: 0.217 ms | 帧间隔: 31.896 ms | 本帧总: 57.934 ms
> D/default : [VideoPerf] paint: 10.274 ms | 全链路(含渲染): 68.208 ms



### **优化方向：YUV 转换加速**

YUV 转换的加速方法主要可以分为**软件层面的优化**和**硬件层面的加速**两大类。选择哪种方法，通常取决于你的应用场景（是 PC 软件、移动端 App 还是嵌入式设备）、性能要求以及成本预算。

下面这个表格汇总了各种加速方法及其核心特点，可以让你先有一个整体的印象。

| 加速方法                 | 核心原理                                                     | 适用平台                        | 性能提升示例                                                 |
| :----------------------- | :----------------------------------------------------------- | :------------------------------ | :----------------------------------------------------------- |
| **🧠 软件优化：指令集**   | 利用CPU的SIMD（单指令多数据流）指令集（如x86的**SSE/AVX**、ARM的**NEON**），并行处理多个像素数据。 | 各种CPU平台                     | 4K图像YUV转RGB，使用SSE2从**2.63秒降至0.60秒**。1080P图像转换缩放，使用NEON从**14ms降至3ms**。 |
| **⚙️ 软件优化：专用库**   | 使用已经高度优化好的专业转换库，如Google的**libyuv**、FFmpeg的**libswscale**、英特尔**IPP**。 | 跨平台                          | 1280x720图像NV12转RGBA，使用libyuv从**90ms降至16ms**。       |
| **🚀 硬件加速：GPU**      | 利用GPU的并行计算能力，通过图形API（如**Vulkan**）或通用计算技术，将转换任务交给GPU处理。 | 配有独立或集成GPU的设备         | 在某些GPU上，性能比CPU库快一个数量级，并能显著减轻CPU负担。  |
| **📦 硬件加速：专用硬件** | 调用芯片上的专用硬件模块（如NVIDIA的**NVENC/NVDEC**、全志平台的**G2D** 2D图形加速器）来完成色彩转换。 | 特定嵌入式平台、NVIDIA Jetson等 | 在资源受限的嵌入式平台上，能显著降低CPU占用率并获得极高效率。 |



​	





