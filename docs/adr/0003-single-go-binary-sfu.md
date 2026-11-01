# 服务端是单进程 Go 二进制，SFU 基于 Pion 自建

控制面（账号、频道、文字、信令）与媒体面（SFU 转发）运行在同一个 Go 进程里。SFU 用 Pion 库实现转发，不部署 livekit-server，也不部署独立 TURN。

_Status_: accepted · 2026-11-01

## 考虑过的方案

- **livekit-server 作 SFU**：媒体栈现成、单二进制、Apache-2.0，但它的客户端 SDK 是 Room 抽象——只能连 LiveKit 服务器，而 LiveKit 官方两次拒绝 P2P。要用它的 SDK 就做不出直连，直连只能另起一套裸 libwebrtc，客户端里会出现两套媒体栈。
- **mediasoup**：给标准 WebRTC 用最好，但它是 Node 模块 + C++ worker 子进程，不是单二进制。
- **Janus**：GPLv3。本项目许可证禁止商用，而 GPL 不允许附加使用限制，二者不能共存。
- **Pion 自建 SFU**（采用）：ICE / DTLS / SRTP / SCTP 全部由库提供，本项目只实现转发策略。

## 后果

- 需要自己实现的部分：订阅关系、流的选择与转发、关键帧请求、带宽反馈的转发。这些是产品逻辑，不是媒体编解码。
- 单机转发上限由 CPU 与上行带宽决定；水平扩展不在 v1 范围。
- 直连打洞失败时回落到本进程的 SFU，不引入 TURN——两套转发路径并存会让"这路流现在走哪"不可观测。
