# PartyUp

自托管的、以频道为单位的桌面**文字 / 语音 / 屏幕共享**软件。面向 2–12 人的朋友群体开黑与共享屏幕。

## 文档

- [产品定义](docs/PRODUCT.md) — 目标、非目标、传输策略、v1 验收
- [开发环境与版本基线](docs/DEV.md) — 装什么、装哪版、怎么验、构建流、已知坑
- [架构](docs/ARCHITECTURE.md) — 组件、C ABI 边界、数据流、路径状态机、风险与 spike
- [协议](docs/PROTOCOL.md) — 信令消息形状（草案）
- [术语表](GLOSSARY.md) — 项目语言，写作与代码命名以此为准
- [架构决策记录](docs/adr/) — 不可逆决定及其理由
- [调研：桌面 GUI 与媒体栈](docs/research/2026-11-01-desktop-media-stack.md) — 选型事实与来源
- [Spike 1：1080p60 帧呈现](docs/spikes/2026-11-01-frame-presentation.md) — 通过，托管呈现路径足够，v1 不做 D3D 互操作
- [Spike 2：libwebrtc 链接](docs/spikes/2026-11-01-libwebrtc-link.md) — 通过，m152 静态库可被 MSVC 链接运行

## 许可证

GPL-3.0，见 [LICENSE](LICENSE)。

## 状态

产品与架构决策已定稿（2026-11-01）。下一步是验证性 spike（帧呈现、libwebrtc 构建、SFU 转发），代码尚未开始。
