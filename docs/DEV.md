# 开发环境与版本基线

> 本文是「怎么装、装哪版、怎么验、走不走代理」的唯一出处。改了版本先改这里，再改机器。

## 原则：两级固定

**第一级 —— 项目声明的依赖：严格锁定，进 git，可复现。**

- .NET SDK：`global.json` 锁定 `10.0.400`（rollForward: latestFeature，只允许 10.0.4xx 内前滚）。
- NuGet 包：`Directory.Packages.props` 集中管理 + **精确版本**（禁止 `*` 与区间）+ `packages.lock.json` 锁传递依赖。
- Go 依赖：`go.mod` 的 `go` 行 + `go.sum`；`GOFLAGS=-mod=readonly`。
- libwebrtc：固定到 commit SHA，不用 tag、不用 master。
- 任何 `latest` / `main` / `*` 都视为缺陷。

**第二级 —— 机器工具链：记录基线 + 校验，不假装能冻结。**

VS 更新会悄悄换掉 MSVC 工具集、CMake 大版本会改默认行为，这些没法靠配置文件冻结。做法是：基线写死在下表，`scripts/check-env.ps1` 开工时比对，**漂移了报错让你决定**，而不是静默用新版本编出一个只在你这台机器成立的二进制。

## 基线表

| 组件 | 基线版本 | 锁定方式 | 本机状态 |
| --- | --- | --- | --- |
| .NET SDK | **10.0.400** | `global.json` | ✅ Windows `C:\Program Files\dotnet` |
| MSVC 工具集 | **VS BuildTools 17.14.44 / v143** | 本表 + 校验脚本 | ✅ 已装 |
| CMake | **4.4.4** | 本表 + `cmake_minimum_required` | ✅ `C:\Program Files\CMake\bin` |
| Go（WSL，构建/运行服务端） | **1.26.8** | `go.mod` 写 `go 1.26.0` | ✅ apt（`golang-go`） |
| Go（Windows，仅本地联调） | 1.27.3 | 不参与项目锁定 | ✅ winget |
| Python（仅 depot_tools 用） | 3.13.16 | 本表 | ✅ winget |
| Avalonia | 引入时固定 | `Directory.Packages.props` + lock | 未引入 |
| libwebrtc | 引入时固定 | FetchContent `GIT_TAG <sha>` | 未引入 |
| Pion | 引入时固定 | `go.mod` / `go.sum` | 未引入 |

两处 Go 版本不一致是**有意为之**：项目只要求 ≥1.26（`go 1.26.0`），WSL 的 apt 版与 Windows 的 winget 版都能编。`toolchain` 行不写死，避免在没有 1.27 的机器上触发自动下载。

## 网络：镜像与代理

构建期会联网的只有下面几步，其余都是本地编译：

| 步骤 | 下载什么 | 通道 | 本机实测 |
| --- | --- | --- | --- |
| `dotnet restore` | Avalonia、SkiaSharp 等 NuGet 包 | **直连 nuget.org**（不需要代理） | `api.nuget.org` 200，0.38 s |
| `go mod download` | Pion 等模块 | **goproxy.cn 镜像** | `goproxy.cn` 200，0.08 s |
| `pip install`（仅 depot_tools） | 若干 wheel | PyPI 国内镜像 | 未验证 |
| libwebrtc 源码 + CIPD | 约 20 GB | **只能走代理，没有镜像** | — |
| `FetchContent`（若用） | 从 GitHub 拉依赖 | 代理，或改成镜像地址 | — |
| winget 安装工具本体 | 安装包 | 走代理（已完成） | — |

命令：

```bash
# WSL：Go 模块走镜像（一次性，写入 ~/.config/go/env）
go env -w GOPROXY=https://goproxy.cn,direct GOSUMDB=sum.golang.google.cn
```

```powershell
# Windows：只有直连失败时才需要，临时给当前会话
$env:HTTPS_PROXY='http://127.0.0.1:24051'; $env:HTTP_PROXY='http://127.0.0.1:24051'
```

