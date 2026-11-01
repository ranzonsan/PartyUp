# 桌面 GUI 与媒体栈调研（2026-11-01）

> 调研性质：基于公开资料的事实收集，**未做本地构建或基准测试**。标「未证实」的条目在进入实现前必须用 spike 验证。本文只服务选型，不是最终决定；决定记录在 `docs/adr/`。

## 结论前置

**Qt 6 Widgets + LiveKit C++ SDK（内含 libwebrtc）+ livekit-server** 是同时满足「Windows 首发 / 跨平台 / 不自造媒体轮子 / 单二进制 SFU」的组合。但「部分成员直连、部分成员中继」的混合拓扑没有任何现成实现支持——这部分是自研工程量的大头。

## 1. 桌面 GUI 框架

| 框架 | 常驻内存 | 视频帧落地路径 | 许可证 | 跨平台代价 |
|---|---|---|---|---|
| Qt 6 Widgets/QML | 未证实（无同条件公开实测） | `QRhiWidget`（Win 默认 D3D11）+ `QRhiTexture::createFrom()` 包原生纹理；`QVideoFrame` 有 `RhiTextureHandle` 类型，可零拷贝 [1][2][3] | LGPLv3（动态链接、可重链接）或商业 [4] | 最低 |
| Avalonia | 未证实 | `ICustomDrawOperation` + `ISkiaSharpApiLeaseFeature` 接 D3D 纹理（官方 `samples/GpuInterop/D3DDemo`）；`NativeControlHost` 有 airspace 限制（原生层恒在最上、无变换）[5][6] | MIT [7] | 低 |
| WinUI 3 | ~58 MB（2021 绘图基准）[8] | `SwapChainPanel`，零拷贝最佳 | MIT，但 WinUI 2.x 包曾带 "Engineering Preview" 禁分发条款，2026 才在 WindowsAppSDK 2.4.0 修正 [9] | **仅 Windows，出局** |
| egui | 未证实 | `egui_wgpu::Renderer::register_native_texture` 可零拷贝 [10] | MIT/Apache | 中；控件/排版需自建，与「不自造轮子」冲突 |
| Tauri (WebView2) | **142 MB**（含 WebView2 子进程；同基准 Electron 149 MB、WPF 74 MB）[11] | `<video>` 由 WebView2 内部解码合成，应用侧拿不到帧 | MIT/Apache | 中；Linux 换 WebKitGTK |
| Flutter Desktop | 71.3 MB（空壳）/ 28.7 MB（无 widget），2024 [12] | `Texture` 外部纹理，主流是像素缓冲上传，零拷贝未证实 | BSD-3 | 中；Windows 嵌入层最弱 |

两条要点：
- **Tauri 在 Windows 只比 Electron 省约 5% 内存**（142 vs 149 MB），「禁 Electron 所以选 Tauri」不成立 [11]。
- 启动时间无同条件横向实测，只能给「亚秒到数秒」量级，未证实。

## 2. 三条 WebRTC 接入路线

| 路线 | 完整链路 | Win 屏幕/回环音频 | AEC/NS/AGC | 许可 | 从零构建代价 |
|---|---|---|---|---|---|
| (a) 官方 libwebrtc | 全：desktop_capture、VP8/9/H264/AV1、transport-cc/GCC、NetEq、解码 | 原生（含 WASAPI loopback） | 现成（APM/AEC3） | BSD-3 | **无官方预编译**；`fetch --nohooks webrtc` ≈20 GB、下载 1–2 h、构建 3–4 h、`webrtc.lib` ≈365 MB；需 depot_tools + VS2022 + Win SDK [13][14] |
| (b) 生态封装 | 全（继承 libwebrtc） | 视封装而定 | 现成 | 视封装 | **分钟级**：LiveKit C++/Rust SDK 有 Win x64 预编译包（自带 protobuf/abseil DLL）[15][16]；`webrtc-sdk/libwebrtc`（m144）供 flutter_webrtc [17] |
| (c) 纯实现 | 不全 | 需自接 | 需外挂 | 各异 | 快，但缺口在别处 |

