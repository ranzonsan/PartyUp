# 打包与分发

> 本文是「产物怎么来、怎么验、怎么换图标、未签名怎么告知用户」的唯一出处。
> 脚本：`scripts/package.ps1`（打包）、`scripts/verify-package.ps1`（验收）、
> `scripts/make-assets.ps1`（占位图标/启动图生成，一次性）。

## 一条命令

在 Windows 上（仓库所在位置即可，WSL 路径也行）：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\package.ps1
```

只打客户端 / 只打服务端：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\package.ps1 -Target client
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\package.ps1 -Target server
```

产物默认落在 `artifacts/`（已 gitignore），并生成 `SHA256SUMS.txt`：

| 产物 | 说明 |
| --- | --- |
| `PartyUp-<ver>-win64-portable.zip` | 绿色版，解压即用；内含一层 `PartyUp-<ver>-win64/` 目录 |
| `PartyUp-<ver>-win64-Setup.exe` | Velopack 一键安装器，per-user 装到 `%LOCALAPPDATA%\io.partyup.client`，不需要管理员 |
| `PartyUp-<ver>-win64.msi` | MSI，带欢迎 / 许可 / 完成页；安装时可选 per-user / per-machine |
| `PartyUp-Server-<ver>-win64.zip` | `partyup.exe` + `start.cmd` + `invite.cmd` + `README.txt` + `LICENSE.txt` |

`artifacts/velopack/` 保留 vpk 的原始输出（nupkg、releases 清单等），方便以后接自动更新，不参与分发。

## 前置依赖（Windows 侧）

| 工具 | 版本 | 用途 | 本机状态 |
| --- | --- | --- | --- |
| .NET SDK | 10.0.400（`global.json` 锁定） | 客户端 publish | 已装 |
| Go | 1.27.3（`C:\Program Files\Go\bin\go.exe`） | 服务端构建（仅 `-Target server/all`） | 已装 |
| vpk | 1.2.0（`dotnet tool install --global vpk`） | Setup.exe / MSI | 已装 |
| tar.exe | Windows 10+ 自带（bsdtar） | 生成 zip | 已装 |

**不需要** Visual Studio / MSVC / CMake：`partyup_media.dll` 用现成构建产物
（`src/media-core/out/partyup_media.dll`），打包脚本从不编译 C++。DLL 缺失时脚本直接报错。

## 版本号：每个组件一个来源

客户端与服务端是**两个独立的版本序列与发布通道**（2026-11-08 起）：

| 组件 | 版本文件 | release tag | 产物 |
|---|---|---|---|
| 客户端 | `client/VERSION` | `v0.1.0` | Setup.exe / MSI / portable.zip / Velopack feed |
| 服务端 | `server/VERSION` | `server-v0.1.0` | Server zip + Docker 镜像 |

以下全部从这两个文件读，不要在别处再写死：

- 产物文件名里的版本号（客户端四件套读 `client/VERSION`，服务端 zip 读 `server/VERSION`）；
- 客户端 assembly/file/informational version（csproj 读 `client/VERSION`）；
- Velopack `--packVersion` 与 MSI 的 `--msiVersion`（由客户端版本补成 `x.y.z.0`）；
- 服务端 `partyup version` 的输出（`go build -ldflags "-X main.version=<server/VERSION>"`；源码里是
  `var version = "dev"`，直接 `go build` 不走打包脚本时显示 dev）。

发版时只改对应那一侧的文件：改客户端 → `client/VERSION` + 打 `v*`；改服务端 → `server/VERSION`
+ 打 `server-v*`。`release-client.yml` / `release-server.yml` 会校验 tag 与对应文件一致。

## 图标：单文件替换

所有图标都来自 **`assets/icon.ico`**（多尺寸 16/32/48/256，占位图）：

- 客户端 exe 图标：`client/PartyUp.Client.csproj` 的 `<ApplicationIcon>`；
- Setup.exe / MSI / 快捷方式图标：`vpk --icon`；
- 安装器启动图：`assets/splash.png`（500x300 PNG，可换成 gif/png/jpeg）。

真 logo 到位后，只替换 `assets/icon.ico`（要求包含 16/32/48/256 四种尺寸）与
`assets/splash.png`，再跑打包脚本即可。需要重新生成占位图（无 Pillow/ImageMagick 环境）：
`powershell -File scripts\make-assets.ps1`。

## 客户端为什么自包含、为什么不 trim

- `--self-contained -r win-x64`：用户不需要装 .NET；
- **不 trim**（`PublishTrimmed=false`）：Avalonia 大量依赖反射，trim 的收益（几十 MB）不值得
  承担运行时缺类型/绑不上的风险；
- `partyup_media.dll` 必须与 `PartyUp.Client.exe` 同目录（客户端从 `AppContext.BaseDirectory`
  显式加载），打包脚本会校验它确实在 payload 里。

## MSI 的「安装时可选安装位置」= 作用域选择，不是目录选择器

Velopack 的 MSI 只有三种 `--instLocation` 语义：

- `PerUser`：免管理员，装到用户目录，**UI 不显示作用域页**；
- `PerMachine`：装到 `Program Files\{publisher}\{packTitle}`，需要管理员；
- `Either`（本项目采用）：**安装时显示作用域页（per-user / per-machine）+ 目录页**，
  用户可选择免管理员的 per-user，或需要 UAC 的 per-machine，并可改安装目录。

