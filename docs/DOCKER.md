# 用 Docker 部署服务端

服务端是**单个静态 Go 二进制**（控制面 HTTP/WebSocket + 同进程 Pion SFU，没有 livekit-server、没有独立 TURN），容器镜像约 20 MB。

网络配置有**两种模式**，选一种：

| 模式 | 适合 | 一句话 |
|---|---|---|
| **host 网络**（推荐，最省事） | VPS、NAS、家里的 Linux 机器 | 容器直接用宿主网络，与裸机运行完全等价 |
| **bridge + 固定端口** | 需要端口隔离、或宿主不便用 host 网络 | 靠 `--ice-port-range` + `--ice-public-ip` 显式告诉客户端该连哪里 |

> Docker Desktop（Windows / macOS）的 host 网络不是真正的宿主网络，**不能**当服务端用——那两个平台请直接跑二进制（见 [PACKAGING.md](PACKAGING.md)）。

## 为什么媒体需要额外配置

| 流量 | 端口 | 说明 |
|---|---|---|
| 控制面（登录/频道/文字/信令） | TCP 8080 | 普通 HTTP + WebSocket |
| 内建 STUN | UDP 3478 | 直连模式（`--voice-mode direct`）下客户端取 srflx 候选 |
| **媒体（语音/屏幕共享）** | **UDP，默认动态** | 不配置时 Pion 从系统临时端口里挑，无法用 `-p` 逐个映射 |

bridge 网络下容器内地址是 `172.17.x.x`，客户端连不到；表现很有迷惑性——**文字聊天一切正常，一进语音就连不上**。

## 模式一：host 网络（推荐）

```bash
docker run -d --name partyup --network host --restart unless-stopped \
  -v "$PWD/partyup-data:/data" \
  ghcr.io/ranzonsan/partyup:latest

docker logs partyup | head -20
#   ICE_PORT_RANGE=unset
#   ICE_PUBLIC_IP=unset
#   BOOTSTRAP_TOKEN=xxxx   ← 用它创建所有者账号
#   INVITE_CODE=xxxx       ← 发给朋友注册
```

公网 / 多网卡 / 有 NAT 时加 `--stun-advertise <公网IP>:3478`（否则通告的是内网地址）。

## 模式二：bridge + 固定端口

```bash
docker run -d --name partyup --restart unless-stopped \
  -p 8080:8080 \
  -p 50000-50010:50000-50010/udp \
  -p 3478:3478/udp \
  -v "$PWD/partyup-data:/data" \
  ghcr.io/ranzonsan/partyup:latest \
  serve --data /data --http 0.0.0.0:8080 \
        --ice-port-range 50000-50010 \
        --ice-public-ip 203.0.113.10 \
        --stun-advertise 203.0.113.10:3478
```

三个参数的分工：

| 参数 | 作用 | 不填会怎样 |
|---|---|---|
| `--ice-port-range 50000-50010` | ICE 的 UDP socket 只用这段端口 | 端口动态，`-p` 无法精确映射 |
| `--ice-public-ip 203.0.113.10` | 把 host 候选改写成该 IP（Pion `SetNAT1To1IPs`） | 客户端拿到 `172.17.x.x`，媒体连不上 |
| `--stun-advertise` | 内建 STUN 通告的地址 | srflx 候选是内网地址 |

要点：

- **家用 NAT**：把这段 UDP 在路由器上转发到宿主机（如 `50000-50010/udp`），`--ice-public-ip` 填**公网 IP**。
- **VPS**（公网 IP 直接绑在网卡上）：`--ice-public-ip` 填公网 IP，端口段无需转发。
- **区间跨度上限 4096**；超了启动即报错（`span ... exceeds limit`）。
- 端口区间应避开系统保留段。Windows 上可用 `netsh int ipv4 show excludedportrange protocol=udp` 查看 Hyper-V/WSL 的保留范围（本机实测该段即使被列出，显式 bind 仍可用；遇到真实占用就换一段）。
- 校验失败会在启动时直接退出并带 flag 名，不会静默忽略：
  ```
  --ice-port-range: span 65535 exceeds limit 4096
  --ice-public-ip: "not-an-ip" is not an IP address
  ```

启动日志会打印实际生效的两项，便于确认配置真的进去了：

```
ICE_PORT_RANGE=50000-50010
ICE_PUBLIC_IP=203.0.113.10
```

## docker compose

仓库带两份可直接改的模板：[`deploy/docker-compose.yml`](../deploy/docker-compose.yml)（host 网络）；bridge 版本是文件末尾的注释块。

```bash
cd deploy
docker compose up -d
docker compose logs -f partyup
```

## 其他参数

| 场景 | 参数 | 为什么 |
|---|---|---|
| 前面有反向代理终止 TLS | `--trust-proxy` | bootstrap 默认只允许回环或 TLS 连接 |
| 改监听端口 | `--http 0.0.0.0:9000` | 记得同步防火墙 |
| 传输模式 | `--voice-mode relay\|direct`、`--screen-mode relay\|direct` | 默认都是 relay；`direct` 更省服务器带宽，但要求客户端能直连 |

## 数据与升级

- **数据**：`/data`（SQLite 库 + 设置）。务必挂卷，否则删容器 = 账号和频道全没了。
- **升级**：`docker pull ghcr.io/ranzonsan/partyup:vX.Y.Z` → `docker compose up -d`。schema 迁移在启动时自动执行。
- **回滚**：镜像 tag 是版本化的；**回滚前先备份 `/data`**（新版本可能已迁移 schema）。
- **健康检查**：镜像自带 `HEALTHCHECK`（GET `/api/server-info`）。

## 排障

| 症状 | 先查什么 |
|---|---|
| 能登录、文字正常，语音/共享连不上 | bridge 是否配了 `--ice-port-range` + `--ice-public-ip`；或直接改 host 网络 |
| `ICE_PUBLIC_IP=unset` | 忘了传 `--ice-public-ip`（bridge 模式必填） |
| 客户端卡在"正在连接服务器" | 8080 是否可达；`docker logs` 有无 READY |
| 启动失败提示 `span ... exceeds limit` | 端口区间跨度 > 4096，缩小它 |
| 启动失败提示 `is not an IP address` | `--ice-public-ip` 传了域名/主机名，这里只接受 IP |
| 直连模式（`direct`）总回落到 relay | `--stun-advertise` 没设；UDP 3478 是否放通 |
| 启动即退出并报 `--data` 相关错误 | 卷写权限（容器以 uid 10001 运行）：`chown -R 10001 /path/to/data` |
