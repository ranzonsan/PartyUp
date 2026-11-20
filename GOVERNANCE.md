# 治理

> 本文回答一个问题：**谁来决定、按什么程序决定、决定记在哪**。

## 模型：BDFL

本项目采用「仁慈独裁者」模式（BDFL）。**最终决策权归项目 owner**，当前为
[@ranzonsan](https://github.com/ranzonsan)。

这不是投票制项目。但「一人拍板」不等于「无据可查」：**产品或不可逆的技术决策必须写进 ADR**，
理由要留在仓库里，而不是留在某个人的记忆里。

## 角色

| 角色 | 谁 | 能做什么 |
| --- | --- | --- |
| **项目 owner** | 当前 1 人，@ranzonsan | 合并、发版、改名、转让、一票裁定 |
| **贡献者** | 任何人 | 提 issue / PR；不能合并 |
| **使用者** | 部署者与终端用户 | 提 issue、参与讨论 |

> **术语冲突提醒**：产品里的「Server owner」指**某个部署实例**的管理员（见
> [GLOSSARY.md](GLOSSARY.md) 与 [PRODUCT.md](docs/PRODUCT.md)），与本文的「项目 owner」
> 不是一回事。后者指本仓库的所有者。写文档时请写全，不要只写 owner。

未来若要引入按域分权的 maintainer，必须先修改本文，并让 `CODEOWNERS` 真正生效。

## 决策程序

1. **日常改动**：走 PR，由 owner 合并。判据见 [CONTRIBUTING.md](CONTRIBUTING.md)。
2. **不可逆决策**：先写 ADR（`docs/adr/`），状态为 `accepted` 之后才能动代码。
3. **争议**：owner 裁定，并在 ADR 里记录裁定理由。不同意可以保留意见，但不能绕过。

必须写 ADR 的情形（其余情况可选）：

- 协议不兼容变更（旧客户端连不上新服务端，或反之）
- C ABI 的破坏性变更（`partyup-media` 导出变化、调用约定变化）
- 传输语义或默认路径变化（谁决定 `relay` / `direct`、回落条件）
- 身份与权限模型变化（角色、邀请码、封禁语义）
- 数据模型不可逆迁移
- 许可证、依赖策略、发布通道的变化
- 放弃某个已经对用户承诺过的能力

## 发布

- 只有 owner 能打 tag 与创建 release。
- 三条独立通道：客户端 `v*`（版本在 `client/VERSION`）、服务端 `server-v*`
  （`server/VERSION`）、媒体核心 `media-core-v*`（ABI 版本记录在 `ARCHITECTURE.md`）。
- 发布前必须过 CI；媒体核心的 ABI 版本变化要同时更新
  [ARCHITECTURE.md](docs/ARCHITECTURE.md) 与 [PROTOCOL.md](docs/PROTOCOL.md) 的兼容说明。

## 所有权与移交

- 仓库、名称与发布签名当前归 @ranzonsan。
- 若 owner 连续 **12 个月**没有合并或发布，应在 issue 中公开讨论移交；移交范围包括仓库、
  镜像地址与 release 签名权。
- 本项目的服务器实例可以由任何人部署，实例的管理权移交是**产品功能**，与项目治理无关。

## 修改本文

修改治理规则需要 owner 明确同意，并在提交信息里写明理由。治理变更不要求 ADR，
但必须在本文的变更处说明生效时间。