注意：仓库的构建脚本会**自己设置 `GOPROXY`**，不依赖机器全局配置——换台机器也能复现同一套行为。上面的 `go env -w` 只是让你在仓库外手动敲 `go` 命令时也顺畅。

## PATH：新装的 Windows 工具进不了已运行的 WSL 会话

DSH 进程的 `PATH` 是启动时的快照。刚才 winget 装的 CMake/Go/Python 在 WSL 里直接敲 `cmake.exe` 会「找不到命令」，但用绝对路径能跑：

```
C:\Program Files\CMake\bin\cmake.exe
C:\Program Files\Go\bin\go.exe
C:\Users\<user>\AppData\Local\Programs\Python\Python313\python.exe
```

三个办法，按顺序选：(1) 脚本里显式拼 PATH；(2) 新开一个 WSL 会话；(3) 正在跑的 DSH 不用重启，调用时用绝对路径即可。

```powershell
$env:Path = "C:\Program Files\CMake\bin;C:\Program Files\Go\bin;$env:Path"
```

## 媒体核心 ABI（v2.14 加法 + v2.15 语义 + v2.16 接收隔离）

`src/media-core` 仍是**加法式 ABI**：旧导出签名/结构布局不变，新能力只加新导出与新结构。
v2.14（v1.1 单流观看 / 音频设备）新增 6 个导出；v2.15 没有新增导出，只增强了已有音频导出的
语义；v2.16（B2 接收隔离）新增 5 个导出，共 **35** 个：

- `pu_engine_caps(engine, uint32_t* caps)`：**能力真源**。bit0 watch、bit1 watch slots、
  bit2 audio devices、bit3 voice level、bit4（v2.15）音频通话中真实热切 + 稳定 ID
  选择 + 系统默认跟随/热插拔回退、bit5（v2.16/B2）独立 receive-only 接收会话
  （receive 导出 + 按会话信令/统计 + 发送暂停）。旧 DLL 没有本导出（或没有对应位）；
  并且旧 DLL 的 `pu_screen_watch/unwatch` **导出存在但返回 `PU_NOT_IMPLEMENTED`**，
  所以判断可用性只能看这个探测，不能看导出是否存在。
- v2.16（B2）严格接收：`pu_screen_receive_open(engine, session_id, &token)` 在发
  `screen.watch` 前建立一条独立的 receive-only PC；`pu_signal_in_session(engine,
  "screen", session_id, payload, len)` 把服务端 offer 灌到该会话（旧会话 id 返回
  `PU_ERR_STATE`）；`pu_screen_receive_close` 只关闭该接收 PC，不碰发布/采集/语音；
  `pu_screen_receive_stats` 按会话报 `active/token/video_tracks/frames_delivered`；
  `SIGNAL_OUT.reserved` 复用为会话 token，客户端据此把 answer 带回正确 session。
  `pu_screen_pause_send(engine, paused)` 只 `SetTrack(nullptr|track_)`，保留采集源、
  视频轨、sender 与发布状态（direct 1v1 的停看控制，不重协商）。
- `pu_screen_watch_ex(engine, sharer_id, slot, generation)`：arm「目标 + 槽位 + 代次」。
  `slot=-1` 用于 direct；relay 用服务端下发的 0/1。原生把 token 捕获进 sink，
  回调时与当前 arm 比对，目标切换/停止后旧帧直接丢弃（`pu_frame_acquire` 也校验 epoch）。
- `pu_audio_devices(engine, kind, out, capacity)`：枚举通话同一个 ADM 的输入(0)/输出(1)设备；
  `out=null` 返回数量。设备名与 guid 固定缓冲（128/64）。`index` 是**当前位置**，
  Windows ADM 的枚举里没有“默认伪设备”项（0 只是第一个具体设备）；`is_default`
  按系统默认端点稳定 ID 匹配，不再假设 `index==0`。跨枚举请用 `guid` 当身份。
