#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""windows-gallery-gui.py —— 真实 Avalonia 画廊应用的「隔离副本 GUI 验收」

与同目录其它脚本的分工（互不替代）：
  * windows-smoke.py       —— 桥的**控制面**（EchoApp / ProcessProbe：run / build-run / 取消杀树 / 参数保真）；
  * windows-gui.py         —— 桥的**显示面**（GuiApp：证明桥能把一个 GUI 进程拉到交互桌面并显示**可见窗口**）；
  * windows-lifecycle.py   —— 桥的**生命周期面**（Worker 启停 / 副 Worker）；
  * windows-gallery-gui.py —— **本文件**：验收**真实业务应用** src/GalleriesManager.Desktop 在
                              **隔离数据根**下能否真的启动出窗口，并证明它**没有触碰用户真实数据**。

为什么需要「隔离副本」而不是直接跑仓库：
  仓库的 src/GalleriesManager.Core/Constants/AppPaths.cs 把数据根硬编码为
  %LOCALAPPDATA%\\galleries-manager（Release）/ galleries-manager-dev（Debug），**没有任何**
  运行时 env / 参数 / profile 覆盖钩子（已只读审计确认）。因此直接 `wdb build-run` 真实应用
  必然读写用户真实的库目录。本脚本的做法是：把仓库源码**复制**到 .wdb/guisrc-<GUID>/ 专用副本，
  只在该副本内打一组**写死的数据根补丁 + 4 项加固**，再用桥的 `--source` 指向副本快照构建运行，
  **原仓库业务代码一行不改**。

副本内补丁（全部由本脚本 --new-copy 施加，且由 --verify 逐条静态断言）：
  P1  AppPaths.Root 钉死到 AppContext.BaseDirectory/_gm_gui_testdata_<GUID>（无 env 后门）
  P2  移除 Velopack 生命周期钩子（VelopackApp.Build().Run()）
  P3  ConnectTelegram 无条件 Skipped（绝不建 TDLib / 绝不联网）
  P4  LLM 加载期不再自动探测硬件
  P5  LlmHardwareProbe.Probe() 调用点失效（连带消灭 nvidia-smi.exe 外部进程）
  P6  移除 DI 注册期对 <Root>/tools/7z 的递归删除
  P7  真实 App 的 Opened 事件注入自证 instrumentation（WDB_GALLERY_GUI + 只截自己窗口的截图 + 自动退出）
  P8  RestorePackagesPath 收束到副本内 _gm_nuget_packages（不改 Windows 全局 SDK 配置）

硬判据（缺一即 FAIL，且绝不在不确定时运行 GUI）：
  H1  副本 patch 静态断言全通过（--verify）；
  H2  运行前后用户真实 %LOCALAPPDATA%\\galleries-manager 与 galleries-manager-dev 的
      「完整元数据 + 必要内容哈希清单」**逐字节一致**（由独立 Windows 只读探针 GalleryGuiProbe 产出）；
  H3  应用自证的 AppPaths.Root 落在桥本次运行副本内，且不是两个真实根；
  H4  真实 HWND 非空、窗口可见、尺寸 > 0；
  H5  截图存在、双通道尺寸/哈希三方一致；**「窗口真的被绘制过」的权威证据是 PNG 通道**
      （Avalonia RenderTargetBitmap 只渲染本应用自己的可视树），由 Windows 侧纯 BCL 解码像素证明非空白。
      BMP 通道（Win32 PrintWindow，只作用于自身 HWND）对 GPU 合成窗口可能只有单一色，属已知特例，
      **不作非空白判据**。两条通道都绝无桌面抓取 API；且本验收**不主张**桌面/整屏像素级证据。

模式：
  --static            纯静态自检（不需要 Worker、不投任何队列任务、绝不运行 GUI）
  --new-copy          生成新的 .wdb/guisrc-<GUID> 专用副本并施加全部补丁（目录已存在则拒绝）
  --verify            对副本做 patch/哈希静态断言（默认取最新的 .wdb/guisrc-*）
  --preflight         只读体检：doctor + Worker 健康 + 队列是否空闲（不安置任何任务）
  --build             只提交一次 `wdb build` 构建副本的 Desktop 工程（需要队列空闲）
  --run               完整验收：verify → baseline(前) → GUI → 截图复核 → baseline(后) → 比对
                      **真实 GUI 运行需要显式 --confirm-gui-run**，否则只报到 READY 并退出（fail closed）
  证据：.wdb/windows-gallery-gui-validation.json（另出同名 .log 人读日志）

退出码：0 = 全通过；1 = 有失败；2 = 用法/环境错误；3 = 阻塞（未确认 / 队列繁忙 / 无 Worker / fail closed）
"""

from __future__ import annotations

import argparse
import datetime as _dt
import hashlib
import json
import ntpath
import os
import re
import shutil
import subprocess
import sys
import tarfile
import time
import uuid
from dataclasses import dataclass, field
from pathlib import Path

# --------------------------------------------------------------------------
# 路径与常量
# --------------------------------------------------------------------------
SCRIPT_DIR = Path(__file__).resolve().parent                 # tools/windows-debug-bridge/tests
BRIDGE_DIR = SCRIPT_DIR.parent                               # tools/windows-debug-bridge
REPO_ROOT = BRIDGE_DIR.parent.parent                         # repo root
DEFAULT_QUEUE = REPO_ROOT / ".wdb" / "queue"
WDB_DIR = REPO_ROOT / ".wdb"
# ---- 证据输出路由（DS/父 2026-10-12 BLOCKER 修复）----
# 事故：`--static` 曾把 canonical 的 `mode=run` 证据覆盖成 `mode=static`（与 smoke 同样的覆盖事故）。
# 结构性修复：**每个 mode 有自己的默认 JSON**，canonical run JSON 只允许 `mode=run` 写入（见 write_evidence 守卫）。
EVIDENCE_RUN = WDB_DIR / "windows-gallery-gui-validation.json"
EVIDENCE_BY_MODE = {
    "run": EVIDENCE_RUN,
    "static": WDB_DIR / "windows-gallery-gui-static.json",
    "verify": WDB_DIR / "windows-gallery-gui-verify.json",
    "new-copy": WDB_DIR / "windows-gallery-gui-newcopy.json",
    "preflight": WDB_DIR / "windows-gallery-gui-preflight.json",
    "build": WDB_DIR / "windows-gallery-gui-build.json",
}
ARTIFACT_DIR = WDB_DIR / "windows-gallery-gui-artifacts"


class EvidenceRoutingError(RuntimeError):
    """证据被路由到不允许的目标（典型：非 run 模式试图写 canonical run JSON）。"""


def evidence_path_for(mode: str) -> Path:
    p = EVIDENCE_BY_MODE.get(mode)
    if p is None:
        raise EvidenceRoutingError(f"未知 mode，拒绝写出证据：{mode!r}")
    return p


def log_path_for(mode: str) -> Path:
    return WDB_DIR / ("windows-gallery-gui-validation.log" if mode == "run"
                      else f"windows-gallery-gui-{mode}.log")

PROBE_REL_PROJECT = "tools/windows-debug-bridge/tests/fixtures/GalleryGuiProbe/GalleryGuiProbe.csproj"
DESKTOP_REL_PROJECT = "src/GalleriesManager.Desktop/GalleriesManager.Desktop.csproj"
TEMPLATE_SELFCHECK = SCRIPT_DIR / "fixtures" / "GalleryGuiProbe" / "templates" / "GalleryGuiSelfCheck.cs.template"

GUI_PREFIX = "WDB_GALLERY_GUI "
PROBE_PREFIX = "WDB_GALLERY_PROBE "

COPY_PREFIX = "guisrc-"
STATE_FILE = ".gm-copy-state.json"

# 副本内被允许 patch 的文件白名单（相对副本根）—— 任何白名单外的差异都视为违规。
PATCHED_FILES = [
    "Directory.Build.props",
    "src/GalleriesManager.Core/Constants/AppPaths.cs",
    "src/GalleriesManager.Desktop/App.axaml.cs",
    "src/GalleriesManager.Desktop/Program.cs",
    "src/GalleriesManager.Desktop/Services/StartupOrchestrator.cs",
    "src/GalleriesManager.Desktop/Services/GalleryGuiSelfCheck.cs",
    "src/GalleriesManager.Desktop/ViewModels/LlmSettingsViewModel.cs",
    "src/GalleriesManager.Desktop/Extensions/ServiceCollectionExtensions.cs",
]

_log_fh = None


def log(msg: str) -> None:
    line = f"[{_dt.datetime.now().strftime('%H:%M:%S')}] {msg}"
    print(line, flush=True)
    global _log_fh
    if _log_fh is not None:
        _log_fh.write(line + "\n")
        _log_fh.flush()


def open_log(path: Path) -> None:
    global _log_fh
    path.parent.mkdir(parents=True, exist_ok=True)
    _log_fh = path.open("w", encoding="utf-8")


# --------------------------------------------------------------------------
# C# / XML 注释剥离（静态断言必须只看**代码**，不看注释）
# --------------------------------------------------------------------------
def strip_cs_comments(text: str) -> str:
    """剥离 C# 的 // 与 /* */ 注释，且**不误伤字符串字面量**内的 // 或 /*。

    支持：普通字符串 "..."（含 \\ 转义）、verbatim 字符串 @"..."（"" 为转义）、字符字面量 '...'。
    """
    out = []
    i, n = 0, len(text)
    state = "code"  # code | line | block | str | vstr | chr
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if state == "code":
            if c == "/" and nxt == "/":
                state = "line"; i += 2; continue
            if c == "/" and nxt == "*":
                state = "block"; i += 2; continue
            if c == "@" and nxt == '"':
                state = "vstr"; out.append(c); out.append(nxt); i += 2; continue
            if c == '"':
                state = "str"; out.append(c); i += 1; continue
            if c == "'":
                state = "chr"; out.append(c); i += 1; continue
            out.append(c); i += 1; continue
        if state == "line":
            if c == "\n":
                state = "code"; out.append(c)
            i += 1; continue
        if state == "block":
            if c == "*" and nxt == "/":
                state = "code"; i += 2; continue
            i += 1; continue
        if state == "str":
            out.append(c)
            if c == "\\":
                if i + 1 < n: out.append(text[i + 1]); i += 2; continue
            elif c == '"':
                state = "code"
            i += 1; continue
        if state == "vstr":
            out.append(c)
            if c == '"':
                if nxt == '"':
                    out.append(nxt); i += 2; continue
                state = "code"
            i += 1; continue
        if state == "chr":
            out.append(c)
            if c == "\\":
                if i + 1 < n: out.append(text[i + 1]); i += 2; continue
            elif c == "'":
                state = "code"
            i += 1; continue
    return "".join(out)


def strip_xml_comments(text: str) -> str:
    return re.sub(r"<!--.*?-->", "", text, flags=re.S)


# --------------------------------------------------------------------------
# 补丁定义（old 必须在目标文件中**恰好出现一次**，否则拒绝）
# --------------------------------------------------------------------------
APPPATHS_OLD = """    public static string Root => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        IsDev ? "galleries-manager-dev" : "galleries-manager");"""

APPPATHS_NEW = """    // ==================================================================================
    // 【隔离 GUI 验收副本 · 唯一数据根补丁】copy-guid={guid}
    //
    // 本副本只供 Windows 调试桥的隔离 GUI 验收使用。数据根**钉死**在本次桥运行副本内部：
    //   AppContext.BaseDirectory = <runs-root>\\<job-id>\\src\\src\\GalleriesManager.Desktop\\bin\\<cfg>\\net10.0\\
    // 因此 Data/Cache/Logs/Tdlib/Tools/LlmModels/Backups/Temp/settings.json/salt 等
    // 全部派生路径（见本文件后文的 Data/Cache/... 定义）都收束在本次任务目录内，绝不触碰
    // %LOCALAPPDATA%\\galleries-manager 与 %LOCALAPPDATA%\\galleries-manager-dev。
    //
    // 刻意**不**提供任何环境变量 / 命令行后门：桥的 Request 合同没有 Environment 字段，
    // 无法注入 env；而未知的 Worker 继承环境反而可能造成意外重定向。故无 GM_TEST_ROOT 之类开关。
    // 原仓库的 %LOCALAPPDATA% 语义与 IsDev 分支在本副本中已不再参与 Root 求值。
    // ==================================================================================
    public static string Root => Path.GetFullPath(
        Path.Combine(AppContext.BaseDirectory, "_gm_gui_testdata_{guid}"));"""

PROGRAM_OLD = "        VelopackApp.Build().Run();"
PROGRAM_NEW = """        // ===== 【隔离 GUI 验收副本 · 加固 2】=====
        // 原实现：VelopackApp.Build().Run();（已移除）
        // 移除原因：Velopack 处理安装/更新/卸载生命周期钩子，可能读写安装目录等宿主状态；
        // 隔离 GUI 验收只需「真实 Avalonia 窗口能否启动」，不需要任何更新器行为。
        // using Velopack; 予以保留以最小化 diff（未使用 using 不产生构建错误）。"""