- (b)：`flutter_webrtc` 的 Windows 系统音 loopback 到 2026-05 的 PR #2060（v1.5.0）才并入 [18]；LiveKit C++ SDK 是否自带桌面采集**未证实**，大概率要自接 `Windows.Graphics.Capture` 喂外部帧源。.NET 侧 `Microsoft.MixedReality.WebRTC` 已停更，不可选。
- (c)：`webrtc-rs` 协议栈完整，AEC 需外挂 `webrtc-audio-processing`（AEC3/NS/AGC）[19]；`str0m` 官方明说不含编解码与采集，只适合做 SFU [20]；`SIPSorcery` 是 BSD-3 **外加 BDS 政治性使用限制**（法务红旗），编解码极少、无 AEC [21]；`GStreamer webrtcbin` 是唯一「非 libwebrtc 的完整链路」（LGPL-2.1，配 `webrtcdsp` 做 AEC、`wasapi` loopback、`d3d11` 输出），代价是运行时 100 MB+ 与 Windows 打包繁琐 [22]。

## 3. SFU 服务端候选

| 候选 | 单二进制难度 | 许可 | 原生客户端互通 | 混合拓扑 |
|---|---|---|---|---|
| LiveKit | **低**：`CGO_ENABLED=0` 的 Go 单文件（官方 Dockerfile 即证）[23] | Apache-2.0 | 走自家信令，须用其 SDK，不能只拿裸 WebRTC | **不支持**；官方两次拒绝 P2P（2022、2025）[24][25] |
| mediasoup | 高：Node 模块 + C++ worker 子进程 [26] | ISC | 标准 WebRTC，互通最好 | 不支持 |
| Pion | 低（纯 Go）[27] | MIT | 标准 WebRTC | 不支持；SFU 逻辑自写（有 `sfu-ws` 示例） |
| Janus | 中 | **GPLv3**，闭源不可用；官方不支持 Windows [28] | 标准 WebRTC | 不支持 |
| str0m | 是库不是服务器 [20] | MIT | 标准 WebRTC | 需自建 |

## 4. 「P2P 优先 + SFU 回落」的先例

- **Jitsi（最佳开源参考）**：恰好 2 人建 P2P，但 **JVB 连接始终保活**；第 3 人进入无缝切回 JVB，回到 2 人再切回直连；统计面板有 `(p2p)` 标签显示当前路径。这是「僵持检测 + 可观测性」的标准答案 [29]。
- **TeamSpeak 6**：屏幕共享纯 P2P（自有 STUN，`turn.teamspeak.com` 只开 STUN）[30]；2025-03 做了「peer-to-peer SFU」（一份编码分发多个直连 peer）[31]；**服务器中继 SFU 到 2026-10 仍在开发**，官方在 issue 中承认，用户已在抱怨无法切服务器转发 [32]。回落路径比直连难一个量级。
- **Discord 不是先例**：语音/视频全程单 SFU，官方称整个通话固定在单台 SFU 上，无 P2P 分支 [33]。
- **Parsec**：私有 UDP，无公开协议文档，未证实。

## 5. 三套自洽组合

**A. Qt 6 Widgets + LiveKit C++ SDK + livekit-server**
风险：Qt LGPL 合规、LiveKit 信令锁定。最可能踩的坑：**「一份编码、多路直发」在 libwebrtc 公开 API 上做不到**——一个 track 只能挂一个 PeerConnection，N 路直连就是 N 次编码。TS6 为此单独做了两年 [31]。建议语音走 SFU，屏幕共享按「1v1 直连、多人中继」降级。

**B. Avalonia + GStreamer webrtcbin + mediasoup/LiveKit**
风险：GStreamer 体积与 Windows 打包；Avalonia airspace 限制（原生视频层压不住 UI）[6]。坑：webrtcbin 拥塞控制弱于 libwebrtc，弱网需实测。

**C. Avalonia/Qt + LiveKit Rust SDK + livekit-server**
风险：Rust FFI 边界与崩溃域；比 A 少一层 C++ 胶水，但排障资料最少。

**一句话推荐**：选 A——编码/拥塞/抖动/AEC 全部外包给 libwebrtc，自研预算全投在「直连/中继切换状态机 + 可观测面板」。

## 6. 对 PartyUp 的直接含义