- `pu_audio_status(engine, pu_audio_status_info*)`：**不是只读快照**，会推进真实重新
  选择/回退：显式选中的设备被拔掉时，通话中会真正切回系统默认（`Stop -> SetDevice(-1)
  -> Init -> Start`），`fallback` 对应位报告；选择为 -1 且 Windows 默认端点变化时
  （数量/名称都不变也能发现，用 CoreAudio `eConsole` 端点 ID）会真正重新选择并重启。
  `generation` 在列表/顺序、默认端点或回退变化时递增；`reserved` 的低两位表示某方向
  因切换/回退失败而没有在运行（不是“已静音”）。要发现热插拔/默认变化必须**周期性
  轮询本函数**——本版没有主动事件通道。
- `pu_audio_set_device(engine, kind, index)`：`index=-1` 显式调用 ADM
  `WindowsDeviceType::kDefaultDevice`（绝不映射成枚举 0）。未在通话中直接生效；
  通话中执行 Stop→SetDevice→Init→Start 事务（短暂停启），**只有新设备真正启动后
  才返回 PU_OK**。失败会先恢复原设备，恢复不了回落系统默认并置 `fallback`，
  两种都失败则返回错误、该方向音频停止并置 fault——不会假成功。选择按 ADM 给出的
  稳定端点 ID 保存，设备顺序变化不会把选择指向别人。
- `pu_voice_level(engine, pu_voice_level_info*)`：本端电平。`valid=0` 表示没有有效数据
  （`GetSignalLevel` 失败且无新鲜 PCM），**不得把 0 当有效静音**；`source=2` 时
  `peak`/`rms` 都是 PCM sink 实测值；`source=1` 时只有 `peak`（GetSignalLevel），
  `rms=0` 表示**未测量**，不能当零 RMS 展示。
- v2.18 `pu_audio_monitor_start/stop/status`：显式独立输入监测，不加入语音、不建 PC、
  不外发、不启动扬声器。复用同一 ADM 与官方 observer；未启用时不采集，不自动解除
  静音。`pu_audio_monitor_info` 固定 40 字节，`state=Idle/Active/Faulted`，
  `running=-1/0/1` 表示最近读回未知/未运行/运行，停止失败不能假定已停；
  `valid=1` 时 peak/RMS 来自 PCM（包括真正全零 PCM），1 秒无新帧则失效。
  旧 DLL 缺三个导出或 native caps bit7 时托管层明确报不支持。状态查询不隐式重启设备；
  热插拔仍需轮询 `pu_audio_status`。monitor 与 VoiceJoin/Leave 共享采集所有权锁，
  入语音前必须确认 monitor 停止；销毁时先停止/Terminate、脱离端口，再释放 wrapper。

构建与自检（不运行采集）：

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File scripts/build-media-dll.ps1
```

脚本在编译链接后用 `dumpbin` 断言导出数 **43**（v2.17 的 40 + v2.18 的 3 个
独立输入监测导出）；不需要 `-Run`（那会真实采屏）。

AUD02 的无硬件预检与生产 Module/fake 端口回归：

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File scripts/build-media-dll.ps1 -RunAudioMonitorProbe -RunAudioMonitor
```

前者直接链接官方 SDK wrapper（须完成 ADM `Init()` 才注册代理 callback）；后者编译
同一份生产 monitor Module，仅用 fake 端口/合成 PCM。两者均不开真实音频设备。
本地证据为 observer 187 与 monitor 152 检查通过，**不等于真实 WASAPI 采集验收**。
显式观看/严格接收的原生回归只用 synthetic 源、不采桌面也不开麦：

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File scripts/build-media-dll.ps1 -RunWatch
```

`-RunWatch` 只运行 `media_watch_regression.exe`（watch-before-track /
track-before-watch / target+slot 过滤 / unwatch / legacy 自动观看，以及 v2.16
严格接收：独立会话 real loopback DTLS/SRTP 出帧、关闭后旧发布者继续发包但新会话
0 帧、关闭接收不改变本端发布）。`-Run` 仍会跑真实 WGC 桌面采集，验证观看链路时不要用它。

音频设备控制回归用 fake ADM + 受控默认端点，直接编译并运行生产逻辑
`src/media-core/src/audio_device_control.cpp`，**不打开真实麦克风/扬声器、不采屏**：

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File scripts/build-media-dll.ps1 -RunAudioSwitch
```

