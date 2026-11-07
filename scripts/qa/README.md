# 验收 / 探针脚本（scripts/qa）

这里是 PartyUp 的**真机验收与一次性探针**脚本集合。它们不进产品构建流程，只用于在 Windows 上
驱动真实客户端（Avalonia GUI + 真实媒体 DLL）与隔离服务端，产出可复核的日志/截图证据。

脚本原样保留各自的验收语义；**默认路径已改为可移植写法**，不再包含任何机器路径或内网 IP。
重跑历史验收时需要按下面的「通用约定」覆盖参数。

## 通用约定

- **PowerShell 7（`pwsh`）**：部分脚本用了 `Add-Type` here-string 与 `?.`/三元等 PS7 语法，
  Windows PowerShell 5.1 会解析失败。
- **副屏**：任何会拉起 GUI 的脚本都传 `--screen 2`（`Screens.All` 1 起算；本仓库的 QA 脚本也统一
  再用 `SetWindowPos(SWP_NOACTIVATE)` 兜底到副屏），**不要**让测试窗口出现在用户主屏、也不要抢焦点。
- **隔离服务端**：先起一个隔离实例（独立数据目录 + 独立端口，通常 18084–18098），再跑客户端脚本：
  最省事的是 `manual-qa-launch.ps1`（一个服务端 + 两个已登录客户端）或 `qa-server-setup.ps1`
  （P2 语音/共享用的隔离服务端）。**不要**连 8080 / 18081 / 18082 上的用户实例。
- **配置隔离**：客户端用 `--appdata <独立配置根>`；**单实例互斥键由 appdata 根派生**，所以不同
  `--appdata` 的客户端可以同时运行，同根只能开一个。
- **默认路径**（全部可用参数覆盖，不再硬编码：
  - QA 根：`$env:LOCALAPPDATA\PartyUpQA`
  - 被测客户端：`$env:LOCALAPPDATA\PartyUpQA\client\PartyUp.Client.exe`（用 `-ClientExe` 指向你的构建产物）
  - QA 服务端：`$env:LOCALAPPDATA\PartyUpQA\server\partyup-qa.exe`（用 `-ServerExe` 覆盖）
  - 证据目录：`$env:LOCALAPPDATA\PartyUpQA\evidence`（`-EvidenceDir` / `-OutDir` 覆盖）
- **共享 helper 留在 `scripts/` 根**：`capture-window.ps1`（按窗口矩形截图，不抢焦点）、
  `uia-dump.ps1`、`uia-click.ps1`、`uia-create-channel.ps1` 是**已跟踪**脚本，本目录脚本用
  `$PSScriptRoot\..\<helper>.ps1` 引用。改目录结构时注意这条相对路径。
- **证据**：脚本只写各自 `-OutDir`/`-EvidenceDir`；过程性证据归档到 `.docs/`（该目录不入库）。

## 索引