MSI 里实际存在的页面（用 WindowsInstaller COM 读 Dialog 表核对，见验收脚本）：
`WelcomeDlg`（欢迎 + 未签名/SmartScreen 说明）、`LicenseAgreementDlg`（GPL-3.0）、
`InstallScopeDlg` + `BrowseDlg`（作用域/安装位置）、`VerifyReadyDlg`、`ProgressDlg`、
`ExitDialog`（完成页）。

**静默安装指定目录**（已实测，任意目录）：

```powershell
msiexec /i PartyUp-<ver>-win64.msi /qn VELOPACK_INSTALLDIR="D:\Apps\PartyUp"
```

实测结论与注意：

- 不传 `VELOPACK_INSTALLDIR` 时，Velopack 1.2.0 模板的 **per-user 默认目录是 `C:\PartyUp`**
  （不是 `%LOCALAPPDATA%`）；要落到 `%LOCALAPPDATA%\io.partyup.client` 就显式传该属性。
  验收脚本即用 `VELOPACK_INSTALLDIR=%LOCALAPPDATA%\io.partyup.client` 做 per-user 安装。
- **不要从网络/UNC 路径直接 `msiexec /i`**：安装由 SYSTEM 的 Windows Installer 服务执行，
  读不到 `\\wsl.localhost\...` 这类共享路径，会报 1620（日志里是 2203）。
  先复制到本地磁盘再装。
- `Setup.exe` 天生 per-user（装到 `%LOCALAPPDATA%\io.partyup.client`，免管理员），
  `--installto` 可在安装时改目录，但一键安装器默认不弹窗。

> **per-machine 路径未验证**：当前开发机是非管理员会话，验收只覆盖 per-user 安装/卸载。
> 发布前如需承诺 per-machine，请用管理员会话补测。

## 未签名：SmartScreen 警告必须告知用户

产物**不签名**（没有代码签名证书）。从网上下载后首次运行，Windows SmartScreen
会弹「Windows 已保护你的电脑」。这是正常提示，但**必须提前告知**，否则用户会以为中了病毒：

- 绿色版：`assets/portable-README.txt`（随 zip 分发为 `README.txt`）；
- MSI：`assets/installer/welcome.md`（欢迎页）；
- 安装器/README/本文档都写明操作：**更多信息 → 仍要运行**。

不要试图隐藏或绕过该提示；要消除它只能购买代码签名证书并给 exe/Setup/MSI 签名（后续可做）。

## 服务端包

`PartyUp-Server-<ver>-win64.zip` 解压后：

- `start.cmd`：`partyup.exe serve --data data --http 0.0.0.0:8080`，双击即用（窗口要一直开着）；
- 首次启动往 stdout 打印 `BOOTSTRAP_TOKEN=` 与 `INVITE_CODE=`，最后一行 `READY`——
  **邀请码就是 `INVITE_CODE=` 那一行**，发给朋友即可；
- 之后再发码：双击 `invite.cmd`（或 `partyup.exe invite --data data`），只打印新的 `INVITE_CODE=`；
- 默认端口：TCP 8080（客户端连接）、UDP 3478（内建 STUN，relay 模式可关）；
- 数据全在 `data\`（SQLite），备份该目录即备份服务器。

## 验收

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\verify-package.ps1
```

自动做（详细日志在 `%TEMP%\partyup-verify\...`）：

1. 绿色版：解压到干净目录 → 启动 → 断言日志 `媒体引擎：已就绪（ABI v2，24/24 导出）` →
   结束进程；
2. Setup.exe：`-s` 静默安装 → 启动断言 → `Update.exe uninstall -s` → 校验安装目录、
   快捷方式、卸载注册表项全部消失；
3. MSI：静默 per-user 安装 → 启动断言 → `msiexec /x` 卸载 → 校验清理；另外用
   WindowsInstaller COM 读取 MSI 的 Dialog/Control 表，确认欢迎/许可/完成/位置页确实存在；
4. 服务端 zip：解压到干净目录 → 起服 → 断言 stdout 出现 `INVITE_CODE=` 与 `READY` →
   `invite` 子命令再发一个码 → 断言版本号与 `VERSION` 一致。

## 已知问题 / 排错

- **首次 restore 慢**：要下载 win-x64 .NET runtime pack（约 100–200 MB），一次命令首跑几分钟
  属正常；之后命中 NuGet 缓存。
- **不要在 `\\wsl.localhost\...` 上直接构建**：`dotnet publish` 会报 MSB3030、`go build`
  会报 `RLock ... Incorrect function`。脚本会先把源码拷到本机
  `%LOCALAPPDATA%\PartyUp\package\src` 再构建。
- **`Compress-Archive` 不能用**：Windows PowerShell 5.1 写出的 zip 条目用反斜杠分隔，
  不是标准 zip；脚本改用系统自带 `tar.exe -a -c -f`。
- **vpk 选项名随版本变**：本机是 1.2.0（`--msiBanner/--msiLogo`），官网最新文档写的是
  `--msiTopBanner/--msiDialogBackground`；脚本按本机 `vpk pack -H` 的实际参数来。
- **杀软隔离 `partyup_media.dll`**：会表现为「媒体引擎不可用」，把程序目录加入白名单。
- **卸载残留检查**：per-user 安装的 `%LOCALAPPDATA%\io.partyup.client`、桌面/开始菜单
  的 `PartyUp.lnk`、`HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\io.partyup.client`。