direct 严格接收隔离（B2b, v2.17）用**双引擎 4 条 PC** 只跑 synthetic：
A/B 各自 `pu_screen_start("synthetic")` 并互相观看（A 的 per-viewer 发送会话
`pu_screen_publish_open` ↔ B 的接收会话 `pu_screen_receive_open`，反向同理）：

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File scripts/build-media-dll.ps1 -RunDirectIsolation
```

它断言（任一不满足即 exit 非 0）：
- 4 条会话都真实出帧：`pu_screen_receive_stats.frames_delivered` 与
  `pu_screen_receive_rtp_stats.packets_received` / `pu_screen_publish_stats.packets_sent`
  同时增长（**不是** `frames_sent`/`has_peer`）；
- A 停看 B：先 `pu_screen_publish_close` 再 `pu_screen_receive_close`，在接收 PC
  仍活着时该会话的 RTP 包数**冻结**（区分 RTP 停发与 ICE/DTLS 保活）；
- 与此同时 A→B 继续交付新帧、A 的发布 RTP 继续增长，两端 `frames_sent`（采集）
  继续增长——证明停看没有停采集/发布/另一方向；
- 重看 B 用新 `receive_id`/新 token，旧会话的 `signal_in`/`signal_in_session` 返回
  `PU_ERR_STATE`；对称的 B 停看 A 同样成立。

它**不覆盖**：跨进程/真实网卡的 UDP 重放与延迟包、真实音频、真实桌面采集、
服务端 direct 分支的端到端（需要 Go 测试与真机联调）。

它覆盖通话中切输入/输出、-1 默认（记录为 `set:-1`）、设备重排、拔掉/回退、
默认端点变化（数量/名称不变）、Set/Stop/Init/Start 失败与回滚、恢复失败回落默认、
两种都失败置 fault、静音/闭音保持、join 前选择/失效回落。v2.15 返修后还覆盖
**安全启动门禁**：输入/输出方向为 muted/deafened 时必须是
`Stop -> Set -> Init -> SetMuted -> readback -> Start`；SetMuted 失败、readback
失败、设备重开清 mute、Start 时重置 mute、恢复旧设备、回退默认各路径都断言
**不存在任何一次未静音的 Start**（fake 记录每次成功 Start 瞬间的 mute 值，而不是
只看最终 bool）。另外覆盖 fault 后重新选择设备才真正恢复、`pu_audio_status`
在未真实恢复前不得清 fault，以及默认端点切换在 Stop 阶段失败后下一次 Refresh
仍会重试。**它验证的是控制逻辑与失败路径，不能代替真实 WASAPI 设备行为**。

已知未闭环（不要当成已实现）：relay 切回旧目标（A→B→A）时 A 的旧在途 RTP 可能短暂到达，
由原生 epoch 丢弃（不等于网络层停收）；旧 UDP/SRTP 包的重放/延迟投递未做专项验证。
direct 严格接收（v2.17）的停看已由 `-RunDirectIsolation` 验证「该会话真实 RTP 停增、
另一方向继续出帧、采集与发布不重启」，但仍未覆盖跨进程/真实硬件的 UDP 重放窗口。
音频方面：控制逻辑与失败路径已由 fake ADM 回归覆盖，但**真实音频硬件
（真实麦克风/扬声器、Windows 端点热插拔、通话中的实际听感/采集效果）仍未验证**；
静音安全门禁依赖 ADM 的 `MicrophoneMute`/`SpeakerMute` 读回，若平台不支持会安全失败
（停该方向并置 fault），仍须真机确认。热插拔/默认变化只在调用 `pu_audio_status` 时推进，
客户端必须轮询。独立输入 monitor 内核/托管接口已实现并通过 fake/SDK 合成回归；
正式 UI 入口与真实设备采集未验，全局 PTT 仍未实现。不得把新增能力位当作真机验收。

## 部署安全边界：bootstrap、TLS 与 `--trust-proxy`（2026-11-07）

bootstrap 会创建全服唯一的 owner，因此除「一次性 token + 24h TTL」外，服务端还要求**连接可信**：
来自回环地址、或直接是 TLS 连接（`r.TLS != nil`），或部署者显式声明 `--trust-proxy`。
不满足 → `403 FORBIDDEN`，日志记 `msg="bootstrap rejected" reason=... remote=... tls=false`。

**三种部署方式**（按推荐顺序）：

1. **本机引导，再开放端口**（最简单）：先在服务器本机 `partyup serve --http 127.0.0.1:8080`
   跑一次，用 stdout 的 `BOOTSTRAP_TOKEN` 在 127.0.0.1 完成 bootstrap，停掉后改成 `:8080`
   对外监听。bootstrap token 用后即焚，重启也不会再生成。
2. **反向代理终止 TLS**：外层 Caddy/Nginx 终结 `https://`，回源到 `127.0.0.1:8080`。
   此时服务端看到的是回环连接，默认即可 bootstrap；若代理与服务器**不在同一台机器**，
   必须加 `--trust-proxy`（服务端才信任 `X-Forwarded-For`，并据此判定客户端地址）。
