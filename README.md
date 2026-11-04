# PartyUp

自托管的、以频道为单位的桌面**文字 / 语音 / 屏幕共享**软件。面向 2–12 人的朋友群体开黑与共享屏幕。

## 文档

- [产品定义](docs/PRODUCT.md) — 目标、非目标、传输策略、v1 验收
- [开发环境与版本基线](docs/DEV.md) — 装什么、装哪版、怎么验、构建流、已知坑
- [打包与分发](docs/PACKAGING.md) — 一条命令产出绿色版 / Setup / MSI / 服务端包，版本源、未签名与 SmartScreen 说明
- [架构](docs/ARCHITECTURE.md) — 组件、C ABI 边界、数据流、路径状态机、风险与 spike
- [协议](docs/PROTOCOL.md) — 信令消息形状（草案）
- [术语表](GLOSSARY.md) — 项目语言，写作与代码命名以此为准
- [架构决策记录](docs/adr/) — 不可逆决定及其理由
- [调研：桌面 GUI 与媒体栈](docs/research/2026-11-01-desktop-media-stack.md) — 选型事实与来源
- [容量基准](docs/CAPACITY.md) — 实测 CPU / 内存 / 带宽，及建议机器配置
- [品牌资产](brand/) — 图标、锁屏图、横竖版 lockup

## 本机开发日志

验证性 spike 的过程记录、实测证据（截图、候选地址、日志）保存在工作区的 `.docs/` 里，**不入库**——它们含内网地址、机器型号与临时 token，且属于「过程」而非「文档」。`docs/` 只放对使用者与贡献者长期有效的定义、协议与决策。

## 许可证

GPL-3.0，见 [LICENSE](LICENSE)。

## 状态

v1 功能完整并通过七条验收（打包分发、邀请码、文字历史持久化、语音、屏幕共享、双传输路径、断线重连）。详见 [产品定义](docs/PRODUCT.md)。
