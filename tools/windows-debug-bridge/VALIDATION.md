# 验收与验证说明 (VALIDATION.md)

本文档明确区分 **Linux 环境通过项** 与 **Windows 实测 / 未测项**。在当前开发与运行环境中，严格禁止将未在真实 Windows 宿主上拉起的项标记为“已通过”。

---

## 1. 当前环境客观条件与限制

1. **宿主交互与执行边界**：
   - WSL 侧可以直接执行只读诊断命令（如只读 PowerShell `Get-CimInstance` 探测 Windows 进程树与内核状态）。
   - 调试桥的核心主任务（编译、目标应用启动、Job Object 级联生命周期）严格遵循安全约定，**必须且只能通过 Windows 前台 Worker 调度执行**，CLI 严禁向 Windows 宿主盘直接写入。
2. **测试与启动依据**：
   - 必须由用户在 Windows 物理机的前台 PowerShell 终端中主动执行 [start-worker.ps1](start-worker.ps1) 或编译好的 `worker` 命令。
   - 调试桥的真实跨系统端到端执行结果依赖该前台 Worker 的响应。当前用户前台 Worker 保持健康存活（PID 17732，workerId `82a3c1b632c27239`）。
3. **安全边界（非进程沙箱）**：
   - 调试桥提供的是源码副本与构建产物隔离，被启动的目标应用仍以 Windows 宿主登录用户权限运行。实机冒烟验收优先使用 [tests/fixtures/EchoApp/EchoApp.csproj](tests/fixtures/EchoApp/EchoApp.csproj)；主桌面客户端验收必须在安全隔离副本中运行，严格禁止在未授权的正常用户数据根下启动。

---

## 2. 验证矩阵划分