3. **服务端自己终止 TLS**：当前二进制不内置 TLS 监听，需要前置代理或 stunnel；不要把
   明文端口直接暴露到公网。

`--trust-proxy` 同时影响日志与限速里的客户端 IP 归属：开启后取 `X-Forwarded-For` 最左值，
**只能**在代理已清洗该头且已终止 TLS 时开启，否则等于让客户端自报 IP。

客户端「先添加服务器」流程用 `GET /api/server-info`（匿名、走登录限速）：返回
`initialized` / `registration` / `voice_mode` / `screen_mode` / `bootstrap_allowed`。
`bootstrap_allowed` 只是 UI 提示，真实授权始终在 `POST /api/bootstrap` 内部。

## 邀请链接与反向代理 base path（REG-03）

`partyup invite` 的旧输出不变（stdout 仍只有一行 `INVITE_CODE`）：

```bash
cd server
go run ./cmd/partyup invite --data ./data
# INVITE_CODE=K7QM3XPD
```

要同时得到可复制链接，显式声明**对外**服务器 base URL；程序不推断、不读 Host/Forwarded：

```bash
go run ./cmd/partyup invite --data ./data --server-url https://example.org/partyup
# INVITE_CODE=K7QM3XPD
# INVITE_LINK=https://example.org/partyup/?code=K7QM3XPD
```

规则（实现 `server/internal/invite`，回归 `server/internal/invite/invite_test.go`、
`server/cmd/partyup/main_test.go`）：

- 只接受绝对 `http`/`https`、非空 host、端口 1–65535；拒 user-info、已有 query/fragment、
  控制字符与首尾空白，缺 scheme 或 `partyup://` 一律拒绝。
- base path 按目录处理并补尾斜杠；百分号编码与 IPv6 字面量交给 `net/url`。非法 URL 在
  创建邀请码**之前**失败，不会留下可用码。
- 链接里的码原样输出；导入端按与 `auth.NormalizeInviteCode` 相同的规则归一化。

客户端（`client/Services/InviteLink.cs`）用 `System.Uri` +
`Microsoft.AspNetCore.WebUtilities.QueryHelpers`（`client/PartyUp.Client.csproj` 的
PackageReference `10.0.10`）。纯 service 测试项目链接同一份生产源码，QueryHelpers 由本机
ASP.NET Core 共享框架提供（不复制逻辑、不需要 Avalonia）：

```bash
dotnet run --project tests/PartyUp.InviteLink.Tests/PartyUp.InviteLink.Tests.csproj
```

正式 `PartyUpApi` 的 BaseAddress 统一补尾斜杠，API 路径改为相对 `api/...`；
`PartyUpConnection` 的 WS 端点为 `<base>/ws`。因此：

| 部署 | HTTP | WS |
| --- | --- | --- |
| root | `http://host/api/login` | `ws://host/ws` |
| 反代前缀 `/partyup/` | `https://host/partyup/api/login` | `wss://host/partyup/ws` |

服务端本身不改根路由；前缀由标准反代的 `StripPrefix` 处理，回归在
`server/internal/server/base_path_test.go`（真实 httptest + `/partyup/ws`）。

