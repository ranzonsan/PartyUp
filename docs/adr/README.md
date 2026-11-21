# 架构决策记录（ADR）

ADR 记录**不可逆或高代价的决定及其理由**。它回答「当时为什么这么定」，而不是「现在是什么」——
后者归 [PRODUCT.md](../PRODUCT.md)、[ARCHITECTURE.md](../ARCHITECTURE.md)、[PROTOCOL.md](../PROTOCOL.md)。

## 什么时候必须写

命中任意一条就要写：

- 协议不兼容变更（旧客户端连不上新服务端，或反之）
- C ABI 的破坏性变更
- 传输语义或默认路径变化（谁决定 `relay` / `direct`、回落条件）
- 身份与权限模型变化
- 数据模型不可逆迁移
- 许可证、依赖策略、发布通道的变化
- 放弃某个已经对用户承诺过的能力

日常改动、内部重构、单个 bug 修复不需要 ADR。

## 怎么写

1. 复制 [TEMPLATE.md](TEMPLATE.md)，命名为 `NNNN-kebab-case-title.md`，编号取当前最大值 +1。
2. 状态用 `proposed` 提交；讨论收敛后由项目 owner 改为 `accepted`。
3. **`accepted` 之前不得动对应的代码。**
4. 决定被推翻时**不要删除**旧 ADR：在新 ADR 里写清取代关系，并在旧文件头部加一段说明
   （参考 [0002](0002-transport-per-media.md) 的做法）。
5. 状态取值：`proposed` / `accepted` / `superseded by NNNN` / `deprecated`。

## 索引

| 编号 | 决策 | 状态 |
| --- | --- | --- |
| [0001](0001-self-hosted-only.md) | 只做自托管，不运营官方实例 | accepted · 2026-11-01 |
| [0002](0002-transport-per-media.md) | 传输路径按媒体类型分别决定 | accepted · 2026-11-01（「模式由用户选择」部分已于 2026-11-03 被服务端配置方案取代） |
| [0003](0003-single-go-binary-sfu.md) | 服务端是单进程 Go 二进制，SFU 基于 Pion 自建 | accepted · 2026-11-01 |
| [0004](0004-flat-channels.md) | 频道只有一层，不分类型 | accepted · 2026-11-01 |
| [0005](0005-local-identity.md) | 身份属于服务器，不做全局账号与联邦 | accepted · 2026-11-01 |
| [0006](0006-client-stack.md) | 客户端用 Avalonia + C++ 媒体核心，不用 Qt | accepted · 2026-11-01 |
| [0007](0007-docs-layering.md) | 文档分层与权威来源：docs/ 入库，.docs/ 只放过程 | accepted · 2026-11-21 |

## 与其它文档的关系

- ADR 记「为什么」，其余文档记「现在是什么」。两者冲突时，以 `accepted` 的 ADR 为准，
  并修正另一份文档。
- 边界本身的变化（[BOUNDARIES.md](../BOUNDARIES.md) 的四类）也必须先走 ADR。