| 模块 / 环节 | 验证项 | 验证状态 | 说明 |
|---|---|---|---|
| **桥自举脚本** ([start-worker.ps1](start-worker.ps1)) | UTF-8 BOM 与括号深度平衡 | **Linux 已通过** | 静态扫描通过（不代表 PowerShell 语法验证） |
| | 仓库外自举编译（纯 BCL、无 Directory.Build.props 继承） | **Linux 已通过** | 模拟独立路径编译成功，产物正常运行 |
| | PowerShell 5.1 参数集兼容性与前台自举执行 | **Windows 实测已通过** | 修复 `Split-Path` 参数集兼容性后，用户已在 Windows 前台实机成功拉起 Worker 并稳定输出心跳 |
| | Windows 宿主环境专用目录所有权与源码同步 | **Windows 实测已通过** | 成功在 Windows 宿主 `%LOCALAPPDATA%\WslDebugBridge` 创建目录、同步源码并完成干净编译 |
| **CLI (Linux 侧)** | `doctor` 命令解析与环境检测 | **Linux 已通过** | [Bridge.csproj](Bridge.csproj) 编译通过，CLI 单元测试覆盖 |
| | `build` / `build-run` / `run` 队列写入与源码冻结（Payload） | **Linux 已通过** | 自动化测试套件通过（含源快照生成与原子 request.json 写入） |
| | `status` / `logs` / `wait` 状态与日志读取 | **Linux 已通过** | 包含流式日志增量拉取与终态快速退出验证 |
| | `stop` / `shutdown` 取消标记与停机文件生成 | **Linux 已通过** | 标记文件原子落盘逻辑已验证 |
| **Worker (核心逻辑与调度)** | 运行根 RunsRoot 所有权标记保护 | **Linux 已通过** | `.wsl-debug-bridge-runs-owner` 校验（新根自动采纳、非空无标记 exit 6、已有同 jobDir 拒绝） |
| | 排队任务取消响应 | **Linux 已通过** | 250ms 周期内响应 cancel 进入终态 |
| | CRT 命令行参数转义与边界匹配 | **Linux 已通过** | 4000 次固定种子 fuzz 与 4000 次运行时差分校验通过 |
| | 异常分支资源释放 | **Linux 已通过** | 挂起进程终止、原生句柄与属性列表安全释放 |
| **Worker (Windows 实机控制面)** | 读取 `request.json` 并认领任务 | **Windows 实测已通过** | 真实 Worker（pid 17732、host <dev-host>、UNC 队列根）轮询认领并通过 |
| | 本地解包至 `%LOCALAPPDATA%\WslDebugBridge\runs` | **Windows 实测已通过** | 实测解包运行目录为 Windows 本地盘符路径（非 UNC）并通过 |
| | Windows 端 `dotnet build` / `dotnet run` 执行 | **Windows 实测已通过** | 真实 Windows .NET 10 执行 EchoApp，14 项 argv 逐字节无损 |
| | 异常构建阻断（坏源码 build 失败不拉起目标进程） | **Windows 实测已通过** | 真实捕获 CS1002 编译错误，exit 1，目标进程严格未启动 |
| | 收到 `cancel` 标记后通过 Job Object 终止 Windows 目标进程树 | **Windows 实测已通过** | 三层树结构（主进程+子+孙）取消后，经 ProcessProbe 证实 3/3 进程彻底消失 |
| | 硬超时自动终止进程树 (`--timeout`) | **Windows 实测已通过** | `--timeout 20` 到达后自动杀灭三层进程树，状态置 `timeout`，CLI `wait` exit 4，ProcessProbe 证实 3/3 进程彻底消失 |
| | 多 TFM 框架匹配与校验 | **Windows 实测已通过** | 多 TFM 项目不传 `--framework` 准确报 `ERR_FRAMEWORK_REQUIRED` 拒绝；传 `net10.0` 与 `net10.0-windows` 分别构建与运行成功 |
| | 日志大文件切片与流式跟随真增量 | **Windows 实测已通过** | 1MiB+ 大日志中段 `--offset` 切片逐字节一致；`logs --follow` 证实 24 行日志在 `running` 期间真实流式到达，非终态补读 |
| | 非 UTF-8 字节替换保真 | **Windows 实测已通过** | 遇到非法字节按标准替换为 `U+FFFD`，不崩溃、不丢弃正常文本 |
| **Worker (Windows 实机生命周期与显示面)** | 专用副 Worker 生命周期与嵌套 Job 级联 | **Windows 实测已通过** | [tests/fixtures/WorkerLifecycleProbe/WorkerLifecycleProbe.csproj](tests/fixtures/WorkerLifecycleProbe/WorkerLifecycleProbe.csproj) 实测 35/35 项全过（权威分组：preflight 1 + UNC 16 + local 16 + safety 2，主 Worker startUtc 全程一致），确证仅强杀副 Worker 进程对象即可由内层 `KILL_ON_JOB_CLOSE` 清理整棵三层树，主 Worker 与探针存活；同 queue 重启置 `interrupted` 且不重放 |
| | 桌面 UI 窗口在 Windows 物理桌面渲染显示 (Win32 治具) | **Windows 实测已通过** | [tests/fixtures/GuiApp/GuiApp.csproj](tests/fixtures/GuiApp/GuiApp.csproj) 实测 49/49 项全过，成功绘制 420x260 窗口（最终轮 HWND 0xE20C16、PID 29308），原生 Win32 API 证明窗口可见并保存自身客户区 PrintWindow PNG 截图（未做人工肉眼核验） |
| | 真实 Avalonia 画廊主程序隔离构建 | **Windows 实测已通过** | 隔离副本内执行 `wdb build`（job 7d91cb，112.2s，exit 0），完整构建通过 |
| | 真实 Avalonia 画廊主程序隔离运行与 UI 渲染 | **Windows 实测已通过** | 隔离副本内执行 `windows-gallery-gui.py --run --confirm-gui-run` 实测 22/22 项硬判据全过（最新 job `20261012T050340354Z-d0f757`，46.2s，exit 0），成功拉起可见窗口（HWND 0xB0A90、PID 23788；整窗 1920x997、客户区 1898x985），Avalonia RenderTargetBitmap PNG 视觉树解码 127 色 verified（非空白且绘制有效）；Win32 PrintWindow BMP 因 Skia/ANGLE GPU 合成呈现全黑如实记录；前后真实用户数据目录元数据（大小与 mtime）逐项一致且 <=8MiB 文件哈希严格逐字节一致（prod 416 文件 2 个超限仅比对元数据，dev 486 文件 8 个超限仅比对元数据），结合 AppPaths.Root 静态钉死确证未触碰真实数据 |
| | 收到 `shutdown` 标记后取消当前任务、杀灭进程树并退出 | **Windows 实测已通过** | 副 Worker 接收 shutdown 标记后验证任务置 `interrupted`、整树清理且副 Worker 退出码 0 |

---

## 3. 验收标准与测试流程

### 阶段一：Linux 本地与测试分支证据 (WSL 内部已通过)
1. `dotnet build tools/windows-debug-bridge/Bridge.csproj -c Release -p:TreatWarningsAsErrors=true` 编译 **0 warning / 0 error**。
2. 同仓官方持久套件 `tests/run-all.sh` 结果：**56 PASS / 0 FAIL / 1 SKIP**（末次实跑 `run-all-20261012-130549.log` exit 0）：
   - `HarnessTests`: 26 passed
   - `SmokeInfraTests`: 4 passed
   - `SmokeCliTests`: 12 passed
   - `SmokeE2ETests`: 14 passed, 1 skipped（注：该 skip 用例为多 TFM 项目在当前测试沙箱中因 `~/.nuget/packages` 缓存只读，第二 TFM 所需的 `Microsoft.WindowsDesktop.App.Ref` 引用包无法写入而跳过，属于环境边界，不得宣称全绿无 skip；若需补测请在可还原的 Linux/WSL 环境进行，切勿在 Windows 下直接运行包含 Linux 原生特性的 `smoke.py`）。