ORCH_OLD = """                        case StartupPhaseKey.ConnectTelegram:
                            var (apiId, apiHash, phone) = _accountService.LoadTelegramCredentials();
                            if (string.IsNullOrEmpty(apiId) || string.IsNullOrEmpty(apiHash) || string.IsNullOrEmpty(phone))
                            {
                                step.Status = StartupStepStatus.Skipped;
                                step.Detail = "未保存凭据";
                            }
                            else
                            {
                                await _sessionManager.InitializeAsync(ct);
                                if (_tdlib.IsConnected)
                                    step.Status = StartupStepStatus.Completed;
                                else
                                {
                                    step.Status = StartupStepStatus.Failed;
                                    step.Detail = "连接失败";
                                }
                            }
                            break;"""

ORCH_NEW = """                        case StartupPhaseKey.ConnectTelegram:
                            // ===== 【隔离 GUI 验收副本 · 加固 3】=====
                            // 原实现：读凭据 → _sessionManager.InitializeAsync(ct) → TDLib 连接（已移除）。
                            // 现在**无条件跳过**：绝不允许任何 Telegram/TDLib 网络连接或 <Root>/tdlib 写入，
                            // 即使隔离 core.db 里意外存在凭据也不连接。凭据读取也一并移除（少一次 DB 读）。
                            _ = _accountService;
                            _ = _tdlib;
                            _ = _sessionManager;
                            step.Status = StartupStepStatus.Skipped;
                            step.Detail = "隔离 GUI 验收：ConnectTelegram 已禁用";
                            break;"""

LLM_LOADFROM_OLD = """        if (LlamaOptionsRefresher.IsAutoMode(s))
        {
            // Auto 模式下以挡位配置为准覆盖自由参数（兼顾旧版本升级与配置漂移容错），并探测硬件以刷新挡位卡
            ApplyTierToParams(_llamaCatalog.FindTier(s.LlmTierChoice));
            _ = RefreshHardwareAsync();
        }"""

LLM_LOADFROM_NEW = """        if (LlamaOptionsRefresher.IsAutoMode(s))
        {
            // Auto 模式下以挡位配置为准覆盖自由参数（兼顾旧版本升级与配置漂移容错）。
            ApplyTierToParams(_llamaCatalog.FindTier(s.LlmTierChoice));
            // ===== 【隔离 GUI 验收副本 · 加固 4】=====
            // 原实现：_ = RefreshHardwareAsync();（已移除）—— 启动路径必须零外部进程。
        }"""

LLM_PROBE_OLD = "        var info = await Task.Run(() => _llmHardware.Probe());"

LLM_PROBE_NEW = """        // ===== 【隔离 GUI 验收副本 · 加固 5】=====
        // 原实现：var info = await Task.Run(() => _llmHardware.Probe());（已移除）
        // 探测会 spawn 外部进程 nvidia-smi.exe。此处保留 async 形状但**不调用任何外部进程**。
        await Task.CompletedTask;
        var info = LlmHardwareInfo.NotDetected;"""

# P4b/P4c：另外两处 RefreshHardwareAsync 调用点（UI 交互路径）。P5 已使探测自身失效，
# 这里再把这俩调用点也去掉，令「本副本内不存在任何 RefreshHardwareAsync 调用」成为可静态断言的不变量。
LLM_CFGMODE_OLD = """            ApplyTierToParams(_llamaCatalog.FindTier(LlmTierChoice));
            _ = RefreshHardwareAsync();"""

LLM_CFGMODE_NEW = """            ApplyTierToParams(_llamaCatalog.FindTier(LlmTierChoice));
            // ===== 【隔离 GUI 验收副本 · 加固 4b】=====
            // 原实现：_ = RefreshHardwareAsync();（已移除）—— 本副本零硬件探测、零外部进程。"""

LLM_TIERCARD_OLD = """        foreach (var c in LlmTierCards)
            c.IsSelected = c.Id == card.Id;
        _ = RefreshHardwareAsync();"""

LLM_TIERCARD_NEW = """        foreach (var c in LlmTierCards)
            c.IsSelected = c.Id == card.Id;
        // ===== 【隔离 GUI 验收副本 · 加固 4c】=====
        // 原实现：_ = RefreshHardwareAsync();（已移除）。"""

SVC_OLD = """        try
        {
            var legacy7z = Path.Combine(AppPaths.Tools, "7z");
            if (Directory.Exists(legacy7z))
                Directory.Delete(legacy7z, recursive: true);
        }
        catch (Exception ex) { Serilog.Log.Warning(ex, "Failed to clean legacy tools/7z"); }"""

SVC_NEW = """        // ===== 【隔离 GUI 验收副本 · 加固 6】=====
        // 原实现在此处对 <Root>/tools/7z 执行 Directory.Delete(..., recursive: true)（已移除）：
        // 这是启动路径上**唯一**的递归删除动作（DI 注册期即执行，早于 Host.Build()）。
        // 隔离验收要求「不删除任何目录」。"""

APP_OLD = """            desktop.MainWindow.Opened += async (_, _) =>
            {
                try
                {
                    await orchestrator.RunAsync(CancellationToken.None);
                }
                catch (Exception ex)
                {
                    Log.Fatal(ex, "Startup orchestrator crashed");
                }
            };"""

APP_NEW = """            desktop.MainWindow.Opened += async (_, _) =>
            {
                try
                {
                    await orchestrator.RunAsync(CancellationToken.None);
                }
                catch (Exception ex)
                {
                    Log.Fatal(ex, "Startup orchestrator crashed");
                }

                // ===== 【隔离 GUI 验收副本 · instrumentation】=====
                // 在**真实** App 的 MainWindow.Opened 事件里、启动编排跑完之后自证：
                // 输出一行 WDB_GALLERY_GUI {json}（OS / AppPaths.Root / PID / 真实 HWND / 可见性 / 尺寸 /
                // 只截自己窗口的双通道截图 + SHA256 / 各启动步骤终态），随后自动 desktop.Shutdown(0) 退出。
                // 全程不等用户点击。实现见 Services/GalleryGuiSelfCheck.cs（仅存在于本副本）。
                GalleryGuiSelfCheck.Run(desktop, orchestrator);
            };"""

PROPS_OLD = "    <CentralPackageVersionOverrideEnabled>true</CentralPackageVersionOverrideEnabled>"
PROPS_NEW = """    <CentralPackageVersionOverrideEnabled>true</CentralPackageVersionOverrideEnabled>

    <!-- ============================================================================
         【隔离 GUI 验收副本】依赖缓存收束（仅本副本的复制版 props，不改仓库、不改全局 SDK 配置）

         RestorePackagesPath：把 NuGet **全局包目录**重定向到本副本内部
           <copy>\\_gm_nuget_packages —— 于是还原出来的包全部落在桥本次运行副本里，
           绝不写入用户真实的 %USERPROFILE%\\.nuget\\packages。
         （桥的 Request 合同没有 Environment 字段，无法用 NUGET_PACKAGES 环境变量达成同样效果。）

         刻意**不设** RestoreNoCache=true：它只禁用 HTTP 缓存、强制每次全量重下，
         **并不增加任何包落点收束**（落点已由 RestorePackagesPath 决定），却显著提高还原失败率。
         ============================================================================ -->
    <RestorePackagesPath>$(MSBuildThisFileDirectory)_gm_nuget_packages</RestorePackagesPath>"""


# --------------------------------------------------------------------------
# P7 三态（在线-LLM 设置 UI）自证断言：针的片段在**拼接处**断开
# --------------------------------------------------------------------------
# 为什么把针拆开写：cmd_static 会扫描**本脚本自身源码**做越权审计（S1..S4），
# 若针的字面量完整出现在本文件里就会形成自指（扫描命中自己的断言常量）。
# 拆在片段中间既不影响子串语义，也不让整针在本文件中出现。
THREE_PHASE_NEEDLES = {
    "P7A_phase_enum":       '"' + 'A' + '"',
    "P7B_phase_enum":       '"' + 'B' + '"',
    "P7C_phase_enum":       '"' + 'C' + '"',
    "P7_phase_count":       "Phase" + "Count = 4",
    "P7_online_backend":    '"On' + 'line"',
    "P7_nav_settings":      "NavigateTo" + "SettingsLlmTab",
    "P7_visual_probe":      "Try" + "ProbeVisualTree",
    "P7_online_preset_names": "onlinePreset" + "Labels",
    "P7_online_endpoint":   "online" + "Endpoint",
    "P7_online_model":      "online" + "Model",
    "P7_tier_panel_flag":   "showLocal" + "TierPanel",
    "P7_backend_selector_flag": "showBackend" + "Selector",
    "P7_phase_summary":     "phases" + "Ok",
    # 视口裁切判据（缺陷 2 防回归）：量测代码必须存在，且必须有**明确结论**的字段。
    "P7_viewport_fit_measure":   "viewport" + "Fit",
    "P7_viewport_no_scroll":     "noVertical" + "ScrollNeeded",
    "P7_viewport_bottom_inside": "bottom" + "WithinViewport",
    "P7_watermark_binding":      "LlmOnline" + "EndpointWatermark",
}


