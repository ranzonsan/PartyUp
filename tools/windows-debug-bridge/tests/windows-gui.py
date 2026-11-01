#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""windows-gui.py —— Windows 调试桥 · GUI 窗口验收（Win32 fixture 路径）

与 tests/windows-smoke.py 的分工（两者互不替代）：
  * windows-smoke.py   —— 桥的**控制面**：提交/构建/运行/参数保真/取消杀树（跑 EchoApp、ProcessProbe）；
  * windows-gui.py     —— 桥的**显示面**：证明 `wdb build-run` 经 Windows 前台 Worker 拉起的进程
                          真的在用户交互桌面上显示了一个**可见窗口**，且客户区被**真实绘制**过。

本脚本只跑自家夹具 tools/windows-debug-bridge/tests/fixtures/GuiApp（纯 BCL net10.0 WinExe，
零 NuGet、零业务依赖、不碰任何用户数据）；**不代表真实 Avalonia 画廊应用已验收**——
该结论只证明「桥能把一个 GUI 进程拉到交互桌面上并显示可见窗口」，见 notTested。

两种模式：
  1) 静态自检（不需要 Worker，也不碰队列）：
         python3 tools/windows-debug-bridge/tests/windows-gui.py --static
     做：夹具源码硬约束扫描（禁 MessageBox / 禁抢焦点 / 仅允许 5 个系统 DLL / 零 NuGet /
     零业务字符串）、csproj 与局部 Directory.Build.props 属性、Linux 侧 `dotnet build` 0 错 0 警、
     PE 子系统 = WINDOWS_GUI(2)（WinExe 的机器可读证据）、脚本自身审计（不得出现 os.kill / signal /
     wdb shutdown）。**此模式绝不声称 GUI 已在 Windows 上通过。**

  2) 实机验收（需要用户在 Windows 前台拉起 Worker）：
         python3 tools/windows-debug-bridge/tests/windows-gui.py [--with-stop]
     - 预检 doctor/worker.json（必须 healthy，且 runsRoot 是 Windows 本地盘符路径）；
     - 用现有 CLI 提交：`wdb build-run --project <GuiApp.csproj> --require-worker --follow --json
       -- --duration-ms 12000`，**边跑边**采 status.json/stdout.log（证明窗口存在时任务确为 running）；
     - 解析夹具输出的 `WDB_GUI {json}`（stdout.log 主通道；必要时回退到 runs 目录里的
       wdb-gui-evidence.jsonl），逐项核对：真实 OS / sessionId 与控制台会话一致 / WinSta0\\Default /
       HWND / IsWindowVisible / 窗口与客户区矩形 / 监视器 / DWM cloaked / WM_PAINT 次数 /
       像素级自证（printClient 权威通道 + screen 桌面回读）/ 未抢焦点 / 自动超时退出；
     - Artifact 与 PID 一致性：status.pid == 夹具 pid == pidfile 内容；artifact 落在 runsRoot 下、
       在 /mnt/c 上真实存在；exePath 为本地盘符（非 UNC）；
     - BMP 独立复核：解析夹具自写的 24bpp BMP（自写头），复核宽高、marker 像素与 SHA256；
       **唯一被持久化的帧是 PrintWindow(PW_CLIENTONLY) 由夹具自己 WM_PRINTCLIENT 画出的客户区**，
       screen 通道只有 GetPixel 单点；脚本额外断言 runs 目录里**不存在**任何整帧/整屏截图文件；
     - 队列纪律：全程只对自己的任务投 cancel，绝不 cancel 别人的任务（差集断言）。
     - `--with-stop` 追加一条：长时长任务 → `wdb stop <自己的 id>` → 终态 canceled 且夹具**没有** exit 事件，
       再由 **Windows 侧 ProcessProbe** 用 pidfile 复核该 PID 确实已消失（WSL 侧不得用 os.kill 判断 Windows PID）。

输出：机器可读证据写 .wdb/windows-gui-validation.json（另出同名 .log 人读日志）。

退出码：0 = 全通过；1 = 有失败；2 = 用法/环境错误；3 = 阻塞（无可用 Worker 等，且非 --static）。
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import random
import re
import shutil
import struct
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path, PureWindowsPath

# --------------------------------------------------------------------------
# 路径与常量
# --------------------------------------------------------------------------
REPO_ROOT = Path(__file__).resolve().parents[3]
BRIDGE_DIR = REPO_ROOT / "tools" / "windows-debug-bridge"
TESTS_DIR = BRIDGE_DIR / "tests"
FIXTURE_GUI = TESTS_DIR / "fixtures" / "GuiApp"
FIXTURE_PROBE = TESTS_DIR / "fixtures" / "ProcessProbe"

GUI_PROJECT = "tools/windows-debug-bridge/tests/fixtures/GuiApp/GuiApp.csproj"
PROBE_PROJECT = "tools/windows-debug-bridge/tests/fixtures/ProcessProbe/ProcessProbe.csproj"

DEFAULT_QUEUE = REPO_ROOT / ".wdb" / "queue"
DEFAULT_EVIDENCE = REPO_ROOT / ".wdb" / "windows-gui-validation.json"
DEFAULT_LOG = REPO_ROOT / ".wdb" / "windows-gui-validation.log"
WORK_ROOT = REPO_ROOT / ".wdb" / "windows-gui"

GUI_PREFIX = "WDB_GUI "
PROBE_PREFIX = "WDB_PROBE "

CLI_OK = 0
CLI_FAILED = 5
CLI_CANCELED = 6
TERMINAL_STATES = {"succeeded", "failed", "canceled", "timeout", "rejected", "interrupted"}

# 夹具允许的 P/Invoke 目标 DLL 白名单（只碰系统 UI/GDI/内核/DWM/NT 查询）
ALLOWED_DLLS = {"user32.dll", "gdi32.dll", "kernel32.dll", "dwmapi.dll", "ntdll.dll"}

# 夹具源码禁止出现的**代码**模式（先剥注释再匹配）
FORBIDDEN_CODE = [
    (r"\bMessageBox[A-Za-z]*\s*\(", "禁止 MessageBox 阻塞等待用户点击（必须自动收尾）"),
    (r"\bSetForegroundWindow\s*\(", "禁止抢用户焦点"),
    (r"\bSetActiveWindow\s*\(", "禁止抢用户焦点"),
    (r"\bBringWindowToTop\s*\(", "禁止抢用户焦点"),
    (r"\bSystem\.Drawing\b", "禁止非 BCL 绘图库"),
    (r"\bSystem\.Windows\.Forms\b", "禁止 WinForms"),
    (r"\bMicrosoft\.Win32\b", "禁止注册表等 Win32 托管封装"),
    (r"\bos\.kill\b", "WSL 侧不得用 os.kill 判断 Windows PID"),
    (r"\bGalleriesManager\b", "夹具不得依赖真实业务代码/命名空间"),
    (r"\bTdlib\b", "夹具不得触碰业务依赖"),
    (r"\bSqlite\b|\bSQLite\b", "夹具不得触碰业务数据库"),
]

# 夹具必须出现的关键机制（代码层）
REQUIRED_CODE = [
    "WS_EX_NOACTIVATE",
    "SW_SHOWNOACTIVATE",
    "PrintWindow",
    "WM_PAINT",
    "WM_PRINTCLIENT",
    "GetPixel",
    "PwClientOnly",
    "Environment.ProcessId",
]

NOT_TESTED = [
    "真实 Avalonia 画廊应用（src/GalleriesManager.Desktop）的窗口渲染：本次只跑 Win32 夹具 GuiApp，"
    "**不得**据此宣称真实 Gallery GUI 已验收",
    "业务数据/用户配置的隔离性：夹具本身不读写业务数据，但桥的「非沙箱」性质未变，真实应用仍以登录用户权限运行",
    "用户肉眼确认（本脚本只有机器可读证据；窗口在用户工作区右下角短暂显示 12s，不抢焦点）",
    "抓屏/OCR 级验证：只做自己窗口客户区的像素回读（printClient + BitBlt），不做桌面级截图比对",
    "多显示器/DPI 缩放的显示正确性（记录 monitorCount/dpi，但未在 150%/200% 缩放下专项验证）",
    "非 \\\\wsl.localhost UNC 的队列根（Windows 本地盘 / SMB）",
    "KILL_ON_JOB_CLOSE 级联杀树与强杀 Worker（需强杀用户 Worker，本脚本绝不这么做）",
    "Worker 自身处于 Job Object 中的嵌套场景",
    "Worker 硬超时终态 timeout",
    "真实桌面会话切换/锁屏（Win+L）状态下的窗口可见性",
]


# --------------------------------------------------------------------------
# 日志 / 子进程
# --------------------------------------------------------------------------
_LOG_FH = None


def log(message: str = "") -> None:
    print(message, flush=True)
    if _LOG_FH is not None:
        _LOG_FH.write(message + "\n")
        _LOG_FH.flush()


def open_log(path: Path) -> None:
    global _LOG_FH
    path.parent.mkdir(parents=True, exist_ok=True)
    _LOG_FH = path.open("w", encoding="utf-8")


@dataclass
class Run:
    code: int
    out: str
    err: str
    argv: list[str]
    seconds: float = 0.0


def run(argv: list[str], cwd: Path | None = None, timeout: float = 600.0) -> Run:
    started = time.time()
    try:
        proc = subprocess.run(argv, cwd=str(cwd or REPO_ROOT), capture_output=True, text=True,
                              timeout=timeout, errors="replace")
        return Run(proc.returncode, proc.stdout or "", proc.stderr or "", argv, time.time() - started)
    except subprocess.TimeoutExpired as exc:
        return Run(124, (exc.stdout or "") if isinstance(exc.stdout, str) else "",
                   (exc.stderr or "") if isinstance(exc.stderr, str) else "", argv, time.time() - started)


# --------------------------------------------------------------------------
# 路径映射（Windows <-> WSL）
# --------------------------------------------------------------------------
def win_to_wsl(win_path: str) -> Path | None:
    m = re.match(r"^([A-Za-z]):[\\/](.*)$", win_path or "")
    if not m:
        return None
    return Path("/mnt") / m.group(1).lower() / m.group(2).replace("\\", "/")


