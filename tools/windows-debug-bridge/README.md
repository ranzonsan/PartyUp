# Windows 调试桥 (tools/windows-debug-bridge)

> ⚠️ **重要声明**：旧文档 [`docs/windows-debug-bridge-spec.md`](../../docs/windows-debug-bridge-spec.md)、旧使用说明 [`docs/windows-debug-bridge-usage.md`](../../docs/windows-debug-bridge-usage.md) 及 [`run-on-windows.ps1`](../../run-on-windows.ps1) **均已废弃且不适用于本工具**。请勿读取、参考或执行任何旧文档与旧脚本，本工具为全新独立设计与实现。

---

## 1. 概述与核心定位

Windows 调试桥是一个纯 .NET 10 BCL 实现的双端工具，旨在让 WSL 侧的开发人员或自动化代理通过标准 CLI 命令，由 Windows 宿主前台 Worker 执行 `dotnet run` / `dotnet build` 调试，并在 Windows 真实桌面上运行应用窗口。

### 核心特性
- **单一工程、纯 BCL**：基于单一工程 [Bridge.csproj](Bridge.csproj)，生成 `wdb`（`wdb.dll`），零第三方 NuGet 依赖，命名空间统一为 `GalleriesManager.Wdb`。自举测试实测成功，完全不依赖外部工程属性。
- **文件队列通信**：调试桥自身**不开放任何 TCP/UDP 网络端口，不注册 Windows 服务，不配置开机自启**。所有调度交互完全通过 Workspace 内的文件系统队列（默认 `<repo>/.wdb/queue`）完成。
- **源码与构建副本物理隔离**：CLI 仅读取当前源码并冻结打包至队列的 `payload/`，**CLI 严禁直接写入 Windows 宿主盘**；Windows 前台 Worker 将其复制到 Windows 本机专属目录（默认 `%LOCALAPPDATA%\WslDebugBridge\runs\<id>\src`）下进行独立构建与运行，彻底隔离 WSL 与 Windows 构建产物，杜绝 UNC/WSL 路径构建限制与 `bin/obj` 锁死污染。
- **运行目录所有权安全约束**：Worker 强制校验运行根（RunsRoot）所有权标记文件 `.wsl-debug-bridge-runs-owner`（首行必须匹配 `wsl-debug-bridge/runs-root/v1`）。空目录或新根自动采纳标记；若已存在非空目录且缺少有效标记，Worker 拒绝启动并以退出码 6 退出；任何已存在的同名任务目录均拒绝写入；入队排队中以 250ms 周期扫描，收到取消标记立即进入取消终态。
- **强受控生命周期与精确参数**：目标进程经 Windows Job Object 隔离（`CREATE_SUSPENDED` -> `AssignProcessToJobObject(KILL_ON_JOB_CLOSE)` -> `ResumeThread`）。原生命令行组装严格匹配 CRT 标准（已完成 4000 次固定种子 fuzz 与 4000 次运行时差分校验）；句柄与属性列表资源泄漏防护已固化；任务取消、超时或 Worker 停机时由 Job Object 强制级联清理整棵进程树，杜绝子进程残留。
  > ⚠️ **已知观测边界（Core 设计限制）**：由于 `Status.Pid` 仅在阶段转换时原子写盘，长驻目标程序在 `running` 运行期间，`status.json` 中的 `pid` 字段可能为 `null` 或记录前一阶段构建宿主的 PID（例如 GuiApp 运行期曾见 `running` 阶段中间 PID 9296，而终态收敛为真实目标 PID 3380）。**不得将运行期的 `status.pid` 作为调试附加（attach）或按 PID 终止进程的依据**；任务停止必须且只能依据任务 ID 投递 `cancel` 标记由所属 Job Object 权威回收，当前 Core 冻结且不保证 livePID 实时更新。

### ⚠️ 重要安全边界与权限声明（非进程沙箱）
1. **源码与产物隔离 ≠ 进程沙箱**：
   - 调试桥所做的隔离仅限于**源码快照与编译产物目录**。
   - 被启动的目标应用（如主程序桌面客户端）**仍以 Windows 登录用户的真实物理权限运行**。
   - 目标程序对 Windows 网络访问、用户本地存储（如 `%LOCALAPPDATA%` / `%APPDATA%`）、SQLite 数据库及本地配置的读取与写入**均遵循该程序自身的业务逻辑**，调试桥**不会也不可能**阻止被测程序对真实宿主用户数据的修改。