| 脚本 | 验什么 | 前置条件 | 典型命令 |
|---|---|---|---|
| `admin-role-gating-probe.ps1` | owner / admin / 普通成员三种角色在「服务器管理」入口与页内的差异（UIA + 真实 HTTP 403/200） | 隔离服务端 18093；构建产物 | `pwsh -File scripts\qa\admin-role-gating-probe.ps1 -ClientExe <构建>\PartyUp.Client.exe` |
| `admin-ui-probe.ps1` | 「服务器管理」页真机验收：成员列表/角色标注/概览/危险设置/邀请码/封禁/审计日志 | 隔离服务端 18091 | `pwsh -File scripts\qa\admin-ui-probe.ps1 -ClientExe <构建>\PartyUp.Client.exe` |
| `audio-probe.ps1` | B1 音频验收：真实设备枚举 + 麦克风电平测试（UIA） | 隔离服务端 18085 | `pwsh -File scripts\qa\audio-probe.ps1 -ClientExe <构建>\PartyUp.Client.exe` |
| `audio-probe2.ps1` | 追问：「正在采集但收不到电平」是否因为选择停在「跟随系统默认」 | 同上 | `pwsh -File scripts\qa\audio-probe2.ps1 -ClientExe <构建>\PartyUp.Client.exe` |
| `auth-qa3-accept.ps1` | 认证链路真机验收编排（logout / TTL / 单端登录 / 限速），调用 server+clients 两个子脚本 | 端口 18084、`%LOCALAPPDATA%\PartyUpQA3` | `pwsh -File scripts\qa\auth-qa3-accept.ps1` |
| `auth-qa3-clients.ps1` | 认证验收的客户端侧（复制/启动两个客户端并断言） | 由 `auth-qa3-accept.ps1` 调用 | 一般直接跑 accept 脚本 |
| `auth-qa3-server.ps1` | 认证验收的隔离服务端启动器（18084 / QA3 数据） | Go 构建的 `partyup-qa.exe` | `pwsh -File scripts\qa\auth-qa3-server.ps1` |
| `close-to-tray-accept.ps1` | 关闭窗口行为：询问框/最小化到托盘/记住选择/直接退出/托盘退出 6 条判据 | 构建产物；pwsh 7 | `pwsh -File scripts\qa\close-to-tray-accept.ps1 -ClientExe <构建>\PartyUp.Client.exe` |
| `i18n-probe.ps1` | i18n 基础设施真机验收（语言切换后文案/资源） | 隔离服务端 18085 | `pwsh -File scripts\qa\i18n-probe.ps1 -ClientExe <构建>\PartyUp.Client.exe` |
| `ice-qa3-accept.ps1` | ICE 固定端口 + 对外 IP 映射（`--ice-port-range` / `--ice-public-ip`） | 隔离服务端 18084；QA3-ice 目录 | `pwsh -File scripts\qa\ice-qa3-accept.ps1` |
| `kick-ban-e2e-probe.ps1` | 踢出 / 封禁 / 解封三态：客户端会话终止、提示文案、重连行为、解封恢复 | 隔离服务端 18088（QA7 数据） | `pwsh -File scripts\qa\kick-ban-e2e-probe.ps1 -ClientExe <构建>\PartyUp.Client.exe` |
| `login-first-screen-probe.ps1` | 第一屏（服务器列表）肉眼验收：副屏窗口 + 按窗口矩形截图 | 已起隔离服务端或空环境 | `pwsh -File scripts\qa\login-first-screen-probe.ps1 -ClientExe <构建>\PartyUp.Client.exe` |
| `login-session-e2e-probe.ps1` | 会话终止（单端登录）：token 被替换 → `session.replaced` → 不自动重连 | 隔离服务端 18087（QA6 数据） | `pwsh -File scripts\qa\login-session-e2e-probe.ps1 -ClientExe <构建>\PartyUp.Client.exe` |
| `login-ui-accept.ps1` | 登录第一屏 7 组 UIA 回归（服务器簿/邀请码/记住我/退出登录） | 两个隔离服务端 18089/18090（QA8 数据） | `pwsh -File scripts\qa\login-ui-accept.ps1 -ClientExe <构建>\PartyUp.Client.exe` |
| `manual-qa-launch.ps1` | 手动测试台：一个隔离服务端 + 两个已登录客户端（alice/bob，各自 `--appdata`） | 构建产物 | `pwsh -File scripts\qa\manual-qa-launch.ps1 -ClientExe <构建>\PartyUp.Client.exe -ServerExe <构建>\partyup-qa.exe` |
| `p2-voice-screen-accept.ps1` | P2 真机验收：真实麦克风入会 + 屏幕共享发布/观看/停止 + 共享中转移 | 隔离服务端；`-ClientDir`/`-Server`/`-EvidenceDir` 必填 | `pwsh -File scripts\qa\p2-voice-screen-accept.ps1 -ClientDir <构建目录> -Server <exe> -EvidenceDir <dir>` |
| `qa-clients-launch.ps1` | 把两个真实账号的客户端留在桌面供人工测试（不自动入会） | 已起隔离服务端 | `pwsh -File scripts\qa\qa-clients-launch.ps1 -ClientExe <构建>\PartyUp.Client.exe` |
| `qa-restore-media-dll.ps1` | A/B 换 DLL 后把媒体 DLL 还原到客户端构建目录并校验哈希（先杀本目录客户端） | `-SourceDll` 必填 | `pwsh -File scripts\qa\qa-restore-media-dll.ps1 -SourceDll <备份.dll> -ExpectShaPrefix <哈希前缀>` |
| `qa-server-setup.ps1` | P2（语音/屏幕共享）验收用的 Windows 隔离服务端：建数据目录、起服务、bootstrap 账号 | Go 构建的 `partyup-qa.exe` | `pwsh -File scripts\qa\qa-server-setup.ps1` |
| `qa5-audio-devices.ps1` | 真实音频端点枚举（IMMDeviceEnumerator COM）+ 默认设备状态 | Windows 有音频设备 | `pwsh -File scripts\qa\qa5-audio-devices.ps1` |
| `qa5-audio-pnp.ps1` | PnP 层音频设备/端点状态快照（`Get-PnpDevice`） | Windows | `pwsh -File scripts\qa\qa5-audio-pnp.ps1` |
| `qa5-audio-registry.ps1` | MMDevices 注册表里的音频端点状态/友好名快照 | Windows | `pwsh -File scripts\qa\qa5-audio-registry.ps1` |
| `qa5-bootstrap-test.ps1` | 冒烟：能在 QA 根下建目录/写文件、打印 PS 版本 | 无 | `pwsh -File scripts\qa\qa5-bootstrap-test.ps1` |
| `qa5-robo-test.ps1` | robocopy 镜像客户端目录到 QA 根并打印返回码/条目数 | 源目录存在 | `pwsh -File scripts\qa\qa5-robo-test.ps1` |
| `qa5-setup.ps1` | 准备两个独立客户端目录（`client-a`/`client-b`）并校验媒体 DLL 哈希 | 源客户端目录 + DLL 存在 | `pwsh -File scripts\qa\qa5-setup.ps1` |
| `role-qa3-accept.ps1` | 角色与权限真机验收（服务端身份规格 §10），复用 auth 的 server/clients 子脚本 | 端口 18084 | `pwsh -File scripts\qa\role-qa3-accept.ps1` |
| `shell-accept.ps1` | v1.1 外壳验收：真实登录 → 频道 → 历史消息 → 名册 → 语音/共享入口 | `-ClientDir`/`-Server`/`-EvidenceDir` 必填 | `pwsh -File scripts\qa\shell-accept.ps1 -ClientDir <构建目录> -Server <exe> -EvidenceDir <dir>` |
| `shell-register-accept.ps1` | P1b 分步注册真实走查（REG-01/02/03/04 客户端接线） | 同上 | `pwsh -File scripts\qa\shell-register-accept.ps1 -ClientDir <构建目录> -Server <exe> -EvidenceDir <dir>` |
| `startup-ui-probe.ps1` | 设置页勾选框 → 真实注册表 / 真实 close-to-tray 行为 | 构建产物 | `pwsh -File scripts\qa\startup-ui-probe.ps1 -ClientExe <构建>\PartyUp.Client.exe` |
| `swap-qa-server.ps1` | 把 QA 服务端换成修复后的二进制并重新拉起客户端 | 旧实例在跑 | `pwsh -File scripts\qa\swap-qa-server.ps1 -ServerExe <新exe>` |
| `ui-watch-probe.ps1` | 定位「观看收不到帧」：发布侧（WGC 静态桌面不出帧）还是转发/接收侧 | 隔离服务端 | `pwsh -File scripts\qa\ui-watch-probe.ps1 -ClientExe <构建>\PartyUp.Client.exe` |
| `voice-rejoin-adm-control.ps1` | ADM 生命周期对照：语音会话前后用设置页麦克风测试验证共享 ADM 能否再起采集（看 `ADM StartRecording` rc 与是否触发重建） | 构建产物（含 `pu_audio_monitor_*` 的 DLL） | `pwsh -File scripts\qa\voice-rejoin-adm-control.ps1 -ClientExe <构建>\PartyUp.Client.exe` |
| `voice-rejoin-capture.ps1` | 加入→离开→再入会 ×3 的**采集侧**计数回归（`voice: capture frames=` 增量、`adm(instance=)` 是否复用），带 `-MediaDll` 可做新旧 DLL A/B | 构建产物（v2.19 DLL + 客户端 capture 日志） | `pwsh -File scripts\qa\voice-rejoin-capture.ps1 -ClientExe <构建>\PartyUp.Client.exe -Label newdll` |
| `voice-rejoin-cleanup.ps1` | 语音 rejoin 测试收尾：停本根进程、检查端口/`settings.json`、清 `%APPDATA%\PartyUp` | 无 | `pwsh -File scripts\qa\voice-rejoin-cleanup.ps1` |
| `voice-rejoin-e2e.ps1` | 双 GUI relay E2E：A 入会/退会/再入会，采样两侧 pcmRx 与原生 track 统计（收侧为辅助判据） | 构建产物两份客户端目录 | `pwsh -File scripts\qa\voice-rejoin-e2e.ps1` |
| `voice-rejoin-native.ps1` | 原生 ABI 探针：直接 LoadLibrary 产品 DLL，join→leave→join 采样 `pu_voice_level` | 产品 DLL 与头文件布局匹配 | `pwsh -File scripts\qa\voice-rejoin-native.ps1 -DllDir <含 partyup_media.dll 的目录>` |
| `voice-rejoin-ui-probe.ps1` | UI 探针：客户端窗口挪副屏 + 探测设置/麦克风测试的 UIA 路径（写元素树到文件） | 构建产物 | `pwsh -File scripts\qa\voice-rejoin-ui-probe.ps1 -ClientExe <构建>\PartyUp.Client.exe` |
| `window-placement-probe.ps1` | 窗口放置验收（只观测）：主副屏坐标、是否抢焦点 | 构建产物 | `pwsh -File scripts\qa\window-placement-probe.ps1 -ClientExe <构建>\PartyUp.Client.exe` |

## 注意

- GUI/服务端脚本属于**重型验收**：先确认没在用户主屏上弹窗、没占用 8080/18081/18082。
- 这些脚本是历史的验收现场遗存，判据写在各自的 `# 判据` 注释里；不要为了让脚本「跑绿」而放宽断言。
- 新增脚本请沿用本目录约定（`$PSScriptRoot` 相对路径、`$env:LOCALAPPDATA` 默认值、`--screen 2`、
  `--appdata` 隔离），并在上表补一行。
