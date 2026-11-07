# PartyUp

> 自托管的、以频道为单位的桌面**文字 / 语音 / 屏幕共享**软件，给 2–12 人的小圈子用（开黑、看片、远程协作都行）。
> 服务端是**一个二进制**，跑在你自己的机器上，不依赖任何云服务。

## 为什么用它

- **一条命令起服务器**：`docker run` 或一个 exe。没有 livekit-server、没有独立 TURN、没有要单独装的数据库。
- **语音 + 屏幕共享**：同一频道内一键加入语音、共享屏幕；传输方式由服务端配置决定（`relay` 省客户端带宽 / `direct` 省服务器带宽），客户端不用管。
- **数据在自己手里**：账号、频道、聊天记录都在你的机器上（SQLite）。没有联邦、没有外部账号体系、没有遥测。
- **服务器管理界面**：成员踢出/封禁、邀请码、审计日志、公告与欢迎语、传输模式与并发上限，所有者/管理员分级。
- **装好即自动更新**：Windows 客户端会自己检查新版本，你确认后才下载，下载完重启生效。

## 快速开始

### 1. 起服务端（Linux，约 30 秒）

```bash
docker run -d --name partyup --network host --restart unless-stopped \
  -v "$PWD/partyup-data:/data" \
  ghcr.io/ranzonsan/partyup:latest

docker logs partyup | head -20
# BOOTSTRAP_TOKEN=xxxx   ← 首次创建所有者账号用（只能用一次）
# INVITE_CODE=xxxx       ← 发给朋友，他们用这个邀请码注册
```

然后打开 `http://<服务器IP>:8080`，用 `BOOTSTRAP_TOKEN` 创建你的管理员账号。

> ⚠️ **`--network host` 是最省事的方式**：语音/屏幕共享走 UDP，端口默认是动态的，Docker 的 bridge 网络下媒体连不上（文字却正常，很容易误判）。
> 需要 bridge 网络时，用 `--ice-port-range 50000-50010 --ice-public-ip <对外IP>` 配合 `-p 50000-50010:50000-50010/udp`（见 [Docker 部署](docs/DOCKER.md)）。
> Docker Desktop（Windows / macOS）的 host 网络不是真正的宿主网络，**不能**当服务端用——那两个平台请直接跑二进制。

**不想用 Docker？** 下载 `PartyUp-Server-<版本>-win64.zip`，解压运行 `start.cmd`；其他平台见 [打包与分发](docs/PACKAGING.md)。

### 2. 装客户端（Windows）

到 [Releases](https://github.com/ranzonsan/partyup/releases) 下载任一：

| 文件 | 适合谁 |
|---|---|
| `PartyUp-<版本>-win64-Setup.exe` | 大多数人：一键安装、按用户安装，**支持自动更新** |
| `PartyUp-<版本>-win64.msi` | 需要走软件分发 / 选安装范围的 |
| `PartyUp-<版本>-win64-portable.zip` | 绿色版：解压即用，不自带自动更新 |

首次启动填服务器地址（如 `http://192.168.1.10:8080`）；没有账号就找管理员要邀请码。

## 它是怎么跑的

```
┌────────────┐   HTTP / WebSocket（控制面：登录、频道、文字、信令）   ┌──────────────────────┐
│  Windows   │ ─────────────────────────────────────────────────────> │ Go 单二进制           │
│  客户端     │                                                        │  · 控制面            │
│ (Avalonia) │ <──────────────── UDP（语音 / 屏幕共享 媒体，SFU 转发）── │  · Pion SFU（同进程） │
└────────────┘                                                        │  · SQLite            │
       │                                                              └──────────────────────┘
       └─ partyup_media.dll（C++ / libwebrtc，窄 C ABI）
```

细节见 [架构](docs/ARCHITECTURE.md) 与 [协议](docs/PROTOCOL.md)；不可逆的决策记在 [ADR](docs/adr/)。

## 部署要求

| 项 | 参考值 |
|---|---|
| 服务端 | 1 核 / 512 MB 起；relay 模式带宽随并发语音与共享线性增长 |
| 网络 | 控制面 TCP 8080；媒体 UDP（动态端口，或配置固定区间）+ 可选 STUN UDP 3478 |
| 客户端 | Windows 10/11 x64，能访问服务器 |

实测 CPU / 内存 / 带宽见 [容量基准](docs/CAPACITY.md)。

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
| `docker.yml` | `v*` tag / `main` | 服务端镜像 → `ghcr.io/ranzonsan/partyup` |
| `release.yml` | `v*` tag | 客户端与打包产物 → draft GitHub Release |

## 已知限制

- 客户端目前只有 **Windows**（架构跨平台，Linux/macOS 未移植）。
- 安装包**未签名**，Windows SmartScreen 会提示"未知发布者"。
- 没有摄像头、没有端到端加密；文字消息只是中继存储，不承诺备份。
- 屏幕共享在 `direct` 传输模式下仅支持 1 对 1。
- 服务端容器推荐 host 网络；bridge 需要固定端口 + 对外 IP 配置（见 [Docker 部署](docs/DOCKER.md)）。

## 状态

v1 功能完整并通过七条验收（打包分发、邀请码、文字历史持久化、语音、屏幕共享、双传输路径、断线重连）。详见 [产品定义](docs/PRODUCT.md)。

## 许可证

[GPL-3.0](LICENSE)。