未接：正式 UI 的粘贴/导入入口、OS 深链注册、浏览器打开、真实外部反代/NAT。链接包含
邀请码，客户端日志不要打印整条链接。

## 校验

```powershell
& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
& "C:\Program Files\CMake\bin\cmake.exe" --version
& "C:\Program Files\Go\bin\go.exe" version
dotnet --version
```

期望：BuildTools 路径、`cmake 4.4.4`、`go1.27.3`、`10.0.400`。

## 严格接收回归（B2 / `screen.watch.isolated.v1`）

两个独立入口，跑的是同一份生产状态机源码（测试项目用 `<Compile Include>` 链接
`client/Services/StrictReceiveController.cs`，不是复制）：

```bash
# 状态机 + 协议语义 fake（不依赖 Avalonia / NuGet；fake 建模服务端 lease：
# lease 身份是 receive_id，target 只是属性；撤销只接受会话 id，误传 sharer 记为 misuse）
dotnet run --project tests/PartyUp.StrictReceive.Tests/PartyUp.StrictReceive.Tests.csproj

# 真实 HTTP/WS：能力广告、跨连接 owner、按 session 精确撤租、断线/退订清理
cd server && go test ./internal/server -run 'TestIsolated' -count=1
```

`dotnet build client/PartyUp.Client.csproj --no-restore` 用来确认生产 adapter
（`ShareSession` / `PartyUpConnection`）仍能编译——只跑那 76 个 checks 不覆盖它们。

**WS 负向观察不能用会继续承载 RPC 的连接**：gorilla/websocket 的读错误是**永久的**
（`NextReader`/`ReadMessage` 一旦返回非 nil error，后续调用永远返回同一个 error）。
`expectNoScreenOffer` 之类的「窗口内不应收到」断言靠读超时实现，超时后该连接即作废；
必须用同频道、同成员的**一次性 observer socket**，观察完关闭，不能在同一 socket 上
接着 `expectError`/`request`（否则会拿到上一次的 `i/o timeout`，而不是真正的断言失败）。

**严格撤租的身份是会话，不是目标**：`IStrictReceiveTransport` 只暴露
`RevokeSessionAsync(StrictReceiveSession)`；服务端 `screen.unwatch` 的 `receive_id`
只清「发起连接持有 + session 完全匹配」的 lease。Stop 先本地关收再按会话撤租，
迟到 ACK 之后还会对同一会话再撤一次（服务端对未知会话幂等）；本地 exactly-once
close 与远端幂等撤租是两件事，不能绑在同一个 `stillOwned` 上。`sharer_id` 继续
保留 legacy 目标语义，旧客户端行为不变。

### 真实 UDP/SRTP 暂扣与旧密文重放（B2 relay 传输层）

```bash
cd server && go test ./internal/sfu -run 'TestVideoReceive' -count=1
```

`video_receive_replay_test.go` 不再只断言「RTP 计数不再增长」或调旧槽位回调，而是
让**生产** `VideoRoom.OpenReceive` / `CloseReceive` / `HandleReceiveSignal` 和
`onTrack → writeToReceivesLocked` 的 fanout 真的把 SRTP 包写出去：

- 房间内所有 PeerConnection（发布者 PC + 每条接收 PC）经 `SettingEngine.SetICEUDPMux`
  共用**同一条**受控 UDP socket（`net.PacketConn` 装饰器，`captureConn`）。因此重放包
  与合法媒体同源（同 socket/端口/IP），不是「旧端口已经没人监听」的空投递。
- 装饰器在 `WriteTo`（SRTP 加密之后、离开本进程之前）按 `pion/rtp` 解析头识别媒体，
  `hold` 期间**交付前暂扣**（返回成功但不写 socket），STUN/DTLS/RTCP 直通；记录有界
  （≤8 条，超出即放行并计 `overflow`），不堆包历史。