def wsl_to_win(path: Path) -> str | None:
    parts = path.resolve().parts
    if len(parts) >= 3 and parts[0] == "/" and parts[1] == "mnt" and len(parts[2]) == 1:
        rest = "\\".join(parts[3:])
        return f"{parts[2].upper()}:\\{rest}"
    return None


def is_unc(p: str) -> bool:
    return bool(p) and p.startswith("\\\\")


def is_local_drive(p: str) -> bool:
    return bool(re.match(r"^[A-Za-z]:[\\/]", p or ""))


# --------------------------------------------------------------------------
# 桥 CLI 调用
# --------------------------------------------------------------------------
class Bridge:
    def __init__(self, repo: Path, queue: Path, source: Path, wdb: list[str],
                 wait_timeout: int = 1800, job_timeout: int = 300):
        self.repo = repo
        self.queue = queue
        self.source = source
        self.wdb = wdb
        # wait_timeout 覆盖「排队 + 执行」的总时长（队列是串行的，别的作者可能正在跑长任务）；
        # job_timeout 只覆盖本任务自身的执行（构建 + 运行）时长。
        self.wait_timeout = wait_timeout
        self.job_timeout = job_timeout

    def _argv(self, verb: str, project: str | None = None, job: str | None = None,
              timeout: int | None = None, reason: str | None = None,
              app_args: list[str] | None = None, follow: bool = False) -> list[str]:
        out = [*self.wdb, verb]
        if job:
            out.append(job)
        out += ["--queue", str(self.queue)]
        if verb in ("run", "build", "build-run"):
            out += ["--source", str(self.source), "--project", project or ""]
            out += ["--timeout", str(timeout if timeout is not None else self.job_timeout)]
            out += ["--json"]
            if follow:
                out += ["--follow", "--wait-timeout", str(self.wait_timeout), "--require-worker"]
            if app_args:
                out.append("--")
                out += [str(a) for a in app_args]
        elif verb == "wait":
            out += ["--json", "--wait-timeout", str(self.wait_timeout)]
        elif verb == "stop":
            if reason:
                out += ["--reason", reason]
        elif verb == "doctor":
            out += ["--json"]
        return out

    def call(self, verb: str, timeout: float = 600.0, **kwargs) -> Run:
        return run(self._argv(verb, **kwargs), cwd=self.repo, timeout=timeout)

    def queue_snapshot(self) -> dict:
        """当前队列状态：Worker 正在跑谁、有哪些未终态任务（用于「提交时队列空闲」取证与礼貌排队）。"""
        state = {}
        try:
            state = json.loads((self.queue / "worker.json").read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            state = {}
        pending = []
        for jid in all_jobs(self.queue):
            st = job_of(self.queue, jid).status()
            if st is None or st.get("state") not in TERMINAL_STATES:
                pending.append({"id": jid, "state": (st or {}).get("state")})
        return {"currentJobId": state.get("currentJobId"), "pending": pending,
                "heartbeatUtc": state.get("heartbeatUtc")}

    def wait_for_idle(self, timeout: float) -> tuple[bool, dict, float]:
        """等队列空闲（不抢在别人任务前面插队、也不取消别人）。返回 (是否空闲, 快照, 等待秒数)。"""
        started = time.time()
        snap = self.queue_snapshot()
        while time.time() - started < timeout:
            snap = self.queue_snapshot()
            if not snap["currentJobId"] and not snap["pending"]:
                return True, snap, round(time.time() - started, 3)
            time.sleep(1.0)
        return False, self.queue_snapshot(), round(time.time() - started, 3)

    def doctor(self) -> dict | None:
        return last_json(self.call("doctor", timeout=120.0).out)

    def wait(self, job: str) -> Run:
        return self.call("wait", job=job, timeout=self.wait_timeout + 60.0)

    def stop(self, job: str, reason: str) -> Run:
        return self.call("stop", job=job, reason=reason, timeout=120.0)


def last_json(text: str) -> dict | None:
    found = None
    for line in (text or "").splitlines():
        line = line.strip()
        if line.startswith("{"):
            try:
                found = json.loads(line)
            except json.JSONDecodeError:
                continue
    return found


def all_json(text: str) -> list[dict]:
    out = []
    for line in (text or "").splitlines():
        line = line.strip()
        if line.startswith("{"):
            try:
                out.append(json.loads(line))
            except json.JSONDecodeError:
                continue
    return out


def resolve_wdb(repo: Path) -> list[str] | None:
    """优先用仓库自带启动器（会自动增量构建），退回直接 dotnet exec 已构建的 wdb.dll。"""
    launcher = BRIDGE_DIR / "wdb"
    if launcher.is_file() and os.access(launcher, os.X_OK):
        return [str(launcher)]
    dll = BRIDGE_DIR / "bin" / "Release" / "net10.0" / "wdb.dll"
    if dll.is_file():
        return ["dotnet", str(dll)]
    return None


# --------------------------------------------------------------------------
# 队列读取
# --------------------------------------------------------------------------
@dataclass
class Job:
    job_id: str
    job_dir: Path

    @property
    def stdout_path(self) -> Path:
        return self.job_dir / "stdout.log"

    @property
    def stderr_path(self) -> Path:
        return self.job_dir / "stderr.log"

    @property
    def status_path(self) -> Path:
        return self.job_dir / "status.json"

    def read_text(self, path: Path) -> str:
        try:
            return path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            return ""

    def stdout_text(self) -> str:
        return self.read_text(self.stdout_path)

    def stderr_text(self) -> str:
        return self.read_text(self.stderr_path)

    def status(self) -> dict | None:
        try:
            return json.loads(self.read_text(self.status_path))
        except (OSError, json.JSONDecodeError):
            return None

    def events(self, prefix: str = GUI_PREFIX) -> list[dict]:
        out = []
        for line in self.stdout_text().splitlines():
            line = line.strip()
            if line.startswith(prefix):
                try:
                    out.append(json.loads(line[len(prefix):]))
                except json.JSONDecodeError:
                    continue
        return out

    def last_event(self, name: str, prefix: str = GUI_PREFIX) -> dict | None:
        found = None
        for ev in self.events(prefix):
            if ev.get("event") == name:
                found = ev
        return found


def job_of(queue: Path, job_id: str) -> Job:
    return Job(job_id, queue / "jobs" / job_id)


def all_jobs(queue: Path) -> list[str]:
    jobs_dir = queue / "jobs"
    if not jobs_dir.is_dir():
        return []
    return sorted(p.name for p in jobs_dir.iterdir() if p.is_dir())


def cancel_map(queue: Path) -> dict[str, bool]:
    return {jid: (queue / "jobs" / jid / "cancel").exists() for jid in all_jobs(queue)}


def poll_until(predicate, timeout: float, interval: float = 0.4):
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        last = predicate()
        if last:
            return last
        time.sleep(interval)
    return None


# --------------------------------------------------------------------------
# 判据函数（纯函数；实机判据与 --static 的 known-dict 自检共用同一实现）
#
# 为什么单独抽出来：**nil 分支不许放行**。`真值 in (False, None)` 这类写法会把
# 「查询不到 / 字段缺失」当成通过 —— 那是 fail open：dwmapi 不可用时窗口可能真被 cloaked，
# 却仍判 PASS。判据必须 fail closed：只有拿到确凿的期望取值才算通过。
# --------------------------------------------------------------------------
def judge_dwm_not_cloaked(value) -> bool:
    """DWM 未 cloaked 的判据：**只接受布尔 False**。True（确实被 cloaked）、None（查询不可用 /
    字段缺失）一律不得 PASS —— 后者应显式失败，由人复核，而不是悄悄算通过。"""
    return value is False


def judge_ready_target_identity_while_running(observations, terminal_pid) -> bool:
    """「running 期间窗口就是被测目标」的判据。

    必须存在**至少一次**观察同时满足：
      * jobState == "running"（任务确在运行期）；
      * 观察里的夹具自报 pid（ready 事件携带的 `pid`，即目标进程身份）**等于终态 PID**
        （status.json 的 pid）；
      * 拿到非空、非 0 的 HWND，且 isWindowVisible is True。
    **刻意不比较 status 的「阶段 pid」**：build-run 在 running 阶段的 status.pid 是构建宿主
    （dotnet/msbuild，实测 9296），不是被拉起的 GUI（实测 3380）；把它当身份既会假绿
    （只凭构建 pid 就通过），也会在被修成强制相等时假红。阶段 pid 只作 metadata 记录。
    """
    if not isinstance(terminal_pid, int):
        return False
    for obs in observations or []:
        if obs.get("jobState") != "running":
            continue
        if obs.get("pid") != terminal_pid:
            continue
        hwnd = obs.get("hwnd")
        if not isinstance(hwnd, str) or hwnd in ("", "0x0"):
            continue
        if obs.get("isWindowVisible") is not True:
            continue
        return True
    return False


def judge_thread_no_active_window(value) -> bool:
    """线程无活动窗口的判据（不抢焦点证据之一）：**只接受字符串 "0x0"**（GetActiveWindow 返回 NULL）。
    字段缺失（None）不得 PASS；非 0 的 HWND 说明本线程有活动窗口，属反证须 FAIL。"""
    return value == "0x0"


# --------------------------------------------------------------------------
# 结果记录
# --------------------------------------------------------------------------
@dataclass
class Case:
    name: str
    ok: bool
    doc: str = ""
    detail: str = ""
    evidence: dict = field(default_factory=dict)
    skipped: bool = False

    def to_json(self) -> dict:
        return {"name": self.name, "ok": self.ok, "skipped": self.skipped, "doc": self.doc,
                "detail": self.detail, "evidence": self.evidence}


class Recorder:
    def __init__(self) -> None:
        self.cases: list[Case] = []

    def add(self, name: str, ok: bool, doc: str, detail: str = "", **evidence) -> bool:
        case = Case(name, bool(ok), doc, detail, evidence)
        self.cases.append(case)
        log(f"  [{'PASS' if ok else 'FAIL'}] {name} — {detail or doc}")
        return bool(ok)

    def skip(self, name: str, doc: str, detail: str = "") -> None:
        self.cases.append(Case(name, True, doc, detail, {}, skipped=True))
        log(f"  [SKIP] {name} — {detail}")

    def totals(self) -> dict:
        return {
            "cases": len(self.cases),
            "passed": sum(1 for c in self.cases if c.ok and not c.skipped),
            "failed": sum(1 for c in self.cases if not c.ok),
            "skipped": sum(1 for c in self.cases if c.skipped),
        }


# --------------------------------------------------------------------------
# 源码静态约束
# --------------------------------------------------------------------------
def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    return text


def sha256_file(path: Path) -> str | None:
    try:
        return hashlib.sha256(path.read_bytes()).hexdigest()
    except OSError:
        return None


def read_pe_subsystem(dll: Path) -> int | None:
    """读 PE 可选头 Subsystem 字段：2 = WINDOWS_GUI（WinExe 的机器可读证据），3 = CONSOLE。"""
    try:
        data = dll.read_bytes()
    except OSError:
        return None
    if len(data) < 0x40 or data[:2] != b"MZ":
        return None
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if pe + 0x40 > len(data) or data[pe:pe + 4] != b"PE\0\0":
        return None
    return struct.unpack_from("<H", data, pe + 24 + 68)[0]


def parse_bmp24(path: Path) -> dict | None:
    """解析夹具自写的 BMP（期望 24bpp BI_RGB 底朝下），返回 {width,height,pixel_at,...}。"""
    try:
        data = path.read_bytes()
    except OSError:
        return None
    if len(data) < 54 or data[0:2] != b"BM":
        return None
    size, _res, off = struct.unpack_from("<III", data, 2)
    hdr = struct.unpack_from("<IiiHHIIiiII", data, 14)
    width, height, planes, bpp, comp = hdr[1], hdr[2], hdr[3], hdr[4], hdr[5]
    if comp != 0 or bpp not in (24, 32):
        return {"error": f"unsupported bmp comp={comp} bpp={bpp}"}
    row = ((width * bpp // 8) + 3) & ~3
    def pixel(x: int, y: int) -> tuple[int, int, int]:
        # 文件里底朝下：第 y 行（顶朝下坐标）落在 (height-1-y)
        base = off + (height - 1 - y) * row + x * (bpp // 8)
        b, g, r = data[base], data[base + 1], data[base + 2]
        return r, g, b
    return {
        "width": width, "height": height, "bpp": bpp, "fileBytes": len(data),
        "declaredSize": size, "pixelAt": pixel,
        "pixel": pixel(13, 13),                     # marker 取样点 (probePointClient)
        "distinctSample": len({pixel(x, y) for x in range(0, width, 7) for y in range(0, height, 7)}),
        "sha256": hashlib.sha256(data).hexdigest(),
    }


def write_png_rgb24(path: Path, width: int, height: int, rows: list[bytes]) -> None:
    """把 RGB24 行数据写成 PNG（zlib + 自写 chunk，零第三方库）。"""
    import struct as _struct
    import zlib as _zlib

    def chunk(tag: bytes, data: bytes) -> bytes:
        return (_struct.pack(">I", len(data)) + tag + data
                + _struct.pack(">I", _zlib.crc32(tag + data) & 0xFFFFFFFF))

    raw = bytearray()
    for row in rows:
        raw.append(0)          # filter type 0 (None)
        raw += row
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", _struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", _zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(png)


def convert_bmp_to_png(bmp: Path, out_dir: Path, job_id: str) -> Path | None:
    """把夹具自写的 24bpp BMP（底朝下）转成 PNG，方便人眼复核「自己窗口长什么样」。"""
    try:
        data = bmp.read_bytes()
    except OSError:
        return None
    if len(data) < 54 or data[0:2] != b"BM":
        return None
    _size, _res, off = struct.unpack_from("<III", data, 2)
    hdr = struct.unpack_from("<IiiHHIIiiII", data, 14)
    width, height, bpp = hdr[1], hdr[2], hdr[4]
    if bpp != 24 or width <= 0 or height <= 0:
        return None
    row = ((width * 3) + 3) & ~3
    rows: list[bytes] = []
    for y in range(height):                      # PNG 顶朝下；BMP 底朝下 → 倒序读
        base = off + (height - 1 - y) * row
        line = bytearray()
        for x in range(width):
            b, g, r = data[base + x * 3], data[base + x * 3 + 1], data[base + x * 3 + 2]
            line += bytes((r, g, b))
        rows.append(bytes(line))
    out = out_dir / f"gui-printclient-{job_id}.png"
    try:
        write_png_rgb24(out, width, height, rows)
        return out
    except OSError:
        return None


# --------------------------------------------------------------------------
# 静态模式
# --------------------------------------------------------------------------
def static_checks(rec: Recorder, do_build: bool) -> dict:
    log("== 静态自检（Linux 侧；不声明 GUI 已通过） ==")
    csproj = FIXTURE_GUI / "GuiApp.csproj"
    program = FIXTURE_GUI / "Program.cs"
    local_props = FIXTURE_GUI / "Directory.Build.props"

    for label, p in (("GuiApp.csproj", csproj), ("Program.cs", program), ("Directory.Build.props", local_props)):
        rec.add(f"static.fixture_file.{label}", p.is_file(), "夹具文件存在", str(p))

    csproj_text = csproj.read_text(encoding="utf-8") if csproj.is_file() else ""
    props_text = local_props.read_text(encoding="utf-8") if local_props.is_file() else ""
    program_raw = program.read_text(encoding="utf-8") if program.is_file() else ""
    program_code = strip_comments(program_raw)

    rec.add("static.csproj.winexe", "<OutputType>WinExe</OutputType>" in csproj_text,
            "OutputType 必须是 WinExe（窗口子系统，无控制台）")
    rec.add("static.csproj.net10", "<TargetFramework>net10.0</TargetFramework>" in csproj_text,
            "TargetFramework 必须是 net10.0（纯 BCL）")
    rec.add("static.csproj.cpm_off",
            "<ManagePackageVersionsCentrally>false</ManagePackageVersionsCentrally>" in csproj_text
            and "<CentralPackageTransitivePinningEnabled>false</CentralPackageTransitivePinningEnabled>" in csproj_text,
            "项目体必须把中央包管理两个开关压回 false（Directory.Packages.props 独立于 Directory.Build.props 被导入）")
    rec.add("static.csproj.no_nuget",
            "<PackageReference" not in csproj_text and "<ProjectReference" not in csproj_text
            and "<Import " not in csproj_text,
            "零 NuGet / 零项目引用 / 零额外 Import（离线可还原）")
    rec.add("static.props.blocks_parent",
            local_props.is_file() and "<Import" not in props_text and "<TargetFramework>" in props_text,
            "局部 Directory.Build.props 必须存在且不 Import 父级（MSBuild 在第一个命中处停止，从而阻断 repo 祖父级 props）")

    hits = [(pat, why) for pat, why in FORBIDDEN_CODE if re.search(pat, program_code)]
    rec.add("static.program.forbidden", not hits, "夹具代码不得出现禁用模式",
            "无禁用模式" if not hits else f"命中：{hits}")
    rec.add("static.program.no_messagebox", "MessageBox" not in program_code,
            "绝不弹 MessageBox 阻塞等待用户点击（必须自动收尾）")

    missing = [s for s in REQUIRED_CODE if s not in program_code]
    rec.add("static.program.required", not missing, "夹具必须实现的机制齐全",
            "齐全" if not missing else f"缺失：{missing}")

    dlls = set(re.findall(r'DllImport\("([^"]+)"', program_code))
    rec.add("static.program.dll_whitelist", dlls <= ALLOWED_DLLS,
            "只允许 P/Invoke user32/gdi32/kernel32/dwmapi/ntdll",
            f"实际：{sorted(dlls)}")

    rec.add("static.program.no_userdata",
            not re.search(r"AppData|LOCALAPPDATA|Roaming|\.db\b|Documents|Desktop\\", program_code),
            "夹具代码不得读写用户业务/个人数据路径",
            "未发现业务路径访问")

    rec.add("static.program.auto_exit", "--duration-ms" in program_code and "PostQuitMessage" in program_code,
            "必须以定时器自动收尾（PostQuitMessage），不得依赖用户交互")
    rec.add("static.program.pidfile", "wdb-gui-" in program_code and ".pid" in program_code,
            "必须落 pidfile，供 Windows 侧 ProcessProbe 事后复核 PID（WSL 禁用 os.kill）")
    # 只允许 PrintWindow(PW_CLIENTONLY) 画自己的客户区 + GetPixel 读单点；任何整帧拷贝都不允许。
    no_bitblt = "BitBlt(" not in program_code
    getpixel_only = "GetPixel(windowDc," in program_code
    rec.add("static.program.capture_scope",
            no_bitblt and getpixel_only and "PwClientOnly = 0x00000001" in program_code,
            "只允许 PrintWindow(PW_CLIENTONLY) 画自己客户区 + GetPixel 单点；代码里不得有任何 BitBlt/整帧拷贝",
            f"noBitBltCalls={no_bitblt} getPixelSinglePoint={getpixel_only} pwClientOnly={('PwClientOnly = 0x00000001' in program_code)}")
    screen_bmps = re.findall(r'wdb-gui-(?!printclient)[a-z0-9-]*-\{Environment\.ProcessId\}\.bmp', program_code)
    rec.add("static.program.single_frame_artifact", not screen_bmps,
            "夹具只允许落一个帧文件（printclient）；不得生成 screen/整屏 BMP",
            f"otherBmpWriters={screen_bmps}")

    src_code = strip_comments(Path(__file__).read_text(encoding="utf-8"))
    kill_calls = re.findall(r"\bos\.kill\s*\(|\bos\.killpg\s*\(|\bsignal\.\w+\s*\(", src_code)
    rec.add("static.script.no_os_kill", not kill_calls,
            "本脚本不得调用 os.kill / signal / killpg 判断 Windows PID（Windows PID 只能由 ProcessProbe 判）",
            f"killCalls={kill_calls}")
    verbs = set(re.findall(r'(?:_argv|call|wait)\("([a-z-]+)"', src_code))
    rec.add("static.script.no_shutdown", "shutdown" not in verbs,
            "本脚本绝不向 Worker 投 shutdown（不得停用户的主 Worker）",
            f"CLI verbs used={sorted(verbs)}")
    stop_args = set(re.findall(r"\bstop\(\s*([A-Za-z_]\w*)\s*,", src_code))
    rec.add("static.script.own_jobs_only",
            bool(stop_args) and stop_args <= {"self", "job_id", "jid"},
            "只能对自己的任务投 cancel（每个 stop 调用点的首参必须是 self/job_id，不得来自外部输入）",
            f"stopFirstArgs={sorted(stop_args)}")

    # ── 判据自身的反证（known-dict）：收紧后的判据必须拒绝 True/None，只接受确凿取值 ──
    known_cases = [
        ("dwm_cloaked_false_passes", judge_dwm_not_cloaked(False), True),
        ("dwm_cloaked_true_fails", judge_dwm_not_cloaked(True), False),
        ("dwm_cloaked_none_fails", judge_dwm_not_cloaked(None), False),
        ("dwm_cloaked_missing_key_fails", judge_dwm_not_cloaked({}.get("dwmCloaked")), False),
        ("active_window_0x0_passes", judge_thread_no_active_window("0x0"), True),
        ("active_window_missing_fails", judge_thread_no_active_window(None), False),
        ("active_window_real_hwnd_fails", judge_thread_no_active_window("0x1A0A2C"), False),
        # running 期间的目标身份（反例核心：只凭 build 阶段 pid 不得通过）
        ("ready_identity_ok", judge_ready_target_identity_while_running(
            [{"jobState": "running", "jobPid": 9296, "pid": 3380, "hwnd": "0x33809A6", "isWindowVisible": True}], 3380), True),
        ("ready_identity_build_pid_only_fails", judge_ready_target_identity_while_running(
            [{"jobState": "running", "jobPid": 9296, "pid": 9296, "hwnd": None, "isWindowVisible": None}], 3380), False),
        ("ready_identity_pid_mismatch_fails", judge_ready_target_identity_while_running(
            [{"jobState": "running", "jobPid": 9296, "pid": 9296, "hwnd": "0x1A0A2C", "isWindowVisible": True}], 3380), False),
        ("ready_identity_not_running_fails", judge_ready_target_identity_while_running(
            [{"jobState": "queued", "jobPid": None, "pid": 3380, "hwnd": "0x33809A6", "isWindowVisible": True}], 3380), False),
        ("ready_identity_hidden_window_fails", judge_ready_target_identity_while_running(
            [{"jobState": "running", "jobPid": 9296, "pid": 3380, "hwnd": "0x33809A6", "isWindowVisible": False}], 3380), False),
        ("ready_identity_no_hwnd_fails", judge_ready_target_identity_while_running(
            [{"jobState": "running", "jobPid": 9296, "pid": 3380, "hwnd": None, "isWindowVisible": True}], 3380), False),
        ("ready_identity_empty_observations_fails", judge_ready_target_identity_while_running([], 3380), False),
        ("ready_identity_no_terminal_pid_fails", judge_ready_target_identity_while_running(
            [{"jobState": "running", "jobPid": 9296, "pid": 3380, "hwnd": "0x33809A6", "isWindowVisible": True}], None), False),
    ]
    for case_name, actual, expect in known_cases:
        rec.add(f"static.judge.{case_name}", actual is expect,
                "判据函数 known-dict 自检（nil 分支必须 fail closed，不得放行）",
                f"judge({case_name}) → {actual}（期望 {expect}）")

    # 冻结的 Windows 证据回放：用**当前**判据复核当时记录的取值，证明收紧判据不会让既有 PASSED 作废
    frozen = Path(DEFAULT_EVIDENCE)
    if frozen.is_file():
        try:
            frozen_json = json.loads(frozen.read_text(encoding="utf-8"))
            fwin = ((frozen_json.get("evidence") or {}).get("ready") or {}).get("window") or {}
            ffocus = ((frozen_json.get("evidence") or {}).get("ready") or {}).get("focus") or {}
            replay_cloak = judge_dwm_not_cloaked(fwin.get("dwmCloaked"))
            replay_focus = judge_thread_no_active_window(ffocus.get("activeWindowOfThread"))
            fmain = ((frozen_json.get("evidence") or {}).get("main") or {})
            fwatch = fmain.get("watch") or {}
            freplay_identity = judge_ready_target_identity_while_running(
                fwatch.get("observations"), (fmain.get("status") or {}).get("pid"))
            rec.add("static.judge.replay_frozen_windows_evidence",
                    replay_cloak and replay_focus and freplay_identity
                    and frozen_json.get("windowsVerified") is True,
                    "用当前判据回放冻结的 Windows 证据（不改证据、不重跑）：收紧后的 cloaked/焦点/running 目标身份三门都应仍然成立",
                    f"frozen={frozen.name} windowsVerified={frozen_json.get('windowsVerified')} "
                    f"dwmCloaked={fwin.get('dwmCloaked')!r}→{replay_cloak} "
                    f"activeWindow={ffocus.get('activeWindowOfThread')!r}→{replay_focus} "
                    f"targetIdentity={freplay_identity}(readyPid={(fwatch.get('observations') or [{}])[0].get('pid')} "
                    f"stagePid={(fwatch.get('observations') or [{}])[0].get('jobPid')} terminalPid={(fmain.get('status') or {}).get('pid')})")
        except (OSError, json.JSONDecodeError) as exc:
            rec.add("static.judge.replay_frozen_windows_evidence", False,
                    "用当前判据回放冻结的 Windows 证据", f"读取失败：{type(exc).__name__}: {exc}")
    else:
        rec.skip("static.judge.replay_frozen_windows_evidence",
                 "冻结证据回放（未找到 .wdb/windows-gui-validation.json）")

    evidence: dict = {"fixtureSha256": {
        "GuiApp.csproj": sha256_file(csproj),
        "Program.cs": sha256_file(program),
        "Directory.Build.props": sha256_file(local_props),
    }}

    if not do_build:
        rec.skip("static.build.linux", "Linux 侧 dotnet build（--no-build 跳过）")
        return evidence

    log("  · Linux 侧编译夹具（仅证明能在 Linux 上编译，不代表 Windows GUI 已通过）")
    dotnet = shutil.which("dotnet")
    if not dotnet:
        rec.add("static.build.linux", False, "Linux 侧编译夹具", "找不到 dotnet")
        return evidence
    r = run([dotnet, "build", str(csproj), "-c", "Release", "--nologo",
             "-nodeReuse:false", "-p:UseSharedCompilation=false"], cwd=REPO_ROOT, timeout=600)
    out = (r.out or "") + (r.err or "")
    zero_warn = "0 个警告" in out or "0 Warning(s)" in out
    zero_err = "0 个错误" in out or "0 Error(s)" in out
    rec.add("static.build.linux", r.code == 0, "Linux 侧 dotnet build 成功",
            f"exit={r.code}", tail=out[-600:])
    rec.add("static.build.zero_warn_err", r.code == 0 and zero_warn and zero_err,
            "Linux 侧编译 0 警告 0 错误", f"warnings0={zero_warn} errors0={zero_err}")

    dll = FIXTURE_GUI / "bin" / "Release" / "net10.0" / "GuiApp.dll"
    subsystem = read_pe_subsystem(dll) if dll.is_file() else None
    rec.add("static.pe_subsystem_gui", subsystem == 2,
            "PE 子系统必须是 WINDOWS_GUI(2) —— WinExe 的机器可读证据",
            f"subsystem={subsystem} (期望 2)")
    evidence["dllSha256"] = sha256_file(dll)
    evidence["peSubsystem"] = subsystem

    # 像素判据本身的离线自证（--selftest 纯逻辑、零 Win32，Linux 可跑；含 R/B 交换反证）
    selftest = None
    st_exit = None
    if dll.is_file():
        st = run([dotnet, str(dll), "--selftest"], cwd=REPO_ROOT, timeout=180)
        st_exit = st.code
        for line in (st.out or "").splitlines():
            line = line.strip()
            if line.startswith("WDB_GUI_SELFTEST "):
                try:
                    selftest = json.loads(line[len("WDB_GUI_SELFTEST "):])
                except json.JSONDecodeError:
                    selftest = None
        names = {c.get("name") for c in (selftest or {}).get("cases", [])}
        rec.add("static.selftest.pixel_logic",
                st.code == 0 and bool(selftest) and selftest.get("ok") is True,
                "夹具 --selftest 必须全绿（BGRA↔COLORREF 色序 / 自写 BMP 头 / 颜色计数；Linux 离线可跑）",
                f"exit={st.code} cases={len((selftest or {}).get('cases', []))} "
                f"failed={[c.get('name') for c in (selftest or {}).get('cases', []) if not c.get('pass')]}")
        rec.add("static.selftest.negative_control",
                "readmarker_rejects_swapped_rb" in names and "readmarker_rejects_before_first_paint" in names,
                "必须有反证用例：R/B 交换的 BGRA 与未绘制帧都**不得**被判成 marker（防止位序写反仍全绿）",
                f"cases={sorted(n for n in names if n)}")
        evidence["selftest"] = selftest

    assets = FIXTURE_GUI / "obj" / "project.assets.json"
    libs = []
    if assets.is_file():
        try:
            libs = list(json.loads(assets.read_text(encoding="utf-8")).get("libraries", {}).keys())
        except (OSError, json.JSONDecodeError):
            libs = ["<unreadable>"]
    rec.add("static.no_nuget_libraries", libs == [], "NuGet 还原结果必须零库（纯 BCL）", f"libraries={libs}")
    evidence["nugetLibraries"] = libs
    return evidence


# --------------------------------------------------------------------------
# 实机模式
# --------------------------------------------------------------------------
@dataclass
class LiveWatch:
    saw_queued: bool = False
    saw_running: bool = False
    running_pid: int | None = None
    ready_while_running: bool = False
    ready_at: float | None = None
    states: list[str] = field(default_factory=list)
    observations: list[dict] = field(default_factory=list)


def watch_job(queue: Path, job_id: str, cli_proc: subprocess.Popen, watch: LiveWatch,
              overall_timeout: float) -> None:
    """边跑边采：任务必须真的进过 running，且 running 期间出现过 ready 事件。"""
    job = job_of(queue, job_id)
    deadline = time.time() + overall_timeout
    while time.time() < deadline:
        status = job.status()
        state = (status or {}).get("state")
        if state and (not watch.states or watch.states[-1] != state):
            watch.states.append(state)
        if status:
            if state == "queued":
                watch.saw_queued = True
            if state == "running":
                watch.saw_running = True
                watch.running_pid = status.get("pid") or watch.running_pid
        ready = job.last_event("ready")
        if ready is not None and watch.ready_at is None:
            watch.ready_at = time.time()
            if state == "running":
                watch.ready_while_running = True
                watch.observations.append({
                    "atEpoch": round(watch.ready_at, 3),
                    "jobState": state, "jobPid": (status or {}).get("pid"),
                    "hwnd": (ready.get("window") or {}).get("hwnd"),
                    "isWindowVisible": (ready.get("window") or {}).get("isWindowVisible"),
                    "pid": ready.get("pid"),
                })
        if state in TERMINAL_STATES and cli_proc.poll() is not None:
            break
        if cli_proc.poll() is not None and state in TERMINAL_STATES:
            break
        time.sleep(0.35)


def glob_in_runs(job_id: str, runs_root_wsl: Path | None, pattern: str) -> Path | None:
    """在 runs 副本里递归找夹具落下的文件。

    注意：runs 副本**保留仓库相对路径**，所以真实位置是
    <runs>/<id>/src/tools/windows-debug-bridge/tests/fixtures/GuiApp/bin/<cfg>/net10.0/ —— 
    而不是 <runs>/<id>/src/GuiApp/...（实测踩过，最初按后者 glob 导致 pidfile 找不到）。
    """
    if runs_root_wsl is None:
        return None
    base = runs_root_wsl / job_id / "src"
    if not base.is_dir():
        return None
    found = sorted(base.glob(pattern))
    return found[-1] if found else None


def glob_pidfile(queue: Path, job_id: str, runs_root_wsl: Path | None) -> Path | None:
    return glob_in_runs(job_id, runs_root_wsl, "**/wdb-gui-*.pid")


def glob_evidence_jsonl(queue: Path, job_id: str, runs_root_wsl: Path | None) -> Path | None:
    return glob_in_runs(job_id, runs_root_wsl, "**/wdb-gui-evidence.jsonl")


def events_from_jsonl(path: Path | None) -> list[dict]:
    if path is None or not path.is_file():
        return []
    out = []
    try:
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            line = line.strip()
            if line.startswith(GUI_PREFIX):
                try:
                    out.append(json.loads(line[len(GUI_PREFIX):]))
                except json.JSONDecodeError:
                    continue
    except OSError:
        return []
    return out


def live_checks(rec: Recorder, bridge: Bridge, args) -> dict:
    queue = bridge.queue
    evidence: dict = {}

    # ── 预检：必须有一个真实 Windows 前台 Worker ────────────────────────────
    log("== 预检：Worker / 队列 / runsRoot ==")
    doctor = bridge.doctor()
    if doctor is None:
        rec.add("env.doctor", False, "doctor --json 必须可解析", "doctor 无 JSON 输出")
        return evidence
    worker = doctor.get("worker") or {}
    state = worker.get("state") or {}
    healthy = bool(doctor.get("workerHealthy")) and bool(state.get("healthy", worker.get("healthy")))
    runs_root = state.get("runsRoot") or ""
    queue_root = state.get("queueRoot") or ""
    rec.add("env.worker_healthy", healthy, "Windows 前台 Worker 必须健康存活",
            f"healthy={healthy} pid={state.get('pid')} host={state.get('hostName')} "
            f"age={worker.get('ageSeconds'):.1f}s" if healthy else f"reason={state.get('reason') or doctor.get('notes')}")
    rec.add("env.runs_root_local_drive", is_local_drive(runs_root),
            "runsRoot 必须是 Windows 本地盘符路径（拒绝 UNC 解包）", f"runsRoot={runs_root}")
    rec.add("env.queue_unc_or_match", str(queue) == str(doctor.get("queueRoot")) or queue_root.endswith(".wdb\\queue"),
            "队列根必须与 doctor 报告一致", f"queue={queue} workerQueueRoot={queue_root}")
    rec.add("env.wsl_side_no_interop", doctor.get("windowsInterop") is False,
            "WSL 侧确实无法直接执行 Windows 程序（必须走 Windows Worker）",
            f"binfmtHandlers={doctor.get('binfmtHandlers')}")

    if not healthy:
        rec.add("env.blocked", False, "无健康 Worker，无法进行实机 GUI 验收",
                "请用户在 Windows 前台运行 start-worker.ps1 后重跑")
        return evidence

    runs_root_wsl = win_to_wsl(runs_root)
    evidence["preflight"] = {
        "wslHost": doctor.get("operatingSystem"), "repoRoot": doctor.get("repoRoot"),
        "queueRoot": str(queue), "worker": state, "runsRootWsl": str(runs_root_wsl) if runs_root_wsl else None,
        "dotnetVersion": doctor.get("dotnetVersion"),
    }
    rec.add("env.runs_root_readable_from_wsl", runs_root_wsl is not None and runs_root_wsl.is_dir(),
            "runsRoot 必须能从 WSL 侧读取（用于独立复核 Artifact/BMP）",
            f"wsl={runs_root_wsl} exists={runs_root_wsl.is_dir() if runs_root_wsl else None}")

    cancels_before = cancel_map(queue)
    jobs_before = set(cancels_before)

    # 礼貌排队：队列是**串行**的，且其他作者也在往同一个默认队列投任务。
    # 本脚本只做两件事：等空闲再提交、按需延长等待——绝不 cancel 别人的任务。
    idle = True
    idle_snap = bridge.queue_snapshot()
    idle_waited = 0.0
    if args.wait_for_idle > 0:
        log(f"== 等待队列空闲（最多 {args.wait_for_idle}s，绝不取消他人任务） ==")
        idle, idle_snap, idle_waited = bridge.wait_for_idle(float(args.wait_for_idle))
        log(f"  · idle={idle} waited={idle_waited}s currentJobId={idle_snap.get('currentJobId')} "
            f"pending={idle_snap.get('pending')}")
    # 这只是「礼貌排队」的观测记录，**不作为通过判据**：队列忙是别人在用，不是本夹具的缺陷；
    # CLI 的 --wait-timeout(=--queue-wait) 才是真正覆盖排队等待的保险。
    rec.add("queue.idle_before_submit", True,
            "记录：提交前是否等到队列空闲（串行队列；其他作者的任务优先，本脚本只等待、不打断、不 cancel 他人）",
            f"idle={idle} waited={idle_waited}s currentJobId={idle_snap.get('currentJobId')} "
            f"pending={idle_snap.get('pending')}")

    # ── 主用例：build-run --follow --require-worker -- --duration-ms 12000 ──
    duration_ms = args.duration_ms
    log(f"== 主用例：build-run GuiApp（自动超时 {duration_ms}ms 退出） ==")
    work = WORK_ROOT / "main"
    work.mkdir(parents=True, exist_ok=True)
    cli_out = work / "cli.stdout.txt"
    cli_err = work / "cli.stderr.txt"
    argv = bridge._argv("build-run", project=GUI_PROJECT, timeout=args.job_timeout, follow=True,
                        app_args=["--duration-ms", str(duration_ms)])
    log("  · CLI: " + " ".join(argv))
    started = time.time()
    with cli_out.open("wb") as fo, cli_err.open("wb") as fe:
        proc = subprocess.Popen(argv, cwd=str(bridge.repo), stdout=fo, stderr=fe)
        # 先拿到 job id（提交行在 CLI 一开始就写出）
        job_id = None
        def find_id():
            for payload in all_json(cli_out.read_text(errors="replace")):
                if payload.get("id"):
                    return payload["id"]
            return None
        job_id = poll_until(find_id, 60.0, 0.25)
        if not job_id:
            proc.wait(timeout=60)
            rec.add("cli.submit", False, "必须能从 CLI stdout 解析任务 id",
                    f"exit={proc.returncode} stdout={cli_out.read_text(errors='replace')[-400:]}")
            return evidence

        log(f"  · 任务 id = {job_id}")
        watch = LiveWatch()
        overall = float(args.queue_wait) + float(args.job_timeout) + 120.0
        watcher = threading.Thread(target=watch_job,
                                   args=(queue, job_id, proc, watch, overall), daemon=True)
        watcher.start()
        cli_code = proc.wait(timeout=overall + 60.0)
        watcher.join(timeout=10.0)
    cli_seconds = time.time() - started
    cli_stdout = cli_out.read_text(errors="replace")
    cli_stderr = cli_err.read_text(errors="replace")
    ev = all_json(cli_stdout)
    submit_result = ev[0] if ev else None
    terminal = ev[-1] if len(ev) > 1 else None

    job = job_of(queue, job_id)
    status = job.status()
    final_state = (status or {}).get("state")
    if terminal and terminal.get("state"):
        final_state = terminal.get("state")

    rec.add("cli.exit_code", cli_code == CLI_OK,
            "CLI 必须 0 退出（应用自身退出码只出现在 status.exitCode）", f"exit={cli_code}",
            stderrTail=cli_stderr[-800:])
    rec.add("cli.terminal_state", final_state == "succeeded" and (status or {}).get("exitCode") == 0,
            "任务终态必须是 succeeded 且退出码 0",
            f"state={final_state} exitCode={(status or {}).get('exitCode')} error={(status or {}).get('error')}")
    rec.add("cli.serial_queue_respected", (submit_result or {}).get("workerHealthy") is True,
            "提交时 Worker 必须健康（--require-worker 已生效）",
            f"workerHealthy={(submit_result or {}).get('workerHealthy')}")

    # ── 活体观察 ────────────────────────────────────────────────────────────
    target_pid = (status or {}).get("pid")
    rec.add("gui.observed_running_state", watch.saw_running
            and any(o.get("jobState") == "running" for o in watch.observations),
            "必须边跑边观察到任务处于 running（本项只证明「观测到了运行期」，**不主张目标身份**）",
            f"states={watch.states} statusStagePid={watch.running_pid}"
            f"（build-run 的 running 阶段 pid 是构建宿主/msbuild，仅作 metadata，不用于身份判定）")
    rec.add("gui.observed_running_target_identity",
            judge_ready_target_identity_while_running(watch.observations, target_pid),
            "running 期间的窗口必须被证明是**本轮目标进程**：夹具自报 pid == 终态 PID，且 HWND 非空、isWindowVisible=true；"
            "只凭 status 的构建阶段 pid 不得通过（不比较 status 阶段 pid 与终态 pid，避免已知的 non-live pid 假红）",
            f"terminalPid={target_pid} observations={watch.observations} "
            f"judgedBy=judge_ready_target_identity_while_running")
    rec.add("gui.ready_while_running", watch.ready_while_running,
            "ready 事件必须出现在任务 running 期间（证明窗口存在时任务确在运行）",
            f"observations={watch.observations}", observedStates=watch.states)

    # ── 夹具证据（stdout 主通道；jsonl 兜底） ───────────────────────────────
    ready = job.last_event("ready")
    exit_ev = job.last_event("exit")
    err_ev = job.last_event("error")
    source_channel = "stdout.log"
    if ready is None and exit_ev is None:
        jsonl = glob_evidence_jsonl(queue, job_id, runs_root_wsl)
        evs = events_from_jsonl(jsonl)
        for e in evs:
            if e.get("event") == "ready":
                ready = e
            elif e.get("event") == "exit":
                exit_ev = e
            elif e.get("event") == "error":
                err_ev = e
        if evs:
            source_channel = f"jsonl:{jsonl}"
    rec.add("gui.events_ready", ready is not None, "必须输出 WDB_GUI ready 事件",
            f"channel={source_channel} stderrTail={cli_stderr[-300:] if ready is None else ''}")
    rec.add("gui.events_exit", exit_ev is not None, "自动超时退出必须输出 WDB_GUI exit 事件",
            f"channel={source_channel}")
    rec.add("gui.no_error_event", err_ev is None, "夹具不得输出 error 事件",
            "无 error" if err_ev is None else json.dumps(err_ev, ensure_ascii=False)[:300])

    if ready is None:
        rec.add("gui.blocked_no_events", False, "没有夹具事件，后续窗口/像素判定无法进行",
                f"stdout.log 尾部：{job.stdout_text()[-500:]}")
        return evidence

    w = ready.get("window") or {}
    sess = ready.get("session") or {}
    paint = ready.get("paint") or {}
    pcl = paint.get("printClient") or {}
    scr = paint.get("screen") or {}
    focus = ready.get("focus") or {}
    envjs = ready.get("env") or {}
    osjs = ready.get("os") or {}
    timing = (exit_ev or {}).get("timing") or ready.get("timing") or {}

    evidence["ready"] = {k: ready.get(k) for k in ("event", "pid", "os", "session", "window", "paint", "focus", "env", "timing", "pidFile")}
    evidence["exit"] = exit_ev

    # ── 真实 OS / 会话 / 交互桌面 ───────────────────────────────────────────
    rec.add("gui.os_windows", osjs.get("platform") == "Win32NT"
            and "Windows" in str(osjs.get("osDescription", "")),
            "必须是真实 Windows OS（拒绝 Linux 假绿）",
            f"platform={osjs.get('platform')} desc={osjs.get('osDescription')} build={osjs.get('build')}")
    rec.add("gui.os_build_sane", isinstance(osjs.get("build"), int) and osjs.get("build", 0) >= 10240,
            "Windows 内核 build 必须合理（RtlGetVersion 实测）",
            f"major={osjs.get('major')}.{osjs.get('minor')} build={osjs.get('build')}")
    rec.add("gui.session_console", bool(sess.get("processSessionIdOk")) and bool(sess.get("isConsoleSession")),
            "进程必须位于**活动控制台会话**（sessionId == WTSGetActiveConsoleSessionId）",
            f"session={sess.get('processSessionId')} console={sess.get('activeConsoleSessionId')}")
    rec.add("gui.desktop_interactive", sess.get("isInteractiveDesktop") is True
            and sess.get("windowStation") == "WinSta0" and sess.get("desktop") == "Default",
            "必须绑定到交互桌面 WinSta0\\Default",
            f"winSta={sess.get('windowStation')} desktop={sess.get('desktop')} "
            f"dwm={sess.get('dwmCompositionEnabled')} desktopWnd={sess.get('desktopWindow')}")

    # ── 窗口可见性 / 几何 ───────────────────────────────────────────────────
    rect = w.get("rect") or {}
    client = w.get("clientRect") or {}
    req = w.get("requestedClientSize") or {}
    overlaps = (rect.get("right", 0) > (w.get("monitorWorkArea") or {}).get("left", 0)
                and rect.get("left", 0) < (w.get("monitorWorkArea") or {}).get("right", 0)
                and rect.get("bottom", 0) > (w.get("monitorWorkArea") or {}).get("top", 0)
                and rect.get("top", 0) < (w.get("monitorWorkArea") or {}).get("bottom", 0))
    rec.add("gui.hwnd_valid", isinstance(w.get("hwndValue"), int) and w.get("hwndValue", 0) > 0
            and bool(w.get("isWindow")) and w.get("className") == "WdbGuiFixtureWindow",
            "必须拿到真实 HWND 且 IsWindow 为真、窗口类名匹配",
            f"hwnd={w.get('hwnd')} class={w.get('className')} title={w.get('title')}")
    rec.add("gui.window_visible", w.get("isWindowVisible") is True and w.get("isIconic") is False,
            "窗口必须**实际可见**（IsWindowVisible=true，非最小化）",
            f"visible={w.get('isWindowVisible')} iconic={w.get('isIconic')} zoomed={w.get('isZoomed')}")
    cloaked_value = w.get("dwmCloaked")
    rec.add("gui.window_not_cloaked", judge_dwm_not_cloaked(cloaked_value),
            "DWM 不得把窗口标记为 cloaked（隐藏窗口的常见伪装）；**只接受布尔 False**，"
            "True 或查询不可用(None)一律 FAIL（fail closed，不把「查不到」当通过）",
            f"dwmCloaked={cloaked_value!r} judgedBy=judge_dwm_not_cloaked")
    reqwin = w.get("requestedWindowSize") or {}
    size_ok = (client.get("width") == req.get("width") and client.get("height") == req.get("height")
               and rect.get("width", 0) >= reqwin.get("width", 0) and rect.get("height", 0) >= reqwin.get("height", 0)
               and rect.get("width", 0) >= 200 and rect.get("height", 0) >= 100)
    rec.add("gui.window_rect_sane", size_ok,
            "客户区必须精确等于请求的 420x260（AdjustWindowRectEx 已把标题栏/边框算进整窗），整窗外框不小于请求值",
            f"rect={rect} client={client} requestedClient={req} requestedWindow={reqwin}")
    rec.add("gui.window_on_screen", w.get("onScreen") is True and overlaps,
            "窗口必须落在真实监视器工作区内且与工作区相交",
            f"monitor={w.get('monitorFromWindow')} monitorCount={w.get('monitorCount')} "
            f"workArea={w.get('monitorWorkArea')} onScreen={w.get('onScreen')} intersects={overlaps}")
    rec.add("gui.window_in_desktop_enumeration", w.get("oursEnumeratedAsTopLevel") is True
            and (w.get("topLevelWindowCount") or 0) > 0,
            "窗口必须出现在 EnumWindows 的可见顶层窗口枚举中（真在桌面上，不是隐藏窗口）",
            f"oursEnumerated={w.get('oursEnumeratedAsTopLevel')} topLevelVisible={w.get('topLevelWindowCount')}")

    # ── 真实绘制 / 像素自证 ─────────────────────────────────────────────────
    rec.add("gui.wm_paint_record", isinstance(paint.get("wmPaintCount"), int) and paint.get("wmPaintCount", 0) >= 1,
            "必须记录到至少一次真实 WM_PAINT",
            f"wmPaintCount={paint.get('wmPaintCount')} first={paint.get('firstPaintUtc')} last={paint.get('lastPaintUtc')}")
    rec.add("gui.pixel_printclient", pcl.get("captured") is True and pcl.get("markerMatches") is True
            and (pcl.get("distinctColors") or 0) >= 3,
            "printClient 通道（PrintWindow）像素自证：marker 逐位相等且颜色数>2",
            f"captured={pcl.get('captured')} markerMatches={pcl.get('markerMatches')} "
            f"distinct={pcl.get('distinctColors')} sha={str(pcl.get('sha256'))[:16]}… err={pcl.get('error')}")
    rec.add("gui.pixel_screen_readback", scr.get("pixelMatchesEver") is True,
            "screen 单点探针（GetDC(自己的 hwnd)+GetPixel）至少一次读到正确 marker 像素",
            f"ever={scr.get('pixelMatchesEver')} final={scr.get('pixelMatchesFinal')} "
            f"pixelFinal={scr.get('pixelFromWindowDc')} expected={paint.get('expectedMarkerColorRef')} err={scr.get('error')}")
    # 防擅自抓屏（父代理明确要求）：screen 通道不得带任何整帧 payload，runs 目录里也不得出现整帧文件。
    scr_frame_keys = [k for k in ("bmpPath", "bmpBytes", "bmpSha256", "sha256", "distinctColors", "frameBytes")
                      if scr.get(k) not in (None, False)]
    stray_frames = []
    if runs_root_wsl is not None:
        src_dir = runs_root_wsl / job_id / "src"
        if src_dir.is_dir():
            stray_frames = [str(q) for q in src_dir.glob("**/*.bmp") if "printclient" not in q.name]
    rec.add("gui.no_foreign_frame_persisted",
            scr.get("framePersisted") is False and not scr_frame_keys and not stray_frames,
            "绝不擅自抓屏：screen 通道只保留单点像素（无帧字节/无 SHA/不落盘），runs 目录里不存在 printclient 之外的任何 BMP",
            f"framePersisted={scr.get('framePersisted')} frameKeys={scr_frame_keys} strayFrames={stray_frames}")

    # ── 不抢焦点 ────────────────────────────────────────────────────────────
    active_window_value = focus.get("activeWindowOfThread")
    rec.add("gui.no_focus_steal", focus.get("oursEverForeground") is False
            and judge_thread_no_active_window(active_window_value),
            "不得抢焦点（全程从未成为前台窗口；线程活动窗口必须是确凿的 0x0，缺失不得算通过）",
            f"oursEverForeground={focus.get('oursEverForeground')} activeWindow={active_window_value!r} "
            f"fgStart={focus.get('foregroundAtStart')} fgExit={focus.get('foregroundAtExit')}")

    # ── 自动收尾 ────────────────────────────────────────────────────────────
    elapsed = timing.get("elapsedMs") or 0
    ok_timing = (exit_ev is not None and duration_ms * 0.75 <= elapsed <= duration_ms + 20000
                 and timing.get("autoExit") is True)
    rec.add("gui.auto_exit_timing", ok_timing,
            "必须在请求时长附近自行退出（不等待用户）",
            f"elapsedMs={elapsed} requested={duration_ms} autoExit={timing.get('autoExit')}")
    rec.add("gui.exit_code_zero", exit_ev is not None and exit_ev.get("exitCode") == 0
            and exit_ev.get("selfCheckOk") is True,
            "夹具自证通过并以 0 退出",
            f"exitCode={(exit_ev or {}).get('exitCode')} selfCheckOk={(exit_ev or {}).get('selfCheckOk')}")

    # ── Artifact / PID 一致性 ───────────────────────────────────────────────
    status_pid = (status or {}).get("pid")
    fixture_pid = ready.get("pid")
    artifact = (status or {}).get("artifact") or ""
    artifact_wsl = win_to_wsl(artifact)
    pidfile = glob_pidfile(queue, job_id, runs_root_wsl)
    pidfile_value = None
    if pidfile is not None:
        try:
            pidfile_value = int(pidfile.read_text(encoding="utf-8").strip())
        except (OSError, ValueError):
            pidfile_value = None
    rec.add("bridge.pid_consistency",
            isinstance(status_pid, int) and status_pid == fixture_pid and pidfile_value == fixture_pid,
            "status.pid == 夹具 pid == pidfile 内容（三方一致）",
            f"status.pid={status_pid} fixture.pid={fixture_pid} pidfile={pidfile_value}")
    rec.add("bridge.artifact_exists",
            bool(artifact_wsl) and artifact_wsl.is_file() and artifact.endswith("GuiApp.dll"),
            "构建产物必须在 runsRoot 下真实存在（经 /mnt/c 独立复核）",
            f"artifact={artifact} wsl={artifact_wsl} exists={artifact_wsl.is_file() if artifact_wsl else None}")
    rec.add("bridge.artifact_under_runs",
            bool(artifact) and bool(runs_root) and artifact.lower().startswith(runs_root.lower())
            and f"\\{job_id}\\" in artifact,
            "产物必须位于本任务的 runs 目录内（本地盘副本，非 UNC 构建）",
            f"runsRoot={runs_root} jobId={job_id}")
    exe_path = envjs.get("exePath") or ""
    rec.add("bridge.exe_local_drive",
            envjs.get("exeOnLocalDrive") is True and envjs.get("exeIsUnc") is False
            and envjs.get("exeExists") is True and exe_path.lower().endswith("guiapp.exe"),
            "目标 exe 必须是 Windows 本地盘上的 GuiApp.exe",
            f"exePath={exe_path} local={envjs.get('exeOnLocalDrive')} unc={envjs.get('exeIsUnc')}")
    cwd = envjs.get("currentDirectory") or ""
    rec.add("bridge.cwd_is_runs_src",
            bool(runs_root) and cwd.lower().startswith(runs_root.lower()) and f"\\{job_id}\\" in cwd,
            "进程工作目录必须是本任务的 runs\\<id>\\src（源码副本隔离）",
            f"cwd={cwd}")
    rec.add("bridge.stdout_redirected", envjs.get("stdoutRedirected") is True
            and envjs.get("outputType") == "WinExe",
            "WinExe 的 stdout 必须被 Worker 重定向（证据通道成立）",
            f"stdoutRedirected={envjs.get('stdoutRedirected')} outputType={envjs.get('outputType')}")

    # ── BMP 独立复核（只截自己客户区） ──────────────────────────────────────
    expected_marker = (255, 0, 200)
    # 磁盘上是「退出前刷新」的那一版 BMP，其自报 SHA 在 exit 事件里；ready 事件里的 SHA 是首版（已被覆盖）。
    pcl_exit = ((exit_ev or {}).get("paint") or {}).get("printClient") or {}
    bmp_report = []
    for label, meta in (("printClient", pcl),):
        p = meta.get("bmpPath")
        wp = win_to_wsl(p) if p else None
        parsed = parse_bmp24(wp) if wp is not None and wp.is_file() else None
        sha_ready = meta.get("bmpSha256")
        sha_exit = pcl_exit.get("bmpSha256")
        entry = {"label": label, "winPath": p, "wslPath": str(wp) if wp else None,
                 "reportedSha256Ready": sha_ready, "reportedSha256Exit": sha_exit,
                 "reportedBytesReady": meta.get("bmpBytes"), "reportedBytesExit": pcl_exit.get("bmpBytes")}
        if parsed:
            entry.update({"width": parsed["width"], "height": parsed["height"], "bpp": parsed["bpp"],
                          "fileBytes": parsed["fileBytes"], "sha256": parsed["sha256"],
                          "markerPixel": list(parsed["pixel"]), "distinctSample": parsed["distinctSample"],
                          "shaMatchExit": sha_exit is not None and parsed["sha256"] == sha_exit,
                          "shaMatchReadyOrExit": parsed["sha256"] in (sha_ready, sha_exit),
                          "markerOk": parsed["pixel"] == expected_marker,
                          "bytesMatchExit": parsed["fileBytes"] == pcl_exit.get("bmpBytes")})
        bmp_report.append(entry)
    bmp_ok = bool(bmp_report) and all(e.get("markerOk") and e.get("shaMatchExit") and e.get("bytesMatchExit")
                                      and (e.get("width") or 0) > 0 and e.get("bpp") == 24 for e in bmp_report)
    rec.add("gui.bmp_independent_verify", bmp_ok,
            "夹具自写的**自己客户区** BMP 必须能被独立解析：尺寸/位深正确、marker 像素正确、SHA256 与 exit 事件自报值逐字节一致",
            json.dumps(bmp_report, ensure_ascii=False)[:900])
    evidence["bmp"] = bmp_report

    png_path = None
    if bmp_report and bmp_report[0].get("wslPath"):
        png_path = convert_bmp_to_png(Path(bmp_report[0]["wslPath"]), Path(args.png_dir), job_id)
        if png_path:
            bmp_report[0]["pngPath"] = str(png_path)
    rec.add("gui.frame_png_for_human_review", png_path is not None,
            "把「自己客户区」的 BMP 转成 PNG（零第三方自写编码器）供人眼复核",
            f"png={png_path}（工作区内，可直接打开）")

    # ── 队列纪律：只动自己的任务 ────────────────────────────────────────────
    time.sleep(0.5)
    cancels_after = cancel_map(queue)
    new_cancels = sorted(j for j, has in cancels_after.items() if has and not cancels_before.get(j, False))
    foreign_cancels = [j for j in new_cancels if j != job_id]
    # 其他作者**合法地**会在这个共享串行队列里新增任务：新增目录本身不是违规，
    # 违规判据只有一条 —— 是否出现了「不是我们自建任务」的 cancel 标记。
    new_jobs_by_others = sorted(j for j in (set(cancels_after) - jobs_before) if j != job_id)
    rec.add("queue.discipline", not foreign_cancels,
            "只允许对自建任务操作：绝不 cancel 别人的任务（他人新增任务目录属正常，仅记录）",
            f"newCancelMarkers={new_cancels} foreignCancels={foreign_cancels} "
            f"newJobsByOthers(仅记录)={new_jobs_by_others} own={job_id}")

    fields = {
        "jobId": job_id,
        "queueAtSubmit": {"idle": idle, "waitedSeconds": idle_waited, "snapshot": idle_snap},
        "cliSeconds": round(cli_seconds, 3),
        "cliExit": cli_code,
        "terminalState": final_state,
        "status": status,
        "watch": {"states": watch.states, "runningPid": watch.running_pid,
                  "readyWhileRunning": watch.ready_while_running, "observations": watch.observations},
        "pidFile": str(pidfile) if pidfile else None,
        "pidFileWin": wsl_to_win(pidfile) if pidfile else None,
        "exitEventSeen": exit_ev is not None,
        "durationMs": duration_ms,
        "cliStdoutTail": cli_stdout[-400:],
        "cliStderrTail": cli_stderr[-400:],
    }
    evidence["main"] = fields

    # ── 可选：wdb stop 杀自己 Job + ProcessProbe 复核 PID 消失 ──────────────
    if args.with_stop:
        evidence["stop"] = stop_case(rec, bridge, runs_root, runs_root_wsl, args, cancels_after, {job_id})
    else:
        rec.skip("gui.stop_case", "wdb stop 终止自己的 Job + ProcessProbe 复核 PID 消失（未传 --with-stop）")

    return evidence


def stop_case(rec: Recorder, bridge: Bridge, runs_root: str, runs_root_wsl: Path | None, args,
              cancels_before: dict[str, bool], ours: set[str]) -> dict:
    log("== 追加用例：wdb stop 终止自己的 Job + ProcessProbe 复核 PID ==")
    job_id = None
    win_pidfile: str | None = None
    info: dict = {}
    try:
        run = bridge.call("build-run", project=GUI_PROJECT, timeout=args.job_timeout,
                          app_args=["--duration-ms", "600000"])
        job_id = (last_json(run.out) or {}).get("id")
        if job_id:
            ours.add(job_id)   # 本用例自建的任务也算「我们的」，否则收尾纪律会把自家 cancel 判成 foreign
        rec.add("stop.submit", bool(job_id), "提交长时长任务（10 分钟，必然需要 stop）", f"id={job_id}")
        if not job_id:
            return info
        job = job_of(bridge.queue, job_id)

        def ready_seen():
            return job.last_event("ready") if job.status() else None
        ready = poll_until(ready_seen, min(float(args.queue_wait), 900.0), 0.4)
        rec.add("stop.ready_before_stop", ready is not None, "stop 之前必须确认窗口已显示（ready 事件）",
                f"hwnd={(ready or {}).get('window', {}).get('hwnd')} pid={(ready or {}).get('pid')}")

        pidfile = glob_pidfile(bridge.queue, job_id, runs_root_wsl)
        win_pidfile = wsl_to_win(pidfile) if pidfile else None
        rec.add("stop.pidfile_present", pidfile is not None and win_pidfile is not None,
                "pidfile 必须已落盘（供 Windows 侧 ProcessProbe 复核）",
                f"wsl={pidfile} win={win_pidfile}")

        sr = bridge.stop(job_id, "windows-gui 验收：停止自建 GUI 任务（Job Object 杀树）")
        rec.add("stop.cli_ok", sr.code == CLI_OK, "wdb stop 必须成功投递 cancel 标记",
                f"exit={sr.code} err={sr.err[-200:]}")

        w = bridge.wait(job_id)
        status = job.status()
        state = (status or {}).get("state")
        final = last_json(w.out) or {}
        state = final.get("state") or state
        rec.add("stop.terminal_canceled", state == "canceled" and w.code == CLI_CANCELED,
                "任务必须进入 canceled 终态（CLI 退出码 6）",
                f"state={state} cliExit={w.code} error={(status or {}).get('error')}")
        rec.add("stop.no_exit_event", job.last_event("exit") is None,
                "被 Job Object 强杀的进程不得输出 exit 事件（证明确实是被外部终止）",
                f"exitSeen={job.last_event('exit') is not None}")

        if win_pidfile:
            # 注意：run/build-run 在**不带 --wait/--follow** 时是「提交即返回」，探针输出不在 CLI stdout，
            # 而在任务目录的 stdout.log 里；因此这里先提交拿 id、再 wait 终态、最后读 stdout.log。
            pr = bridge.call("run", project=PROBE_PROJECT, timeout=args.job_timeout,
                             app_args=["--pidfile", win_pidfile, "--min-pids", "1",
                                       "--wait-until-gone-seconds", "15",
                                       "--expect-name", "GuiApp"])
            probe_job = (last_json(pr.out) or {}).get("id")
            probe_status = None
            if probe_job:
                bridge.wait(probe_job)
                probe_status = job_of(bridge.queue, probe_job).status()
            probe = None
            if probe_job:
                for line in job_of(bridge.queue, probe_job).stdout_text().splitlines():
                    line = line.strip()
                    if line.startswith(PROBE_PREFIX):
                        try:
                            probe = json.loads(line[len(PROBE_PREFIX):])
                        except json.JSONDecodeError:
                            continue
            rec.add("stop.pid_gone_windows_side",
                    bool(probe_job) and (probe_status or {}).get("state") == "succeeded"
                    and bool(probe) and probe.get("allGone") is True and probe.get("pidCount", 0) >= 1,
                    "Windows 侧 ProcessProbe 必须证实被杀的 GUI PID 已消失（WSL 不用 os.kill）",
                    f"probeJob={probe_job} probeExit={(probe_status or {}).get('exitCode')} "
                    f"allGone={(probe or {}).get('allGone')} pids={(probe or {}).get('pids')} "
                    f"status={(probe or {}).get('status')} queryErrors={(probe or {}).get('queryErrors')}")
            info["probe"] = probe
            info["probeJob"] = probe_job
            info["probeExit"] = (probe_status or {}).get("exitCode")
        else:
            rec.add("stop.pid_gone_windows_side", False,
                    "Windows 侧 ProcessProbe 必须证实被杀的 GUI PID 已消失",
                    "没有 pidfile，无法复核（判定失败而非跳过）")

        info.update({"jobId": job_id, "terminalState": state, "cliExit": w.code,
                     "pidfileWin": win_pidfile, "readyHwnd": (ready or {}).get("window", {}).get("hwnd")})
    finally:
        # 收尾：只对自己刚创建的任务补一次 cancel（幂等），绝不碰别人的任务
        if job_id:
            try:
                bridge.stop(job_id, "windows-gui 验收收尾（自建任务，幂等）")
            except Exception:  # noqa: BLE001 - 收尾失败不掩盖主判定
                pass
        cancels_after = cancel_map(bridge.queue)
        foreign = sorted(j for j, has in cancels_after.items()
                         if has and not cancels_before.get(j, False) and j not in ours)
        rec.add("stop.no_foreign_cancel", not foreign,
                "stop 用例全过程也只 cancel 自建任务", f"foreign={foreign} ours={sorted(ours)}")
    return info


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------
def main() -> int:
    ap = argparse.ArgumentParser(description="Windows 调试桥 · GUI 窗口验收（Win32 fixture 路径）")
    ap.add_argument("--static", action="store_true", help="仅做静态自检（不碰队列/Worker，不声明 GUI 通过）")
    ap.add_argument("--no-build", action="store_true", help="--static 时跳过 Linux 侧 dotnet build")
    ap.add_argument("--queue", default=str(DEFAULT_QUEUE), help="队列根（默认仓库 .wdb/queue）")
    ap.add_argument("--source", default=str(REPO_ROOT), help="源码根（默认仓库根）")
    ap.add_argument("--evidence", default=str(DEFAULT_EVIDENCE), help="证据 JSON 输出路径")
    ap.add_argument("--log", default=str(DEFAULT_LOG), help="人读日志路径")
    ap.add_argument("--duration-ms", type=int, default=12000, help="主用例窗口显示时长（默认 12000）")
    ap.add_argument("--job-timeout", type=int, default=300, help="单任务执行超时秒数（默认 300）")
    ap.add_argument("--queue-wait", type=int, default=1800,
                    help="CLI --wait-timeout：覆盖「排队 + 执行」的总等待秒数（默认 1800，串行队列可能被他人占用）")
    ap.add_argument("--wait-for-idle", type=int, default=900,
                    help="提交前最多等待队列空闲的秒数（默认 900；0 = 不等待）")
    ap.add_argument("--with-stop", action="store_true", help="追加 wdb stop + ProcessProbe 复核用例")
    ap.add_argument("--png-dir", default=str(REPO_ROOT / ".wdb" / "windows-gui-artifacts"),
                    help="把夹具自绘客户区 BMP 转成 PNG 的输出目录（工作区内，零第三方）")
    args = ap.parse_args()

    args.evidence = str(Path(args.evidence))
    open_log(Path(args.log))
    rec = Recorder()
    started = time.time()
    started_utc = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())

    log("=" * 78)
    log("Windows 调试桥 · GUI 窗口验收（Win32 fixture；不代表真实 Gallery 应用已验收）")
    log(f"mode = {'static' if args.static else 'full'} | repo = {REPO_ROOT}")
    log("=" * 78)

    evidence: dict = {}
    blocked: str | None = None
    try:
        if args.static:
            evidence["static"] = static_checks(rec, do_build=not args.no_build)
        else:
            wdb = resolve_wdb(REPO_ROOT)
            if not wdb:
                rec.add("env.wdb_cli", False, "桥 CLI 必须可用", "既无 wdb 启动器也无已构建 wdb.dll")
                blocked = "wdb CLI 不可用"
            else:
                log(f"  · CLI = {' '.join(wdb)}")
                bridge = Bridge(REPO_ROOT, Path(args.queue), Path(args.source), wdb,
                                wait_timeout=args.queue_wait, job_timeout=args.job_timeout)
                evidence = live_checks(rec, bridge, args)
    except KeyboardInterrupt:
        rec.add("run.interrupted", False, "运行被中断", "KeyboardInterrupt")
    except Exception as exc:  # noqa: BLE001 - 验收脚本必须把异常变成可读证据
        import traceback
        rec.add("run.exception", False, "脚本异常", f"{type(exc).__name__}: {exc}")
        log(traceback.format_exc())

    totals = rec.totals()
    ok = totals["failed"] == 0 and totals["passed"] > 0 and blocked is None
    result = "PASSED" if ok else ("BLOCKED" if blocked else "FAILED")

    payload = {
        "tool": "windows-gui",
        "schema": 1,
        "mode": "static" if args.static else "full",
        "result": result,
        "ok": ok,
        "windowsVerified": bool(ok and not args.static),
        "blockedReason": blocked,
        "startedUtc": started_utc,
        "finishedUtc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "durationSeconds": round(time.time() - started, 3),
        "host": {"python": sys.version.split()[0], "platform": sys.platform,
                 "wslHost": evidence.get("preflight", {}).get("wslHost") if isinstance(evidence, dict) else None},
        "target": {"project": GUI_PROJECT, "queue": args.queue, "source": args.source,
                   "fixtureDir": str(FIXTURE_GUI)},
        "pretest": {"staticOnly": args.static, "withStop": args.with_stop, "durationMs": args.duration_ms},
        "totals": totals,
        "cases": [c.to_json() for c in rec.cases],
        "evidence": evidence,
        "notTested": NOT_TESTED,
    }
    if args.static:
        payload["host"]["wslHost"] = None
        payload["disclaimer"] = ("静态模式只证明夹具源码/编译约束与脚本自身合规；"
                                 "**未**在 Windows 上拉起窗口，不得声称 GUI 已验收。")

    out_path = Path(args.evidence)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    tmp = out_path.with_suffix(out_path.suffix + ".tmp")
    tmp.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")
    tmp.replace(out_path)

    log("-" * 78)
    log(f"结果：{result} | windowsVerified={payload['windowsVerified']} | "
        f"{totals['passed']} passed / {totals['failed']} failed / {totals['skipped']} skipped")
    log(f"证据：{out_path}")
    log(f"日志：{args.log}")
    if blocked:
        log(f"阻塞：{blocked}")
    log("-" * 78)
    return 0 if ok else (3 if blocked else 1)


if __name__ == "__main__":
    sys.exit(main())