1. 「禁 Electron」如果只为内存，Tauri 不是答案（只省 5%）；真正的解是原生控件栈。Avalonia（MIT）法务比 Qt（LGPL 动态链接义务）轻，但 Qt 的 D3D 零拷贝是文档级现成能力，Avalonia 需要自写 `ICustomDrawOperation`。
2. 屏幕共享的 P2P 只适合 1v1：多人直连要么 N 次编码，要么自研「一份编码、多路分发」（TS6 用了两年）。这直接推翻「观看者 ≤3 默认直连」的设想。
3. 混合拓扑（部分成员直连、部分中继）没有任何现成库支持，必须自建拓扑状态机；MVP 建议先做「整频道二选一」。
4. 「服务端单二进制」与「直接采用 livekit-server」冲突：livekit-server 是独立 Go 进程。可选：内嵌为 Go 库（非官方支持）、随包分发两个进程由一个启动器管理、或基于 Pion 自建 SFU。
5. 没有任何现成 SFU 提供「P2P 直连 + 中继」双路径；所谓「SFU 兼作 relay」是我们自己在客户端侧做的回落，服务端只需能接受回流。

## 来源

[1] Qt Licensing https://doc.qt.io/qt-6/licensing.html
[2] QVideoFrame https://doc.qt.io/qt-6/qvideoframe.html
[3] QRhiWidget https://doc.qt.io/qt-6/qrhiwidget.html
[4] Qt LGPL 义务 https://www.qt.io/development/open-source-lgpl-obligations
[5] Avalonia 原生互操作 https://docs.avaloniaui.net/docs/app-development/native-interop （2026-04-03）
[6] GPU interop 讨论 https://github.com/AvaloniaUI/Avalonia/discussions/16836 （2024-08-28）
[7] Avalonia License https://avaloniaui.net/legal/13 （MIT，2024-04-24）
[8] XamlBenchmark https://github.com/Noemata/XamlBenchmark （2021）
[9] WindowsAppSDK #6654 https://github.com/microsoft/WindowsAppSDK/issues/6654 （2026-08）
[10] egui-wgpu Renderer https://docs.rs/egui-wgpu/latest/egui_wgpu/struct.Renderer.html
[11] Tauri/Electron/WPF 实测 https://dawloom.com/blog/tauri-vs-electron-vs-wpf-benchmark/ （2026-10-13）
[12] Flutter Windows 内存 https://stackoverflow.com/questions/78492721 （2024）
[13] libwebrtc 构建 https://gist.github.com/aliforever/045ce51621665fe96f4a0614a4e3a95f （20 GB / 365 MB）
[14] 构建耗时 https://mahsa-hanifi.medium.com/how-to-save-time-building-webrtc-3a05546c924a （2019，3–4 h）
[15] LiveKit C++ SDK https://docs.livekit.io/reference/client-sdk-cpp/md_README.html
[16] rust-sdks build_windows.cmd https://github.com/livekit/rust-sdks/blob/main/webrtc-sys/libwebrtc/build_windows.cmd
[17] webrtc-sdk/libwebrtc https://github.com/webrtc-sdk/libwebrtc （m144）
[18] flutter_webrtc PR #2060 https://github.com/flutter-webrtc/flutter-webrtc/pull/2060 （2026-05-25）
[19] webrtc-audio-processing https://docs.rs/crate/webrtc-audio-processing/latest
[20] str0m https://docs.rs/str0m/latest/str0m/
[21] SIPSorcery LICENSE https://github.com/sipsorcery-org/sipsorcery/blob/master/LICENSE.md
[22] GStreamer webrtcbin https://gstreamer.freedesktop.org/documentation/webrtc/
[23] livekit Dockerfile https://github.com/livekit/livekit/blob/master/Dockerfile
[24] LiveKit 拒绝 P2P https://github.com/livekit/client-sdk-js/issues/270 （2022-06-17）
[25] LiveKit P2P 再拒 https://github.com/livekit/livekit/issues/3468 （2025-03-01）
[26] mediasoup Design https://mediasoup.org/documentation/v3/mediasoup/design/
[27] Pion WebRTC https://github.com/pion/webrtc/blob/main/LICENSE （MIT）
[28] Janus Gateway https://github.com/meetecho/janus-gateway/ （GPLv3）
[29] Jitsi P2P https://jitsi.org/blog/p2p4121/ （2017-07-17）
[30] TS6 屏幕共享原理 https://community.teamspeak.com/t/how-does-share-screen-technically-work/56850 （2025-02-05）
[31] TS6 peer-to-peer SFU https://community.teamspeak.com/t/ts6-beta-community-update-insights/58116 （2025-03-13）
[32] TS6 server #118 https://github.com/teamspeak/teamspeak6-server/issues/118 （2026-10-16）
[33] Discord 语音架构 https://discord.com/blog/how-discord-handles-two-and-a-half-million-concurrent-voice-users-using-webrtc （2018-09-10）