3. 辅助开发证据：
   - 临时 Worker 夹具矩阵 **54/54** 通过；
   - 独立安全检查矩阵 **27/27** 通过；
   - 独立自举测试（无 props 继承干净目录编译）通过；
   - Native 命令行组装（4000 次 CRT fuzz 与 4000 次运行时差分）全通过。
   - `python3 tools/windows-debug-bridge/tests/windows-smoke.py --static` 实测 **6 passed**。

### 阶段二：Windows 实测验收 (真实前台 Worker 实机已验证)
1. **用户前台启动 Worker（实测记录）**：
   用户在 Windows 物理机终端成功拉起前台 Worker：
   - Host: `<dev-host>` (PID 17732，workerId `82a3c1b632c27239`)
   - QueueRoot: `\\wsl.localhost\ai-dev\...\.wdb\queue`
   - RunsRoot: `C:\Users\<user>\AppData\Local\WslDebugBridge\runs`
2. **WSL 端发起自动化验收（全量实测记录）**：
   - **基础控制面冒烟**：`python3 tools/windows-debug-bridge/tests/windows-smoke.py` 实测 **12/12 PASSED**（33.2s，包含 6 个离线与 6 个真实 Windows 宿主用例，证据 `.wdb/windows-validation.json`）；
   - **Day2 边界用例整组**：`python3 tools/windows-debug-bridge/tests/windows-smoke.py --day2` 实测 **8/8 PASSED**（73.9s，包含硬超时三层杀灭、多 TFM 错误阻断与正确选择、1MiB+ 大日志切片、`logs --follow` 真实流式到达、非 UTF-8 替换，证据 `.wdb/windows-day2-boundaries.json`）；
   - **Win32 桌面治具绘制**：`python3 tools/windows-debug-bridge/tests/windows-gui.py` 实测 **49/49 PASSED**（最终轮 HWND 0xE20C16、PID 29308，成功绘制 420x260 窗口并保存 PrintWindow 客户区截图，证据 `.wdb/windows-gui-validation.json`）；
   - **副 Worker 生命周期与嵌套 Job**：`python3 tools/windows-debug-bridge/tests/windows-lifecycle.py` 实测 **35/35 PASSED**（25.5s，job `20261012T044653823Z-f2bed9`，UNC/本地盘双队列各 16 项 + preflight/safety，确证内层 `KILL_ON_JOB_CLOSE` 级联杀灭，主 Worker 未被触碰，证据 `.wdb/windows-lifecycle-validation.json`）；
   - **Avalonia 画廊主程序隔离运行**：`python3 tools/windows-debug-bridge/tests/windows-gallery-gui.py --run --confirm-gui-run` 实测 **22/22 硬判据全过**（job `20261012T050340354Z-d0f757`，46.2s，exit 0，HWND 0xB0A90、PID 23788，Avalonia RenderTargetBitmap PNG 视觉树解码 127 色 verified，真实数据目录元数据一致且限额内哈希一致，证据 `.wdb/windows-gallery-gui-validation.json` 与 `.wdb/windows-gallery-gui-artifacts/20261012T050340354Z-d0f757.png`）。
3. **明确保留的已知边界（Limitations & Non-goals）**：
   - **主前台 Worker 强杀未测**：本次生命周期验收通过专用副 Worker 证明了 `KILL_ON_JOB_CLOSE` 级联与嵌套特性，未对用户当前前台主 Worker（PID 17732）发起强杀；
   - **Core livePID 观测限制**：`Status.Pid` 仅在阶段转换时写盘，长驻应用运行期间 `status.pid` 不保证实时反映目标进程，禁止用于调试附加（attach）或按 PID 终止；
   - **Avalonia Win32 PrintWindow 限制**：Avalonia 的 Skia/ANGLE GPU 合成窗口在 Win32 `PrintWindow` 下呈现全黑属于 Windows 已知特例，非空白与真实绘制证据以 PNG 视觉树为准，不主张桌面全屏捕获；
   - **真实数据目录大文件哈希限制**：真实数据目录清单内容哈希上限为 8MiB，超过 8MiB 的大文件（prod 2 个、dev 8 个）仅按大小和修改时间做元数据一致性比对；
   - **未测项**：控制台代码页切换（非 UTF-8 编码环境）、大于 2 个 TFM 的复杂工程、网络 SMB 共享盘作为队列根等场景。
