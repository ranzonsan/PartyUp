# PartyUp

PartyUp 是一个面向小型团体的轻量级自托管桌面语音、文字交流与屏幕共享工具。

## 特点

- 轻量且易于上手：使用 .NET 10 + Avalonia，界面简洁流畅且性能占用低。
- 完整的交流体验：语音、视频以及文字媒体一应俱全。
- 简单的权限模型：仅 owner、admin、user 三级模型，减少繁琐配置
- 部署快捷：服务端内置控制面、Pion SFU、STUN 与 SQLite 存储。仅使用一条命令或单个可执行文件即可稳定运行，不需要额外服务。
- 低服务端性能需求：（即将）支持点对点（Peer to Peer）语音和视频传输，尽可能减少服务器网络压力。

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
- 至少 2 个能够被您的用户访问的端口。一个用于服务器的主要通信，另一个用于语音传输。

> [!NOTE]
> 对于端口的需求可能会随着功能更新而改变。

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

使用文档请见 [Wiki](https://github.com/ranzonsan/PartyUp/wiki)。
有关项目开发的文档，请见 [Docs](docs)。

## 从源码构建

```bash
# 服务端（Go 1.26+）
cd server && go build ./cmd/partyup

# 客户端（.NET 10 + Avalonia）
dotnet build client/PartyUp.Client.csproj -c Release

# 原生媒体库（需要 MSVC + libwebrtc 预编译包，见 docs/DEV.md）
pwsh -File scripts/build-media-dll.ps1
```

## 贡献

- **[贡献指南](CONTRIBUTING.md)**：本章节明确了为本项目贡献代码、文档或其他内容的方法与界限。
- **[项目边界](docs/BOUNDARIES.md)**：本章节从范围、架构、协作、法律四部分规定了一个（新）需求的可行性。
- **[行为准则](CODE_OF_CONDUCT.md)**：本章节规定了参与 issue（工单）、Pull Request（合并请求）与讨论必须要遵守的规则。
- **[安全策略](SECURITY.md)**：本章节规定了项目的漏洞提交规范。请通过私密渠道提交漏洞，不在公开 issue 里展示细节。
- **[治理](GOVERNANCE.md)**：本章节说明了项目的决策权、ADR 流程与发布流程；**[名称使用](TRADEMARK.md)** 规定了本项目的名称及图标的使用。

本项目允许且欢迎各种在规范约束下生成的 AI 代码。

## 许可证

PartyUp 使用 [GPL-3.0](LICENSE) 作为许可证。

请留意，PartyUp 所使用的各种开源组件的许可证不尽相同。