2. **实机验收建议**：
   - Windows 实机冒烟验证**必须优先使用测试治具应用**（如 [tests/fixtures/EchoApp/EchoApp.csproj](tests/fixtures/EchoApp/EchoApp.csproj)），验证前台启动、日志流式回传与 Job 杀灭。
   - 运行主项目 [src/GalleriesManager.Desktop/GalleriesManager.Desktop.csproj](../../src/GalleriesManager.Desktop/GalleriesManager.Desktop.csproj) 前，必须确认被测应用的配置与测试数据，防止污染宿主环境下的真实画廊库与数据库。

---

## 2. 架构与通信协议

### 2.1 目录组织与文件队列契约

通信队列默认位于仓库根目录下的 `.wdb/queue/`（由 [Contracts.cs](Contracts.cs) 定义的 [BridgeLayout](Contracts.cs#L22) 统筹）：

```
.wdb/queue/
├── worker.json                 # Worker 心跳与元数据（PID、启动时间、最后心跳时间；>15s 视为失联）
├── shutdown                    # Worker 退出标记（空文件；Worker 检测到后立即取消当前任务并杀灭进程树，随后自身退出 0）
└── jobs/
    └── <id>/                   # 任务 ID（格式形如 20261011T093932978Z-feda64）
        ├── payload/            # CLI 冻结的源码快照（先写入完成）
        ├── payload-report.json # 源快照报告（文件数、总字节、排除项清单与哈希）
        ├── request.json        # 任务请求（原子落盘作为任务提交完成标记，且必须为最后写入）
        ├── status.json         # Worker 回写的任务执行状态（queued/running/succeeded/failed/canceled/timeout/rejected/interrupted）
        ├── stdout.log          # 任务标准输出追加日志
        ├── stderr.log          # 任务标准错误追加日志
        └── cancel              # 任务取消标记（空文件，CLI 发起 stop 时创建）
```

### 2.2 提交与调度时序
1. **源码冻结**：CLI 扫描源码（跳过 `.git`、`bin`、`obj`、超大模型文件等），将快照写入 `queue/jobs/<id>/payload/` 并生成 `payload-report.json`。
2. **原子提交**：CLI 原子写入 `request.json`，Worker 轮询到该文件后认领任务并开始复制。
3. **Worker 本地执行**：Worker 将 payload 复制到专用本地运行目录（`%LOCALAPPDATA%\WslDebugBridge\runs\<id>\src`），在 Windows 本机执行 `dotnet build` 或启动目标程序，持续追加 `stdout.log` / `stderr.log` 并定期刷新 `status.json`。
4. **状态与日志监控**：CLI 通过轮询 `status.json` 与增量读取 `.log` 文件完成 `wait`、`logs` 与 `status` 操作。任务终态（`succeeded`/`failed`/`canceled`/`timeout`/`rejected`/`interrupted`）达成后退出。
5. **强受控终止与停机**：
   - **单任务停止 (`stop`)**：向任务目录写入 `cancel` 空文件，Worker 捕获后立即通过 Job Object 强制杀灭目标进程树，任务写终态 `canceled`。
   - **Worker 停机 (`shutdown`)**：向队列根写入 `shutdown` 空文件，Worker 发现后**不会等待当前任务自然结束**，而是立即取消当前正在运行的任务（写终态 `interrupted`）、强制杀灭 Job Object 进程树，随后 Worker 自身干净退出（退出码 0）。

---

## 3. 快速启动：Windows 前台 Worker 启动与自动冒烟验收

Worker由用户在Windows前台终端启动；核心构建/运行通过文件队列调度。WSL只读PowerShell进程诊断本次可用，不承担构建/运行调度。随后由 WSL 端自动化脚本主动接管端到端验收。

### 3.1 第一步：Windows 前台启动 Worker（用户执行）

在 Windows 物理机的 PowerShell 中直接运行以下单行命令（默认队列为本仓库的 `.wdb\queue`）：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File \\wsl.localhost\<distro>\<user>\galleries-manager\tools\windows-debug-bridge\start-worker.ps1 -Queue '\\wsl.localhost\<distro>\<user>\galleries-manager\.wdb\queue'
```

> **运行说明**：
> 1. [start-worker.ps1](start-worker.ps1) 会自动校验路径安全性，将桥工程源码复制到本机专用目录 `%LOCALAPPDATA%\WslDebugBridge\tool\build-<GUID>` 下干净编译。
> 2. 编译成功后在前台执行：`wdb.exe worker --queue "\\wsl.localhost\<distro>\<user>\galleries-manager\.wdb\queue" --runs-root "%LOCALAPPDATA%\WslDebugBridge\runs"`。
> 3. 控制台将持续输出 Worker 心跳与任务调度日志；保持前台运行。

### 3.2 第二步：WSL 侧一键自动化验收（WSL 终端/代理执行）

用户启动 Worker 后，WSL 侧可通过自动化测试脚本主动发起跨系统联调验收，替代肉眼观察任务管理器，全自动核验并输出机器可读证据：

```bash
# 1. 基础跨系统进程冒烟（无需桌面交互）
# 静态自检（离线模式，无需 Worker 即可验证脚本逻辑与代码合规）
python3 tools/windows-debug-bridge/tests/windows-smoke.py --static
# 全量联调验收（需 Windows 前台 Worker 保持运行）
python3 tools/windows-debug-bridge/tests/windows-smoke.py --evidence evidence-windows.json

# 2. 真实桌面 GUI 交互验证（Win32 桌面治具）
# 在 Windows 桌面拉起 420x260 交互窗口、自检 HWND/PID/PrintWindow 渲染截图并安全关闭
python3 tools/windows-debug-bridge/tests/windows-gui.py

# 3. 隔离副本构建与真实 GUI 验收（Avalonia 画廊主程序）
# 在用户授权的安全隔离副本内验证主桌面客户端编译构建（原仓库业务代码保持只读）：
python3 tools/windows-debug-bridge/tests/windows-gallery-gui.py --new-copy
python3 tools/windows-debug-bridge/tests/windows-gallery-gui.py --build
# 真实 GUI 运行需显式确认授权（未授权正常数据根，必须以隔离副本运行）：
python3 tools/windows-debug-bridge/tests/windows-gallery-gui.py --run --confirm-gui-run
```

**验收项覆盖**：
- 自动确认宿主为真实 Windows OS 与盘符本地 cwd（拒绝 Linux 假绿；注意 WSL 主机名可能与 Windows 相同，不作为判据）；
- 验证任务提交、本地盘解包构建与真实进程启动；
- 验证 `ProcessProbe` 跨进程探测（通过 Windows 真实 `Process.GetProcessById` 严格确认三层进程树在 `stop` 取消后彻底完全消失）；
- 验证真实 Windows 桌面窗口交互（纯 Win32 交互桌面治具成功渲染并捕获 PrintClient PNG 截图）；
- 验证真实 Avalonia 业务应用（[src/GalleriesManager.Desktop](../../src/GalleriesManager.Desktop)）在隔离数据根下成功拉起 GUI 窗口（HWND 0xB0A90、PID 23788），Avalonia 自身视觉树 RenderTargetBitmap 解码验证有效（127 色非空白），且宿主真实数据目录前后元数据严格一致（零污染）；
- 验收结果自动写入机器可读证据文件（`.wdb/windows-validation.json`、`.wdb/windows-day2-boundaries.json`、`.wdb/windows-gui-validation.json` 与 `.wdb/windows-gallery-gui-validation.json`），清晰标明通过项与已知边界。

---

## 4. WSL 侧 CLI 使用指令

建议先编译桥工具生成产物，使用 Bash 数组包装调用（安全处理路径空格并避免 `dotnet run` 的 MSBuild 输出污染标准输出）：

```bash
# 预先编译（一次性）
dotnet build tools/windows-debug-bridge/Bridge.csproj -c Release

# 声明 CLI 数组
WDB=(dotnet "$(pwd)/tools/windows-debug-bridge/bin/Release/net10.0/wdb.dll")
```

### 4.1 环境自检 (`doctor`)
检测队列目录可写性、Worker 存活与心跳状态、源码目录可快照性：
```bash
"${WDB[@]}" doctor
# 或以 JSON 输出
"${WDB[@]}" doctor --json
```

### 4.2 仅构建 (`build`)
冻结当前源码，交由 Windows Worker 执行 `dotnet build`：
```bash
"${WDB[@]}" build --project src/GalleriesManager.Desktop/GalleriesManager.Desktop.csproj
# 提交并阻塞等待构建完成
"${WDB[@]}" build --project src/GalleriesManager.Desktop/GalleriesManager.Desktop.csproj --wait
```

### 4.3 构建并运行 (`build-run`)
在 Windows 端编译成功后，直接拉起目标应用：
```bash
# 冒烟推荐优先测试 EchoApp 治具
"${WDB[@]}" build-run --project tools/windows-debug-bridge/tests/fixtures/EchoApp/EchoApp.csproj --follow

# 运行主桌面客户端（请确保已知悉非沙箱环境的真实配置影响）
"${WDB[@]}" build-run --project src/GalleriesManager.Desktop/GalleriesManager.Desktop.csproj --follow
```

### 4.4 直接运行 (`run`)
直接在 Windows 端执行目标程序，支持透传应用参数（`-- <参数>`）：
```bash
"${WDB[@]}" run --project tools/windows-debug-bridge/tests/fixtures/EchoApp/EchoApp.csproj -- --message "Hello Windows"
```

### 4.5 查询状态 (`status`)
查看全局队列与 Worker 存活状态，或查看具体任务状态：
```bash
# 查看队列概览与最近 10 个任务
"${WDB[@]}" status

# 查看特定任务状态（位置参数传递任务 ID）
"${WDB[@]}" status <id>

# 以 JSON 格式输出状态详情
"${WDB[@]}" status <id> --json
```

### 4.6 获取实时日志 (`logs`)
查看任务的标准输出或标准错误日志：
```bash
# 读取最新 200 行日志
"${WDB[@]}" logs <id>

# 流式跟随（follow）输出日志
"${WDB[@]}" logs <id> --follow

# 仅查看 stderr，或查看所有流（stdout/stderr 均带前缀）
"${WDB[@]}" logs <id> --stream all

# 按字节偏移增量拉取日志
"${WDB[@]}" logs <id> --offset 4096
```

### 4.7 阻塞等待 (`wait`)
阻塞等待任务达到终态（`succeeded`/`failed`/`canceled`/`timeout`/`rejected`/`interrupted`）：
```bash
# 等待任务达到终态
"${WDB[@]}" wait <id>

# 等待并同步流式输出日志（默认轮询间隔 500ms，超时 900s）
"${WDB[@]}" wait <id> --follow --wait-timeout 300 --poll 200
```

### 4.8 停止任务 (`stop`)
向指定运行中任务投递 `cancel` 标记文件，指示 Worker 强制杀灭 Job Object 进程树：
```bash
"${WDB[@]}" stop <id> --reason "用户主动停止调试"
```

### 4.9 退出 Worker (`shutdown`)
向队列投递 `shutdown` 标记。Worker 检测到标记后将**立即取消当前正在运行的任务**（状态置为 `interrupted`）、强制杀灭 Job Object 目标进程树，随后 Worker 自身以退出码 0 干净退出：
```bash
"${WDB[@]}" shutdown --reason "工作结束安全退出"
```

---

## 5. 自动化代理可复制调用工作流范例

自动化代理执行构建调试与测试时，标准且纯净的调用脚本如下：

```bash
#!/usr/bin/env bash
set -euo pipefail

# 1. 确保桥产物已构建（避免 dotnet run 的 MSBuild 输出干扰 JSON）
dotnet build tools/windows-debug-bridge/Bridge.csproj -c Release --nologo > /dev/null
WDB=(dotnet "$(pwd)/tools/windows-debug-bridge/bin/Release/net10.0/wdb.dll")

# 2. 检查 Worker 状态（要求 Worker 必须健康存活）
"${WDB[@]}" doctor --json

# 3. 提交 build-run 任务，stdout 单行通过 python3 解析 JSON 获取任务 ID
# （冒烟验证优先使用 EchoApp）
SUBMIT_JSON=$("${WDB[@]}" build-run --project tools/windows-debug-bridge/tests/fixtures/EchoApp/EchoApp.csproj --json)
JOB_ID=$(python3 -c "import sys, json; print(json.loads(sys.argv[1])['id'])" "$SUBMIT_JSON")
echo "已提交任务 ID: $JOB_ID"

# 4. 流式等待任务（例如观测 15 秒）
"${WDB[@]}" logs "$JOB_ID" --follow --wait-timeout 15 || true

# 5. 调试核对完成后强制终止目标程序进程树
"${WDB[@]}" stop "$JOB_ID" --reason "自动化测试完成，回收进程"
```