def patch_specs(guid: str):
    return [
        ("P1_apppaths_root_pin", "src/GalleriesManager.Core/Constants/AppPaths.cs",
         APPPATHS_OLD, APPPATHS_NEW.format(guid=guid)),
        ("P2_remove_velopack", "src/GalleriesManager.Desktop/Program.cs", PROGRAM_OLD, PROGRAM_NEW),
        ("P3_disable_connect_telegram", "src/GalleriesManager.Desktop/Services/StartupOrchestrator.cs", ORCH_OLD, ORCH_NEW),
        ("P4_llm_no_auto_hwprobe", "src/GalleriesManager.Desktop/ViewModels/LlmSettingsViewModel.cs", LLM_LOADFROM_OLD, LLM_LOADFROM_NEW),
        ("P5_llm_probe_inert", "src/GalleriesManager.Desktop/ViewModels/LlmSettingsViewModel.cs", LLM_PROBE_OLD, LLM_PROBE_NEW),
        ("P4b_llm_cfgmode_no_probe", "src/GalleriesManager.Desktop/ViewModels/LlmSettingsViewModel.cs", LLM_CFGMODE_OLD, LLM_CFGMODE_NEW),
        ("P4c_llm_tiercard_no_probe", "src/GalleriesManager.Desktop/ViewModels/LlmSettingsViewModel.cs", LLM_TIERCARD_OLD, LLM_TIERCARD_NEW),
        ("P6_remove_legacy7z_delete", "src/GalleriesManager.Desktop/Extensions/ServiceCollectionExtensions.cs", SVC_OLD, SVC_NEW),
        ("P7_instrument_opened", "src/GalleriesManager.Desktop/App.axaml.cs", APP_OLD, APP_NEW),
        ("P8_restore_path_contained", "Directory.Build.props", PROPS_OLD, PROPS_NEW),
    ]


# --------------------------------------------------------------------------
# 通用小工具
# --------------------------------------------------------------------------
def sha256_file(p: Path) -> str:
    h = hashlib.sha256()
    with p.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def sha256_text(s: str) -> str:
    return hashlib.sha256(s.encode("utf-8")).hexdigest()


def run(argv, cwd: Path | None = None, timeout: float = 600.0):
    t0 = time.time()
    try:
        p = subprocess.run([str(a) for a in argv], cwd=str(cwd or REPO_ROOT),
                           capture_output=True, text=True, timeout=timeout, errors="replace")
        return p.returncode, p.stdout or "", p.stderr or "", time.time() - t0
    except subprocess.TimeoutExpired as exc:
        return 124, (exc.stdout or "") if isinstance(exc.stdout, str) else "", \
               (exc.stderr or "") if isinstance(exc.stderr, str) else "", time.time() - t0


def last_json(text: str):
    found = None
    for line in (text or "").splitlines():
        line = line.strip()
        if line.startswith("{"):
            try:
                found = json.loads(line)
            except json.JSONDecodeError:
                continue
    return found


def all_json(text: str):
    out = []
    for line in (text or "").splitlines():
        line = line.strip()
        if line.startswith("{"):
            try:
                out.append(json.loads(line))
            except json.JSONDecodeError:
                continue
    return out


def win_to_wsl(win_path: str) -> Path | None:
    m = re.match(r"^([A-Za-z]):[\\/](.*)$", win_path or "")
    if not m:
        return None
    return Path("/mnt") / m.group(1).lower() / m.group(2).replace("\\", "/")


def resolve_wdb():
    launcher = BRIDGE_DIR / "wdb"
    if launcher.is_file() and os.access(launcher, os.X_OK):
        return [str(launcher)]
    dll = BRIDGE_DIR / "bin" / "Release" / "net10.0" / "wdb.dll"
    if dll.is_file():
        return ["dotnet", str(dll)]
    return None


def resolve_copy(explicit: str | None) -> Path | None:
    if explicit:
        p = Path(explicit)
        return p if p.is_dir() else None
    cands = sorted([d for d in WDB_DIR.glob(COPY_PREFIX + "*") if d.is_dir()],
                   key=lambda d: d.stat().st_mtime, reverse=True)
    return cands[0] if cands else None


# --------------------------------------------------------------------------
# 桥 CLI 封装
# --------------------------------------------------------------------------
@dataclass
class Bridge:
    queue: Path
    wdb: list

    def _argv(self, verb, project=None, source=None, job=None, job_timeout=None,
              app_args=None, follow=False, wait_timeout=900, extra=None):
        out = [*self.wdb, verb]
        if job:
            out.append(job)
        out += ["--queue", str(self.queue)]
        if verb in ("run", "build", "build-run"):
            out += ["--source", str(source), "--project", project]
            if job_timeout is not None:
                out += ["--timeout", str(job_timeout)]
            if extra:
                out += extra
            out += ["--json"]
            if follow:
                out += ["--follow", "--wait-timeout", str(wait_timeout), "--require-worker"]
            if app_args:
                out.append("--")
                out += [str(a) for a in app_args]
        elif verb == "wait":
            out += ["--json", "--wait-timeout", str(wait_timeout)]
        elif verb == "doctor":
            out += ["--json"]
        return out

    def call(self, verb, sub_timeout=600.0, **kw):
        return run(self._argv(verb, **kw), cwd=REPO_ROOT, timeout=sub_timeout)

    def doctor(self):
        rc, out, err, _ = self.call("doctor", sub_timeout=120.0)
        return last_json(out)

    def submit_wait(self, verb, source, project, wait_timeout=1800, job_timeout=None,
                    app_args=None, extra=None):
        """提交并阻塞等待终态；返回 (rc, stdout, stderr, parsed_submit_json, elapsed)。"""
        rc, out, err, secs = self.call(
            verb, source=source, project=project, follow=True,
            wait_timeout=wait_timeout, job_timeout=job_timeout,
            app_args=app_args, extra=extra,
            sub_timeout=wait_timeout + 300.0)
        return rc, out, err, last_json(out), secs


def worker_state(queue: Path) -> dict | None:
    p = queue / "worker.json"
    if not p.is_file():
        return None
    try:
        return json.loads(p.read_text(encoding="utf-8"))
    except Exception:
        return None


def worker_healthy(state: dict | None, max_age_s: float = 20.0) -> tuple[bool, str]:
    if not state:
        return False, "worker.json 不存在"
    hb = state.get("heartbeatUtc")
    if not hb:
        return False, "worker.json 缺少 heartbeatUtc"
    try:
        t = _dt.datetime.fromisoformat(hb.replace("Z", "+00:00"))
        if t.tzinfo is None:
            t = t.replace(tzinfo=_dt.timezone.utc)
    except Exception:
        return False, f"heartbeatUtc 无法解析：{hb}"
    age = (_dt.datetime.now(_dt.timezone.utc) - t).total_seconds()
    if age > max_age_s:
        return False, f"心跳过期 {age:.1f}s > {max_age_s}s"
    return True, f"心跳 {age:.1f}s 前"


def queue_busy(queue: Path) -> tuple[bool, str]:
    """队列是否有在跑 / 排队中的任务（避免堵住其他作者）。"""
    jobs = queue / "jobs"
    if not jobs.is_dir():
        return False, "无 jobs 目录"
    active = []
    for d in jobs.iterdir():
        if not d.is_dir():
            continue
        sp = d / "status.json"
        if not sp.is_file():
            active.append((d.name, "no-status"))
            continue
        try:
            st = json.loads(sp.read_text(encoding="utf-8")).get("state")
        except Exception:
            active.append((d.name, "unreadable"))
            continue
        if st not in ("succeeded", "failed", "canceled", "timeout", "rejected", "interrupted"):
            active.append((d.name, st))
    ws = worker_state(queue)
    if ws and ws.get("currentJobId"):
        active.append((ws["currentJobId"], "worker-currentJobId"))
    if active:
        return True, "; ".join(f"{n}={s}" for n, s in active[:6])
    return False, "空闲"


def await_idle(queue: Path, seconds: float, interval: float) -> tuple[bool, str]:
    """有界等待队列空闲：低频复查（默认 20s），绝不高频轮询、绝不强插别的任务。

    返回 (busy, detail)。seconds<=0 表示只查一次。
    """
    busy, why = queue_busy(queue)
    if not busy or seconds <= 0:
        return busy, why
    log(f"队列繁忙（{why}）→ 有界等待：最多 {seconds:.0f}s，每 {interval:.0f}s 复查一次（非 busy-poll）")
    deadline = time.time() + seconds
    while busy and time.time() < deadline:
        time.sleep(interval)
        busy, why = queue_busy(queue)
    if not busy:
        log("队列已空闲，立即投任务")
    else:
        log(f"等待超时，队列仍繁忙（{why}）")
    return busy, why


@dataclass
class Job:
    job_id: str
    job_dir: Path

    def status(self) -> dict | None:
        p = self.job_dir / "status.json"
        if not p.is_file():
            return None
        try:
            return json.loads(p.read_text(encoding="utf-8"))
        except Exception:
            return None

    def stdout(self) -> str:
        p = self.job_dir / "stdout.log"
        return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""

    def stderr(self) -> str:
        p = self.job_dir / "stderr.log"
        return p.read_text(encoding="utf-8", errors="replace") if p.is_file() else ""

    def protocol(self, prefix: str):
        rec = []
        for line in self.stdout().splitlines():
            line = line.strip()
            if line.startswith(prefix):
                try:
                    rec.append(json.loads(line[len(prefix):]))
                except json.JSONDecodeError:
                    rec.append({"__unparsable__": line})
        return rec


# --------------------------------------------------------------------------
# 副本生成与静态断言
# --------------------------------------------------------------------------
def new_copy(guid: str) -> tuple[Path, list]:
    dest = WDB_DIR / (COPY_PREFIX + guid)
    checks = []
    if dest.exists():
        raise RuntimeError(f"目标副本目录已存在，拒绝复用/覆盖：{dest}")
    dest.mkdir(parents=False)  # exclusive：父目录必须已存在；dest 刚被确认不存在

    # 只复制构建真实应用所必需的最小集合（bin/obj 绝不带入）。
    entries = ["src", "Directory.Build.props", "Directory.Packages.props", "GalleriesManager.slnx", "VERSION", "global.json"]
    for e in entries:
        src = REPO_ROOT / e
        if not src.exists():
            continue
        dst = dest / e
        if src.is_dir():
            shutil.copytree(src, dst,
                            ignore=shutil.ignore_patterns("bin", "obj", "__pycache__"),
                            symlinks=False)
        else:
            shutil.copy2(src, dst)
    checks.append({"name": "copy_created", "ok": True, "detail": str(dest)})

    applied = []
    for name, rel, old, new in patch_specs(guid):
        f = dest / rel
        if not f.is_file():
            raise RuntimeError(f"补丁目标文件不存在：{rel}")
        text = f.read_text(encoding="utf-8")
        cnt = text.count(old)
        if cnt != 1:
            raise RuntimeError(f"补丁 {name} 的锚点在 {rel} 中出现 {cnt} 次（要求恰好 1 次），拒绝继续")
        f.write_text(text.replace(old, new, 1), encoding="utf-8")
        applied.append({"patch": name, "file": rel})
    checks.append({"name": "patches_applied", "ok": True, "detail": f"{len(applied)} 处"})

    # instrumentation 源文件（模板 → 副本）
    if not TEMPLATE_SELFCHECK.is_file():
        raise RuntimeError(f"缺少 instrumentation 模板：{TEMPLATE_SELFCHECK}")
    target = dest / "src/GalleriesManager.Desktop/Services/GalleryGuiSelfCheck.cs"
    shutil.copy2(TEMPLATE_SELFCHECK, target)
    applied.append({"patch": "P7b_selfcheck_file", "file": "src/GalleriesManager.Desktop/Services/GalleryGuiSelfCheck.cs"})

    manifest = {rel: sha256_file(dest / rel) for rel in PATCHED_FILES if (dest / rel).is_file()}
    state = {
        "guid": guid,
        "createdUtc": _dt.datetime.now(_dt.timezone.utc).isoformat(),
        "patches": applied,
        "patchedFileSha256": manifest,
        "sourceRepo": str(REPO_ROOT),
    }
    (dest / STATE_FILE).write_text(json.dumps(state, indent=2, ensure_ascii=False), encoding="utf-8")
    return dest, checks


