# 参与贡献

> 这份文档说明**什么样的改动会被接受、怎么提、怎么验**。
> 产品范围见 [PRODUCT.md](docs/PRODUCT.md)，四类边界见 [BOUNDARIES.md](docs/BOUNDARIES.md)，术语以 [GLOSSARY.md](GLOSSARY.md) 为准。

## 先读三份

1. **[PRODUCT.md](docs/PRODUCT.md) 的「非目标」** —— 越界的改动会被直接关闭，这属于范围问题，不是评审意见的分歧。
2. **[BOUNDARIES.md](docs/BOUNDARIES.md)** —— 范围 / 架构 / 协作 / 法律四条线的入口，每条都指向权威文档。
3. **[GLOSSARY.md](GLOSSARY.md)** —— 文档与代码命名都用这里的词。不要为同一个概念自创同义词。

## 欢迎的改动

| 类型 | 例子 |
| --- | --- |
| 缺陷修复 | 带最小复现步骤，能说明根因更好 |
| 部署与运维 | Docker、Windows 服务端包、排障脚本、日志可读性 |
| 客户端可用性 | 键盘操作、焦点顺序、高分屏、错误提示措辞 |
| 媒体质量与稳定性 | 有前后对比数据（丢帧、延迟、CPU、带宽） |
| 文档 | 纠错、补示例、翻译 |
| 测试与 CI | 让失败更早被发现的改动 |

## 不会被接受的改动

| 改动 | 原因 |
| --- | --- |
| 官方托管 / 注册即用的 SaaS | [ADR-0001](docs/adr/0001-self-hosted-only.md) |
| 无服务器（纯直连 / 局域网）模式 | [ADR-0002](docs/adr/0002-transport-per-media.md) |
| 移动端、会议能力（日历、录制、举手、主持人转移） | PRODUCT 非目标 |
| 端到端加密 | 与中继架构冲突，见 PRODUCT「安全边界」 |
| v1 里的逐成员混合路径 / 成员中继 | 已明确排到 v1 之后，见 PRODUCT 非目标 |
| 更换核心栈（Qt、Electron、自研 SFU、libwebrtc 大版本跳跃） | 需要先有 accepted 的 ADR |
| 遥测、外部账号体系、云依赖 | 与自托管定位冲突 |

不确定是否越界时，先开 issue 问，不要先写代码。

## 开发环境

最小验证命令如下，完整工具链与已知坑见 [DEV.md](docs/DEV.md)。

```bash
# 服务端（Go）
cd server && go build ./cmd/partyup && go test ./...

# 客户端（.NET 10 + Avalonia）
dotnet build client/PartyUp.Client.csproj -c Release

# 原生媒体库（Windows + MSVC + libwebrtc 预编译包）
pwsh -File scripts/build-media-dll.ps1
```

C++ 媒体核心只能在 Windows 上构建。仓库的 CI 跑在 GitHub Actions：`ci.yml` 跑 Go 测试与客户端编译，
`media-core.yml` 构建 DLL，`docker.yml` 与两条 release 工作流负责分发。

## 提交

- 一个提交做一件事；不要在一次提交里混入格式化、重命名与行为变更。
- 提交消息用中文，第一行不超过 50 字，动词开头：`新增` / `修复` / `重构` / `文档` / `构建` / `测试`。
- 第一行写「做了什么」，正文写「为什么」；「改了什么」由 diff 说明。
- 示例：

  ```
  修复：邀请码过期后客户端仍显示可加入

  服务端返回 INVITE_EXPIRED 时客户端只弹了通用错误，
  用户会反复重试。现在按错误码给出「邀请码已过期，请找管理员要新的」。
  ```

## PR 要求

- 说明**目的**与**影响面**：协议、C ABI、数据迁移、文档，各自是否涉及。
- 给出**验证方式**：跑过的命令与结果。不能跑的要写明原因，不要假装通过。
- 改了协议就同步 [PROTOCOL.md](docs/PROTOCOL.md)；改了不可逆决策就走 [ADR](docs/adr/)。
- 只改 `docs/`，**不要动 `.docs/`** —— 后者是过程记录与实测证据，不入库。

## AI 参与

允许，并且这个项目本身就是这么长出来的。但有几条硬要求：

- 在 PR 描述里说明**哪个环节用了 AI**（起草代码、写测试、查资料），不必写模型名。
- 你必须能解释每一行改动的理由。「生成后没看」视为未完成。
- 不接受大规模、无验证的批量生成：重写模块、全仓改名、自动修 lint。
- 不接受用脚本绕过评审的批量改写（见 [AGENTS.md](AGENTS.md) 的代码改动纪律）。
- AI 产出的测试必须真跑过，附上输出。

## 评审与合并

- 本项目采用 **BDFL**：最终合并权在 owner，见 [GOVERNANCE.md](GOVERNANCE.md)。
- 合并即表示你同意以 **GPL-3.0** 授权你的贡献（inbound = outbound）。**不需要签 CLA。**
- 你必须有权提交这些代码；由雇主或第三方条款引起的纠纷由提交者自负。
- 评审不承诺响应时间。超过 60 天没有回应的 PR 可能被关闭，需要时随时重开。

## 行为与安全

- 行为问题 → [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md)
- 安全漏洞 → [SECURITY.md](SECURITY.md)，**不要**在公开 issue 里贴细节
- 使用问题 → 用 issue 模板，或到 Discussions