- 释放/重放走 `conn.WriteTo` 原字节：`TestVideoReceiveWithheldDatagramDeliversOnRelease`
  是 positive control（会话仍有效时释放 → 客户端解密后收到已知 seq/payload，证明装置
  真的会投递媒体、且不是被 anti-replay 挡掉）；`TestVideoReceiveReplayOldCiphertextToNewSessions`
  覆盖 A(g1)→B(g2)→A(g3)：g1 密文投给 g3 零旧 payload，g3 当期新媒体继续通过；
  `TestVideoReceiveStrictRebuildAndLegacyRefreshBoundary` 覆盖严格重建
  （`CloseReceive` + 新 `receive_id`）与 legacy `Refresh` 的边界（Refresh 只重建混合
  PC，不替换/关闭严格接收会话），以及停看后不再交付停看后发出的媒体。
- 断言全部基于客户端**真正解开 SRTP 之后**的 RTP（seq/payload），不是给 sink 贴 epoch
  标签；每个测试的可识别 payload 带唯一 seq，避免读到上一轮残留。
- 唯一的生产侧改动是一个窄 seam：`NewVideo` 抽出 `newVideoAPI()` 并走
  `newVideoWithAPI(...)`，让测试能注入带 `ICEUDPMux` 的 `webrtc.API`；生产入口与房间
  行为不变（无第二条会话实现）。

**仍未覆盖（不得写成已验证）**：native libwebrtc（Windows）侧的旧 UDP 重放、`direct`
（无 SFU）模式的重放、真实硬件/双机 E2E、真实语音帧。WS 信令路径由
`go test ./internal/server -run 'TestIsolated'` 单独覆盖，两者不能互相替代。

## direct 严格接收（B2b / `screen.publish.sessions.v1`，v2.17）

`--screen-mode=direct` 下严格观看没有 SFU：服务端只做会话仲裁+信令转发，共享者
为单个 viewer 建独立发送传输。契约见 `docs/PROTOCOL.md` §5.1.2；实现分层：

- 服务端 `server/internal/server/screen_direct.go`：`directScreenSession`
  （session=receive_id、nonce、generation、viewer 连接、sharer 发布连接）+
  `routeDirectScreenSignal`（方向/owner/nonce/generation 校验）+ 关会话时给对端推
  `viewer_close` / `receive_close`。
- 共享者客户端 `client/Services/DirectPublishController.cs`：`viewer_arm` →
  `pu_screen_publish_open`（新 PC，复用采集源/视频轨），`viewer_close` → 只关该
  会话；native `SIGNAL_OUT.reserved`（token）→ 只发给对应 viewer。
- 观看者客户端 `StrictReceiveController`：`receive_close` 用 `CloseRemote` 只关本地
  接收传输（**不撤租**，服务端已清理）；direct 视图的 answer/candidate 按会话回给
  共享者（不再硬编码 `target:"sfu"`）。

```bash
# 共享者侧状态机（纯逻辑，无 native/桌面/网络）：arm 幂等、只关该会话、
# 失败不登记、旧 token 不复活、receive_close 不撤租
dotnet run --project tests/PartyUp.StrictReceive.Tests/PartyUp.StrictReceive.Tests.csproj

# 服务端 direct 会话仲裁（WS）：arm/路由/方向/nonce/跨成员/停看/重看新身份
cd server && go test ./internal/server -run 'TestDirect|TestIsolated' -count=1
```

**native token 必须在引擎内全局唯一**：接收会话与发送会话共用同一个计数器
（`session_token_seq_`）。两套计数器会出现「接收 1 / 发送 1」同引擎撞车，客户端
无法按 `SIGNAL_OUT.reserved` 判断该把信令灌进哪条会话（B2b 首个 spike 就是这样
红的）。spike 的路由也按 (引擎, token) 建键，不能只按 token。

**`pu_screen_pause_send` 不是 direct 严格隔离**：它只换同一条 PC 上的发送轨，
网络层 RTP 继续；停看必须关闭对应传输（`pu_screen_publish_close` /
`pu_screen_receive_close`），发布/采集/语音不受影响。


（本仓库的 DSH 写入上下文若被 workspace-write 策略限制到 `~/.cache/go-build` 只读，
`go test` 会在改码后因写缓存失败；不要改 GOCACHE 绕过，交由 Lead 用原命令执行。）

## 调试桥（wdb，首选）