def verify_copy(copy: Path) -> list:
    """对副本做 patch 静态断言 + 差异白名单 + 哈希清单。返回 checks 列表。"""
    checks = []

    def add(name, ok, detail=""):
        checks.append({"name": name, "ok": bool(ok), "detail": detail})

    def code(rel):
        return strip_cs_comments((copy / rel).read_text(encoding="utf-8"))

    def raw(rel):
        return (copy / rel).read_text(encoding="utf-8")

    state = {}
    if (copy / STATE_FILE).is_file():
        try:
            state = json.loads((copy / STATE_FILE).read_text(encoding="utf-8"))
        except Exception:
            state = {}
    guid = state.get("guid") or copy.name[len(COPY_PREFIX):]

    # ---- P1 数据根钉死，且无任何 env / 真实目录回退 ----
    ap = code("src/GalleriesManager.Core/Constants/AppPaths.cs")
    add("P1_root_pinned", f'Path.Combine(AppContext.BaseDirectory, "_gm_gui_testdata_{guid}")' in ap)
    add("P1_no_env_backdoor", "GM_TEST_ROOT" not in ap)
    add("P1_no_localappdata", "LocalApplicationData" not in ap)
    add("P1_no_real_root_literals",
        ('"galleries-manager-dev"' not in ap) and ('"galleries-manager"' not in ap))
    add("P1_path_fullpath_used", "Path.GetFullPath(" in ap)

    # ---- P2..P6 四项加固 ----
    prog = code("src/GalleriesManager.Desktop/Program.cs")
    add("P2_no_velopack_call", "VelopackApp.Build()" not in prog)

    orch = code("src/GalleriesManager.Desktop/Services/StartupOrchestrator.cs")
    add("P3_no_initialize_async", "InitializeAsync" not in orch)
    add("P3_no_load_credentials", "LoadTelegramCredentials" not in orch)
    add("P3_forced_skipped", "ConnectTelegram 已禁用" in orch)

    llm = code("src/GalleriesManager.Desktop/ViewModels/LlmSettingsViewModel.cs")
    add("P4_no_startup_hwprobe", "_ = RefreshHardwareAsync();" not in llm)
    add("P5_no_probe_call", "_llmHardware.Probe()" not in llm)
    add("P5_probe_inert", "LlmHardwareInfo.NotDetected" in llm)

    svc = code("src/GalleriesManager.Desktop/Extensions/ServiceCollectionExtensions.cs")
    add("P6_no_directory_delete", "Directory.Delete" not in svc)

    # ---- P7 instrumentation ----
    appcs = code("src/GalleriesManager.Desktop/App.axaml.cs")
    add("P7_wired", "GalleryGuiSelfCheck.Run(desktop, orchestrator)" in appcs)
    sc = code("src/GalleriesManager.Desktop/Services/GalleryGuiSelfCheck.cs")
    add("P7_prefix", "WDB_GALLERY_GUI " in sc)
    add("P7_shutdown", "desktop.Shutdown(0)" in sc)
    add("P7_win32_capture", "PrintWindow(" in sc)
    # Win32 截图正确性不变量（父代理 2026-10-12 指出的两处真实缺陷，必须防回归）：
    #  * 不可用 CreateCompatibleDC(0)+CreateCompatibleBitmap（新 DC 初选 1x1 单色位图 ⇒ 得到单色位图）；
    #  * 不可在位图仍选入 DC 时调 GetDIBits；
    #  * 必须是真 24bpp DIB，且内存 DC 的格式参考取自**自己窗口**的 DC。
    add("P7_dib_section_24bpp", "CreateDIBSection" in sc and "biBitCount = 24" in sc)
    add("P7_no_compatible_bitmap", "CreateCompatibleBitmap" not in sc)
    add("P7_no_getdibits", "GetDIBits" not in sc)
    add("P7_own_window_dc_ref", "GetDC(hwnd)" in sc and "ReleaseDC(hwnd, hdcWindow)" in sc)
    add("P7_reads_dib_memory", "Marshal.Copy(ppvBits" in sc)
    # DS review 2026-10-12：不得用恒假布尔冒充运行期判据；ok 必须反映实际捕获结果。
    add("P7_shotcaptures_removed", "shotCapturesDesktop" not in sc,
        "不得输出恒为 false 的 shotCapturesDesktop 布尔冒充运行期证据")
    add("P7_ok_reflects_result", 'info["ok"] = primaryOk' in sc, "ok 必须反映实际捕获结果")
    add("P7_scope_attestation", "shotOwnWindowOnlyEvidence" in sc, "输出只截自身窗口的方法学/范围声明")
    add("P7_avalonia_capture", "RenderTargetBitmap" in sc)
    add("P7_no_desktop_capture",
        not re.search(r"BitBlt|CopyFromScreen|GetDesktopWindow|GetDC\(0\)|GetDC\(IntPtr\.Zero\)|GetWindowDC", sc))
    add("P7_no_messagebox", "MessageBox" not in sc)
    add("P7_reports_root", "rootIsIsolated" in sc and "realLocalAppData" in sc)

    # ---- P7 四态（在线-LLM 设置 UI）自证：结构性断言，防模板被改回单态 ----
    # 语义：自证代码必须真的**逐态切换 VM 状态并各自出图/出快照**，而不是只截一张图；
    # 并且必须真的把每个状态通过 Emit 写进 stdout 协议通道（只落盘不算"WSL 侧可见"）。
    missing = [k for k, needle in THREE_PHASE_NEEDLES.items() if needle not in sc]
    add("P7_three_phase_self_proof", not missing, f"missing={missing}")
    add("P7_phase_emits_protocol", "Emit(" + "info)" in sc,
        "每个状态必须各输出一行协议行（只写盘不进协议通道会让 WSL 侧看不见三态证据）")
    # 视口裁切判据（缺陷 2「底部文案被裁半行」的机器化防回归）：
    # 量测代码必须真的产出两组可判定结论，而不是只输出一堆无关数字。
    add("P7_viewport_fit_measure_present", "viewport" + "Fit" in sc and "fits" in sc,
        "必须量测并输出在线卡-视口适配结论（fits/noVerticalScrollNeeded/bottomWithinViewport）")
    add("P7_no_extent_guess", "Extent.Height" in sc and "Viewport.Height" in sc,
        "滚动需求必须来自 ScrollViewer 的 Extent/Viewport 实测，不得靠估算")
    # 三态各自的 PNG 文件名必须带状态后缀，否则三张图会互相覆盖。
    add("P7_phase_png_suffix",
        "PngBaseName" in sc and '"-" + phase' in sc,
        '每个状态必须输出独立的 PNG 文件（PngBaseName + "-" + phase）')
    # 状态 C 的两个期望：先填手填值，再切预设 ⇒ 切完必须为空（清空联动）。
    # 注意：清空联动实现在 **LlmSettingsViewModel**（sc 是自证文件的代码），不可张冠李戴。
    add("P7_clears_endpoint_after_preset_switch",
        ("LlmOnlineEndpoint = str" + "ing.Empty") in llm and ("LlmOnlineModel = str" + "ing.Empty") in llm,
        "切预设清空手填端点/模型的联动必须存在")

    # ---- P8 依赖缓存收束 ----
    props = strip_xml_comments(raw("Directory.Build.props"))
    add("P8_restore_path_contained", "RestorePackagesPath" in props)
    add("P8_no_restore_nocache", "RestoreNoCache" not in props)

    # ---- 差异白名单（**双向**；DS review 2026-10-12 要求补齐反向）----
    AUTH_ADDED = {"src/GalleriesManager.Desktop/Services/GalleryGuiSelfCheck.cs", STATE_FILE}
    # 正向：仓库 src 的每个文件，在副本里要么逐字节相同，要么差异落在授权白名单内。
    differing, missing = [], []
    for r, _, files in os.walk(REPO_ROOT / "src"):
        if any(seg in ("bin", "obj", "__pycache__") for seg in Path(r).parts):
            continue
        for fn in files:
            rel = (Path(r).relative_to(REPO_ROOT) / fn).as_posix()
            a, b = REPO_ROOT / rel, copy / rel
            if not b.is_file():
                missing.append(rel)
            elif sha256_file(a) != sha256_file(b):
                differing.append(rel)
    add("W1_only_whitelisted_diffs", set(differing) <= set(PATCHED_FILES), f"differing={differing}")
    add("W2_no_missing_files", not missing, f"missing={missing[:10]}")

    # 反向：副本里出现的**任何**文件都必须来自仓库、或是显式授权的新增。
    # 这条断言堵住「结构上允许副本被塞进任意额外 .cs」的漏洞。
    extra_in_copy = []
    copy_file_count = 0
    for r, dirs, files in os.walk(copy):
        dirs[:] = [d for d in dirs if d not in ("bin", "obj", "__pycache__", "_gm_nuget_packages")]
        for fn in files:
            copy_file_count += 1
            rel = (Path(r).relative_to(copy) / fn).as_posix()
            if rel in AUTH_ADDED:
                continue
            if not (REPO_ROOT / rel).is_file():
                extra_in_copy.append(rel)
    add("W3_no_unauthorized_extra_files", not extra_in_copy,
        f"extra={extra_in_copy[:10]} (copy_files={copy_file_count}, authorized={len(AUTH_ADDED)})")

    # 根文件等同性：未打补丁的根文件必须逐字节等同；Directory.Build.props 只允许精确的 P8 变更。
    for rel in ("Directory.Packages.props", "GalleriesManager.slnx", "VERSION", "global.json"):
        a, b = REPO_ROOT / rel, copy / rel
        if a.is_file():
            add(f"W4_root_identical_{Path(rel).stem}", b.is_file() and sha256_file(a) == sha256_file(b), rel)
    try:
        copy_props = (copy / "Directory.Build.props").read_text(encoding="utf-8")
        repo_props = (REPO_ROOT / "Directory.Build.props").read_text(encoding="utf-8")
        add("W5_dbprops_only_authorized_patch",
            copy_props.replace(PROPS_NEW, PROPS_OLD, 1) == repo_props,
            "撤销 P8 后与仓库逐字节相同 ⇒ 差异仅此一处")
    except Exception as ex:
        add("W5_dbprops_only_authorized_patch", False, repr(ex))

    # ---- 哈希清单 ----
    manifest = {rel: sha256_file(copy / rel) for rel in PATCHED_FILES if (copy / rel).is_file()}
    add("H1_manifest_complete", len(manifest) == len(PATCHED_FILES),
        f"{len(manifest)}/{len(PATCHED_FILES)}")
    if state.get("patchedFileSha256"):
        same = state["patchedFileSha256"] == manifest
        add("H2_hash_matches_creation", same, "与创建时记录一致" if same else "哈希与创建时不一致")

    return checks, guid, manifest


