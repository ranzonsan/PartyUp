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

## 校验

```powershell
& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
& "C:\Program Files\CMake\bin\cmake.exe" --version
& "C:\Program Files\Go\bin\go.exe" version
dotnet --version
```

期望：BuildTools 路径、`cmake 4.4.4`、`go1.27.3`、`10.0.400`。

## 调试桥（wdb，首选）

Windows 侧跑 GUI 的正确姿势走调试桥，不用手工 rsync。它来自 `galleries-manager` 项目，整份拷贝在 [`tools/windows-debug-bridge`](../tools/windows-debug-bridge)。

工作方式：WSL 里的 CLI 把仓库源码冻结成快照写进 `.wdb/queue/jobs/<id>/payload/`，Windows 上的前台 Worker 取走，复制到 `%LOCALAPPDATA%\WslDebugBridge\runs\<id>\src`，**在本地磁盘构建并运行**，日志流式回写队列。整个过程不开端口，只走文件。

这解决了两个老问题：产物落在本机磁盘，原生库不会像 UNC 那样被 PATH 上的旧版本抢走；窗口跑在真实桌面上，同时 Job Object 保证 `stop` 能杀干净整棵进程树。

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