Windows 侧跑 GUI 的正确姿势走调试桥，不用手工 rsync。它来自 `galleries-manager` 项目，整份拷贝在 [`tools/windows-debug-bridge`](../tools/windows-debug-bridge)。

工作方式：WSL 里的 CLI 把仓库源码冻结成快照写进 `.wdb/queue/jobs/<id>/payload/`，Windows 上的前台 Worker 取走，复制到 `%LOCALAPPDATA%\WslDebugBridge\runs\<id>\src`，**在本地磁盘构建并运行**，日志流式回写队列。整个过程不开端口，只走文件。

这解决了两个老问题：产物落在本机磁盘，原生库不会像 UNC 那样被 PATH 上的旧版本抢走；窗口跑在真实桌面上，同时 Job Object 保证 `stop` 能杀干净整棵进程树。

客户端 stdout 日志固定是 **UTF-8**（`AppLog` 静态构造里设置 `Console.OutputEncoding`）：Windows 默认控制台代码页是 cp936，若把 stdout 重定向到文件后用 WSL 按 UTF-8 读，中文会变乱码——所以日志编码按 UTF-8 约定，读日志不需要 `iconv`。若遇到历史遗留的 cp936 日志，用 `iconv -f cp936 -t utf-8` 转。

**启动 Worker（在 Windows 的 PowerShell 7 里执行，保持窗口开着）**

```powershell
Set-Location C:\
pwsh -NoProfile -ExecutionPolicy Bypass -File `
  '\\wsl.localhost\<distro>\<repo>\tools\windows-debug-bridge\start-worker.ps1' `
  -Queue '\\wsl.localhost\<distro>\<repo>\.wdb\queue'
```

**在 WSL 里使用**

```bash
scripts/wdb.sh doctor                      # 队列可写 + Worker 心跳
scripts/wdb.sh build-run --project spikes/frame-presentation/Spike.Frame.csproj --follow
scripts/wdb.sh logs <id> --follow           # 另开一个窗口跟日志
scripts/wdb.sh stop <id> --reason "调试结束"
```

`scripts/wdb.sh` 首次调用会自动编译 `wdb`。任务 ID 从 `build-run` 的 `--json` 输出里取。

**注意**：必须在 PartyUp 目录内构建 `wdb`——仓库根是靠向上找 `.git` 判定的，用别的仓库编出来的 dll 会快照错项目。

## 备用：不经桥的构建与运行

桥不可用时（比如 CI 或临时排查），可以直接走 UNC 编译 + 本地运行，但**运行目录必须在本机磁盘**，否则会遇到上面那个原生库抢占问题。

```bash
# 编译：UNC 可行
pwsh.exe -NoProfile -Command "Set-Location C:\; dotnet build '\\wsl.localhost\<distro>\<repo>\spikes\frame-presentation\Spike.Frame.csproj' -c Debug"

# 运行：先把产物拷到本地，或把 Git 的 mingw64\bin 从 PATH 摘掉
pwsh.exe -NoProfile -Command "\$env:PATH = ((\$env:PATH -split ';') | ? { \$_ -and (\$_ -notlike '*Git*') }) -join ';'; & dotnet '<...>\Spike.Frame.dll'"
```

## 已知坑

1. **UDP 不通**：WSL2 的 localhost 转发只覆盖 TCP；WebRTC 媒体走 UDP。本地联调时服务端要么跑在 Windows，要么开 `networkingMode=mirrored`，要么放另一台机器。
2. **UNC 工作目录**：`cmd.exe` 在 `\\wsl.localhost\...` 下直接失败；`pwsh` 能接受，但外部程序会继承 UNC cwd，仍应显式 `Set-Location`。
3. **WSL 无 GPU**：`/dev/dxg` 缺失，Linux 构建是软件渲染，不能用来测帧率。
4. **`obj/` 与 `bin/` 不进 rsync**：跨 9P 读写会让增量编译退化；同步时排除。
5. **沙箱写不出工作区**：DSH 的 workspace-write 策略下，`go env -w`（写 `~/.config/go/env`）会被拒；这类全局配置要么你手动执行，要么由仓库脚本在构建时注入环境变量。