# --------------------------------------------------------------------------
# 证据写出
# --------------------------------------------------------------------------
_SCRIPT_SHA = None


def script_sha() -> str:
    global _SCRIPT_SHA
    if _SCRIPT_SHA is None:
        _SCRIPT_SHA = sha256_file(Path(__file__))
    return _SCRIPT_SHA


def write_evidence(ev: dict, target: Path | None = None) -> Path:
    # 每份证据都带脚本指纹：冻结期可据此核对"证据确实由该 revision 产出"。
    ev.setdefault("script", {
        "path": "tools/windows-debug-bridge/tests/windows-gallery-gui.py",
        "sha256": script_sha(),
    })
    mode = ev.get("mode")
    path = Path(target) if target is not None else evidence_path_for(mode)
    # ★结构性防覆盖守卫：canonical run JSON 只允许 mode=run 写入。
    if path.resolve() == EVIDENCE_RUN.resolve() and mode != "run":
        raise EvidenceRoutingError(
            f"拒绝写出：mode={mode!r} 不得覆盖 canonical run JSON（{EVIDENCE_RUN}）")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(ev, indent=2, ensure_ascii=False), encoding="utf-8")
    return path


# --------------------------------------------------------------------------
# 子命令实现
# --------------------------------------------------------------------------
def cmd_static(args) -> int:
    ev = {"mode": "static", "startedUtc": _dt.datetime.now(_dt.timezone.utc).isoformat()}
    checks = []

    def add(n, ok, d=""):
        checks.append({"name": n, "ok": bool(ok), "detail": d})

    # 脚本自身审计：不得越权杀进程 / 不得停 Worker / 不得递归删除目录 / 不得绕过 GUI 运行确认。
    # 注意：断言所用的「针」必须由片段拼出，否则针的字面量会出现在被扫描的自身上，形成自指假失败。
    src = Path(__file__).read_text(encoding="utf-8")
    shut_a = '"' + "shut" + "down" + '"'
    shut_b = "'" + "shut" + "down" + "'"
    del_flag = "-" + "-" + "delete"
    mirror_tool = "rs" + "ync"
    add("S1_no_kill", not re.search(r"\bos\.kill\b|\bsignal\.SIGKILL\b", src))
    add("S2_no_wdb_shutdown", shut_a not in src and shut_b not in src,
        "脚本绝不向队列写停机标记")
    add("S3_no_recursive_delete", del_flag not in src and mirror_tool not in src,
        "脚本绝不使用递归删除类手段")
    add("S4_no_gui_run_without_confirm", "confirm" + "-gui-run" in src,
        "真实 GUI 运行必须显式确认")

    # 模板与夹具存在性
    add("F1_template_exists", TEMPLATE_SELFCHECK.is_file(), str(TEMPLATE_SELFCHECK))
    probe_csproj = REPO_ROOT / PROBE_REL_PROJECT
    add("F2_probe_exists", probe_csproj.is_file(), str(probe_csproj))
    add("F3_template_not_globbed", TEMPLATE_SELFCHECK.suffix == ".template")

    copy = resolve_copy(args.copy)
    if copy is None:
        add("C1_copy_present", False, "尚无 .wdb/guisrc-* 副本（先跑 --new-copy）")
    else:
        add("C1_copy_present", True, str(copy))
        v, guid, manifest = verify_copy(copy)
        checks.extend(v)
        ev["copyRoot"] = str(copy)
        ev["copyGuid"] = guid
        ev["patchManifest"] = manifest

    # 防覆盖 known-negative：静态载荷**必须**被结构性拒绝写入 canonical run JSON。
    try:
        write_evidence({"mode": "static", "staticChecks": []}, EVIDENCE_RUN)
        add("S5_run_json_write_guard", False, "静态载荷竟被允许写入 canonical run JSON")
    except EvidenceRoutingError:
        add("S5_run_json_write_guard", True, "静态载荷写 canonical run JSON 被结构性拒绝")
    add("S6_static_default_target_not_run_json",
        evidence_path_for("static") != EVIDENCE_RUN, str(evidence_path_for("static")))

    ev["staticChecks"] = checks
    ev["ok"] = all(c["ok"] for c in checks)
    ev["finishedUtc"] = _dt.datetime.now(_dt.timezone.utc).isoformat()
    written = write_evidence(ev)
    for c in checks:
        log(("PASS " if c["ok"] else "FAIL ") + c["name"] + ("  " + c["detail"] if c["detail"] else ""))
    log(f"static: {'OK' if ev['ok'] else 'FAILED'}  -> {written}")
    log(f"canonical run JSON 未被触碰：{EVIDENCE_RUN}")
    return 0 if ev["ok"] else 1


def cmd_new_copy(args) -> int:
    guid = args.guid or uuid.uuid4().hex
    log(f"生成新副本 guid={guid}")
    dest, checks = new_copy(guid)
    log(f"副本已创建：{dest}")
    v, g, manifest = verify_copy(dest)
    checks.extend(v)
    ok = all(c["ok"] for c in checks)
    for c in checks:
        log(("PASS " if c["ok"] else "FAIL ") + c["name"] + ("  " + c["detail"] if c["detail"] else ""))
    ev = {
        "mode": "new-copy",
        "copyRoot": str(dest),
        "copyGuid": guid,
        "patchManifest": manifest,
        "staticChecks": checks,
        "ok": ok,
        "finishedUtc": _dt.datetime.now(_dt.timezone.utc).isoformat(),
    }
    write_evidence(ev)
    log(f"new-copy: {'OK' if ok else 'FAILED'}")
    return 0 if ok else 1


def cmd_verify(args) -> int:
    copy = resolve_copy(args.copy)
    if copy is None:
        log("未找到副本；先执行 --new-copy")
        return 2
    checks, guid, manifest = verify_copy(copy)
    ok = all(c["ok"] for c in checks)
    for c in checks:
        log(("PASS " if c["ok"] else "FAIL ") + c["name"] + ("  " + c["detail"] if c["detail"] else ""))
    ev = {
        "mode": "verify",
        "copyRoot": str(copy),
        "copyGuid": guid,
        "patchManifest": manifest,
        "staticChecks": checks,
        "ok": ok,
        "finishedUtc": _dt.datetime.now(_dt.timezone.utc).isoformat(),
    }
    write_evidence(ev)
    log(f"verify: {'OK' if ok else 'FAILED'}")
    return 0 if ok else 1


def cmd_preflight(args) -> int:
    queue = Path(args.queue)
    wdb = resolve_wdb()
    if wdb is None:
        log("无法定位 wdb CLI（既无 tools/windows-debug-bridge/wdb 也无 Release wdb.dll）")
        return 2
    rc, out, err, _ = run([*wdb, "doctor", "--queue", str(queue), "--json"], timeout=120.0)
    doc = last_json(out)
    st = worker_state(queue)
    healthy, why = worker_healthy(st)
    busy, busywhy = queue_busy(queue)
    ev = {
        "mode": "preflight",
        "queue": str(queue),
        "doctor": doc,
        "doctorRc": rc,
        "worker": st,
        "workerHealthy": healthy,
        "workerHealthDetail": why,
        "queueBusy": busy,
        "queueBusyDetail": busywhy,
        "ok": bool(doc) and healthy and not busy,
        "finishedUtc": _dt.datetime.now(_dt.timezone.utc).isoformat(),
    }
    write_evidence(ev)
    log(f"doctor rc={rc}")
    log(f"worker healthy={healthy} ({why})")
    log(f"queue busy={busy} ({busywhy})")
    log(f"preflight: {'OK' if ev['ok'] else 'BLOCKED'}")
    return 0 if ev["ok"] else 3


def cmd_build(args) -> int:
    queue = Path(args.queue)
    copy = resolve_copy(args.copy)
    if copy is None:
        log("未找到副本；先执行 --new-copy")
        return 2
    checks, guid, manifest = verify_copy(copy)
    if not all(c["ok"] for c in checks):
        log("副本静态断言未全通过 → fail closed，拒绝构建")
        return 3
    busy, why = await_idle(queue, args.await_idle_seconds, args.idle_poll_interval)
    if busy and not args.allow_busy_queue:
        log(f"队列仍繁忙（{why}）→ 不投任务（避免堵住其他作者）。可稍后再试或显式 --allow-busy-queue")
        return 3

    wdb = resolve_wdb()
    br = Bridge(queue=queue, wdb=wdb)
    log(f"提交 wdb build（source={copy}）…")
    rc, out, err, submit, secs = br.submit_wait(
        "build", source=copy, project=DESKTOP_REL_PROJECT,
        wait_timeout=args.wait_timeout, job_timeout=None,
        extra=["--exclude", "_gm_nuget_packages"],
    )
    ev = {
        "mode": "build",
        "copyRoot": str(copy),
        "copyGuid": guid,
        "rc": rc,
        "elapsedSec": round(secs, 1),
        "submit": submit,
        "stdoutTail": out[-4000:],
        "stderrTail": err[-4000:],
        "finishedUtc": _dt.datetime.now(_dt.timezone.utc).isoformat(),
    }
    jid = (submit or {}).get("id") or (submit or {}).get("jobId")
    if jid:
        j = Job(jid, queue / "jobs" / jid)
        ev["jobId"] = jid
        ev["status"] = j.status()
        ev["stdoutTail"] = j.stdout()[-4000:]
        ev["stderrTail"] = j.stderr()[-4000:]
    ok = rc == 0 and (ev.get("status") or {}).get("state") == "succeeded"
    ev["ok"] = ok
    write_evidence(ev)
    log(f"build: {'OK' if ok else 'FAILED'} (rc={rc}, {secs:.1f}s, job={jid})")
    return 0 if ok else 1


