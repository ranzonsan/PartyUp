# PartyUp

PartyUp 是一个面向小型团体的轻量级自托管桌面语音、文字交流与屏幕共享工具。

## 特点

- 轻量且易于上手：使用.NET 10 + Avalonia，界面简洁流畅且性能占用低。
- 完整的交流体验：语音、视频以及文字媒体一应俱全。
- 简单的权限模型：仅owner、admin、user三级模型，减少繁琐配置
- 部署快捷：服务端内置控制面、Pion SFU、STUN 与 SQLite 存储。仅使用一条命令或单个可执行文件即可稳定运行，不需要额外服务。
- 低服务端性能需求：（即将）支持语音、视频端到端，尽可能减少服务器网络压力。

## 快速开始

### 1. 使用客户端（Windows）

从 [Releases](https://github.com/ranzonsan/partyup/releases/latest) 下载最新版安装包：

- `PartyUp-<版本>-win64-Setup.exe`：普通用户推荐。支持自动更新。
- `PartyUp-<版本>-win64.msi`：适合需要指定安装路径或通过软件分发安装的场景。
- `PartyUp-<版本>-win64-portable.zip`：免安装绿色版，不支持自动更新。

首次启动后填写服务器地址，并用管理员提供的邀请码注册加入。

### 2. 部署服务端（自建服务器）  

您的服务器需要满足以下需求：
- 至少 512MB 可用存储及 256MB 可用内存。
- 至少1 个能够被您的用户访问的端口

PartyUp 默认使用 Docker 镜像打包服务端，您可使用以下命令快捷建立一个服务端：

```bash
docker run -d --name partyup --network host --restart unless-stopped \
  -v "$PWD/partyup-data:/data" \
  ghcr.io/ranzonsan/partyup:latest
```

成功启动后，通过日志查看初始化 Token。

```bash
docker logs partyup | head -20
```

使用初始化 Token 可注册此服务器的服主账户。  

> [!CAUTION]
> 请在服主注册前妥善保管好初始化 Token。任何能够连接到服务器的人都能够通过此 Token 注册服主账户并接管服务器。

## 已知限制及问题

- 仅支持 Windows 客户端。
- 安装包未签名，Windows SmartScreen 会提示"未知发布者"。
- 端到端功能仍在开发中。

## 文档

| 文档 | 内容 |
|---|---|
| [产品定义](docs/PRODUCT.md) | 目标、非目标、传输策略、v1 验收 |
| [架构](docs/ARCHITECTURE.md) | 组件、C ABI 边界、数据流、路径状态机 |
| [协议](docs/PROTOCOL.md) | 信令消息形状 |
| [Docker 部署](docs/DOCKER.md) | 容器化、host 网络 vs 固定端口、排障 |
| [打包与分发](docs/PACKAGING.md) | 绿色版 / Setup / MSI / 服务端包、版本源、SmartScreen |
| [开发环境](docs/DEV.md) | 工具链基线、构建流、已知坑 |
| [术语表](GLOSSARY.md) | 项目语言，写作与代码命名以此为准 |
| [项目边界](docs/BOUNDARIES.md) | 范围 / 架构 / 协作 / 法律四条线，提需求前先看 |
| [文档索引](docs/README.md) | 按身份（使用者 / 部署者 / 贡献者 / agent）找到该读的文档 |
| [架构决策记录](docs/adr/) | 不可逆决定及其理由 |
| [调研：桌面 GUI 与媒体栈](docs/research/2026-11-01-desktop-media-stack.md) | 选型事实与来源 |
| [品牌资产](brand/) | 图标、锁屏图、横竖版 lockup |

验证性过程记录与实测证据（截图、候选地址、临时 token）保存在 `.docs/`，**不入库**——它们含内网地址与机器信息，且属于过程而非文档。

## 从源码构建

```bash
# 服务端（Go 1.26+）
cd server && go build ./cmd/partyup

# 客户端（.NET 10 + Avalonia）
dotnet build client/PartyUp.Client.csproj -c Release

# 原生媒体库（需要 MSVC + libwebrtc 预编译包，见 docs/DEV.md）
pwsh -File scripts/build-media-dll.ps1
```

CI 全部跑在 GitHub Actions：

| workflow | 触发 | 产出 |
|---|---|---|
| `ci.yml` | push / PR | Go 测试 + 客户端编译 |
| `media-core.yml` | 仅 `src/media-core/**` 或手动 | `partyup_media.dll` → `media-core-v*` release |
| `docker.yml` | `server-v*` tag / `main` | 服务端镜像 → `ghcr.io/ranzonsan/partyup` |
| `release-client.yml` | `v*` tag | 客户端三件套 → draft Release（Latest） |
| `release-server.yml` | `server-v*` tag | 服务端 zip → draft Release（不发 Latest） |

## 参与

- **[贡献指南](CONTRIBUTING.md)**：什么样的改动会被接受、怎么提、怎么验。**动手前先读其中的「不会被接受的改动」。**
- **[项目边界](docs/BOUNDARIES.md)**：范围 / 架构 / 协作 / 法律四条线。不确定一个需求该不该做，看这里。
- **[行为准则](CODE_OF_CONDUCT.md)**：参与 issue、PR 与讨论的前提。
- **[安全策略](SECURITY.md)**：漏洞请走私密渠道，不要在公开 issue 里贴细节。
- **[治理](GOVERNANCE.md)**：决策权、ADR 流程与发布流程；**[名称使用](TRADEMARK.md)** 说明 fork 时该怎么改名。

用 AI 写代码也欢迎，但需要披露并附上真跑过的验证——规则在贡献指南里。

## 许可证

PartyUp 使用 [GPL-3.0](LICENSE) 作为许可证。
