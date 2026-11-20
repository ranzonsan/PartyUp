# 文档索引

本目录（`docs/`）**入库**，面向使用者与贡献者。过程记录、实测证据、含内网信息的材料在 `.docs/`，
**不入库**——判据见文末「文档分层」。

## 按身份读

| 你是 | 按顺序读 |
| --- | --- |
| **第一次听说这个项目** | [README](../README.md) → [PRODUCT](PRODUCT.md) |
| **要部署服务器** | [README](../README.md) 快速开始 → [DOCKER](DOCKER.md) → [PACKAGING](PACKAGING.md) → [CAPACITY](CAPACITY.md) |
| **要改代码** | [CONTRIBUTING](../CONTRIBUTING.md) → [BOUNDARIES](BOUNDARIES.md) → [ARCHITECTURE](ARCHITECTURE.md) → [PROTOCOL](PROTOCOL.md) → [DEV](DEV.md) |
| **要判断一个需求该不该做** | [BOUNDARIES](BOUNDARIES.md) → [PRODUCT](PRODUCT.md)「非目标」 |
| **AI agent** | [AGENTS.md](../AGENTS.md) → [BOUNDARIES](BOUNDARIES.md) → [GLOSSARY](../GLOSSARY.md) |

## 全部文档

| 文档 | 内容 | 状态 |
| --- | --- | --- |
| [PRODUCT.md](PRODUCT.md) | 产品定义、目标与非目标、传输策略、安全边界 | v0.1 |
| [ARCHITECTURE.md](ARCHITECTURE.md) | 组件、三条边界、数据流、路径状态机 | v0.1 |
| [PROTOCOL.md](PROTOCOL.md) | 信令消息形状与错误码 | v1.1 草案 |
| [BOUNDARIES.md](BOUNDARIES.md) | 范围 / 架构 / 协作 / 法律四类边界与判据 | v1 |
| [DEV.md](DEV.md) | 工具链基线、构建流、已知坑 | 持续更新 |
| [DOCKER.md](DOCKER.md) | 容器化、host 网络 vs 固定端口、排障 | v0.1 |
| [PACKAGING.md](PACKAGING.md) | 绿色版 / Setup / MSI / 服务端包、版本源 | v0.1 |
| [CAPACITY.md](CAPACITY.md) | 实测 CPU / 内存 / 带宽基准 | 持续更新 |
| [adr/](adr/) | 架构决策记录：不可逆决定与理由 | 6 篇 |
| [research/](research/) | 选型调研（带来源） | — |
| [design/](design/) | v1.1 的界面与契约设计 | — |
| [GLOSSARY.md](../GLOSSARY.md) | 项目语言，写作与命名以此为准 | v1 |

## 根目录的社区文件

| 文件 | 作用 |
| --- | --- |
| [CONTRIBUTING.md](../CONTRIBUTING.md) | 贡献流程、PR 门槛、AI 参与规则 |
| [CODE_OF_CONDUCT.md](../CODE_OF_CONDUCT.md) | 行为准则与举报渠道 |
| [SECURITY.md](../SECURITY.md) | 漏洞报告、支持版本、威胁模型 |
| [GOVERNANCE.md](../GOVERNANCE.md) | 决策权、ADR 流程、发布与移交 |
| [TRADEMARK.md](../TRADEMARK.md) | 名称与图标的使用边界 |
| [AGENTS.md](../AGENTS.md) | 给 AI agent 的仓库地图与硬规则 |

## 文档分层

判断标准很简单：**三个月后是否仍然成立。**

- 仍然成立（产品事实、协议形状、部署步骤、架构理由）→ `docs/`，入库。
- 只对当时的决策成立（实验记录、实测截图、候选地址、临时 token、内网 IP）→ `.docs/`，
  **gitignore，不入库**。

**绝不把 `.docs/` 的内容抄进 `docs/`。** 需要引用实测结论时，写结论与编号，不写原始证据。