def _probe_baseline(br: Bridge, label: str, wait_timeout: float, allow_busy: bool):
    """提交一次 GalleryGuiProbe --baseline 任务并解析其清单。"""
    log(f"提交 baseline 探针（{label}）…")
    rc, out, err, submit, secs = br.submit_wait(
        "run", source=REPO_ROOT, project=PROBE_REL_PROJECT,
        wait_timeout=wait_timeout, job_timeout=None, app_args=["--baseline"],
    )
    jid = (submit or {}).get("id") or (submit or {}).get("jobId")
    rec = {"label": label, "rc": rc, "elapsedSec": round(secs, 1), "jobId": jid}
    if not jid:
        rec["error"] = "未取得 job id"
        rec["stdoutTail"] = out[-2000:]
        rec["ok"] = False
        return rec
    j = Job(jid, br.queue / "jobs" / jid)
    st = j.status() or {}
    rec["state"] = st.get("state")
    prot = j.protocol(PROBE_PREFIX)
    rec["protocolCount"] = len(prot)
    if prot:
        p = prot[-1]
        rec["probe"] = {k: v for k, v in p.items() if k != "roots"}
        rec["roots"] = [
            {k: v for k, v in r.items() if k != "entries"}
            for r in (p.get("roots") or [])
        ]
        rec["failClosed"] = bool(p.get("failClosed"))
    rec["ok"] = (rec.get("state") == "succeeded") and bool(prot) and not rec.get("failClosed")
    return rec


def cmd_run(args) -> int:
    queue = Path(args.queue)
    ev = {"mode": "run", "startedUtc": _dt.datetime.now(_dt.timezone.utc).isoformat()}
    copy = resolve_copy(args.copy)
    if copy is None:
        log("未找到副本；先执行 --new-copy")
        return 2

    # ---------- 阶段 0：副本静态断言（fail closed 前哨） ----------
    checks, guid, manifest = verify_copy(copy)
    ev["copyRoot"] = str(copy)
    ev["copyGuid"] = guid
    ev["patchManifest"] = manifest
    ev["staticChecks"] = checks
    if not all(c["ok"] for c in checks):
        ev["verdict"] = "FAILED"
        ev["reason"] = "副本静态断言未全通过 → fail closed"
        ev["finishedUtc"] = _dt.datetime.now(_dt.timezone.utc).isoformat()
        write_evidence(ev)
        log("副本静态断言未全通过 → fail closed，不进入任何队列操作")
        return 3
    log("副本静态断言全通过")

    wdb = resolve_wdb()
    if wdb is None:
        ev["verdict"] = "FAILED"; ev["reason"] = "无法定位 wdb CLI"
        write_evidence(ev); return 2
    br = Bridge(queue=queue, wdb=wdb)

    # ---------- 阶段 1：Worker / 队列体检 ----------
    st = worker_state(queue)
    healthy, why = worker_healthy(st)
    busy, busywhy = queue_busy(queue)
    ev["worker"] = st
    ev["workerHealthy"] = healthy
    ev["queueBusy"] = busy
    if not healthy:
        ev["verdict"] = "BLOCKED"; ev["reason"] = f"Worker 不健康：{why}"
        write_evidence(ev); log(ev["reason"]); return 3
    if busy and not args.allow_busy_queue:
        busy, busywhy = await_idle(queue, args.await_idle_seconds, args.idle_poll_interval)
    if busy and not args.allow_busy_queue:
        ev["verdict"] = "BLOCKED"; ev["reason"] = f"队列繁忙：{busywhy}"
        write_evidence(ev); log(ev["reason"] + "（有界等待后仍繁忙，未强插队）"); return 3

    # ---------- 阶段 2：真实 GUI 运行需要显式确认（结构性 fail closed） ----------
    if not args.confirm_gui_run:
        ev["verdict"] = "READY_NOT_RUN"
        ev["reason"] = "已就绪但未获 --confirm-gui-run；未运行任何 GUI"
        ev["finishedUtc"] = _dt.datetime.now(_dt.timezone.utc).isoformat()
        write_evidence(ev)
        log("=== READY（未运行 GUI）===")
        log("副本已生成且静态断言全通过；Worker 健康、队列空闲。")
        log("追加 --confirm-gui-run 才会真正执行隔离 GUI 运行。")
        return 3

    # ---------- 阶段 3：baseline（前） ----------
    before = _probe_baseline(br, "before", args.wait_timeout, args.allow_busy_queue)
    ev["baselineBefore"] = before
    if not before.get("ok"):
        ev["verdict"] = "FAILED"
        ev["reason"] = "运行前 baseline 探针失败（权限/IO 读不到真实目录）→ fail closed，拒绝运行 GUI"
        ev["finishedUtc"] = _dt.datetime.now(_dt.timezone.utc).isoformat()
        write_evidence(ev); log(ev["reason"]); return 3
    log("baseline(前) 完成")

    # ---------- 阶段 4：真实 GUI（build-run 隔离副本） ----------
    log("提交真实应用 build-run（隔离副本）…")
    rc, out, err, submit, secs = br.submit_wait(
        "build-run", source=copy, project=DESKTOP_REL_PROJECT,
        wait_timeout=args.wait_timeout, job_timeout=0,
        extra=["--exclude", "_gm_nuget_packages"],
    )
    gui = {"rc": rc, "elapsedSec": round(secs, 1), "submit": submit}
    jid = (submit or {}).get("id") or (submit or {}).get("jobId")
    ev["guiJob"] = gui
    prot = []
    cap = None
    if not jid:
        # 极罕见：CLI 未回传 job id。**绝不在此早退** —— after-baseline 仍必须执行
        # （父 2026-10-12 硬性要求：GUI 失败也要拿到运行后的真实目录清单）。
        gui["jobId"] = None
        gui["state"] = None
        gui["exitCode"] = None
        gui["stdoutTail"] = out[-3000:]
        gui["stderrTail"] = err[-3000:]
        ev["guiSubmitError"] = "GUI 任务未取得 job id（仍继续执行 after-baseline）"
        log("警告：GUI 任务未取得 job id —— 仍将执行 after-baseline 与判据")
    else:
        j = Job(jid, queue / "jobs" / jid)
        stt = j.status() or {}
        gui["jobId"] = jid
        gui["state"] = stt.get("state")
        gui["exitCode"] = stt.get("exitCode")
        gui["artifact"] = stt.get("artifact")
        gui["pid"] = stt.get("pid")
        gui["stdoutTail"] = j.stdout()[-3000:]
        gui["stderrTail"] = j.stderr()[-3000:]
        prot = j.protocol(GUI_PREFIX)
        gui["protocolCount"] = len(prot)
        # 2026-10-12：自证协议改为**三态各出一行 stage=captured**（A 本地默认 / B 在线 / C 切预设）。
        # 兼容写法：canonical "cap"（驱动既有单态判据与截图复核逻辑）取 A 态，三态整体另存 guiPhases。
        caps = sorted((p for p in prot if p.get("stage") == "captured"),
                      key=lambda p: p.get("phaseIndex", 0))
        cap = caps[0] if caps else None
        ev["guiProtocol"] = cap
        ev["guiPhases"] = caps
        if len(caps) != 4:
            ev["guiPhaseCaptureProblem"] = f"期望 4 个 stage=captured 协议行，实得 {len(caps)}"
        log(f"GUI 任务终态={gui['state']} exit={gui['exitCode']} 协议行={len(prot)} 四态捕获={len(caps)}")

    # ---------- 阶段 5：截图独立复核（Windows 侧 probe） ----------
    # 2026-10-12 四态：每个状态的 PNG 各自提交一次 --verify-shot（Windows 侧纯 BCL 解码像素），
    # 「非空白 = 真的被绘制过」逐态成立，而不是只证明其中一张。
    def _verify_shot_for(app_cap):
        """对单个状态截图提交 --verify-shot 探针，返回复核记录（含 Windows 侧实算哈希）。"""
        if not app_cap:
            return None
        win_bmp = app_cap.get("shotBmpPath")
        win_png = app_cap.get("shotPngPath")
        if not (win_bmp or win_png):
            return None
        rec = {"rc": None, "ok": False}
        args_shot = ["--verify-shot"]
        if win_bmp:
            args_shot += ["--bmp", win_bmp]
            if app_cap.get("shotBmpWidth") and app_cap.get("shotBmpHeight"):
                args_shot += ["--expect-w", str(app_cap["shotBmpWidth"]), "--expect-h", str(app_cap["shotBmpHeight"])]
        if win_png:
            args_shot += ["--png", win_png]
            if app_cap.get("shotPngWidth") and app_cap.get("shotPngHeight"):
                args_shot += ["--png-expect-w", str(app_cap["shotPngWidth"]), "--png-expect-h", str(app_cap["shotPngHeight"])]
        rc2, out2, err2, submit2, secs2 = br.submit_wait(
            "run", source=REPO_ROOT, project=PROBE_REL_PROJECT,
            wait_timeout=args.wait_timeout, job_timeout=None, app_args=args_shot,
        )
        jid2 = (submit2 or {}).get("id") or (submit2 or {}).get("jobId")
        rec = {"rc": rc2, "jobId": jid2, "elapsedSec": round(secs2, 1), "ok": False}
        if jid2:
            j2 = Job(jid2, queue / "jobs" / jid2)
            rec["state"] = (j2.status() or {}).get("state")
            pr = j2.protocol(PROBE_PREFIX)
            rec["probe"] = pr[-1] if pr else None
            rec["ok"] = rec["state"] == "succeeded" and bool(pr) and bool((pr[-1] or {}).get("ok"))
        else:
            rec["stderrTail"] = err2[-1500:]
        # WSL 侧也各读一次文件，核对存在性与 sha256（与 probe 的哈希三方交叉验证）
        for tag, wp in (("Bmp", win_to_wsl(win_bmp) if win_bmp else None),
                        ("Png", win_to_wsl(win_png) if win_png else None)):
            if wp and wp.is_file():
                rec[f"wsl{tag}Path"] = str(wp)
                rec[f"wsl{tag}Bytes"] = wp.stat().st_size
                rec[f"wsl{tag}Sha256"] = sha256_file(wp)
        return rec

    shot = None
    phase_meta = []
    if cap and (cap.get("shotBmpPath") or cap.get("shotPngPath")):
        log("提交截图复核探针（--verify-shot，四态各自 BMP+PNG 双通道逐通道尺寸）…")
        shot = _verify_shot_for(cap)     # 兼容既有单态字段：取状态 A 的复核记录
        for pc in caps:
            meta = {"phase": pc.get("phase"), "phaseIndex": pc.get("phaseIndex")}
            ver = _verify_shot_for(pc)
            if ver is not None:
                meta["probe"] = ver.get("probe")
                meta["probeOk"] = bool(ver.get("ok"))
                meta["wslPngPath"] = ver.get("wslPngPath")
                meta["wslPngBytes"] = ver.get("wslPngBytes")
                meta["wslPngSha256"] = ver.get("wslPngSha256")
            meta["appPngPath"] = pc.get("shotPngPath")
            meta["appPngSha256"] = pc.get("shotPngSha256")
            meta["appPngBytes"] = pc.get("shotPngBytes")
            meta["llmUiState"] = pc.get("llmUiState")
            meta["visualTreeProbe"] = pc.get("visualTreeProbe")
            meta["hwndInt"] = pc.get("hwndInt")
            meta["isWindowVisible"] = pc.get("isWindowVisible")
            meta["appOk"] = pc.get("ok")
            phase_meta.append(meta)
    ev["shotVerify"] = shot
    ev["phaseShotVerify"] = phase_meta

    # 把「本应用自己窗口」的三态 PNG 复制进工作区，作为可直接 read_image 的用户交付物
    # （免去读 /mnt/c 的权限依赖）。**不是**桌面截图。
    if cap and cap.get("shotPngPath"):
        artifacts = []
        for pc in caps:
            src_png = win_to_wsl(pc.get("shotPngPath")) if pc.get("shotPngPath") else None
            if not (src_png and src_png.is_file()):
                continue
            try:
                ARTIFACT_DIR.mkdir(parents=True, exist_ok=True)
                dest = ARTIFACT_DIR / f"{gui.get('jobId') or 'unknown'}-phase{pc.get('phase') or 'X'}.png"
                shutil.copy2(src_png, dest)
                artifacts.append({
                    "path": str(dest),
                    "phase": pc.get("phase"),
                    "sha256": sha256_file(dest),
                    "bytes": dest.stat().st_size,
                    "jobId": gui.get("jobId"),
                    "note": "本应用自己窗口的 Avalonia RenderTargetBitmap PNG（own visual tree；非桌面截图）",
                })
                log(f"own-window PNG 交付物（状态 {pc.get('phase')}）-> {dest}")
            except Exception as ex:
                ev["shotArtifactError"] = repr(ex)
        if artifacts:
            ev["shotArtifacts"] = artifacts
            ev["shotArtifact"] = artifacts[0]   # 兼容既有单态字段（取状态 A）

    # ---------- 阶段 6：baseline（后）——**无论 GUI 成败都必须执行** ----------
    log("提交 baseline 探针（after；无论 GUI 成败都执行）…")
    after = _probe_baseline(br, "after", args.wait_timeout, args.allow_busy_queue)
    ev["baselineAfter"] = after
    if not after.get("ok"):
        log("警告：after-baseline 探针未成功 —— 隔离判据将 fail closed（不采信「无变化」）")

    # ---------- 阶段 7：硬判据 ----------
    hc = {}

    def root_of(rec, label):
        for r in (rec.get("roots") or []):
            if r.get("label") == label:
                return r
        return None

    def same_root(a, b):
        ra, rb = root_of(a, "prod"), root_of(b, "prod")
        ra2, rb2 = root_of(a, "dev"), root_of(b, "dev")
        return (ra is not None and rb is not None and ra.get("manifestSha256") == rb.get("manifestSha256")
                and ra.get("fileCount") == rb.get("fileCount") and ra.get("totalBytes") == rb.get("totalBytes")
                and ra2 is not None and rb2 is not None
                and ra2.get("manifestSha256") == rb2.get("manifestSha256")
                and ra2.get("fileCount") == rb2.get("fileCount") and ra2.get("totalBytes") == rb2.get("totalBytes"))

    hc["real_dirs_unchanged"] = bool(before.get("ok") and after.get("ok") and same_root(before, after))
    hc["baseline_not_failclosed"] = (not before.get("failClosed")) and (not after.get("failClosed"))
    hc["gui_job_succeeded"] = gui.get("state") == "succeeded"
    hc["gui_app_exit_zero"] = gui.get("exitCode") == 0
    hc["gui_protocol_present"] = bool(cap)
    hc["root_isolated"] = bool(cap and cap.get("rootIsIsolated") is True)

    # ---- 精确定位核对（父代理 2026-10-12 要求：不再只信应用自报的 bool/标记）----
    # 1) root 必须**逐字符**等于 join(baseDirectory, "_gm_gui_testdata_<本次副本 GUID>")（Windows 路径规范化后比较）
    base_dir = (cap or {}).get("baseDirectory") or ""
    expected_root = ntpath.join(base_dir, "_gm_gui_testdata_" + guid) if base_dir else ""
    actual_root = (cap or {}).get("root") or ""
    hc["root_exact_path"] = bool(expected_root and actual_root
                                 and ntpath.normcase(actual_root) == ntpath.normcase(expected_root))
    # 2) baseDirectory 必须落在本次任务的 status.artifact 所在目录之下（即真的是本次 runs 副本内）
    art = gui.get("artifact") or ""
    art_dir = ntpath.dirname(art) if art else ""
    hc["base_dir_under_artifact_dir"] = bool(base_dir and art_dir and (
        ntpath.normcase(base_dir) == ntpath.normcase(art_dir)
        or ntpath.normcase(base_dir).startswith(ntpath.normcase(art_dir) + "\\")))
    # 3) 应用自报 PID 必须等于桥 status 里的 PID
    hc["pid_matches_status"] = bool(cap and gui.get("pid") is not None
                                    and cap.get("pid") == gui.get("pid"))
    # 4) 截图哈希**逐通道**三方交叉一致：应用内自算 == Windows 探针实算 == WSL 侧实算
    probe_obj = (shot or {}).get("probe") or {}
    sha_pairs = {}
    for tag in ("Bmp", "Png"):
        app_sha = (cap or {}).get(f"shot{tag}Sha256")
        win_sha = (probe_obj.get(tag.lower()) or {}).get("sha256")
        wsl_sha = (shot or {}).get(f"wsl{tag}Sha256")
        if app_sha:   # 该通道存在才判定
            hc[f"shot_sha_{tag.lower()}_win_probe_matches_app"] = bool(win_sha and app_sha == win_sha)
            hc[f"shot_sha_{tag.lower()}_wsl_matches_app"] = bool(wsl_sha and app_sha == wsl_sha)
        sha_pairs[tag] = {"app": app_sha, "winProbe": win_sha, "wsl": wsl_sha}

    ev["crossChecks"] = {
        "copyGuid": guid,
        "baseDirectory": base_dir,
        "expectedRoot": expected_root,
        "actualRoot": actual_root,
        "statusArtifact": art,
        "statusArtifactDir": art_dir,
        "statusPid": gui.get("pid"),
        "appPid": (cap or {}).get("pid"),
        "shotSha256PerChannel": sha_pairs,
    }

    hc["hwnd_present"] = bool(cap and cap.get("hwndInt"))
    hc["window_visible"] = bool(cap and cap.get("isWindowVisible") is True)
    hc["window_size_positive"] = bool(cap and (cap.get("clientRect") or {}).get("w", 0) > 0
                                      and (cap.get("clientRect") or {}).get("h", 0) > 0)
    hc["shot_ok"] = bool(cap and cap.get("ok") is True and cap.get("shotChannelsOk", 0) >= 1)
    # 「只截自身窗口」是**静态代码 + 捕获路径方法学**结论，**不是**运行期布尔（DS review 2026-10-12 F3）。
    # 依据：--verify 的静态禁止项（无 BitBlt/GetDC(0)/GetDesktopWindow/CopyFromScreen、DC 参考取自自身 HWND、
    # 真 24bpp DIB、无 GetDIBits、无恒假 shotCapturesDesktop）+ 应用上报的方法学范围声明 + 捕获方法在允许集合内。
    static_ok = {c["name"]: c["ok"] for c in checks}
    own_window_static = all(static_ok.get(n) for n in (
        "P7_no_desktop_capture", "P7_own_window_dc_ref", "P7_dib_section_24bpp",
        "P7_no_compatible_bitmap", "P7_no_getdibits", "P7_shotcaptures_removed"))
    scope_ev = ((cap or {}).get("shotOwnWindowOnlyEvidence") or {})
    hc["shot_own_window_only_static_proof"] = bool(
        own_window_static
        and scope_ev.get("scope") == "static-code + capture-path-attestation"
        and scope_ev.get("screenApisUsed") is False
        and (cap or {}).get("shotMethod") in ("avalonia-rendertargetbitmap", "win32-printwindow-hwnd"))
    ev["shotOwnWindowOnlyScope"] = "static-code + capture-path-attestation (NOT a runtime probe; see staticChecks P7_*)"
    # 第 22 条：PNG 通道必须存在且被 Windows 侧独立解码证明非空白 —— 这是「真的被绘制过」的权威判据。
    png_obj = (probe_obj.get("png") or {})
    hc["shot_png_nonblank_required"] = bool(
        (cap or {}).get("shotPngPath")
        and png_obj.get("pixelDecode") == "ok"
        and png_obj.get("nonBlank") is True)
    hc["shot_verified_on_windows"] = bool(shot and shot.get("ok"))

    # ---- 四态（在线-LLM 设置 UI）硬判据 ----
    # 语义：四个状态各自的 **VM 状态快照 + 可视树旁证 + Windows 侧像素解码**必须与预期一致。
    # 这些判据只看"UI 状态与画面"，与是否真的发起过在线请求无关（本验收从不发起在线请求）。
    phase_by_name = {m.get("phase"): m for m in phase_meta}
    hc["ui_four_phase_captured"] = (len(caps) == 4
                                    and {p.get("phase") for p in caps} == {"A", "B", "C", "D"})

    def _st(name):
        return (phase_by_name.get(name) or {}).get("llmUiState") or {}

    def _vt(name):
        return (phase_by_name.get(name) or {}).get("visualTreeProbe") or {}

    st_a, st_b, st_c, st_d = _st("A"), _st("B"), _st("C"), _st("D")

    # 状态 A：本地默认态——后端 Cpu、非在线模式、本地挡位卡在、后端选择器在自动模式下隐藏。
    hc["ui_state_A_local_default"] = bool(
        st_a and st_a.get("backend") == "Cpu" and st_a.get("isOnlineMode") is False
        and st_a.get("showLocalTierPanel") is True and st_a.get("showLocalAdvancedPanel") is False
        and st_a.get("showBackendSelector") is False)
    # 状态 B：在线模式态——**按显示名选中在线项**后后端变 Online 且下拉回显一致（证明下拉里真有该项且绑定通）,
    # 本地挡位/高级/下载块隐藏、后端选择器常驻。
    hc["ui_state_B_online_mode"] = bool(
        st_b and st_b.get("backend") == "Online" and st_b.get("isOnlineMode") is True
        and st_b.get("showLocalTierPanel") is False and st_b.get("showLocalAdvancedPanel") is False
        and st_b.get("showBackendSelector") is True)
    hc["ui_state_B_backend_item_present"] = bool(
        st_b and any("在线 API" in (lbl or "") for lbl in (st_b.get("backendLabels") or []))
        and "在线 API" in (st_b.get("backendLabel") or ""))
    # 在线预设下拉确有 6 家预设可选（llm-models.json onlinePresets）。
    hc["ui_state_B_preset_labels_present"] = bool(
        st_b and len(st_b.get("onlinePresetLabels") or []) >= 2
        and (st_b.get("onlinePresetLabel") or "") != "")
    # 状态 C：切预设联动态——两输入框被清空，且预设确实换了、提示文案已按新预设刷新。
    hc["ui_state_C_preset_switch_clears"] = bool(
        st_c and st_c.get("backend") == "Online" and st_c.get("isOnlineMode") is True
        and st_c.get("onlineEndpoint") == "" and st_c.get("onlineModel") == "")
    hc["ui_state_C_preset_changed"] = bool(
        st_c and (st_c.get("onlinePresetLabel") or "") != ""
        and (st_c.get("onlinePresetLabel") or "") != (st_b.get("onlinePresetLabel") or ""))
    # 状态 C 的提示文案必须已按新预设刷新（回落目标可见）。
    hc["ui_state_C_hint_refreshed"] = bool(
        st_c and (st_c.get("onlineEndpointHint") or "") != ""
        and (st_c.get("onlineModelHint") or "") != "")

    # ---- 水印跟随预设（缺陷 1 防回归）：水印必须动态反映当前预设，而不是写死的例子 ----
    hc["ui_watermark_follows_preset_B"] = bool(
        st_b and "api.deepseek.com/v1" in (st_b.get("onlineEndpointWatermark") or "")
        and "deepseek-chat" in (st_b.get("onlineModelWatermark") or ""))
    hc["ui_watermark_follows_preset_C"] = bool(
        st_c and "dashscope.aliyuncs.com/compatible-mode/v1" in (st_c.get("onlineEndpointWatermark") or "")
        and "qwen-plus" in (st_c.get("onlineModelWatermark") or ""))
    # 可视树里输入框的 PlaceholderText 也必须同步（证明绑定真的生效，而不只是 VM 属性变了）。
    _wm_boxes_c = (_vt("C").get("textBoxesByPlaceholder") or {})
    hc["ui_watermark_bound_in_view"] = bool(
        any(("dashscope.aliyuncs.com" in k) for k in _wm_boxes_c)
        and any(("qwen-plus" in k) for k in _wm_boxes_c))

    # ---- 【钉死判据】ui_online_card_fits_viewport ----
    # 语义：状态 B 与 C 下，在线卡最底部可见控件（模型输入框下方的提示行）的有效边界必须落在视口内，
    # 且该态不需要垂直滚动（ExtentHeight <= ViewportHeight）。这正是「底部文案被裁半行」的机器化防回归。
    def _vf(name):
        return (_vt(name).get("viewportFit") or {})

    def _fits(name):
        vf = _vf(name)
        return bool(vf and vf.get("fits") is True
                    and vf.get("noVerticalScrollNeeded") is True
                    and vf.get("bottomWithinViewport") is True
                    and vf.get("bottomLineVisible") is True
                    and (vf.get("bottomMostControlText") or "") != ""
                    and (vf.get("lowestVisibleText") or "") != "")

    hc["ui_online_card_fits_viewport"] = bool(_fits("B") and _fits("C"))
    # 状态 D：切回本地——在线模式退出、本地挡位卡恢复、在线配置卡消失（在线↔本地可互切）。
    hc["ui_state_D_back_to_local"] = bool(
        st_d and st_d.get("isOnlineMode") is False and st_d.get("showLocalTierPanel") is True
        and st_d.get("showBackendSelector") is False
        and "在线 API" not in (st_d.get("backendLabel") or ""))

    # 四态截图：文件存在、VM 快照可用、Windows 侧像素解码 ok + 非空白（逐态），
    # 且应用自算哈希 == Windows 探针实算哈希 == WSL 侧实算哈希（防陈旧/被替换的证据）。
    for _ph in ("A", "B", "C", "D"):
        _m = phase_by_name.get(_ph) or {}
        _png = ((_m.get("probe") or {}).get("png") or {})
        hc[f"ui_png_{_ph}_nonblank_on_windows"] = bool(
            _m.get("wslPngPath") and _png.get("pixelDecode") == "ok" and _png.get("nonBlank") is True)
        hc[f"ui_png_{_ph}_sha_matches_app"] = bool(
            _m.get("appPngSha256") and _png.get("sha256") == _m.get("appPngSha256")
            and _m.get("wslPngSha256") == _m.get("appPngSha256"))
        hc[f"ui_state_{_ph}_snapshot_present"] = bool(_m.get("llmUiState") and _m.get("wslPngPath"))

    # 可视树旁证：设置页确实被渲染出来（不是只改了 VM），且三态都取到了该页面。
    hc["ui_settings_view_rendered"] = bool(_vt("A").get("settingsViewPresent") is True)
    # LLM 子页（下标 4）确实被选中，即"LLM 面板真的在屏幕上"。
    hc["ui_llm_tab_selected"] = bool(_vt("A").get("settingsTabIndex") == 4)
    # 在线模式下，端点/模型两个输入框在可视树里可见，且确实是空的（与快照互证）。
    _boxes_c = (_vt("C").get("textBoxesByPlaceholder") or {})
    hc["ui_visual_online_textboxes_visible"] = bool(
        any(("留空即用预设端点" in k) and (v.get("isEffectivelyVisible") or 0) >= 1
            for k, v in _boxes_c.items())
        and any(("留空即用预设首选模型" in k) and (v.get("isEffectivelyVisible") or 0) >= 1
                for k, v in _boxes_c.items()))
    hc["ui_visual_online_textboxes_empty"] = bool(
        _boxes_c
        and any(("留空即用预设端点" in k) and (v.get("isEffectivelyVisible") or 0) >= 1 for k, v in _boxes_c.items())
        and any(("留空即用预设首选模型" in k) and (v.get("isEffectivelyVisible") or 0) >= 1 for k, v in _boxes_c.items())
        and all(t == "" for k, v in _boxes_c.items() if "留空即用预设" in k for t in (v.get("text") or [])))
    # 密钥框必须是 PasswordBox 语义（PasswordChar 已显式设过，而非 TextBox 默认的空字符）。
    hc["ui_visual_apikey_is_passwordbox"] = bool(
        _vt("B") and any(("sk-..." in k) and (v.get("isPasswordBox") is True)
                         for k, v in ((_vt("B").get("textBoxesByPlaceholder")) or {}).items()))
    steps = {s.get("key"): s.get("status") for s in (cap or {}).get("startupSteps", [])} if cap else {}
    hc["connect_telegram_skipped"] = steps.get("ConnectTelegram") == "Skipped"
    hc["initdb_completed"] = steps.get("InitDb") == "Completed"

    ev["limitations"] = [
        "PrintWindow(BMP 通道) 对 Avalonia 的 Skia/ANGLE **GPU 合成窗口**实测得到单一色（全黑；"
        "PW_RENDERFULLCONTENT 与 PW_CLIENTONLY 两个旗标都试过）——这是 Windows 图形栈的已知特例，"
        "**不是隔离失败**，故不作非空白判据。",
        "「非空白 = 真的被绘制过」的权威通道是 **PNG**（Avalonia RenderTargetBitmap）。它只渲染**本应用"
        "自己的可视树**，因此**不证明**桌面/屏幕上被 DWM 合成后的像素；本验收**不主张整屏像素级证据**，"
        "也绝不做桌面抓屏。",
        "「只截自身窗口」是**静态代码 + 捕获路径方法学**结论"
        "（scope=static-code + capture-path-attestation），不是运行期布尔。",
        "真实目录清单的内容哈希上限为 8 MiB（hashLimitBytes=8388608）；超过上限的文件**只比对 size+mtime**"
        "（各根 skippedHashCount 见 baselineBefore.baselineAfter）。baseline 摘要 manifestSha256 覆盖**全部条目**"
        "（含超大文件的 size+mtime 行）。因此结论严格表述为「**元数据逐项一致 + 限额内内容哈希一致 + Root "
        "静态隔离支持未污染**」，**不宣称** 4.88 GB 库的每一个字节都被哈希比对过。",
        "本文件由 --run 独占写出；--static/--verify/--new-copy/--preflight/--build 各自写独立 JSON，"
        "结构上不可能覆盖本文件（见 write_evidence 的 EvidenceRoutingError 守卫）。",
        "【四态 UI 验收的交互方式】Avalonia 无法被外部程序化注入鼠标事件（本治具不具备 OS 级点击注入能力），"
        "因此四个状态由**应用内部**的 instrumentation 在 UI 线程上直接改 ViewModel 公开属性（等价于用户操作同一"
        "绑定点）实现：A=仅程序化导航到设置页 LLM 子页（不改任何设置）；B=把「运行后端」下拉**按显示名选中**在线项；"
        "C=先写入手填端点/模型再切换服务商预设；D=把「运行后端」按显示名切回本地项。因此本验收主张的是"
        "「绑定-可见性-清空联动在真实渲染下成立」，**不主张**「鼠标点击路径被验证过」。",
        "【零网络】四态全程只改 UI/VM 状态：不点任何下载按钮、不发起任何在线请求、不跑整理任务；"
        "LLM 总闸默认关闭（快照中的 llmEnabled 为 false），Telegram 由 P3 无条件 Skipped。",
    ]

    ev["hardCriteria"] = hc
    ev["hardCriteriaCount"] = len(hc)
    ev["verdict"] = "PASSED" if all(hc.values()) else "FAILED"
    ev["finishedUtc"] = _dt.datetime.now(_dt.timezone.utc).isoformat()
    written = write_evidence(ev)

    for k, v in hc.items():
        log(("PASS " if v else "FAIL ") + "hard." + k)
    log(f"verdict={ev['verdict']}  ({len(hc)} 条硬判据)  -> {written}")
    return 0 if ev["verdict"] == "PASSED" else 1


# --------------------------------------------------------------------------
def main() -> int:
    ap = argparse.ArgumentParser(add_help=True, description="真实 Avalonia 画廊应用的隔离副本 GUI 验收")
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--static", action="store_true", help="纯静态自检（不投队列、不运行 GUI）")
    g.add_argument("--new-copy", action="store_true", help="生成新副本并施加全部补丁")
    g.add_argument("--verify", action="store_true", help="对副本做静态断言")
    g.add_argument("--preflight", action="store_true", help="只读体检 doctor/Worker/队列")
    g.add_argument("--build", action="store_true", help="只提交一次 wdb build")
    g.add_argument("--run", action="store_true", help="完整验收（需 --confirm-gui-run 才真跑 GUI）")

    ap.add_argument("--copy", default=None, help="指定副本目录（默认取最新 .wdb/guisrc-*）")
    ap.add_argument("--guid", default=None, help="--new-copy 使用的 GUID（默认随机）")
    ap.add_argument("--queue", default=str(DEFAULT_QUEUE), help="队列根")
    ap.add_argument("--wait-timeout", type=int, default=1800, help="单任务等待上限（秒）")
    ap.add_argument("--allow-busy-queue", action="store_true", help="允许在队列繁忙时投任务（默认拒绝）")
    ap.add_argument("--await-idle-seconds", type=float, default=0.0,
                    help="队列繁忙时最多等这么久（秒）再投；0=不等待（默认）。低频复查，非 busy-poll")
    ap.add_argument("--idle-poll-interval", type=float, default=20.0,
                    help="上述等待的复查间隔（秒，默认 20）")
    ap.add_argument("--confirm-gui-run", action="store_true",
                    help="显式确认可运行真实 GUI（缺省则只报到 READY，绝不运行）")
    args = ap.parse_args()

    mode = ("static" if args.static else "new-copy" if args.new_copy else "verify" if args.verify
            else "preflight" if args.preflight else "build" if args.build else "run")
    open_log(log_path_for(mode))
    try:
        if args.static:
            return cmd_static(args)
        if args.new_copy:
            return cmd_new_copy(args)
        if args.verify:
            return cmd_verify(args)
        if args.preflight:
            return cmd_preflight(args)
        if args.build:
            return cmd_build(args)
        if args.run:
            return cmd_run(args)
        return 2
    finally:
        if _log_fh is not None:
            _log_fh.close()


if __name__ == "__main__":
    sys.exit(main())
