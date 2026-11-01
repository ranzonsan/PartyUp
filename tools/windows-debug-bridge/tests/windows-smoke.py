#!/usr/bin/env python3
"""Windows 调试桥 · 真实 Windows 前台 Worker 的 WSL 侧自动验收（stdlib only）。

定位
    用户在 Windows 前台执行一次
        powershell.exe -NoProfile -ExecutionPolicy Bypass -File <UNC>\\tools\\windows-debug-bridge\\start-worker.ps1 -Queue '<UNC 队列路径>'
    之后，本脚本从 WSL 侧接管联调：用桥 CLI 提交任务、读回真实 Windows 进程吐出的证据，
    把「Windows 侧到底验了什么、没验什么」写成机器可读 JSON，供人复核。
    它替代「让用户开任务管理器肉眼看进程是否还在」这种不可复现的验收方式。

绝对不做（代码级自审，见 case static.self-audit）
    * 不启动 Worker：脚本的 CLI 契约表里没有 `worker` 子命令，也不会拉起任何 Worker 进程；
    * 不传 `--allow-non-windows`：那是 fixture 用的 Linux 降级开关，一旦出现就等于假绿；
    * 不调 `shutdown`：用户的前台 Worker 归用户所有；本脚本只提交自己的任务、只停自己的任务；
    * 不杀任何进程：进程树是否真的没了，全部交给 Windows 侧 ProcessProbe 的只读查询判定；
    * 不写不删宿主（Windows）上的任何东西：唯一由脚本决定路径的写入面是 WSL 侧工作区
      （队列、临时 Source、证据 JSON，以及经 \\\\wsl.localhost UNC 落回工作区内的 marker 文件）；
    * 不碰真实画廊数据：Source 永远是临时目录里自建的 fixture 副本，从不指向 src/ 或用户数据目录。

不健康就明确失败（不假绿）
    Worker 心跳失联、队列不存在、worker.json 缺失、或 worker.json 显示的是 **Linux 宿主**
    （QueueRoot/RunsRoot 是 Linux 路径）时，脚本以退出码 2 结束并写一份 result=BLOCKED 的证据 JSON，
    任何用例都不会被标成 passed。

能证明什么 / 不能证明什么
    能：真实 Windows 上，桥 CLI → 队列 → Windows Worker → CreateProcessW/Job 这条链是否真的把进程跑起来、
        argv 是否逐字节无损、cwd 是否在 Windows 本地盘（非 UNC）、构建失败时是否**没有**启动目标进程、
        取消长驻任务时**3 级进程链**（不是「3 个进程」——root + 两个直系兄弟会被判负）是否全部消失
        （链由各层自报 level + 内核真值父 PID 证明，消失由 ProcessProbe 在 Windows 侧只读判定）。
    不能：Avalonia GUI 窗口本身；UNC 队列；Worker 已在 Job 中的嵌套场景；KILL_ON_JOB_CLOSE 级联
        （需强杀 Worker，本脚本绝不这么做）；控制台代码页/非 UTF-8；长时超时终态。
        这些都在证据 JSON 的 notTested 里逐条列出 —— 不要把本脚本的 PASS 读成 GUI 验收通过。

用法
    python3 tools/windows-debug-bridge/tests/windows-smoke.py --static   # 不需要 Worker，今天就能跑
    python3 tools/windows-debug-bridge/tests/windows-smoke.py --list     # 列出用例
    python3 tools/windows-debug-bridge/tests/windows-smoke.py            # 全量（需要健康的 Windows Worker）
    python3 tools/windows-debug-bridge/tests/windows-smoke.py --case win.tree-cancel-3layer
    开关：--queue --work-root --evidence --wdb --job-timeout --wait-timeout --filter --strict-skips --clean

退出码
    0 = 选中用例全部 passed（skipped 允许，但会在 JSON 里逐条说明；--strict-skips 时 skipped 也算失败）
    1 = 至少一个用例 failed
    2 = BLOCKED（全量模式下 Worker 不健康 / 队列缺失 / 无法定位 CLI / 用法错误）
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import re
import select
import shutil
import signal
import subprocess
import sys
import threading
import time
import uuid
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from pathlib import Path

# --------------------------------------------------------------------------
# 路径与常量
# --------------------------------------------------------------------------
REPO_ROOT = Path(__file__).resolve().parents[3]
BRIDGE_DIR = REPO_ROOT / "tools" / "windows-debug-bridge"
TESTS_DIR = BRIDGE_DIR / "tests"
FIXTURE_ECHO = TESTS_DIR / "fixtures" / "EchoApp"
FIXTURE_PROBE = TESTS_DIR / "fixtures" / "ProcessProbe"
# day2（2026-10-12）新增的独立夹具；与兄弟作者的 GuiApp/WorkerLifecycleProbe 无关
FIXTURE_SPINNER = TESTS_DIR / "fixtures" / "LogSpinner"
FIXTURE_MTFM = TESTS_DIR / "fixtures" / "MultiTfm"

DEFAULT_QUEUE = REPO_ROOT / ".wdb" / "queue"
DEFAULT_WORK_ROOT = REPO_ROOT / ".wdb" / "windows-smoke"
DEFAULT_EVIDENCE = REPO_ROOT / ".wdb" / "windows-validation.json"
# day2 边界用例单独落盘，**绝不覆盖** day1 的证据文件
DEFAULT_EVIDENCE_DAY2 = REPO_ROOT / ".wdb" / "windows-day2-boundaries.json"
# 纯离线自检（--static）单独落盘：绝不能把「Windows 没测」的结果写进 Windows 证据文件
DEFAULT_EVIDENCE_STATIC = REPO_ROOT / ".wdb" / "windows-static-check.json"

ECHO_PREFIX = "WDB_ECHO "
PROBE_PREFIX = "WDB_PROBE "
PROBE_SELFTEST_PREFIX = "WDB_PROBE_SELFTEST "
MTFM_PREFIX = "WDB_MTFM "

MODE_BUILD_RUN = "build-run"
MODE_RUN = "run"

# CLI 退出码（Cli.cs / CliExit）
CLI_OK = 0
CLI_FAILED = 5
CLI_CANCELED = 6
CLI_TIMEOUT = 4  # CliExit.Timeout：Worker 以 timeout 终态收尾时 `wdb wait` 的退出码

TERMINAL_STATES = {"succeeded", "failed", "canceled", "timeout", "rejected", "interrupted"}

# 唯一的同步点：CLI 契约（逐字对照 Cli.cs 的 --help 与 CliArgs.ValueOptions）。
# 这里刻意**不含** worker / shutdown —— static.self-audit 会断言这一点。
WDB_VERBS: dict[str, list[str]] = {
    "run": ["run", "--queue", "{queue}", "--source", "{source}", "--project", "{project}", "--timeout", "{job_timeout}", "--json"],
    "build": ["build", "--queue", "{queue}", "--source", "{source}", "--project", "{project}", "--timeout", "{job_timeout}", "--json"],
    "build-run": ["build-run", "--queue", "{queue}", "--source", "{source}", "--project", "{project}", "--timeout", "{job_timeout}", "--json"],
    "status": ["status", "{job}", "--queue", "{queue}", "--json"],
    "queue-status": ["status", "--queue", "{queue}", "--json"],
    "wait": ["wait", "{job}", "--queue", "{queue}", "--wait-timeout", "{wait_timeout}", "--json"],
    "stop": ["stop", "{job}", "--queue", "{queue}", "--reason", "{reason}"],
    # day2：多 TFM 正向必须能显式传 --framework（不污染 day1 的基础模板）
    "build-run-fw": ["build-run", "--queue", "{queue}", "--source", "{source}", "--project", "{project}",
                     "--framework", "{framework}", "--timeout", "{job_timeout}", "--json"],
    # day2：plain 日志（不给 --json）—— stdout 承载原样文本、stderr 承载 stderr 流文本
    "logs": ["logs", "{job}", "--queue", "{queue}", "--stream", "{stream}", "--offset", "{offset}",
             "{--follow|follow}", "{--json|json}"],
    "help": ["--help"],
    "version": ["--version"],
}
FORBIDDEN_WORDS = ("worker", "shutdown", "--allow-non-windows")
PASSTHROUGH_VERBS = {"run", "build-run", "build-run-fw"}

# 被测 argv：空格/引号/反斜杠/制表/中文 emoji/百分号/美元花括号/前导横线/空串/元字符。
ARGV_EDGE = [
    "--exit", "0",
    "plain",
    "with space",
    'quote"inside',
    "back\\slash",
    "trailing\\",
    "tab\there",
    "中文与emoji😀",
    "%PATH%",
    "${HOME}",
    "--leading-dash",
    "a=b;c|d&e>f<g^h",
    "",
]

# 边界空白码点：.NET 的 PasteArguments 判定为需要引号（char.IsWhiteSpace || '"'）。
# 历史实现只认 {' ', '\t', '\n', '\v', '"'}，这些码点是与运行时不等价的边界，单列用例看住。
ARGV_WHITESPACE_EDGE = [
    "--exit", "0",
    "cr\rhere",
    "ff\fhere",
    "vt\vhere",
    "nbsp\u00a0here",
    "ideo\u3000space",
    "nel\u0085here",
]

TREE_ARGS = ["--spawn", "1", "--depth", "2", "--repeat", "--pidfile", "tree-pids.txt",
             "--level-file", "tree-levels.jsonl"]
TREE_LAYERS = 3
TREE_PIDFILE_NAME = "tree-pids.txt"        # 裸格式：每行一个 PID（保持与 Linux/ProcessProbe 兼容）
TREE_LEVELFILE_NAME = "tree-levels.jsonl"  # 每层一行 JSON：pid/内核父 PID/level —— 用于证明「3 级链」


def judge_tree_chain(records: list[dict] | None, expected_layers: int = TREE_LAYERS) -> tuple[bool, list[str], list[dict]]:
    """纯判据：records 是否构成**恰好 expected_layers 级、逐级父子相连**的链。

    只数个数是不够的 —— 「root + 两个直系兄弟」同样有 3 个进程。链证明要求：
      * 记录数恰为 N；PID 互不相同；
      * 每个 level 0..N-1 **恰好一条**记录（两个 level=1 的兄弟会在这里被拒）；
      * 对每个 level L>0：`parentPidKernel == level(L-1).pid`（内核真值，非自报）。
    """
    problems: list[str] = []
    if not records:
        return False, ["no-records"], []
    pids = [r.get("pid") for r in records]
    if len(records) != expected_layers:
        problems.append(f"record-count={len(records)}!={expected_layers}")
    if len(set(pids)) != len(pids):
        problems.append("duplicate-pid")

    by_level: dict[int, list[dict]] = {}
    for r in records:
        level = r.get("level")
        if not isinstance(level, int):
            problems.append(f"bad-level:{level!r}")
            continue
        by_level.setdefault(level, []).append(r)

    levels = sorted(by_level)
    if levels != list(range(expected_layers)):
        problems.append(f"levels={levels}")
    for level, group in sorted(by_level.items()):
        if len(group) != 1:
            problems.append(f"level{level}-count={len(group)}")  # ← root+两个直系兄弟在此被拒

    chain: list[dict] = []
    for level in range(1, expected_layers):
        child = (by_level.get(level) or [None])[0]
        parent = (by_level.get(level - 1) or [None])[0]
        if not child or not parent:
            break
        ok = child.get("parentPidKernel") == parent.get("pid")
        chain.append({"level": level, "childPid": child.get("pid"), "parentPid": parent.get("pid"),
                      "kernelPpid": child.get("parentPidKernel"), "linked": ok})
        if not ok:
            problems.append(f"level{level}-kernel-ppid={child.get('parentPidKernel')}!=parent={parent.get('pid')}")
    return (not problems), problems, chain


def judge_os_chain(entries: list[dict] | None, expected_layers: int = TREE_LAYERS) -> tuple[bool, list[str], list[int]]:
    """纯判据：**仅凭 Windows 内核报告的 (pid, ppid)** 判定是否构成 N 级单链。

    与 judge_tree_chain（读夹具自报 sidecar）互为独立来源：这里不看夹具说了什么，
    只看 `Win32_Process.ProcessId/ParentProcessId`。
    「root + 两个直系兄弟」在这里必然被抓：根节点有 2 个子进程。
    """
    problems: list[str] = []
    if not entries:
        return False, ["no-os-entries"], []
    by_pid = {int(e["pid"]): e for e in entries}
    if len(by_pid) != len(entries):
        problems.append("duplicate-os-pid")
    if len(entries) != expected_layers:
        problems.append(f"os-entries={len(entries)}!={expected_layers}")
    roots = [e for e in entries if int(e["ppid"]) not in by_pid]
    if len(roots) != 1:
        problems.append(f"roots={[int(r['pid']) for r in roots]}")
        return False, problems, []
    levels: dict[int, dict] = {}
    current: dict | None = roots[0]
    level = 0
    while current is not None and level < expected_layers + 1:
        levels[level] = current
        kids = [e for e in entries if int(e["ppid"]) == int(current["pid"])]
        if len(kids) > 1:
            problems.append(f"level{level}-has-{len(kids)}-children")
        current = kids[0] if len(kids) == 1 else None
        level += 1
    if len(levels) != expected_layers:
        problems.append(f"os-chain-depth={len(levels)}!={expected_layers}")
    return (not problems), problems, [int(levels[k]["pid"]) for k in sorted(levels)]


def judge_live_follow(events: list[dict] | None, terminal_t: float | None,
                      min_while_running: int = 3) -> tuple[bool, list[str]]:
    """纯判据：events 是否证明「跟随期间真增量」，而不是「终态一次性补读」。

    events 每条：{"index": 行号, "t": 单调到达时刻, "state": 到达时的任务状态, "fileSize": 到达时日志文件大小}
    要求：
      * 至少 min_while_running 条 payload 行是**任务仍 running 时**到达的；
      * 首条 payload 到达时刻严格早于终态时刻（若给了 terminal_t）；
      * 这些 running 期间到达的行之间存在文件继续增长（中段 size 严格增大）。
    只由终态补读产生的样本（全部 state != running，或只有一条）必然被判负。
    """
    problems: list[str] = []
    events = events or []
    if len(events) < min_while_running:
        problems.append(f"payload-events={len(events)}<{min_while_running}")
    running = [e for e in events if e.get("state") == "running"]
    if len(running) < min_while_running:
        problems.append(f"while-running={len(running)}<{min_while_running}")
    if events and terminal_t is not None and not (events[0].get("t", float("inf")) < terminal_t):
        problems.append("first-arrival-not-before-terminal")
    # 中段增长：只看**已知**的文件大小（stat 失败记为 -1，不计入）；已知样本不足 2 条也算判负。
    sizes = [int(e.get("fileSize")) for e in running if int(e.get("fileSize") or -1) >= 0]
    if len(sizes) < 2:
        problems.append(f"unknown-file-sizes:{sizes}")
    elif sizes[-1] <= sizes[0]:
        problems.append(f"no-midrun-growth:{sizes}")
    return (not problems), problems


BROKEN_CSPROJ = """<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <OutputType>Exe</OutputType>
    <TargetFramework>net10.0</TargetFramework>
    <AssemblyName>BrokenEcho</AssemblyName>
    <RootNamespace>BrokenEcho</RootNamespace>
    <ImplicitUsings>enable</ImplicitUsings>
  </PropertyGroup>
</Project>
"""

BROKEN_PROGRAM = """// windows-smoke 专用的坏源码：必须在**构建阶段**失败，用来证明「构建失败 → 绝不启动目标进程」。
// 故意写成语义无法解析的表达式（CS1002/CS1026/CS1519 级），不是缺包也不是缺 SDK。
internal static class Program
{
    private static int Main(string[] args)
    {
        this is intentionally not C#
        return 0
    }
}
"""

NOT_TESTED = [
    "Avalonia/Desktop GUI 窗口与真实画廊应用（本脚本只跑 EchoApp / LogSpinner / MultiTfm / ProcessProbe 等夹具，不是 GUI 验收，不得宣称 GUI 已通过；day2 覆盖的 timeout/多 TFM/日志字节/非 UTF-8 策略也都不含 GUI 窗口）",
    "非 wsl.localhost 形式的队列根（Windows 本地盘或 SMB/网络盘）：默认队列由 Windows 侧经 \\\\wsl.localhost UNC 访问，已随全部 win 用例覆盖；未测的是 --queue 指向 Windows 本地盘或 SMB 的情形",
    "Worker 自身已处于 Job Object 中时的嵌套 AssignProcessToJobObject（取决于宿主启动方式）",
    "KILL_ON_JOB_CLOSE 级联杀树（需强杀 Worker 才能验，本脚本绝不杀用户的 Worker）",
    "控制台代码页（CP）未改变也未测；非 UTF-8 裸字节只记录了**策略**——Worker 的日志通道是 UTF-8 文本通道，非法字节被替换为 U+FFFD（day2.nonutf8-boundaries），本脚本不主张支持非 UTF-8 透传",
    "Worker 硬超时终态 timeout 已由 day2.timeout-treekill-probe 覆盖（--timeout 含构建耗时，state=timeout、CLI wait=4、Job 杀净三层树）；未覆盖的是 300s 量级长等待与「超时后重入/多次超时」",
    "logs --tail 的窗口语义；plain logs --follow 的**真增量**（payload 行在任务非终态时陆续到达 + 中段文件增长）与逐字节一致性均已由 day2.logs-follow-live-and-bytes 覆盖（并有纯判据反证：终态一次性样本必须判负）",
    "多 TFM / --framework 的 TargetPath 求值已由 day2 覆盖（net10.0 与 net10.0-windows 各自运行成功、不传时 ERR_FRAMEWORK_REQUIRED）；未覆盖 TFM 数 ≠ 2、自定义 TFM 名、多平台组合（如 -android）",
    "非 %LOCALAPPDATA% 位置的 runs-root（Windows 本地盘 C:\\Users\\<user>\\AppData\\Local\\WslDebugBridge\\runs 已随全部 win 用例覆盖；未测 D:\\ 盘或 UNC runs-root）",
]

# --------------------------------------------------------------------------
# 日志 / 子进程 / 断言记录
# --------------------------------------------------------------------------
_LOG_HANDLE = None


def log(message: str = "") -> None:
    print(message, flush=True)
    if _LOG_HANDLE is not None:
        _LOG_HANDLE.write(message + "\n")
        _LOG_HANDLE.flush()


def open_log(path: Path) -> None:
    global _LOG_HANDLE
    path.parent.mkdir(parents=True, exist_ok=True)
    _LOG_HANDLE = open(path, "w", encoding="utf-8")


@dataclass
class Run:
    argv: list[str]
    code: int
    out: str
    err: str
    timed_out: bool = False

    @property
    def cmd(self) -> str:
        return " ".join(quote_arg(a) for a in self.argv)

    def tail(self, limit: int = 1200) -> str:
        return (f"$ {self.cmd}\n[exit={self.code} timed_out={self.timed_out}]\n"
                f"--- stdout ---\n{self.out[-limit:]}\n--- stderr ---\n{self.err[-limit:]}")


def quote_arg(s: str) -> str:
    return f'"{s}"' if any(c in s for c in ' \t"') else s


def kill_tree(proc: subprocess.Popen) -> None:
    try:
        os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
    except Exception:
        try:
            proc.kill()
        except Exception:
            pass


def run(argv: list[str], cwd: Path | None = None, timeout: float = 600.0) -> Run:
    """不经 shell：argv 数组传递，stdin=DEVNULL，独立进程组（超时可整组回收）。"""
    proc = subprocess.Popen(
        [str(a) for a in argv],
        cwd=str(cwd) if cwd else None,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        start_new_session=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        env=os.environ.copy(),
    )
    try:
        out, err = proc.communicate(timeout=timeout)
        timed_out = False
    except subprocess.TimeoutExpired:
        kill_tree(proc)
        out, err = proc.communicate()
        timed_out = True
    return Run([str(a) for a in argv], proc.returncode, out or "", err or "", timed_out)


class CaseFailure(AssertionError):
    pass


@dataclass
class RunBytes:
    """不解码的子进程结果：day2 的日志用例必须按**字节**比对，text 模式会把无效 UTF-8 悄悄替换掉。"""

    argv: list[str]
    code: int
    out: bytes
    err: bytes
    timed_out: bool = False

    @property
    def cmd(self) -> str:
        return " ".join(quote_arg(a) for a in self.argv)


def run_bytes(argv: list[str], cwd: Path | None = None, timeout: float = 300.0) -> RunBytes:
    proc = subprocess.Popen(
        [str(a) for a in argv],
        cwd=str(cwd) if cwd else None,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        start_new_session=True,
        env=os.environ.copy(),
    )
    try:
        out, err = proc.communicate(timeout=timeout)
        timed_out = False
    except subprocess.TimeoutExpired:
        kill_tree(proc)
        out, err = proc.communicate()
        timed_out = True
    return RunBytes([str(a) for a in argv], proc.returncode, out or b"", err or b"", timed_out)


class SkipCase(Exception):
    def __init__(self, reason: str, evidence: dict | None = None) -> None:
        super().__init__(reason)
        self.evidence = evidence or {}


class Rec:
    """逐条断言的记录器：证据 JSON 里能看到每条断言的名字与实测值。"""

    def __init__(self) -> None:
        self.checks: list[dict] = []
        self.failed: list[str] = []

    def check(self, name: str, cond: bool, detail: object = "") -> bool:
        entry: dict = {"check": name, "pass": bool(cond)}
        if detail != "":
            entry["detail"] = _short(detail)
        self.checks.append(entry)
        if not cond:
            self.failed.append(name)
        return bool(cond)

    def equal(self, name: str, expected: object, actual: object) -> bool:
        return self.check(name, expected == actual, {"expected": expected, "actual": actual})

    def equal_bytes(self, name: str, expected: bytes, actual: bytes) -> bool:
        """逐字节比对；失败时给出长度、首个差异偏移与十六进制片段（日志用例的核心断言）。"""
        if expected == actual:
            return self.check(name, True, f"{len(expected)} bytes 相等")
        offset = next((i for i, (a, b) in enumerate(zip(expected, actual)) if a != b), min(len(expected), len(actual)))
        detail = {
            "expectedLen": len(expected),
            "actualLen": len(actual),
            "firstDiffAt": offset,
            "expectedHex": _bhex(expected[max(0, offset - 8):offset + 24]),
            "actualHex": _bhex(actual[max(0, offset - 8):offset + 24]),
        }
        return self.check(name, False, detail)

    def finish(self) -> None:
        if not self.failed:
            return
        details = [f"{e['check']}={e.get('detail', '')}" for e in self.checks if not e["pass"]]
        more = "" if len(details) <= 4 else f" …(+{len(details) - 4})"
        raise CaseFailure("断言失败：" + "; ".join(details[:4]) + more)


def _short(value: object, limit: int = 600) -> object:
    if not isinstance(value, str):
        try:
            value = json.dumps(value, ensure_ascii=False)
        except (TypeError, ValueError):
            value = repr(value)
    return value if len(value) <= limit else value[:limit] + f"…(+{len(value) - limit})"


def _bhex(data: bytes, limit: int = 96) -> str:
    h = data[:limit].hex(" ")
    return h if len(data) <= limit else h + f"…(+{len(data) - limit}B)"


@dataclass
class CaseResult:
    name: str
    status: str  # passed | failed | skipped
    reason: str = ""
    evidence: dict | None = None
    duration: float = 0.0


@dataclass
class Ctx:
    dotnet: str
    wdb_argv_prefix: list[str]
    repo: Path
    queue: Path
    work_root: Path
    evidence_path: Path
    mode: str  # static | full
    job_timeout: int
    wait_timeout: int
    tree_ready_seconds: float
    pidfile_confirm_seconds: float
    clean: bool
    strict_skips: bool
    timeout_case_seconds: int = 20  # day2：timeout 用例提交任务时使用的 --timeout（含构建时间）


# --------------------------------------------------------------------------
# Windows 路径 <-> WSL 路径（纯函数；static.path-mapping 单测）
# --------------------------------------------------------------------------
def win_to_wsl(win_path: str) -> Path | None:
    """C:\\a\\b -> /mnt/c/a/b；\\\\wsl.localhost\\<distro>\\home\\x -> /home/x；真 UNC/未知盘符 -> None。"""
    if not win_path:
        return None
    p = win_path.strip().strip('"').replace("/", "\\")

    m = re.match(r"^\\\\wsl(\$|\.localhost)\\([^\\]+)(\\.*)?$", p)
    if m:
        rest = (m.group(3) or "").lstrip("\\")
        return Path("/" + rest.replace("\\", "/")) if rest else Path("/")

    if p.startswith("\\\\"):
        return None

    m = re.match(r"^([A-Za-z]):\\(.*)$", p)
    if not m:
        return None
    root = Path(f"/mnt/{m.group(1).lower()}")
    if not root.exists():
        return None
    rest = m.group(2).strip("\\")
    return root / rest.replace("\\", "/") if rest else root


def wsl_to_win_unc(linux_path: Path) -> str | None:
    """与 Cli.cs 的 ToWindowsUnc 同款规则（marker 文件走它落回工作区）。"""
    distro = os.environ.get("WSL_DISTRO_NAME", "").strip()
    if not distro:
        return None
    return "\\\\wsl.localhost\\" + distro + str(Path(linux_path).resolve()).replace("/", "\\")


def is_windows_drive_path(value: str) -> bool:
    return bool(re.match(r"^[A-Za-z]:\\", value or ""))


def is_unc_path(value: str) -> bool:
    v = (value or "").lstrip()
    return v.startswith("\\\\") or v.startswith("//")


def win_join(directory: str, name: str) -> str:
    return directory.rstrip("\\/") + "\\" + name


def strip_comments(text: str) -> str:
    """粗略去掉行注释，用于「源码里不得出现 Kill( 之类」的静态自审。"""
    return "\n".join(line.split("//", 1)[0] for line in text.splitlines())


# --------------------------------------------------------------------------
# 桥 CLI 适配层（唯一与 Cli.cs 同步的地方）
# --------------------------------------------------------------------------
class Bridge:
    def __init__(self, ctx: Ctx) -> None:
        self.ctx = ctx

    def argv(self, verb: str, app_args: list[str] | None = None, **kwargs) -> list[str]:
        out: list[str] = []
        for token in WDB_VERBS[verb]:
            if token.startswith("{") and token.endswith("}"):
                inner = token[1:-1]
                if "|" in inner:
                    # 形如 {--follow|follow}：仅在 kwargs['follow'] 为真时追加布尔开关
                    flag, key = inner.split("|", 1)
                    if kwargs.get(key):
                        out.append(flag)
                    continue
                value = kwargs.get(inner)
                if value is None:
                    # 可选占位符缺失：把刚压入的独立选项一并撤回，避免悬空的 "--timeout"
                    if out and out[-1].startswith("--"):
                        out.pop()
                    continue
                out.append(str(value))
            else:
                out.append(token)
        if app_args:
            if verb not in PASSTHROUGH_VERBS:
                raise AssertionError(f"{verb} 不支持 '--' 透传")
            out.append("--")
            out.extend(str(a) for a in app_args)
        return [*self.ctx.wdb_argv_prefix, *out]

    def call(self, verb: str, proc_timeout: float = 600.0, app_args: list[str] | None = None, **kwargs) -> Run:
        return run(self.argv(verb, app_args=app_args, **kwargs), cwd=self.ctx.repo, timeout=proc_timeout)

    def submit(self, verb: str, project: str, source: Path, app_args: list[str] | None = None,
               proc_timeout: float = 600.0, job_timeout: int | None = None,
               framework: str | None = None) -> tuple[str, Run]:
        r = self.call(verb, proc_timeout=proc_timeout, app_args=app_args, queue=self.ctx.queue,
                      source=source, project=project,
                      job_timeout=self.ctx.job_timeout if job_timeout is None else job_timeout,
                      framework=framework)
        job_id = ""
        for line in (r.out or "").splitlines():
            line = line.strip()
            if line.startswith("{"):
                try:
                    payload = json.loads(line)
                except json.JSONDecodeError:
                    continue
                job_id = payload.get("id") or ""
                if job_id:
                    break
        return job_id, r

    def status(self, job_id: str | None = None) -> dict | None:
        verb = "status" if job_id else "queue-status"
        r = self.call(verb, proc_timeout=120.0, job=job_id, queue=self.ctx.queue)
        if r.code != CLI_OK:
            return None
        for line in (r.out or "").splitlines():
            line = line.strip()
            if line.startswith("{"):
                try:
                    return json.loads(line)
                except json.JSONDecodeError:
                    continue
        return None

    def wait(self, job_id: str) -> Run:
        return self.call("wait", proc_timeout=self.ctx.wait_timeout + 60.0, job=job_id,
                         queue=self.ctx.queue, wait_timeout=self.ctx.wait_timeout)

    def stop(self, job_id: str, reason: str) -> Run:
        return self.call("stop", proc_timeout=120.0, job=job_id, queue=self.ctx.queue, reason=reason)

    def logs_bytes(self, job_id: str, stream: str, offset: int = 0, follow: bool = False,
                   json_mode: bool = False, proc_timeout: float = 300.0) -> "RunBytes":
        """plain logs：**不解码**，把 CLI 的原始字节拿回来做逐字节断言。"""
        argv = self.argv("logs", job=job_id, queue=self.ctx.queue, stream=stream, offset=offset,
                         follow=follow, json=json_mode)
        return run_bytes(argv, cwd=self.ctx.repo, timeout=proc_timeout)

    def queue_idle(self) -> tuple[bool, dict]:
        """队列是否空闲（无 running/queued 任务）——只在**提交任何任务之前**用，避免把别人的长任务当成自己的超时。"""
        view = self.status(None) or {}
        busy = [{"id": j.get("id"), "state": j.get("state")} for j in (view.get("jobs") or [])
                if j.get("state") not in TERMINAL_STATES]
        current = ((view.get("worker") or {}).get("state") or {}).get("currentJobId")
        return (not busy and not current), {"busy": busy, "currentJobId": current}


@dataclass
class JobArtifacts:
    job_id: str
    job_dir: Path

    @property
    def stdout_path(self) -> Path:
        return self.job_dir / "stdout.log"

    @property
    def stderr_path(self) -> Path:
        return self.job_dir / "stderr.log"

    def stdout_text(self) -> str:
        return self._read(self.stdout_path)

    def stderr_text(self) -> str:
        return self._read(self.stderr_path)

    def stdout_bytes(self) -> bytes:
        return self._read_bytes(self.stdout_path)

    def stderr_bytes(self) -> bytes:
        return self._read_bytes(self.stderr_path)

    @staticmethod
    def _read(path: Path) -> str:
        try:
            return path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            return ""

    @staticmethod
    def _read_bytes(path: Path) -> bytes:
        try:
            return path.read_bytes()
        except OSError:
            return b""

    def find_prefixed_json(self, prefix: str) -> dict | None:
        found = None
        for line in self.stdout_text().splitlines():
            line = line.strip()
            if line.startswith(prefix):
                try:
                    found = json.loads(line[len(prefix):])
                except json.JSONDecodeError:
                    continue
        return found

    def has_prefix(self, prefix: str) -> bool:
        return any(line.strip().startswith(prefix) for line in self.stdout_text().splitlines())


def job_artifacts(ctx: Ctx, job_id: str) -> JobArtifacts:
    return JobArtifacts(job_id, ctx.queue / "jobs" / job_id)


def wait_terminal(bridge: Bridge, job_id: str) -> tuple[Run, dict | None]:
    r = bridge.wait(job_id)
    status = None
    for line in (r.out or "").splitlines():
        line = line.strip()
        if line.startswith("{"):
            try:
                status = json.loads(line)
            except json.JSONDecodeError:
                continue
    if status is None:
        status = bridge.status(job_id)
    return r, status


def poll_until(predicate, timeout: float, interval: float = 0.5):
    deadline = time.time() + timeout
    while time.time() < deadline:
        got = predicate()
        if got:
            return got
        time.sleep(interval)
    return None


# --------------------------------------------------------------------------
# Source 准备（只写工作区；绝不改原 fixture）
# --------------------------------------------------------------------------
def copy_fixture(source: Path, dest: Path) -> Path:
    """bin/obj 必须排除：否则会把 Linux 侧 apphost 一起快照进 Windows 构建目录。

    重复调用（同一 work-root 复跑同一用例）时先清掉上一次的副本；只清「确实是本次 fixture 副本」
    的目录（必须含同名 csproj），否则宁可报错也不删别人的东西。
    """
    dest.parent.mkdir(parents=True, exist_ok=True)
    if dest.exists():
        if not (dest / (source.name + ".csproj")).is_file():
            raise RuntimeError(f"拒绝覆盖看起来不是 fixture 副本的目录：{dest}")
        shutil.rmtree(dest)
    shutil.copytree(source, dest, ignore=shutil.ignore_patterns("bin", "obj", ".git", ".tmp"))
    return dest


def make_broken_source(dest: Path) -> Path:
    project_dir = dest / "BrokenEcho"
    project_dir.mkdir(parents=True, exist_ok=True)
    (project_dir / "BrokenEcho.csproj").write_text(BROKEN_CSPROJ, encoding="utf-8")
    (project_dir / "Program.cs").write_text(BROKEN_PROGRAM, encoding="utf-8")
    return project_dir


# --------------------------------------------------------------------------
# 用例注册（显式函数引用，不做名字反射）
# --------------------------------------------------------------------------
CASES: list[tuple[str, bool, str, object, str]] = []  # (name, needs_worker, doc, fn, group)


def case(name: str, needs_windows: bool = True, doc: str = "", group: str = "base"):
    def deco(fn):
        CASES.append((name, needs_windows, doc, fn, group))
        return fn
    return deco


def track(shared: dict, job_id: str) -> str:
    if job_id and job_id not in shared["createdJobs"]:
        shared["createdJobs"].append(job_id)
    return job_id


# ---- 静态用例（不需要 Worker） -------------------------------------------

@case("static.self-audit", needs_windows=False, doc="脚本自身不得含 worker/--allow-non-windows/shutdown 契约")
def case_self_audit(ctx: Ctx, rec: Rec) -> dict:
    tokens = [t for template in WDB_VERBS.values() for t in template]
    bad = [w for w in ("worker", "shutdown") if w in WDB_VERBS]
    rec.equal("no-forbidden-verb", [], bad)
    rec.check("no-allow-non-windows-token", not any(t == "--allow-non-windows" for t in tokens),
              [t for t in tokens if "allow-non-windows" in t])
    rec.equal("verb-allowlist",
              sorted(["run", "build", "build-run", "build-run-fw", "status", "queue-status", "wait", "stop",
                      "logs", "help", "version"]),
              sorted(WDB_VERBS))
    rec.check("passthrough-only-run-modes",
              PASSTHROUGH_VERBS == {"run", "build-run", "build-run-fw"}, sorted(PASSTHROUGH_VERBS))
    return {"verbs": sorted(WDB_VERBS), "forbiddenWords": list(FORBIDDEN_WORDS),
            "note": "本脚本只提交/查询/停止自己的任务；没有 worker 与 shutdown 通道，故不可能启动或关闭用户的 Worker"}


@case("static.path-mapping", needs_windows=False, doc="Windows<->WSL 路径映射纯函数")
def case_path_mapping(ctx: Ctx, rec: Rec) -> dict:
    ev: dict = {"distro": os.environ.get("WSL_DISTRO_NAME", ""), "wslHostname": platform.node()}
    rec.equal("unc-none", None, win_to_wsl("\\\\server\\share\\x"))
    rec.equal("wsl-localhost", "/home/u/x", str(win_to_wsl("\\\\wsl.localhost\\ai-dev\\home\\u\\x")))
    rec.equal("wsl-dollar", "/home/u/x", str(win_to_wsl("\\\\wsl$\\ai-dev\\home\\u\\x")))
    rec.check("drive-detect", is_windows_drive_path("C:\\a") and not is_windows_drive_path("\\\\a\\b"))
    rec.check("unc-detect", is_unc_path("\\\\srv\\sh") and is_unc_path("//srv/sh") and not is_unc_path("C:\\a"))
    rec.equal("win-join", "C:\\a\\b\\" + TREE_PIDFILE_NAME, win_join("C:\\a\\b\\", TREE_PIDFILE_NAME))
    unc = wsl_to_win_unc(Path("/home/u/x"))
    rec.check("wsl-unc-shape", unc is None or unc.startswith("\\\\wsl"), unc)
    ev["sampleUnc"] = unc
    ev["mntSample"] = sorted(p.name for p in Path("/mnt").glob("*") if p.is_dir())[:8] if Path("/mnt").is_dir() else []

    if Path("/mnt/c").is_dir():
        rec.equal("drive-lower", "/mnt/c/Users/x/y", str(win_to_wsl("C:\\Users\\x\\y")))
        rec.equal("drive-upper", "/mnt/c/a/b", str(win_to_wsl("c:\\a\\b")))
        rec.equal("forward-slashes", "/mnt/d/a/b", str(win_to_wsl("D:/a/b")) if Path("/mnt/d").is_dir()
                  else str(win_to_wsl("C:/a/b")).replace("/mnt/c", "/mnt/d"))
    else:
        ev["note"] = "没有 /mnt/c → 盘符映射断言跳过（三层树用例也会因此 SKIP）"
    ev["unknownDrive"] = win_to_wsl("Q:\\nope")
    return ev


@case("static.fixtures", needs_windows=False, doc="fixture 齐备、坏源码可生成、fixture 副本排除 bin/obj")
def case_fixtures(ctx: Ctx, rec: Rec) -> dict:
    rec.check("echoapp-csproj", (FIXTURE_ECHO / "EchoApp.csproj").is_file())
    rec.check("echoapp-program", (FIXTURE_ECHO / "Program.cs").is_file())
    rec.check("probe-csproj", (FIXTURE_PROBE / "ProcessProbe.csproj").is_file())
    rec.check("probe-program", (FIXTURE_PROBE / "Program.cs").is_file())

    probe_code = strip_comments((FIXTURE_PROBE / "Program.cs").read_text(encoding="utf-8"))
    for banned in ("Kill(", "CloseMainWindow", "taskkill", "TerminateProcess"):
        rec.check(f"probe-code-has-no-{banned.strip('(')}", banned not in probe_code, banned)
    rec.check("probe-queries-getprocessbyid", "Process.GetProcessById" in probe_code)
    rec.check("probe-rejects-query-error-as-gone", "query_error" in probe_code and "ArgumentException" in probe_code)

    # day2 夹具（本脚本自有）
    rec.check("spinner-csproj", (FIXTURE_SPINNER / "LogSpinner.csproj").is_file())
    rec.check("spinner-program", (FIXTURE_SPINNER / "Program.cs").is_file())
    rec.check("mtfm-csproj", (FIXTURE_MTFM / "MultiTfm.csproj").is_file())
    rec.check("mtfm-program", (FIXTURE_MTFM / "Program.cs").is_file())
    # 多 TFM 夹具必须自带空 Directory.Build.props 隔断，否则会继承仓库根的单数 TargetFramework
    rec.check("mtfm-shields-root-props", (FIXTURE_MTFM / "Directory.Build.props").is_file())
    mtfm_text = (FIXTURE_MTFM / "MultiTfm.csproj").read_text(encoding="utf-8")
    declared = re.search(r"<TargetFrameworks>([^<]*)</TargetFrameworks>", mtfm_text)
    tfms = [t.strip() for t in (declared.group(1).split(";") if declared else []) if t.strip()]
    rec.equal("mtfm-source-declares-two", ["net10.0", "net10.0-windows"], tfms)
    rec.check("mtfm-source-has-enable-windows-targeting", "<EnableWindowsTargeting>true</EnableWindowsTargeting>" in mtfm_text)
    rec.check("mtfm-source-has-no-singular-tfm", "<TargetFramework>" not in mtfm_text)
    echo_text = (FIXTURE_ECHO / "EchoApp.csproj").read_text(encoding="utf-8")
    rec.check("echo-single-tfm-has-no-plural", "<TargetFrameworks" not in echo_text and "<TargetFramework>" in echo_text)

    # XML 注释里出现 ASCII 双横线是非法的（MSB4025）——2026-10-12 真机就是这么把两个新夹具打废的，
    # 这里做静态守卫：本脚本自有的 csproj/props 注释内不得出现 "--"。
    offenders = []
    for path in (FIXTURE_SPINNER / "LogSpinner.csproj", FIXTURE_MTFM / "MultiTfm.csproj",
                 FIXTURE_MTFM / "Directory.Build.props", FIXTURE_PROBE / "ProcessProbe.csproj"):
        text = path.read_text(encoding="utf-8")
        for lineno, line in enumerate(text.splitlines(), 1):
            stripped = line.replace("<!--", "").replace("-->", "")
            if "--" in stripped:
                offenders.append(f"{path.name}:{lineno}: {line.strip()[:80]}")
    rec.check("no-ascii-double-dash-in-xml-comments", not offenders, offenders)

    # 更硬的守卫：夹具 csproj/props 必须真的是合法 XML（ElementTree 与 MSBuild 同样拒绝注释里的 "--"）
    xml_errors = []
    for path in (FIXTURE_SPINNER / "LogSpinner.csproj", FIXTURE_MTFM / "MultiTfm.csproj",
                 FIXTURE_MTFM / "Directory.Build.props", FIXTURE_PROBE / "ProcessProbe.csproj",
                 FIXTURE_ECHO / "EchoApp.csproj"):
        try:
            ET.parse(path)
        except ET.ParseError as exc:
            xml_errors.append(f"{path.name}: {exc}")
    rec.check("fixture-xml-parses", not xml_errors, xml_errors)

    staged = ctx.work_root / "static" / "broken-src"
    make_broken_source(staged)
    rec.check("broken-csproj-generated", (staged / "BrokenEcho" / "BrokenEcho.csproj").is_file())
    rec.check("broken-program-generated", (staged / "BrokenEcho" / "Program.cs").is_file())

    echo_copy = copy_fixture(FIXTURE_ECHO, ctx.work_root / "static" / "echo-src" / "EchoApp")
    rec.check("fixture-copy-excludes-bin", not (echo_copy / "bin").exists() and not (echo_copy / "obj").exists())
    rec.check("fixture-copy-has-program", (echo_copy / "Program.cs").is_file())
    return {"brokenDir": str(staged / "BrokenEcho"), "echoCopy": str(echo_copy)}


@case("static.wdb-cli", needs_windows=False, doc="桥 CLI --help/--version 可用（help 走 stderr）")
def case_wdb_cli(ctx: Ctx, rec: Rec) -> dict:
    bridge = Bridge(ctx)
    h = bridge.call("help", proc_timeout=120.0)
    rec.equal("help-exit", CLI_OK, h.code)
    text = h.err + h.out
    for verb in ("run", "build", "build-run", "status", "logs", "wait", "stop", "doctor"):
        rec.check(f"help-mentions-{verb}", verb in text)
    v = bridge.call("version", proc_timeout=120.0)
    rec.equal("version-exit", CLI_OK, v.code)
    rec.check("version-semver", bool(re.match(r"^\d+\.\d+\.\d+", v.out.strip())), v.out.strip())
    return {"cli": ctx.wdb_argv_prefix, "version": v.out.strip()}


@case("static.tree-and-follow-judges", needs_windows=False,
      doc="纯判据反证：3 级链判据拒绝 root+两个直系兄弟；增量判据拒绝终态一次性样本")
def case_static_judges(ctx: Ctx, rec: Rec) -> dict:
    # --- 链判据 ---
    chain = [{"pid": 10, "level": 0, "parentPidKernel": 999},
             {"pid": 11, "level": 1, "parentPidKernel": 10},
             {"pid": 12, "level": 2, "parentPidKernel": 11}]
    ok, problems, links = judge_tree_chain(chain, 3)
    rec.check("chain-accepts-real-3-level", ok, {"problems": problems, "links": links})
    rec.check("chain-links-are-verified", len(links) == 2 and all(l["linked"] for l in links), links)

    siblings = [{"pid": 10, "level": 0, "parentPidKernel": 999},
                {"pid": 11, "level": 1, "parentPidKernel": 10},
                {"pid": 12, "level": 1, "parentPidKernel": 10}]  # root + 两个直系兄弟：同样是 3 个进程
    ok2, problems2, _ = judge_tree_chain(siblings, 3)
    rec.check("chain-rejects-root-plus-two-siblings", not ok2, problems2)
    rec.check("chain-names-the-reason", any("level1-count=2" in p for p in problems2), problems2)

    flat2 = [{"pid": 10, "level": 0, "parentPidKernel": 999}, {"pid": 11, "level": 1, "parentPidKernel": 10}]
    rec.check("chain-rejects-wrong-count", not judge_tree_chain(flat2, 3)[0])
    broken = [{"pid": 10, "level": 0, "parentPidKernel": 999},
              {"pid": 11, "level": 1, "parentPidKernel": 10},
              {"pid": 12, "level": 2, "parentPidKernel": 77}]  # 断链：孙的内核父不是子
    ok3, problems3, _ = judge_tree_chain(broken, 3)
    rec.check("chain-rejects-broken-link", not ok3, problems3)
    rec.check("chain-rejects-empty", not judge_tree_chain([], 3)[0])

    # --- 内核链判据（独立来源：Win32_Process 的 pid/ppid）---
    os_chain = [{"pid": 10, "ppid": 777}, {"pid": 11, "ppid": 10}, {"pid": 12, "ppid": 11}]
    ok_os, p_os, order_os = judge_os_chain(os_chain, 3)
    rec.check("os-chain-accepts-real-3-level", ok_os, {"problems": p_os, "order": order_os})
    rec.equal("os-chain-order", [10, 11, 12], order_os)
    os_siblings = [{"pid": 10, "ppid": 777}, {"pid": 11, "ppid": 10}, {"pid": 12, "ppid": 10}]
    ok_os2, p_os2, _ = judge_os_chain(os_siblings, 3)
    rec.check("os-chain-rejects-root-plus-two-siblings", not ok_os2, p_os2)
    rec.check("os-chain-names-two-children", any("2-children" in p for p in p_os2), p_os2)
    rec.check("os-chain-rejects-two-roots",
              not judge_os_chain([{"pid": 10, "ppid": 1}, {"pid": 11, "ppid": 2}, {"pid": 12, "ppid": 11}], 3)[0])
    rec.check("os-chain-rejects-empty", not judge_os_chain([], 3)[0])

    # --- OS 查询卫生 + 源 gating（全部离线可判，不占队列）---
    script = powershell_query_script([11, 22])
    rec.check("os-query-requests-only-needed-fields",
              "ProcessId,ParentProcessId,Name" in script and "Command" + "Line" not in script,
              {"script": script, "why": "没人消费命令行字段，就绝不请求它（避免把命令行里的敏感内容带进证据）"})
    rec.check("os-query-filters-server-side",
              '-Filter "ProcessId=11 OR ProcessId=22"' in script and "Where-Object" not in script,
              {"script": script, "why": "PID 过滤必须在 server 端，不能拉回全机进程再客户端筛"})
    empty = windows_query_processes([])
    rec.check("os-query-empty-ids-fail-closed",
              empty.get("source") is None and empty.get("ok") is False and empty.get("script") is None,
              {"source": empty.get("source"), "ok": empty.get("ok"), "script": empty.get("script"),
               "note": empty.get("note")})
    rec.check("os-query-empty-ids-does-not-record-raw", not (empty.get("raw") or "").strip(), empty.get("raw"))

    ps_sample = {"source": "powershell", "ok": True, "missing": [],
                 "entries": [{"pid": 10, "ppid": 5, "name": "EchoApp.exe"},
                             {"pid": 11, "ppid": 10, "name": "EchoApp.exe"}]}
    rec.check("strong-source-accepts-powershell-with-ppids", judge_os_source_strong(ps_sample)[0],
              judge_os_source_strong(ps_sample))
    tasklist_sample = {"source": "tasklist", "ok": True, "limited": True, "missing": [],
                       "entries": [{"pid": 10, "ppid": -1, "name": "EchoApp.exe"}]}
    tl_ok, tl_reason = judge_os_source_strong(tasklist_sample)
    rec.check("strong-source-rejects-tasklist-fallback", not tl_ok, {"ok": tl_ok, "reason": tl_reason})
    rec.check("strong-source-rejects-no-interop", not judge_os_source_strong({"source": None})[0])
    rec.check("strong-source-rejects-unknown-ppid",
              not judge_os_source_strong({"source": "powershell", "ok": True, "missing": [],
                                          "entries": [{"pid": 10, "ppid": -1, "name": "x"}]})[0])
    rec.check("strong-source-rejects-missing-pids",
              not judge_os_source_strong({"source": "powershell", "ok": False, "missing": [10],
                                          "entries": [{"pid": 11, "ppid": 5, "name": "x"}]})[0])

    # --- 增量判据 ---
    live = [{"index": i, "t": 1.0 + i, "state": "running", "fileSize": 100 + 50 * i} for i in range(5)]
    ok_live, p_live = judge_live_follow(live, terminal_t=10.0, min_while_running=3)
    rec.check("follow-judge-accepts-live-sample", ok_live, p_live)

    end_only = [{"index": i, "t": 9.0, "state": "succeeded", "fileSize": 900} for i in range(8)]
    ok_neg, p_neg = judge_live_follow(end_only, terminal_t=9.0, min_while_running=3)
    rec.check("follow-judge-rejects-end-only-drain", not ok_neg, p_neg)

    too_few = [{"index": i, "t": 1.0 + i, "state": "running", "fileSize": 100 + i} for i in range(2)]
    rec.check("follow-judge-rejects-too-few-live-lines",
              not judge_live_follow(too_few, terminal_t=10.0, min_while_running=3)[0])
    no_growth = [{"index": i, "t": 1.0 + i, "state": "running", "fileSize": 500} for i in range(5)]
    ok_ng, p_ng = judge_live_follow(no_growth, terminal_t=10.0, min_while_running=3)
    rec.check("follow-judge-rejects-no-midrun-growth", not ok_ng, p_ng)
    after_terminal = [{"index": i, "t": 11.0 + i, "state": "running", "fileSize": 100 + i} for i in range(5)]
    rec.check("follow-judge-rejects-first-arrival-after-terminal",
              not judge_live_follow(after_terminal, terminal_t=10.0, min_while_running=3)[0])

    return {"chainPositive": {"ok": ok, "problems": problems, "links": links},
            "chainNegativeSiblings": {"accepted": ok2, "problems": problems2},
            "followPositive": {"ok": ok_live, "problems": p_live},
            "followNegativeEndOnly": {"accepted": ok_neg, "problems": p_neg},
            "note": "两个判据都是纯函数，且都有「必须判负」的反例；真机用例调用的是同一份判据"}


@case("static.probe-build-selftest", needs_windows=False,
      doc="ProcessProbe 仓库外构建 + 自检 + Linux 真实进程判定（只读、不 kill）")
def case_probe_build_selftest(ctx: Ctx, rec: Rec) -> dict:
    src = copy_fixture(FIXTURE_PROBE, ctx.work_root / "static" / "probe-src" / "ProcessProbe")
    artifacts = ctx.work_root / "static" / "probe-artifacts"
    shutil.rmtree(artifacts, ignore_errors=True)
    build = run([ctx.dotnet, "build", str(src / "ProcessProbe.csproj"), "-c", "Release",
                 "--artifacts-path", str(artifacts), "--nologo", "-v", "q"], cwd=ctx.repo, timeout=600.0)
    rec.check("probe-build-exit", build.code == 0, build.tail())
    dll = artifacts / "bin" / "ProcessProbe" / "release" / "ProcessProbe.dll"
    if not rec.check("probe-dll-exists", dll.is_file(), str(dll)):
        return {"build": build.tail(600)}

    selftest = run([ctx.dotnet, str(dll), "--selftest"], cwd=ctx.repo, timeout=120.0)
    payload = _parse_prefixed(selftest.out, PROBE_SELFTEST_PREFIX)
    rec.equal("selftest-exit", CLI_OK, selftest.code)
    rec.check("selftest-json", payload is not None, selftest.tail())
    if payload:
        rec.check("selftest-ok", payload.get("ok") is True,
                  [c for c in payload.get("cases", []) if not c.get("pass")])
        rec.check("selftest-covers-query-error",
                  any("query_error" in str(c.get("name")) for c in payload.get("cases", [])))

    work = ctx.work_root / "static" / "probe-run"
    work.mkdir(parents=True, exist_ok=True)

    gone_file = work / "gone.txt"
    gone_file.write_text("4194303\n", encoding="utf-8")
    r_gone = run([ctx.dotnet, str(dll), "--pidfile", str(gone_file)], cwd=ctx.repo, timeout=120.0)
    j_gone = _parse_prefixed(r_gone.out, PROBE_PREFIX)
    rec.equal("bogus-pid-exit", CLI_OK, r_gone.code)
    rec.check("bogus-pid-allgone", bool(j_gone and j_gone.get("allGone") is True), j_gone)

    empty_file = work / "empty.txt"
    empty_file.write_text("\n   \n", encoding="utf-8")
    r_empty = run([ctx.dotnet, str(dll), "--pidfile", str(empty_file), "--pidfile-wait-seconds", "0"],
                  cwd=ctx.repo, timeout=120.0)
    j_empty = _parse_prefixed(r_empty.out, PROBE_PREFIX)
    rec.equal("empty-pidfile-exit", 2, r_empty.code)
    rec.check("empty-pidfile-is-error", bool(j_empty and j_empty.get("status") == "error"), j_empty)

    sleeper = subprocess.Popen(["sleep", "25"], stdin=subprocess.DEVNULL,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(0.4)
        alive_file = work / "alive.txt"
        alive_file.write_text(f"{sleeper.pid}\n", encoding="utf-8")
        r_alive = run([ctx.dotnet, str(dll), "--pidfile", str(alive_file), "--interval-ms", "100"],
                      cwd=ctx.repo, timeout=120.0)
        j_alive = _parse_prefixed(r_alive.out, PROBE_PREFIX)
        rec.equal("alive-exit", 1, r_alive.code)
        rec.check("alive-detected", bool(j_alive and j_alive.get("alive") == 1
                                        and j_alive.get("allGone") is False), j_alive)
        rec.check("probe-did-not-kill", sleeper.poll() is None, f"poll={sleeper.poll()}")
    finally:
        try:
            sleeper.kill()
            sleeper.wait(timeout=10)
        except Exception:
            pass

    return {"dll": str(dll), "selftestOk": bool(payload and payload.get("ok"))}


# ---- Windows 用例（需要健康的前台 Worker） --------------------------------

@case("win.run-argv-bytes", doc="mode=run：EchoApp 经 dotnet run 启动；os 含 Windows、cwd 为本机盘符、argv 逐字节")
def case_run_argv_bytes(ctx: Ctx, rec: Rec, bridge: Bridge, shared: dict) -> dict:
    src = copy_fixture(FIXTURE_ECHO, ctx.work_root / "case-run" / "src" / "EchoApp")
    job_id, submit = bridge.submit(MODE_RUN, "EchoApp/EchoApp.csproj", src.parent, app_args=ARGV_EDGE)
    rec.check("submit-ok", submit.code == CLI_OK and bool(job_id), submit.tail())
    if not job_id:
        raise SkipCase("提交失败，无法继续")
    track(shared, job_id)
    jr, status = wait_terminal(bridge, job_id)
    art = job_artifacts(ctx, job_id)
    echo = art.find_prefixed_json(ECHO_PREFIX)
    _assert_echo_and_status(rec, jr, status, echo, ARGV_EDGE, art)
    return _echo_evidence(job_id, echo, status,
                          note="run 模式经 dotnet run 二次转递 argv；逐字节不符时先区分 SDK 侧引用化与桥侧转义")


@case("win.buildrun-argv-bytes", doc="mode=build-run：桥直接启动构建产物；argv 逐字节 + artifact 为 Windows 路径 + 哨兵正对照")
def case_buildrun_argv_bytes(ctx: Ctx, rec: Rec, bridge: Bridge, shared: dict) -> dict:
    src = copy_fixture(FIXTURE_ECHO, ctx.work_root / "case-buildrun" / "src" / "EchoApp")
    marker = ctx.work_root / "markers" / f"run-{uuid.uuid4().hex[:8]}.txt"
    marker_unc = wsl_to_win_unc(marker)
    app_args = ["--marker", marker_unc, *ARGV_EDGE] if marker_unc else list(ARGV_EDGE)

    job_id, submit = bridge.submit(MODE_BUILD_RUN, "EchoApp/EchoApp.csproj", src.parent, app_args=app_args)
    rec.check("submit-ok", submit.code == CLI_OK and bool(job_id), submit.tail())
    if not job_id:
        raise SkipCase("提交失败，无法继续")
    track(shared, job_id)
    jr, status = wait_terminal(bridge, job_id)
    art = job_artifacts(ctx, job_id)
    echo = art.find_prefixed_json(ECHO_PREFIX)
    _assert_echo_and_status(rec, jr, status, echo, app_args, art)

    artifact = (status or {}).get("artifact") or ""
    rec.check("artifact-windows-path", is_windows_drive_path(artifact), artifact)
    rec.check("artifact-is-echoapp-dll", artifact.lower().endswith("echoapp.dll"), artifact)
    if echo and (status or {}).get("pid") is not None:
        rec.equal("pid-equals-status-pid", (status or {}).get("pid"), echo.get("pid"))

    marker_ok = False
    if marker_unc:
        content = marker.read_text(encoding="utf-8", errors="replace") if marker.exists() else ""
        marker_ok = bool(content.strip()) and str((echo or {}).get("pid", "")) in content
        rec.check("marker-written-with-pid", marker_ok, {"path": str(marker), "content": _short(content)})
    else:
        rec.check("marker-unc-available", False,
                  "无法构造 \\\\wsl.localhost UNC（WSL_DISTRO_NAME 未设置）→ 哨兵正对照不可用")
    shared["marker_control"] = {"controlOk": marker_ok, "path": str(marker)}

    ev = _echo_evidence(job_id, echo, status,
                        note="桥直接启动 apphost；argv 不经 SDK 二次转递，是 argv 保真性的主证据")
    ev["marker"] = {"path": str(marker), "unc": marker_unc, "controlOk": marker_ok}
    return ev


@case("win.buildrun-broken-no-sentinel", doc="坏源码：构建阶段失败，绝不启动目标进程（无哨兵、stdout.log 无 WDB_ECHO）")
def case_buildrun_broken_no_sentinel(ctx: Ctx, rec: Rec, bridge: Bridge, shared: dict) -> dict:
    src_root = ctx.work_root / "case-broken" / "src"
    make_broken_source(src_root)
    marker = ctx.work_root / "markers" / f"broken-{uuid.uuid4().hex[:8]}.txt"
    marker_unc = wsl_to_win_unc(marker)
    app_args = ["--marker", marker_unc, "--exit", "0"] if marker_unc else ["--exit", "0"]

    job_id, submit = bridge.submit(MODE_BUILD_RUN, "BrokenEcho/BrokenEcho.csproj", src_root, app_args=app_args)
    rec.check("submit-ok", submit.code == CLI_OK and bool(job_id), submit.tail())
    if not job_id:
        raise SkipCase("提交失败，无法继续")
    track(shared, job_id)
    jr, status = wait_terminal(bridge, job_id)
    art = job_artifacts(ctx, job_id)

    rec.equal("wait-exit-failed", CLI_FAILED, jr.code)
    rec.equal("state-failed", "failed", (status or {}).get("state"))
    rec.check("exit-code-nonzero", (status or {}).get("exitCode") not in (None, 0), (status or {}).get("exitCode"))
    rec.check("error-mentions-build", "构建失败" in ((status or {}).get("error") or ""), (status or {}).get("error"))
    # 与 marker 无关的独立证据：目标进程从未运行 → 日志里不可能有 EchoApp 的输出
    rec.check("no-echo-output", not art.has_prefix(ECHO_PREFIX), _short(art.stdout_text()[-800:]))

    control_ok = bool((shared.get("marker_control") or {}).get("controlOk"))
    if marker.exists():
        rec.check("marker-absent", False,
                  {"path": str(marker), "content": _short(marker.read_text(encoding="utf-8", errors="replace"))})
    else:
        rec.check("marker-absent", True, {"path": str(marker), "markerControlValid": control_ok})
    return {
        "jobId": job_id,
        "state": (status or {}).get("state"),
        "exitCode": (status or {}).get("exitCode"),
        "error": (status or {}).get("error"),
        "marker": str(marker),
        "markerControlValid": control_ok,
        "markerControlFrom": "win.buildrun-argv-bytes",
        "stdoutTail": _short(art.stdout_text()[-600:]),
    }


@case("win.argv-whitespace-edge", doc="argv 含 CR/FF/VT/NBSP/U+3000/U+0085：与 .NET PasteArguments 的边界对齐")
def case_argv_whitespace_edge(ctx: Ctx, rec: Rec, bridge: Bridge, shared: dict) -> dict:
    src = copy_fixture(FIXTURE_ECHO, ctx.work_root / "case-argv-edge" / "src" / "EchoApp")
    job_id, submit = bridge.submit(MODE_BUILD_RUN, "EchoApp/EchoApp.csproj", src.parent,
                                   app_args=ARGV_WHITESPACE_EDGE)
    rec.check("submit-ok", submit.code == CLI_OK and bool(job_id), submit.tail())
    if not job_id:
        raise SkipCase("提交失败，无法继续")
    track(shared, job_id)
    jr, status = wait_terminal(bridge, job_id)
    art = job_artifacts(ctx, job_id)
    echo = art.find_prefixed_json(ECHO_PREFIX)
    _assert_echo_and_status(rec, jr, status, echo, ARGV_WHITESPACE_EDGE, art)
    return _echo_evidence(job_id, echo, status,
                          note="这些码点若被判为「不需要引号」，在把 CR/FF/Unicode 空白当分隔符的解析器上会被拆开；"
                               "本用例是「与 .NET 运行时边界一致」的看门狗")


@case("win.tree-cancel-3layer", doc="三层树（spawn1/depth2）cancel：stop 后 wait=canceled(6)，ProcessProbe 判全部 gone")
def case_tree_cancel_3layer(ctx: Ctx, rec: Rec, bridge: Bridge, shared: dict) -> dict:
    src = copy_fixture(FIXTURE_ECHO, ctx.work_root / "case-tree" / "src" / "EchoApp")
    job_id, submit = bridge.submit(MODE_BUILD_RUN, "EchoApp/EchoApp.csproj", src.parent, app_args=TREE_ARGS)
    rec.check("submit-ok", submit.code == CLI_OK and bool(job_id), submit.tail())
    if not job_id:
        raise SkipCase("提交失败，无法继续")
    track(shared, job_id)
    art = job_artifacts(ctx, job_id)
    ev: dict = {"jobId": job_id, "treeArgs": TREE_ARGS}

    echo = poll_until(lambda: art.find_prefixed_json(ECHO_PREFIX),
                      timeout=ctx.tree_ready_seconds, interval=0.5)
    rec.check("tree-started", echo is not None, _short(art.stdout_text()[-600:]))
    if not echo:
        raise SkipCase("长驻任务未在超时内吐出 WDB_ECHO 行，无法取得 cwd/pid", ev)

    ev["echo"] = {k: echo.get(k) for k in ("pid", "ppid", "cwd", "childPids", "os", "isChild")}
    cwd = str(echo.get("cwd") or "")
    rec.check("cwd-windows-drive", is_windows_drive_path(cwd), cwd)
    rec.check("cwd-not-unc", not is_unc_path(cwd), cwd)

    win_pidfile = win_join(cwd, TREE_PIDFILE_NAME)
    ev["pidfileWindows"] = win_pidfile
    ev["knownFromStdout"] = sorted({int(echo.get("pid") or 0),
                                    *[int(p) for p in (echo.get("childPids") or []) if str(p).isdigit()]} - {0})
    # 停前：恰好 3 条裸 PID + sidecar 证明 3 级链（内核父 PID）+ 三层此刻全部存活
    chain = confirm_tree_chain(ctx, bridge, rec, shared, cwd, ctx.pidfile_confirm_seconds, "tree")
    ev["chain"] = chain
    layers_confirmed = (len(chain.get("pids") or []) == TREE_LAYERS
                        and bool(chain.get("chain")) and all(link.get("linked") for link in chain["chain"])
                        and not chain.get("problems"))
    ev["layersConfirmed"] = layers_confirmed

    stop = bridge.stop(job_id, "windows-smoke tree cancel")
    jr, status = wait_terminal(bridge, job_id)
    ev["stopExit"] = stop.code
    ev["waitExit"] = jr.code
    ev["state"] = (status or {}).get("state")
    ev["exitCode"] = (status or {}).get("exitCode")
    rec.equal("stop-exit", CLI_OK, stop.code)
    rec.equal("wait-exit-canceled", CLI_CANCELED, jr.code)
    rec.equal("state-canceled", "canceled", (status or {}).get("state"))
    rec.check("wait-not-timed-out", not jr.timed_out, jr.cmd)

    if not layers_confirmed and not chain.get("observable"):
        # 读不到 = 环境限制：明确跳过，绝不把计数当链结论
        partial = _probe_via_bridge(ctx, rec, bridge, shared, win_pidfile, TREE_LAYERS,
                                    expect_name="EchoApp",
                                    note="pidfile 在 Windows 侧可读；停前未能在 WSL 侧观测到 pidfile/sidecar → 不作三层结论")
        ev["probePartial"] = _probe_summary(partial)
        raise SkipCase(
            f"停前无法从 WSL 观测 pidfile/sidecar（{ev.get('pidfileWsl')} / {ev.get('levelFileWsl')}）→ "
            f"按约定跳过三层断言；stop/wait 证据仍在：wait={jr.code} state={ev['state']}；"
            f"Windows 侧 pidfile 读到的 pidCount={(partial or {}).get('pidCount')}",
            ev)

    probe = _probe_via_bridge(ctx, rec, bridge, shared, win_pidfile, TREE_LAYERS,
                              expect_name="EchoApp",
                              note="停后用 ProcessProbe 在 Windows 侧只读判定这些 PID 是否全部 gone")
    ev["probe"] = _probe_summary(probe)
    rec.check("probe-all-gone", bool(probe) and probe.get("allGone") is True and probe.get("ok") is True,
              {"allGone": (probe or {}).get("allGone"), "ok": (probe or {}).get("ok"),
               "alive": (probe or {}).get("alive"), "queryErrors": (probe or {}).get("queryErrors")})
    rec.equal("probe-gone-count", TREE_LAYERS, (probe or {}).get("gone"))
    rec.equal("probe-query-errors", 0, (probe or {}).get("queryErrors"))
    rec.check("probe-count-at-least-3", int((probe or {}).get("pidCount") or 0) >= TREE_LAYERS,
              (probe or {}).get("pidCount"))
    rec.check("probe-os-windows", "windows" in str((probe or {}).get("os", "")).lower(), (probe or {}).get("os"))
    rec.check("layers-verified", layers_confirmed and bool(probe) and probe.get("allGone") is True,
              {"layersConfirmed": layers_confirmed, "probeAllGone": (probe or {}).get("allGone")})
    return ev


@case("win.postflight", doc="收尾：Worker 仍健康、本脚本创建的任务都到终态（不留悬挂任务）")
def case_postflight(ctx: Ctx, rec: Rec, bridge: Bridge, shared: dict) -> dict:
    view = bridge.status(None)
    rec.check("queue-status-read", view is not None)
    worker = (view or {}).get("worker") or {}
    state = worker.get("state") or {}
    rec.check("worker-still-healthy", worker.get("healthy") is True,
              {"healthy": worker.get("healthy"), "ageSeconds": worker.get("ageSeconds")})
    rec.check("worker-still-windows", not looks_like_linux_worker(state),
              {"queueRoot": state.get("queueRoot"), "runsRoot": state.get("runsRoot")})
    jobs = {j.get("id"): j for j in ((view or {}).get("jobs") or [])}
    unfinished = [{"id": jid, "state": (jobs.get(jid) or {}).get("state")}
                  for jid in shared.get("createdJobs", [])
                  if jid in jobs and (jobs[jid] or {}).get("state") not in TERMINAL_STATES]
    rec.check("all-my-jobs-terminal", not unfinished, unfinished)
    return {"worker": {"pid": state.get("pid"), "hostName": state.get("hostName"),
                       "runsRoot": state.get("runsRoot"), "queueRoot": state.get("queueRoot"),
                       "ageSeconds": worker.get("ageSeconds")},
            "createdJobs": shared.get("createdJobs", []), "unfinished": unfinished}


# ---- day2（2026-10-12）边界用例：真实 Windows 上补 timeout / 多 TFM / 日志字节 / 非 UTF-8 ----

def _read_win_file(cwd: str, relative: str) -> bytes | None:
    """经被启动进程的 Windows cwd 映射回 WSL，读回**Windows 侧真实消费的那个文件**（源副本）。"""
    mapped = win_to_wsl(cwd)
    if mapped is None:
        return None
    try:
        return (mapped / relative.replace("\\", "/")).read_bytes()
    except OSError:
        return None


def _assert_probe_all_gone(rec: Rec, probe: dict | None, layers: int, label: str) -> None:
    rec.check(f"{label}-probe-all-gone",
              bool(probe) and probe.get("allGone") is True and probe.get("ok") is True,
              {"allGone": (probe or {}).get("allGone"), "ok": (probe or {}).get("ok"),
               "alive": (probe or {}).get("alive"), "queryErrors": (probe or {}).get("queryErrors")})
    rec.equal(f"{label}-probe-gone-count", layers, (probe or {}).get("gone"))
    rec.equal(f"{label}-probe-query-errors", 0, (probe or {}).get("queryErrors"))
    rec.check(f"{label}-probe-os-windows", "windows" in str((probe or {}).get("os", "")).lower(),
              (probe or {}).get("os"))


def confirm_tree_chain(ctx: Ctx, bridge: Bridge, rec: Rec, shared: dict, cwd: str, window: float,
                       label: str, verify_liveness: bool = True) -> dict:
    """停/超时**之前**确认「恰好 TREE_LAYERS 级且逐级父子相连」，并确认这些进程此刻全部存活。

    四个判据（两个独立来源，缺一不可）：
      1. 裸 PIDfile（旧格式）恰好 N 条 PID；
      2. sidecar（夹具自报 level + 内核父 PID）构成 N 级链；
      3. **WSL→Windows 互操作**只读查询内核（Win32_Process）——这些 PID 此刻全部存在；
      4. 同一查询报告的 ParentProcessId 独立重建出的链也是 N 级（root + 两个直系兄弟会被拒）。

    ⚠️ 为什么不用 `_probe_via_bridge`：Worker 是**串行**的，经桥提交的查询任务必须等我自己的任务
    结束才会被领取 ⇒ 它拿到的是「任务已经死了」的世界（并且会把 stop 拖到任务超时）。
    因此存活证据改由不占队列的互操作只读查询给出；任务终态之后仍然照旧用 ProcessProbe 复核全 gone。
    """
    mapped = win_to_wsl(cwd)
    wsl_pidfile = (mapped / TREE_PIDFILE_NAME) if mapped is not None else None
    wsl_levelfile = (mapped / TREE_LEVELFILE_NAME) if mapped is not None else None
    win_pidfile = win_join(cwd, TREE_PIDFILE_NAME)
    ev: dict = {"pidfileWsl": str(wsl_pidfile) if wsl_pidfile else None,
                "levelFileWsl": str(wsl_levelfile) if wsl_levelfile else None,
                "pidfileWindows": win_pidfile}

    def ready():
        if wsl_pidfile is None or wsl_levelfile is None:
            return None
        pids = _read_pids(wsl_pidfile)
        all_lines = _read_levels(wsl_levelfile)
        records = [r for r in all_lines if r.get("type") != "liveness"]
        ok, _problems, _chain = judge_tree_chain(records, TREE_LAYERS)
        return {"pids": pids, "records": records} if (ok and len(pids) == TREE_LAYERS) else None

    snapshot = poll_until(ready, timeout=window, interval=0.25)
    if snapshot is None:
        all_lines = _read_levels(wsl_levelfile) if wsl_levelfile else []
        snapshot = {"pids": _read_pids(wsl_pidfile) if wsl_pidfile else [],
                    "records": [r for r in all_lines if r.get("type") != "liveness"]}
    pids = snapshot["pids"]
    records = snapshot["records"]
    chain_ok, problems, chain = judge_tree_chain(records, TREE_LAYERS)
    ev.update({"pids": pids, "records": records, "chain": chain, "problems": problems,
               "observable": bool(pids) or bool(records)})
    rec.check(f"{label}-bare-pidfile-exactly-{TREE_LAYERS}", len(pids) == TREE_LAYERS, pids)
    rec.check(f"{label}-chain-links-kernel-truth", chain_ok,
              {"problems": problems, "chain": chain, "records": records,
               "why": "sidecar：每 level 恰一条 + level L 的内核父 PID == level L-1 的 PID"})

    if verify_liveness and pids:
        osq = windows_query_processes(pids)
        strong, strong_reason = judge_os_source_strong(osq)
        chain_from_os_ok, os_problems, os_order = judge_os_chain(osq.get("entries"), TREE_LAYERS)
        names = [e.get("name") for e in osq.get("entries") or []]
        ev["osQuery"] = {"source": osq.get("source"), "ok": osq.get("ok"), "found": osq.get("found"),
                         "missing": osq.get("missing"), "entries": osq.get("entries"),
                         "chainProblems": os_problems, "chainOrder": os_order,
                         "limited": osq.get("limited"), "note": osq.get("note"),
                         "strongSource": strong, "strongReason": strong_reason,
                         "script": osq.get("script"), "measuredBeforeStop": True}
        # 正向硬断言：只有 PowerShell + 内核父 PID 已知，才算"独立第二源"可用
        rec.check(f"{label}-os-strong-source", strong,
                  {"source": osq.get("source"), "reason": strong_reason, "entries": osq.get("entries"),
                   "why": "只有 source=='powershell' 且内核父 PID 已知才够格；tasklist 无父 PID，必须显式 skip/FAIL"})
        if not strong:
            raise SkipCase(
                f"独立 OS 第二源不可用（{strong_reason}）⇒ 本用例不把降级源当第二源，显式跳过："
                f"source={osq.get('source')} limited={osq.get('limited')} note={osq.get('note')}", ev)
        rec.check(f"{label}-all-alive-before-stop",
                  bool(osq.get("ok")) and len(osq.get("found") or []) == TREE_LAYERS,
                  {"found": osq.get("found"), "missing": osq.get("missing"), "source": osq.get("source")})
        rec.check(f"{label}-os-names-are-fixture",
                  all("echo" in str(n).lower() for n in names) if names else False, names)
        rec.check(f"{label}-os-chain-independent", chain_from_os_ok,
                  {"problems": os_problems, "entries": osq.get("entries"), "order": os_order})
        # 两个独立来源必须给出同一组 PID 顺序（夹具自报 vs 内核）
        rec.check(f"{label}-os-order-matches-sidecar",
                  [link["childPid"] for link in chain] == os_order[1:],
                  {"sidecarOrder": [link["childPid"] for link in chain], "osOrder": os_order})
    return ev


def follow_live(ctx: Ctx, bridge: Bridge, job_id: str, stream: str, patterns: list[bytes],
                max_seconds: float, log_path: Path) -> dict:
    """Popen 逐块读 `wdb logs --follow`，记录**每个 payload 行**首次出现在 CLI 输出中的单调时刻。

    只看 payload 行（如 OUT0007|…），不看 SDK build 输出 —— 后者不能作为增量证据。
    同时记录到达时刻的任务状态与日志文件大小，用于判定「真增量」而非「终态补读」。
    """
    argv = bridge.argv("logs", job=job_id, queue=ctx.queue, stream=stream, offset=0, follow=True, json=False)
    proc = subprocess.Popen(
        argv, cwd=str(ctx.repo), stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0, env=os.environ.copy(),
    )
    pipe = proc.stdout if stream == "stdout" else proc.stderr
    other = proc.stderr if stream == "stdout" else proc.stdout
    other_chunks: list[bytes] = []
    payload = bytearray()
    events: list[dict] = []
    seen: set[int] = set()
    buf = bytearray()
    deadline = time.monotonic() + max_seconds
    timed_out = False

    def pump_other():
        """对侧管道必须并行抽干，否则 64KB 管道写满会把 CLI 卡死。"""
        while True:
            try:
                chunk = os.read(other.fileno(), 65536)
            except OSError:
                return
            if not chunk:
                return
            other_chunks.append(chunk)

    reader = threading.Thread(target=pump_other, daemon=True)
    reader.start()

    while True:
        if time.monotonic() > deadline:
            timed_out = True
            kill_tree(proc)
            break
        try:
            ready_fds, _, _ = select.select([pipe], [], [], 0.2)
        except (OSError, ValueError):
            break
        if ready_fds:
            try:
                chunk = os.read(pipe.fileno(), 65536)
            except OSError:
                chunk = b""
            if chunk:
                payload.extend(chunk)
                buf.extend(chunk)
                while b"\n" in buf:
                    line, _, rest = bytes(buf).partition(b"\n")
                    buf[:] = rest
                    for index, pattern in enumerate(patterns):
                        if index in seen or pattern not in line:
                            continue
                        seen.add(index)
                        state = (bridge.status(job_id) or {}).get("state")
                        try:
                            size = log_path.stat().st_size
                        except OSError:
                            size = -1
                        events.append({"index": index, "t": round(time.monotonic() - _FOLLOW_T0, 3),
                                       "state": state, "fileSize": size,
                                       "line": line[:80].decode("utf-8", "replace")})
                continue
        if proc.poll() is not None:
            # 进程已退出：把管道里剩下的读干净再走
            for _ in range(50):
                try:
                    ready_fds, _, _ = select.select([pipe], [], [], 0.05)
                except (OSError, ValueError):
                    break
                if not ready_fds:
                    break
                try:
                    rest = os.read(pipe.fileno(), 65536)
                except OSError:
                    break
                if not rest:
                    break
                payload.extend(rest)
            break

    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        kill_tree(proc)
    reader.join(timeout=2)
    finished = round(time.monotonic() - _FOLLOW_T0, 3)
    return {"stream": stream, "code": proc.returncode, "out": bytes(payload),
            "side": b"".join(other_chunks), "events": events, "timedOut": timed_out,
            "finishedAt": finished}


_FOLLOW_T0 = time.monotonic()


@case("day2.timeout-treekill-probe", group="day2",
      doc="Worker 硬超时（--timeout）：state=timeout、CLI wait=4，三层树被 Job 杀干净（ProcessProbe 3/3 gone）")
def case_day2_timeout_treekill(ctx: Ctx, rec: Rec, bridge: Bridge, shared: dict) -> dict:
    timeout_s = int(ctx.timeout_case_seconds)
    src = copy_fixture(FIXTURE_ECHO, ctx.work_root / "day2-timeout" / "src" / "EchoApp")
    t0 = time.time()
    job_id, submit = bridge.submit(MODE_BUILD_RUN, "EchoApp/EchoApp.csproj", src.parent,
                                   app_args=TREE_ARGS, job_timeout=timeout_s)
    rec.check("submit-ok", submit.code == CLI_OK and bool(job_id), submit.tail())
    if not job_id:
        raise SkipCase("提交失败，无法继续")
    track(shared, job_id)
    art = job_artifacts(ctx, job_id)
    ev: dict = {"jobId": job_id, "timeoutSeconds": timeout_s, "treeArgs": TREE_ARGS}

    echo = poll_until(lambda: art.find_prefixed_json(ECHO_PREFIX),
                      timeout=ctx.tree_ready_seconds, interval=0.5)
    rec.check("app-started-before-timeout", echo is not None, _short(art.stdout_text()[-600:]))
    if not echo:
        raise SkipCase("超时窗口内没看到 WDB_ECHO（可能在建构建阶段就被超时打断），无法取得 cwd/pid", ev)

    cwd = str(echo.get("cwd") or "")
    ev["echo"] = {k: echo.get(k) for k in ("pid", "cwd", "childPids", "os")}
    rec.check("cwd-windows-drive", is_windows_drive_path(cwd), cwd)

    # day2 自证之一：单 TFM 项目的**复数** TargetFrameworks 为空（读回 Windows 侧真实消费的 csproj）
    csproj = _read_win_file(cwd, "EchoApp/EchoApp.csproj")
    if csproj is None:
        ev["singleTfmProof"] = {"readable": False, "note": "读不到 Windows 侧 csproj（映射不可用）"}
    else:
        text = csproj.decode("utf-8", errors="replace")
        ev["singleTfmProof"] = {"readable": True, "hasPluralTargetFrameworks": "<TargetFrameworks" in text,
                                "hasSingularTargetFramework": "<TargetFramework>" in text}
        rec.check("single-tfm-plural-empty", "<TargetFrameworks" not in text and "<TargetFramework>" in text,
                  ev["singleTfmProof"])

    win_pidfile = win_join(cwd, TREE_PIDFILE_NAME)
    window = min(ctx.pidfile_confirm_seconds, max(4.0, timeout_s - 4.0))
    # 超时前：恰好 3 条裸 PID + sidecar 证明 3 级链（内核父 PID）+ 三层此刻全部存活
    chain = confirm_tree_chain(ctx, bridge, rec, shared, cwd, window, "timeout-tree")
    ev["chain"] = chain
    layers_confirmed = (len(chain.get("pids") or []) == TREE_LAYERS
                        and bool(chain.get("chain")) and all(link.get("linked") for link in chain["chain"])
                        and not chain.get("problems"))
    ev["layersConfirmed"] = layers_confirmed
    if not layers_confirmed and not chain.get("observable"):
        raise SkipCase("超时窗口内未能从 WSL 观测 pidfile/sidecar → 不作三层结论（其余断言已记录）", ev)

    jr, status = wait_terminal(bridge, job_id)
    elapsed = time.time() - t0
    ev.update({"waitExit": jr.code, "state": (status or {}).get("state"),
               "exitCode": (status or {}).get("exitCode"), "error": (status or {}).get("error"),
               "elapsedSeconds": round(elapsed, 2)})
    rec.equal("wait-exit-timeout", CLI_TIMEOUT, jr.code)
    rec.equal("state-timeout", "timeout", (status or {}).get("state"))
    rec.check("error-mentions-stop", "停止" in ((status or {}).get("error") or ""), (status or {}).get("error"))
    rec.check("timeout-driven-not-app-exit", elapsed >= timeout_s * 0.6,
              {"elapsedSeconds": round(elapsed, 2), "timeoutSeconds": timeout_s})

    probe = _probe_via_bridge(ctx, rec, bridge, shared, win_pidfile, TREE_LAYERS, expect_name="EchoApp",
                              note="超时收尾后由 Windows 侧只读判定三层是否全部 gone")
    ev["probe"] = _probe_summary(probe)
    _assert_probe_all_gone(rec, probe, TREE_LAYERS, "timeout")
    return ev


@case("day2.multitfm-negative-no-framework", group="day2",
      doc="多 TFM 未传 --framework：以 ERR_FRAMEWORK_REQUIRED 失败，且绝不启动目标进程")
def case_day2_multitfm_negative(ctx: Ctx, rec: Rec, bridge: Bridge, shared: dict) -> dict:
    src = copy_fixture(FIXTURE_MTFM, ctx.work_root / "day2-mtfm-neg" / "src" / "MultiTfm")
    marker = ctx.work_root / "markers" / f"mtfm-neg-{uuid.uuid4().hex[:8]}.txt"
    marker_unc = wsl_to_win_unc(marker)
    args = ["--marker", marker_unc, "--exit", "0"] if marker_unc else ["--exit", "0"]

    job_id, submit = bridge.submit(MODE_BUILD_RUN, "MultiTfm/MultiTfm.csproj", src.parent, app_args=args)
    rec.check("submit-ok", submit.code == CLI_OK and bool(job_id), submit.tail())
    if not job_id:
        raise SkipCase("提交失败，无法继续")
    track(shared, job_id)
    jr, status = wait_terminal(bridge, job_id)
    art = job_artifacts(ctx, job_id)

    rec.equal("wait-exit-failed", CLI_FAILED, jr.code)
    rec.equal("state-failed", "failed", (status or {}).get("state"))
    error = (status or {}).get("error") or ""
    rec.check("error-err-framework-required", "ERR_FRAMEWORK_REQUIRED" in error, error)
    rec.check("no-target-started", not art.has_prefix(MTFM_PREFIX), _short(art.stdout_text()[-800:]))
    rec.check("marker-absent", not marker.exists(), {"path": str(marker)})
    return {
        "jobId": job_id, "state": (status or {}).get("state"), "exitCode": (status or {}).get("exitCode"),
        "error": error, "marker": str(marker), "markerAbsent": not marker.exists(),
        "stdoutTail": _short(art.stdout_text()[-700:]),
    }


def _multitfm_positive(ctx: Ctx, rec: Rec, bridge: Bridge, shared: dict, framework: str,
                       expect_symbol: str) -> dict:
    src = copy_fixture(FIXTURE_MTFM, ctx.work_root / f"day2-mtfm-{framework.replace('.', '_')}" / "src" / "MultiTfm")
    job_id, submit = bridge.submit("build-run-fw", "MultiTfm/MultiTfm.csproj", src.parent,
                                   app_args=["--exit", "0"], framework=framework)
    rec.check("submit-ok", submit.code == CLI_OK and bool(job_id), submit.tail())
    if not job_id:
        raise SkipCase("提交失败，无法继续")
    track(shared, job_id)
    jr, status = wait_terminal(bridge, job_id)
    art = job_artifacts(ctx, job_id)
    mtfm = art.find_prefixed_json(MTFM_PREFIX)
    ev = {"jobId": job_id, "frameworkArg": framework, "expectedSymbol": expect_symbol, "waitExit": jr.code,
          "state": (status or {}).get("state"), "exitCode": (status or {}).get("exitCode"),
          "artifact": (status or {}).get("artifact"), "error": (status or {}).get("error"),
          "mtfm": None if mtfm is None else {k: mtfm.get(k) for k in
                                             ("tfm", "platform", "compiledFor", "os", "cwd",
                                              "baseDirectory", "processPath", "tfmAttribute",
                                              "targetPlatformAttribute", "definedSymbols")},
          "stdoutTail": _short(art.stdout_text()[-500:])}

    rec.equal("wait-exit-ok", CLI_OK, jr.code)
    rec.equal("state-succeeded", "succeeded", (status or {}).get("state"))
    rec.check("mtfm-json-present", mtfm is not None, _short(art.stdout_text()[-700:]))
    if mtfm:
        # 权威判据：被启动程序自己所在的目录末段 == 请求的 TFM（不依赖任何 #if 猜测）
        base_dir = str(mtfm.get("baseDirectory") or "")
        ran_dir = _framework_dir_of(base_dir)
        ev["ranFromFrameworkDir"] = ran_dir
        rec.equal("ran-from-requested-framework-dir", framework, ran_dir)
        rec.check("os-contains-windows", "windows" in str(mtfm.get("os", "")).lower(), mtfm.get("os"))
        rec.check("cwd-windows-drive", is_windows_drive_path(str(mtfm.get("cwd") or "")), mtfm.get("cwd"))
        rec.check("process-path-inside-runs-root",
                  "WslDebugBridge" in str(mtfm.get("processPath") or "")
                  or "WslDebugBridge" in base_dir, mtfm.get("processPath"))
    artifact = (status or {}).get("artifact") or ""
    rec.check("artifact-under-framework", fw_path_fragment(framework) in artifact, artifact)
    return ev


def _framework_dir_of(base_directory: str) -> str | None:
    r"""从 C:\...\bin\Debug\<tfm>\ 这种 BaseDirectory 里取出 <tfm>（多 TFM 构建下即真正运行的 TFM）。"""
    m = re.search(r"\\(net\d+(?:\.\d+)?(?:-[a-z0-9\.]+)?)\\?$", base_directory.rstrip("\\") + "\\", re.IGNORECASE)
    return m.group(1) if m else None


def fw_path_fragment(framework: str) -> str:
    """TargetPath 里 TFM 出现在路径分隔符包围处：...\\bin\\Debug\\net10.0\\MultiTfm.dll"""
    return "\\" + framework + "\\"


@case("day2.multitfm-net10", group="day2",
      doc="多 TFM 正向（--framework net10.0）：运行成功且自报 TFM=net10.0；并证明声明恰 2 个 TFM")
def case_day2_multitfm_net10(ctx: Ctx, rec: Rec, bridge: Bridge, shared: dict) -> dict:
    ev = _multitfm_positive(ctx, rec, bridge, shared, "net10.0", "NET10_0")

    # day2 自证之二：Windows 侧真实消费的 csproj 里 TargetFrameworks **恰 2 项**
    cwd = str(((ev.get("mtfm") or {}).get("cwd")) or "")
    raw = _read_win_file(cwd, "MultiTfm/MultiTfm.csproj")
    if raw is None:
        ev["tfmCountProof"] = {"readable": False, "note": "读不到 Windows 侧 csproj（映射不可用）→ 恰 2 的断言跳过"}
        rec.check("tfm-count-readback", True, ev["tfmCountProof"])
        return ev
    text = raw.decode("utf-8", errors="replace")
    m = re.search(r"<TargetFrameworks>([^<]*)</TargetFrameworks>", text)
    tfms = [t.strip() for t in (m.group(1).split(";") if m else []) if t.strip()]
    ev["tfmCountProof"] = {"readable": True, "declared": tfms, "count": len(tfms),
                           "hasPlural": "<TargetFrameworks>" in text,
                           "enableWindowsTargeting": "<EnableWindowsTargeting>true</EnableWindowsTargeting>" in text,
                           "sha256": hashlib.sha256(raw).hexdigest()}
    rec.equal("tfm-count-exactly-2", 2, len(tfms))
    rec.equal("tfm-list", ["net10.0", "net10.0-windows"], tfms)
    rec.check("enable-windows-targeting", ev["tfmCountProof"]["enableWindowsTargeting"])
    return ev


@case("day2.multitfm-net10-windows", group="day2",
      doc="多 TFM 正向（--framework net10.0-windows）：运行成功且自报 TFM=net10.0-windows（#if 符号证明）")
def case_day2_multitfm_net10_windows(ctx: Ctx, rec: Rec, bridge: Bridge, shared: dict) -> dict:
    return _multitfm_positive(ctx, rec, bridge, shared, "net10.0-windows", "NET10_0_WINDOWS")


@case("day2.tree-negative-rootsiblings", group="day2",
      doc="反例（真实 Windows）：--spawn 2 --depth 1 产生 root+两个直系兄弟 = 3 个 PID，链判据必须判负")
def case_day2_tree_negative_rootsiblings(ctx: Ctx, rec: Rec, bridge: Bridge, shared: dict) -> dict:
    # 3 个进程但不是 3 级链：用来证明「计数」与「链」不是一回事（阴性对照）。
    flat_args = ["--spawn", "2", "--depth", "1", "--repeat",
                 "--pidfile", TREE_PIDFILE_NAME, "--level-file", TREE_LEVELFILE_NAME]
    src = copy_fixture(FIXTURE_ECHO, ctx.work_root / "day2-neg-tree" / "src" / "EchoApp")
    job_id, submit = bridge.submit(MODE_BUILD_RUN, "EchoApp/EchoApp.csproj", src.parent, app_args=flat_args)
    rec.check("submit-ok", submit.code == CLI_OK and bool(job_id), submit.tail())
    if not job_id:
        raise SkipCase("提交失败，无法继续")
    track(shared, job_id)
    art = job_artifacts(ctx, job_id)
    ev: dict = {"jobId": job_id, "flatArgs": flat_args}

    echo = poll_until(lambda: art.find_prefixed_json(ECHO_PREFIX), timeout=ctx.tree_ready_seconds, interval=0.5)
    rec.check("rootsiblings-started", echo is not None, _short(art.stdout_text()[-500:]))
    if not echo:
        raise SkipCase("反例任务未产出 WDB_ECHO，无法取得 cwd", ev)
    cwd = str(echo.get("cwd") or "")
    ev["echo"] = {k: echo.get(k) for k in ("pid", "cwd", "childPids", "level")}

    mapped = win_to_wsl(cwd)
    wsl_pidfile = (mapped / TREE_PIDFILE_NAME) if mapped else None
    wsl_levelfile = (mapped / TREE_LEVELFILE_NAME) if mapped else None

    def ready():
        if not wsl_pidfile or not wsl_levelfile:
            return None
        pids = _read_pids(wsl_pidfile)
        records = [r for r in _read_levels(wsl_levelfile) if r.get("type") != "liveness"]
        return {"pids": pids, "records": records} if len(pids) >= TREE_LAYERS and len(records) >= TREE_LAYERS else None

    snapshot = poll_until(ready, timeout=ctx.pidfile_confirm_seconds, interval=0.25)
    if snapshot is None:
        stop_fallback = bridge.stop(job_id, "windows-smoke rootsiblings cleanup")
        jr_fallback, _ = wait_terminal(bridge, job_id)
        raise SkipCase(f"未能在窗口内观测到 3 条 PID/记录（stop={stop_fallback.code} wait={jr_fallback.code}）→ "
                       f"反例不成立，按环境限制跳过", ev)

    pids = snapshot["pids"]
    records = snapshot["records"]
    ok, problems, links = judge_tree_chain(records, TREE_LAYERS)
    # 内核真值同样必须拒（root 有 2 个孩子）；与正向用例用**同一套**源 gating：只认 PowerShell
    osq = windows_query_processes(pids)
    strong, strong_reason = judge_os_source_strong(osq)
    os_ok, os_problems, os_order = judge_os_chain(osq.get("entries"), TREE_LAYERS)
    ev.update({"pids": pids, "records": records, "problems": problems, "links": links,
               "pidCountEqualsThree": len(pids) == TREE_LAYERS,
               "osQuery": {"source": osq.get("source"), "ok": osq.get("ok"), "found": osq.get("found"),
                           "entries": osq.get("entries"), "chainProblems": os_problems, "chainOrder": os_order,
                           "strongSource": strong, "strongReason": strong_reason, "note": osq.get("note")}})
    rec.check("neg-os-strong-source", strong, {"source": osq.get("source"), "reason": strong_reason})
    if not strong:
        bridge.stop(job_id, "windows-smoke rootsiblings cleanup")
        wait_terminal(bridge, job_id)
        raise SkipCase(f"负向用例的独立 OS 源不可用（{strong_reason}）⇒ 与正向同样显式跳过，不静默降级", ev)
    # 反例的全部意义：进程数确实等于 3、也确实全活着，但**两个判据都必须拒绝它是 3 级链**
    rec.check("neg-three-pids-observed", len(pids) == TREE_LAYERS, pids)
    rec.check("neg-three-alive-by-os", osq.get("ok") is True and len(osq.get("found") or []) == TREE_LAYERS,
              {"found": osq.get("found"), "source": osq.get("source")})
    rec.check("neg-chain-judge-rejects", not ok, {"problems": problems, "records": records})
    rec.check("neg-rejected-because-two-level1",
              any("level1-count=2" in p or "levels=" in p for p in problems), problems)
    rec.check("neg-os-chain-judge-rejects", not os_ok,
              {"problems": os_problems, "entries": osq.get("entries"), "source": osq.get("source")})
    rec.check("neg-os-sees-two-children-of-root",
              any("2-children" in p for p in os_problems), os_problems)

    stop = bridge.stop(job_id, "windows-smoke rootsiblings stop")
    jr, status = wait_terminal(bridge, job_id)
    ev.update({"stopExit": stop.code, "waitExit": jr.code, "state": (status or {}).get("state")})
    rec.equal("neg-wait-exit-canceled", CLI_CANCELED, jr.code)

    win_pidfile = win_join(cwd, TREE_PIDFILE_NAME)
    probe = _probe_via_bridge(ctx, rec, bridge, shared, win_pidfile, TREE_LAYERS, expect_name="EchoApp",
                              note="反例任务停止后，3 个 PID（root+2 兄弟）必须全部 gone")
    ev["probe"] = _probe_summary(probe)
    _assert_probe_all_gone(rec, probe, TREE_LAYERS, "neg")
    return ev


@case("day2.logs-follow-live-and-bytes", group="day2",
      doc="plain logs --follow：真增量证明（payload 行在任务非终态时陆续到达 + 中段文件增长）+ 全量逐字节等于日志文件")
def case_day2_logs_follow_bytes(ctx: Ctx, rec: Rec, bridge: Bridge, shared: dict) -> dict:
    out_lines, err_lines, interval_ms, chunk = 24, 24, 300, 0  # ≈7.2s 慢速多行，留出真增量观察窗
    src = copy_fixture(FIXTURE_SPINNER, ctx.work_root / "day2-follow" / "src" / "LogSpinner")
    args = ["--stdout-lines", str(out_lines), "--stderr-lines", str(err_lines),
            "--interval-ms", str(interval_ms), "--chunk-bytes", str(chunk), "--exit", "0"]
    job_id, submit = bridge.submit(MODE_BUILD_RUN, "LogSpinner/LogSpinner.csproj", src.parent, app_args=args)
    rec.check("submit-ok", submit.code == CLI_OK and bool(job_id), submit.tail())
    if not job_id:
        raise SkipCase("提交失败，无法继续")
    track(shared, job_id)
    art0 = job_artifacts(ctx, job_id)
    pat_out = [f"OUT{i:04d}|".encode("utf-8") for i in range(out_lines)]
    pat_err = [f"ERR{i:04d}|".encode("utf-8") for i in range(err_lines)]

    def payload_lines_out() -> int:
        data = art0.stdout_bytes()
        return sum(1 for pat in pat_out if pat in data)

    # 关键：等到**目标进程已经开始产出 payload**（而不是 SDK build 输出）再附着，
    # 这样「首条 payload 到达时刻」才是有意义的时间点。
    seen = poll_until(lambda: payload_lines_out() >= 1 or None, timeout=60.0, interval=0.1)
    state_at_attach = (bridge.status(job_id) or {}).get("state")
    ev: dict = {"jobId": job_id, "spinnerArgs": args, "stateAtAttach": state_at_attach,
                "payloadLinesInFileAtAttach": payload_lines_out(),
                "logSizeAtAttach": {"stdout": len(art0.stdout_bytes()), "stderr": len(art0.stderr_bytes())}}
    rec.check("attach-after-first-payload-line", seen is not None,
              {"payloadInFile": payload_lines_out(), "state": state_at_attach,
               "stdoutTail": _short(art0.stdout_text()[-300:])})
    rec.check("attached-while-running", state_at_attach == "running",
              {"stateAtAttach": state_at_attach,
               "hint": "附着时任务已终态 ⇒ 无法证明增量（本用例不把终态补读当增量证据）"})

    # 两条流**并发**跟随（顺序跟随会让后一条只剩终态补读）
    live: dict = {}
    max_seconds = min(180.0, max(60.0, ctx.wait_timeout))

    def run_follower(key: str, stream: str, patterns: list[bytes], log_path: Path) -> None:
        live[key] = follow_live(ctx, bridge, job_id, stream, patterns, max_seconds, log_path)

    t_out = threading.Thread(target=run_follower, args=("stdout", "stdout", pat_out, art0.stdout_path))
    t_err = threading.Thread(target=run_follower, args=("stderr", "stderr", pat_err, art0.stderr_path))
    t_out.start()
    t_err.start()
    t_out.join(timeout=max_seconds + 30)
    t_err.join(timeout=max_seconds + 30)

    jr, status = wait_terminal(bridge, job_id)
    art = job_artifacts(ctx, job_id)
    ev["waitExit"] = jr.code
    ev["state"] = (status or {}).get("state")

    exp_out = "".join(f"OUT{i:04d}|中文行|" + "x" * chunk + "\n" for i in range(out_lines)).encode("utf-8")
    exp_err = "".join(f"ERR{i:04d}|错误中文行|" + "y" * chunk + "\n" for i in range(err_lines)).encode("utf-8")
    log_out = art.stdout_bytes()
    log_err = art.stderr_bytes()

    rec.equal("wait-exit-ok", CLI_OK, jr.code)
    rec.equal("state-succeeded", "succeeded", (status or {}).get("state"))
    for key in ("stdout", "stderr"):
        rec.check(f"{key}-follow-exit-ok", live.get(key, {}).get("code") == CLI_OK, live.get(key, {}).get("code"))
        rec.check(f"{key}-follow-not-timed-out", not live.get(key, {}).get("timedOut"))

    # ---- 真增量判据（纯函数；末段一次性样本必须被判负，见 static.tree-and-follow-judges）----
    verdicts = {}
    for key in ("stdout", "stderr"):
        events = (live.get(key) or {}).get("events") or []
        terminal_t = (live.get(key) or {}).get("finishedAt")
        ok, problems = judge_live_follow(events, terminal_t, min_while_running=3)
        verdicts[key] = {"ok": ok, "problems": problems, "eventCount": len(events),
                         "whileRunning": sum(1 for e in events if e.get("state") == "running"),
                         "firstEvent": events[0] if events else None, "lastEvent": events[-1] if events else None,
                         "finishedAt": terminal_t}
        rec.check(f"{key}-live-incremental", ok,
                  {"problems": problems, "events": events[:24], "finishedAt": terminal_t,
                   "why": "≥3 条 payload 行在任务 running 时到达 + 首条早于终态 + 期间日志文件持续增长"})
        rec.check(f"{key}-payload-arrived-while-running",
                  any(e.get("state") == "running" for e in events), events[:12])
    ev["live"] = verdicts

    # 注意：build-run 模式下 stdout.log 前面还有 dotnet build 的输出，所以**不能**假设文件 == 夹具输出。
    # 真正可断言的是：(a) 夹具输出是文件尾部；(b) CLI 的跟随输出与文件逐字节相等。
    rec.check("stdout-file-ends-with-payload", log_out.endswith(exp_out),
              {"fileLen": len(log_out), "payloadLen": len(exp_out), "tailHex": _bhex(log_out[-32:])})
    rec.equal_bytes("stdout-follow-equals-file", log_out, (live.get("stdout") or {}).get("out") or b"")
    rec.check("stderr-file-ends-with-payload", log_err.endswith(exp_err),
              {"fileLen": len(log_err), "payloadLen": len(exp_err), "tailHex": _bhex(log_err[-32:])})
    # plain --stream stderr 的内容走 CLI 的 **stderr**（Cli.cs EmitLogChunks：stderr 块写 Console.Error）
    rec.equal_bytes("stderr-follow-equals-file", log_err, (live.get("stderr") or {}).get("out") or b"")
    rec.check("stdout-stream-has-no-cli-noise", ((live.get("stdout") or {}).get("side") or b"") == b"",
              _bhex(((live.get("stdout") or {}).get("side") or b"")[:64]))
    rec.check("chinese-utf8-survived", "中文行".encode("utf-8") in log_out
              and "错误中文行".encode("utf-8") in log_err)

    ev["expected"] = {"stdoutPayloadBytes": len(exp_out), "stderrPayloadBytes": len(exp_err),
                      "stdoutHexHead": _bhex(exp_out[:48]), "stderrHexHead": _bhex(exp_err[:48])}
    ev["logBytes"] = {"stdout": len(log_out), "stderr": len(log_err)}
    return ev


@case("day2.logs-big-and-offset-bytes", group="day2",
      doc="plain logs 大日志（1 MiB+）：全量读与中段 --offset 切片都与日志文件逐字节相等")
def case_day2_logs_big_and_offset_bytes(ctx: Ctx, rec: Rec, bridge: Bridge, shared: dict) -> dict:
    bulk = 1 << 20  # 1 MiB
    src = copy_fixture(FIXTURE_ECHO, ctx.work_root / "day2-biglog" / "src" / "EchoApp")
    args = ["--bytes", str(bulk), "--stderr", "中文错误行", "--exit", "0"]
    job_id, submit = bridge.submit(MODE_BUILD_RUN, "EchoApp/EchoApp.csproj", src.parent, app_args=args)
    rec.check("submit-ok", submit.code == CLI_OK and bool(job_id), submit.tail())
    if not job_id:
        raise SkipCase("提交失败，无法继续")
    track(shared, job_id)
    jr, status = wait_terminal(bridge, job_id)
    art = job_artifacts(ctx, job_id)
    log_out = art.stdout_bytes()
    log_err = art.stderr_bytes()

    rec.equal("wait-exit-ok", CLI_OK, jr.code)
    rec.equal("state-succeeded", "succeeded", (status or {}).get("state"))
    rec.check("bulk-large-enough", len(log_out) >= bulk, {"stdoutBytes": len(log_out), "bulk": bulk})
    rec.check("bulk-markers", b"<<BULK>>" in log_out and b"<<ENDBULK>>" in log_out)
    # build-run 模式下 stdout.log 前面还有 dotnet build 的输出，故**不能**假设 WDB_ECHO 在文件开头；
    # 只要求它存在（字节级正确性由下面的 full/offset 逐字节比对覆盖）。
    rec.check("echo-line-present", art.find_prefixed_json(ECHO_PREFIX) is not None, _bhex(log_out[:48]))

    full = bridge.logs_bytes(job_id, "stdout", offset=0, proc_timeout=120.0)
    rec.equal("full-read-exit", CLI_OK, full.code)
    rec.equal_bytes("full-read-bytes-exact", log_out, full.out)

    mid = len(log_out) // 2
    tail = bridge.logs_bytes(job_id, "stdout", offset=mid, proc_timeout=120.0)
    rec.equal("offset-read-exit", CLI_OK, tail.code)
    rec.equal_bytes("offset-slice-bytes-exact", log_out[mid:], tail.out)

    err_read = bridge.logs_bytes(job_id, "stderr", offset=0, proc_timeout=120.0)
    rec.equal_bytes("stderr-bytes-exact", log_err, err_read.err)
    rec.check("stderr-chinese", "中文错误行".encode("utf-8") in log_err, _bhex(log_err[:64]))

    return {"jobId": job_id, "stdoutBytes": len(log_out), "stderrBytes": len(log_err),
            "offsetProbeAt": mid, "artifact": (status or {}).get("artifact"),
            "stdoutHexHead": _bhex(log_out[:32]), "stdoutHexTail": _bhex(log_out[-32:])}


@case("day2.nonutf8-boundaries", group="day2",
      doc="非 UTF-8 裸字节：按**真实 bytes** 记录 Windows 落盘与 CLI plain 输出的处理策略（不主张支持）")
def case_day2_nonutf8_boundaries(ctx: Ctx, rec: Rec, bridge: Bridge, shared: dict) -> dict:
    raw = bytes([0x80, 0xFF, 0xC3, 0x28, 0xFE, 0x0A])
    src = copy_fixture(FIXTURE_ECHO, ctx.work_root / "day2-badutf8" / "src" / "EchoApp")
    job_id, submit = bridge.submit(MODE_BUILD_RUN, "EchoApp/EchoApp.csproj", src.parent,
                                   app_args=["--bad-utf8", "--exit", "0"])
    rec.check("submit-ok", submit.code == CLI_OK and bool(job_id), submit.tail())
    if not job_id:
        raise SkipCase("提交失败，无法继续")
    track(shared, job_id)
    jr, status = wait_terminal(bridge, job_id)
    art = job_artifacts(ctx, job_id)
    log_out = art.stdout_bytes()
    log_err = art.stderr_bytes()
    fffd = "\ufffd".encode("utf-8")

    rec.equal("wait-exit-ok", CLI_OK, jr.code)
    rec.equal("state-succeeded", "succeeded", (status or {}).get("state"))
    # 与编码无关的部分必须完好：ASCII 标记与 0x28 '(' 都要在
    rec.check("ascii-markers-intact", b"<<BAD>>" in log_out and b"<<END>>" in log_out
              and b"<<BAD>>" in log_err and b"<<END>>" in log_err)

    cli1 = bridge.logs_bytes(job_id, "stdout", offset=0, proc_timeout=120.0)
    cli2 = bridge.logs_bytes(job_id, "stdout", offset=0, proc_timeout=120.0)
    rec.equal("cli-exit-ok", CLI_OK, cli1.code)
    rec.check("cli-deterministic", cli1.out == cli2.out and cli1.err == cli2.err)
    rec.check("cli-does-not-drop-stream", len(cli1.out) > 0)

    raw_present = raw in log_out
    policy = "exact" if raw_present else ("u+fffd-replaced" if fffd in log_out else "other")
    # 记录为主：桥的日志通道是**文本**通道（Worker 用 UTF-8 StreamReader 读、再以 UTF-8 写日志），
    # 因此非法字节是否原样落盘属于实测边界，本脚本不对「支持非 UTF-8 透传」作任何主张。
    rec.check("policy-recorded", policy in {"exact", "u+fffd-replaced", "other"}, policy)
    rec.check("cli-output-matches-log-file", cli1.out == log_out,
              {"cliLen": len(cli1.out), "fileLen": len(log_out)})
    return {
        "jobId": job_id,
        "policy": policy,
        "claim": "本用例不主张支持非 UTF-8 透传；只记录实测策略",
        "rawBytesPresentInLogFile": raw_present,
        "replacementCharCount": {"stdout": log_out.count(fffd), "stderr": log_err.count(fffd)},
        "lengths": {"stdoutLog": len(log_out), "stderrLog": len(log_err), "cliStdout": len(cli1.out)},
        "stdoutHex": _bhex(log_out[:160]),
        "stderrHex": _bhex(log_err[:160]),
        "asciiMarkersIntact": b"<<BAD>>" in log_out and b"<<END>>" in log_out,
    }


# ---- 断言与探针辅助 -------------------------------------------------------

def _assert_echo_and_status(rec: Rec, jr: Run, status: dict | None, echo: dict | None,
                            submitted_args: list[str], art: JobArtifacts) -> None:
    rec.equal("wait-exit-ok", CLI_OK, jr.code)
    rec.equal("state-succeeded", "succeeded", (status or {}).get("state"))
    rec.equal("exit-code-zero", 0, (status or {}).get("exitCode"))
    rec.check("echo-json-present", echo is not None, _short(art.stdout_text()[-900:]))
    if not echo:
        return

    os_text = str(echo.get("os") or "")
    rec.check("os-contains-windows", "windows" in os_text.lower(), os_text)
    cwd = str(echo.get("cwd") or "")
    rec.check("cwd-windows-drive", is_windows_drive_path(cwd), cwd)
    rec.check("cwd-not-unc", not is_unc_path(cwd), cwd)
    rec.check("cwd-not-wsl-share", "wsl.localhost" not in cwd.lower(), cwd)
    rec.check("not-child", echo.get("isChild") is False, echo.get("isChild"))

    argv = echo.get("argv") or []
    argv_hex = echo.get("argvHex") or []
    rec.equal("argc", len(submitted_args), echo.get("argc"))
    rec.equal("argv-exact", submitted_args, argv)
    rec.equal("argv-hex-length", len(argv), len(argv_hex))
    if len(argv) == len(submitted_args) and len(argv_hex) == len(argv):
        mismatched = [i for i, a in enumerate(argv) if str(argv_hex[i]).lower() != a.encode("utf-8").hex()]
        rec.check("argvHex-per-byte", not mismatched,
                  [{"index": i, "arg": argv[i], "hex": argv_hex[i], "expected": argv[i].encode("utf-8").hex()}
                   for i in mismatched[:5]])
    else:
        rec.check("argvHex-per-byte", False, {"argv": len(argv), "hex": len(argv_hex)})


def _echo_evidence(job_id: str, echo: dict | None, status: dict | None, note: str) -> dict:
    return {
        "jobId": job_id,
        "note": note,
        "mode": (status or {}).get("mode"),
        "state": (status or {}).get("state"),
        "exitCode": (status or {}).get("exitCode"),
        "pidStatus": (status or {}).get("pid"),
        "artifact": (status or {}).get("artifact"),
        "echo": None if echo is None else {k: echo.get(k) for k in
                                           ("os", "cwd", "pid", "ppid", "runtime", "framework", "argc",
                                            "argv", "argvHex", "childPids")},
    }


def _probe_via_bridge(ctx: Ctx, rec: Rec, bridge: Bridge, shared: dict, windows_pidfile: str,
                      min_pids: int, expect_name: str, note: str, wait_gone: float = 20.0) -> dict | None:
    """经同一 Windows 桥 build-run ProcessProbe，读回 PID 存活判定（只查询、不 kill）。

    pidfile 必须给**绝对 Windows 路径**：ProcessProbe 的 cwd 是它自己的 src 副本，
    相对路径会落到别处；该绝对路径由被取消任务 stdout 里的 cwd 反推得到。
    """
    probe_src_root = ctx.work_root / "probe" / "src"
    copy_fixture(FIXTURE_PROBE, probe_src_root / "ProcessProbe")
    job_id, submit = bridge.submit(MODE_BUILD_RUN, "ProcessProbe/ProcessProbe.csproj", probe_src_root,
                                   app_args=["--pidfile", windows_pidfile, "--min-pids", str(min_pids),
                                             "--expect-name", expect_name,
                                             "--wait-until-gone-seconds", str(int(wait_gone)),
                                             "--interval-ms", "250"])
    if submit.code != CLI_OK or not job_id:
        rec.check("probe-submit-ok", False, submit.tail())
        return None
    track(shared, job_id)
    jr, _status = wait_terminal(bridge, job_id)
    art = job_artifacts(ctx, job_id)
    payload = art.find_prefixed_json(PROBE_PREFIX)
    rec.check("probe-submit-ok", True, job_id)
    rec.check("probe-ran-on-windows", bool(payload) and "windows" in str(payload.get("os", "")).lower(),
              (payload or {}).get("os"))
    if not payload:
        rec.check("probe-json-present", False, _short(art.stdout_text()[-800:]))
        return None
    payload = dict(payload)
    payload["jobId"] = job_id
    payload["waitExit"] = jr.code
    payload["note"] = note
    return payload


def _probe_summary(payload: dict | None) -> dict | None:
    if payload is None:
        return None
    return {k: payload.get(k) for k in
            ("jobId", "status", "ok", "allGone", "pidCount", "gone", "alive", "queryErrors",
             "countOk", "minPids", "attempts", "waitedSeconds", "pids", "results", "os", "cwd", "note")}


def _parse_prefixed(text: str, prefix: str) -> dict | None:
    found = None
    for line in (text or "").splitlines():
        line = line.strip()
        if line.startswith(prefix):
            try:
                found = json.loads(line[len(prefix):])
            except json.JSONDecodeError:
                continue
    return found


def _read_pids(path: Path) -> list[int]:
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return []
    pids: list[int] = []
    for token in text.replace("\r", " ").replace("\n", " ").split():
        if token.strip().isdigit():
            value = int(token)
            if value > 0 and value not in pids:
                pids.append(value)
    return pids


def _read_levels(path: Path) -> list[dict]:
    """读 sidecar（每行一条 JSON）；无法解析的行直接丢弃（判据会因记录数不足而失败，不会假绿）。"""
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return []
    records: list[dict] = []
    for line in text.splitlines():
        line = line.strip()
        if not line.startswith("{"):
            continue
        try:
            records.append(json.loads(line))
        except json.JSONDecodeError:
            continue
    return records


# WSL→Windows 互操作可用的只读探针（**不经桥队列**，因此可以在任务运行中执行）。
# 这一点很关键：Worker 是串行的，任何经桥提交的查询任务都必须等我的任务结束才会被领取。
WSL_POWERSHELL = Path("/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe")
WSL_TASKLIST = Path("/mnt/c/Windows/System32/tasklist.exe")


def _powershell_pid_filter(pids: list[int]) -> str:
    """WQL 过滤条件：PID 过滤放在 **server 端**（`-Filter`），不拉回全机进程再客户端筛。"""
    return " OR ".join(f"ProcessId={p}" for p in pids)


def powershell_query_script(pids: list[int]) -> str:
    """只读查询脚本：仅请求 `ProcessId,ParentProcessId,Name` 三个字段。

    **刻意不请求 CommandLine**：既然没人消费它，就绝不读取 —— 避免把命令行里的敏感内容带进证据。
    `pids` 为空时由调用方 fail closed（不查询全机）。
    """
    return ('Get-CimInstance -ClassName Win32_Process -Filter "'
            + _powershell_pid_filter(pids)
            + '" -Property ProcessId,ParentProcessId,Name | ConvertTo-Json -Compress')


def judge_os_source_strong(osq: dict | None) -> tuple[bool, str]:
    """纯判据：这份 OS 查询结果**够不够格充当独立第二源**。

    只有 PowerShell（带内核父 PID）才算够格；tasklist 只有存活、没有父 PID ⇒ 明确"不够格"，
    调用方必须显式 skip/FAIL，绝不能静默把它当第二源用。
    """
    if not osq:
        return False, "no-result"
    source = osq.get("source")
    if source is None:
        return False, "no-interop-channel"
    if source != "powershell":
        return False, f"degraded-source:{source}（只有存活、无内核父 PID）"
    if osq.get("limited"):
        return False, "marked-limited"
    entries = osq.get("entries") or []
    if not entries:
        return False, "no-entries"
    if any(not isinstance(e.get("ppid"), int) or int(e["ppid"]) <= 0 for e in entries):
        return False, "kernel-ppid-unknown"
    if osq.get("missing"):
        return False, f"missing:{osq.get('missing')}"
    return True, ""


def windows_query_processes(pids: list[int], timeout: float = 30.0) -> dict:
    """向 Windows 内核只读查询这些 PID 是否存在、其内核父 PID 与映像名。

    返回 {"ok":bool,"source":"powershell"|"tasklist"|None,"entries":[{pid,ppid,name}],
          "missing":[pid],"found":[…],"raw":…,"script":…,"note":…}
    互操作不可用时 ok=False 且 source=None —— 调用方必须据此降级或跳过，绝不当作通过。
    **ids 为空时 fail closed**：返回 source=None 且不执行任何查询，绝不退化成"查询全机进程"。
    """
    wanted = sorted({int(p) for p in pids if int(p) > 0})
    result: dict = {"ok": False, "source": None, "entries": [], "missing": list(wanted),
                    "found": [], "wanted": wanted, "script": None, "raw": "", "note": ""}
    if not wanted:
        result["note"] = "ids-empty: fail closed（不查询全机进程、不执行任何子进程）"
        return result

    if WSL_POWERSHELL.exists():
        script = powershell_query_script(wanted)
        result["script"] = script
        try:
            proc = subprocess.run([str(WSL_POWERSHELL), "-NoProfile", "-NonInteractive", "-Command", script],
                                  capture_output=True, text=True, timeout=timeout, errors="replace")
        except (OSError, subprocess.SubprocessError) as exc:
            result["raw"] = f"{type(exc).__name__}: {exc}"
        else:
            result["source"] = "powershell"
            result["raw"] = (proc.stdout or "").strip()[:2000]
            payload: object = None
            for line in reversed((proc.stdout or "").splitlines()):
                line = line.strip()
                if line.startswith("{") or line.startswith("["):
                    try:
                        payload = json.loads(line)
                        break
                    except json.JSONDecodeError:
                        continue
            for item in (payload if isinstance(payload, list) else [payload] if isinstance(payload, dict) else []):
                try:
                    result["entries"].append({"pid": int(item.get("ProcessId")),
                                              "ppid": int(item.get("ParentProcessId")),
                                              "name": str(item.get("Name") or "")})
                except (TypeError, ValueError):
                    continue
            found = [e["pid"] for e in result["entries"]]
            result["found"] = found
            result["missing"] = [p for p in wanted if p not in found]
            result["ok"] = not result["missing"]
            return result

    if WSL_TASKLIST.exists():
        # 显式降级通道：只有存活、**没有内核父 PID** ⇒ 不够格当独立第二源（judge_os_source_strong 会拒）。
        result["source"] = "tasklist"
        result["limited"] = True
        found: list[int] = []
        for pid in wanted:
            try:
                proc = subprocess.run([str(WSL_TASKLIST), "/FI", f"PID eq {pid}", "/FO", "CSV", "/NH"],
                                      capture_output=True, text=True, timeout=timeout, errors="replace")
            except (OSError, subprocess.SubprocessError):
                continue
            head = (proc.stdout or "").strip().splitlines()
            if head and "no tasks" not in head[0].lower() and str(pid) in (proc.stdout or ""):
                found.append(pid)
                result["entries"].append({"pid": pid, "ppid": -1, "name": head[0].split('","')[0].strip('"')})
        result["raw"] = f"tasklist found={found}"
        result["found"] = found
        result["missing"] = [p for p in wanted if p not in found]
        result["ok"] = not result["missing"]
        result["note"] = "tasklist 降级源：无父 PID，不得作为独立第二源（正向硬断言要求 source=='powershell'）"
    return result


def looks_like_linux_worker(state: dict) -> bool:
    """Linux 侧 Worker（--allow-non-windows）的可靠判据：队列/运行根是 Linux 路径。

    刻意**不**比对主机名：WSL2 的 hostname 默认就是 Windows 机器名，比对会把健康的 Windows
    Worker 误判成 Linux Worker（假阴性）。主机名只作为证据记录，不参与判定。
    """
    queue_root = str((state or {}).get("queueRoot") or "")
    runs_root = str((state or {}).get("runsRoot") or "")
    if queue_root.startswith("/") or runs_root.startswith("/"):
        return True
    if not is_windows_drive_path(runs_root) and not is_unc_path(runs_root):
        return True
    return False


# --------------------------------------------------------------------------
# Preflight（不健康就 BLOCKED）
# --------------------------------------------------------------------------
def preflight(ctx: Ctx, bridge: Bridge) -> tuple[dict, str | None]:
    info: dict = {"queueRoot": str(ctx.queue), "queueExists": ctx.queue.is_dir(),
                  "wslHostname": platform.node()}
    if not ctx.queue.is_dir():
        return info, f"队列目录不存在：{ctx.queue}（尚未在 Windows 前台启动 Worker，或用的是别的 --queue）"

    view = bridge.status(None)
    if view is None:
        return info, "无法执行 `wdb status --json`（队列存在但读不出队列状态）——拒绝在状态未知时继续"

    worker = view.get("worker")
    info["workerView"] = worker
    if not worker:
        return info, "worker.json 不存在或不可读：队列上的 Worker 从未运行过"
    info["workerState"] = worker.get("state")
    if worker.get("healthy") is not True:
        return info, (f"Worker 心跳不健康（ageSeconds={worker.get('ageSeconds')}，阈值 15s）"
                      "——请在 Windows 前台启动/重启 Worker")
    state = worker.get("state") or {}
    if looks_like_linux_worker(state):
        return info, ("队列上的 Worker 看起来是 **Linux 宿主**（QueueRoot/RunsRoot 是 Linux 路径）："
                      "那是 `--allow-non-windows` 的 fixture 降级 Worker，拒绝在它上面做 Windows 验收")
    info["workerHostNameEqualsWslHost"] = (str(state.get("hostName") or "").lower() == platform.node().lower())
    return info, None


# --------------------------------------------------------------------------
# 主流程
# --------------------------------------------------------------------------
def find_dotnet() -> str:
    for candidate in (os.environ.get("DOTNET"), shutil.which("dotnet"), str(Path.home() / ".dotnet" / "dotnet")):
        if candidate and Path(candidate).exists():
            return candidate
    raise SystemExit("找不到 dotnet（设 DOTNET 环境变量或把 dotnet 放进 PATH）")


def resolve_wdb(repo: Path, explicit: str | None, dotnet: str) -> list[str] | None:
    """返回 CLI 前缀 argv（[dotnet, wdb.dll] 或 [可执行文件]）。"""
    candidates: list[Path] = []
    if explicit:
        p = Path(explicit)
        candidates.append((p / "wdb.dll") if p.is_dir() else p)
    if os.environ.get("WDB_DLL"):
        candidates.append(Path(os.environ["WDB_DLL"]))
    candidates.append(BRIDGE_DIR / "bin" / "Release" / "net10.0" / "wdb.dll")
    candidates.append(BRIDGE_DIR / "bin" / "Debug" / "net10.0" / "wdb.dll")
    for c in candidates:
        if c.is_file():
            return [dotnet, str(c)] if c.suffix.lower() == ".dll" else [str(c)]

    build = run([dotnet, "build", str(BRIDGE_DIR / "Bridge.csproj"), "-c", "Release", "--nologo", "-v", "q"],
                cwd=repo, timeout=900.0)
    dll = BRIDGE_DIR / "bin" / "Release" / "net10.0" / "wdb.dll"
    if build.code == 0 and dll.is_file():
        log("（已现场构建桥 CLI）")
        return [dotnet, str(dll)]
    return None


def select_cases(args) -> list[tuple[str, bool, str, object, str]]:
    selected = []
    for name, needs_windows, doc, fn, group in CASES:
        if args.filter and args.filter.lower() not in name.lower():
            continue
        if args.case and name != args.case:
            continue
        if args.static and needs_windows:
            continue
        if args.day1 and group != "base":
            continue
        if args.day2 and group != "day2":
            continue
        selected.append((name, needs_windows, doc, fn, group))
    return selected


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Windows 调试桥 · WSL 侧真实 Windows 验收（只收证据；绝不启动 Worker、绝不 --allow-non-windows）",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--static", action="store_true", help="只跑不依赖 Worker 的离线用例（今天就能跑）")
    parser.add_argument("--list", action="store_true", help="列出用例后退出")
    parser.add_argument("--case", help="只跑单个用例（精确名，见 --list）")
    parser.add_argument("--filter", help="用例名子串过滤")
    parser.add_argument("--day1", action="store_true", help="只跑 day1 基础 11 用例（不重复 day2 边界用例）")
    parser.add_argument("--day2", action="store_true", help="只跑 day2 边界用例（默认证据改存 windows-day2-boundaries.json）")
    parser.add_argument("--queue", help=f"队列根（默认 {DEFAULT_QUEUE}）")
    parser.add_argument("--work-root", help=f"临时工作根（默认 {DEFAULT_WORK_ROOT}）")
    parser.add_argument("--evidence", help=f"证据 JSON 输出路径（默认 {DEFAULT_EVIDENCE}）")
    parser.add_argument("--wdb", help="桥 CLI 的 dll/目录（默认自动查找，找不到则现场构建）")
    parser.add_argument("--job-timeout", type=int, default=600, help="任务硬超时秒（默认 600）")
    parser.add_argument("--wait-timeout", type=int, default=720, help="wdb wait 的 --wait-timeout（默认 720）")
    parser.add_argument("--tree-ready-seconds", type=float, default=150.0, help="等三层树吐出 cwd 的秒数")
    parser.add_argument("--pidfile-confirm-seconds", type=float, default=40.0, help="等 pidfile 出现 3 条 PID 的秒数")
    parser.add_argument("--timeout-case-seconds", type=int, default=20,
                        help="day2 timeout 用例的任务 --timeout（含构建耗时，默认 20s 秒）")
    parser.add_argument("--wait-idle-seconds", type=float, default=0.0,
                        help="提交任何任务前最多等队列空闲多少秒（0=不等；与别人共用串行队列时建议 900）")
    parser.add_argument("--strict-skips", action="store_true", help="skipped 也算失败")
    parser.add_argument("--clean", action="store_true", help="收尾删除本次临时工作根（默认保留取证）")
    args = parser.parse_args()

    selected = select_cases(args)
    # 「这一份 JSON 是不是整组跑出来的」的期望条数（--filter/--case 时 selected 会小于它）
    if args.static:
        expected_full_set = sum(1 for _n, nw, _d, _f, _g in CASES if not nw)
    elif args.day1:
        expected_full_set = sum(1 for _n, _nw, _d, _f, g in CASES if g == "base")
    elif args.day2:
        expected_full_set = sum(1 for _n, _nw, _d, _f, g in CASES if g == "day2")
    else:
        expected_full_set = len(CASES)
    if args.list:
        for name, needs_windows, doc, _fn, group in selected:
            print(f"[{'win ' if needs_windows else 'static'}·{group}] {name} — {doc}")
        print(f"共 {len(selected)} 个用例（--static 只跑 static；--day1/--day2 分组；全量需要健康的 Windows Worker）")
        return 0
    if not selected:
        print("没有匹配的用例", file=sys.stderr)
        return 2

    dotnet = find_dotnet()
    queue = Path(args.queue).resolve() if args.queue else DEFAULT_QUEUE
    stamp = time.strftime("%Y%m%d-%H%M%S") + "-" + uuid.uuid4().hex[:6]
    work_root = (Path(args.work_root).resolve() if args.work_root else DEFAULT_WORK_ROOT) / stamp
    if args.evidence:
        evidence_path = Path(args.evidence).resolve()
    elif args.static:
        evidence_path = DEFAULT_EVIDENCE_STATIC   # 纯离线自检绝不覆盖 Windows 证据
    elif args.day2:
        evidence_path = DEFAULT_EVIDENCE_DAY2     # day2 绝不覆盖 day1 的证据
    else:
        evidence_path = DEFAULT_EVIDENCE
    work_root.mkdir(parents=True, exist_ok=True)
    open_log(evidence_path.parent / (evidence_path.stem + ".log"))

    prefix = resolve_wdb(REPO_ROOT, args.wdb, dotnet)
    if prefix is None:
        log("找不到也无法构建桥 CLI（wdb.dll）：请先 dotnet build tools/windows-debug-bridge/Bridge.csproj -c Release")
        return 2

    ctx = Ctx(dotnet=dotnet, wdb_argv_prefix=prefix, repo=REPO_ROOT, queue=queue, work_root=work_root,
              evidence_path=evidence_path, mode="static" if args.static else "full",
              job_timeout=args.job_timeout, wait_timeout=args.wait_timeout,
              tree_ready_seconds=args.tree_ready_seconds, pidfile_confirm_seconds=args.pidfile_confirm_seconds,
              clean=args.clean, strict_skips=args.strict_skips,
              timeout_case_seconds=args.timeout_case_seconds)
    bridge = Bridge(ctx)
    shared: dict = {"createdJobs": [], "marker_control": {}}

    started = time.time()
    log(f"windows-smoke · {len(selected)} 个用例 · mode={ctx.mode}")
    log(f"  repo     = {REPO_ROOT}")
    log(f"  cli      = {' '.join(ctx.wdb_argv_prefix)}")
    log(f"  queue    = {ctx.queue}")
    log(f"  work     = {ctx.work_root}")
    log(f"  evidence = {evidence_path}")
    log("-" * 78)

    blocked: str | None = None
    preflight_info: dict = {}
    if ctx.mode == "full":
        preflight_info, blocked = preflight(ctx, bridge)
        if blocked:
            log(f"[BLOCKED] {blocked}")
        elif args.wait_idle_seconds > 0:
            # 队列是串行的：若别的作者正在跑长任务，本脚本的时序类用例（timeout/日志跟随）会被排队污染。
            # 只在**提交任何任务之前**等空闲；等待期不投递任何任务。
            t_idle = time.time()
            info: dict = {}
            ok_idle = poll_until(lambda: (bridge.queue_idle()[0] or None), timeout=args.wait_idle_seconds,
                                 interval=2.0) is not None
            _, info = bridge.queue_idle()
            preflight_info["idleWait"] = {"waitedSeconds": round(time.time() - t_idle, 1),
                                          "idle": ok_idle, "last": info}
            log(f"队列空闲等待：waited={preflight_info['idleWait']['waitedSeconds']}s idle={ok_idle}")
    else:
        log("（--static：不检查也不触碰 Worker；Windows 侧一律未测）")

    results: list[CaseResult] = []
    if blocked is None:
        for name, needs_windows, _doc, fn, _group in selected:
            t0 = time.time()
            rec = Rec()
            try:
                evidence = fn(ctx, rec, bridge, shared) if needs_windows else fn(ctx, rec)
                rec.finish()
                results.append(CaseResult(name, "passed", evidence=evidence or {}, duration=time.time() - t0))
                log(f"[PASS] {name} ({time.time() - t0:.1f}s)")
            except SkipCase as exc:
                results.append(CaseResult(name, "skipped", reason=str(exc),
                                          evidence=exc.evidence or {}, duration=time.time() - t0))
                log(f"[SKIP] {name} — {exc}")
            except CaseFailure as exc:
                results.append(CaseResult(name, "failed", reason=str(exc),
                                          evidence={"checks": rec.checks}, duration=time.time() - t0))
                log(f"[FAIL] {name} ({time.time() - t0:.1f}s)\n       {exc}")
            except Exception as exc:  # 脚本/环境异常也必须可见
                results.append(CaseResult(name, "failed", reason=f"{type(exc).__name__}: {exc}",
                                          evidence={"checks": rec.checks}, duration=time.time() - t0))
                log(f"[FAIL] {name} ({type(exc).__name__})\n       {exc}")
            finally:
                last = results[-1] if results else None
                if last is not None and last.name == name and rec.checks:
                    ev = dict(last.evidence or {})
                    ev.setdefault("checks", rec.checks)
                    last.evidence = ev

    passed = sum(1 for r in results if r.status == "passed")
    failed = [r for r in results if r.status == "failed"]
    skipped = [r for r in results if r.status == "skipped"]
    for r in skipped:
        log(f"  [skipped] {r.name}: {r.reason}")

    result_code = ("STATIC_ONLY" if ctx.mode == "static"
                   else "BLOCKED_WORKER_NOT_HEALTHY" if blocked
                   else "FAILED" if failed else "PASSED")
    docs = {
        "tool": "windows-smoke",
        "schema": 1,
        "mode": ctx.mode,
        "result": result_code,
        "ok": blocked is None and not failed and not (ctx.strict_skips and skipped),
        "windowsVerified": ctx.mode == "full" and blocked is None and bool(passed) and not failed,
        "guiVerified": False,  # 顶层再显式声明一次：本脚本从不做 GUI 验收
        "scriptPath": str(Path(__file__).resolve()),
        "scriptSha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        "blockedReason": blocked,
        "startedUtc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(started)),
        "finishedUtc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "durationSeconds": round(time.time() - started, 2),
        "host": {"node": platform.node(), "platform": platform.platform(), "python": sys.version.split()[0],
                 "wslDistro": os.environ.get("WSL_DISTRO_NAME", "")},
        "target": {"repoRoot": str(REPO_ROOT), "queueRoot": str(ctx.queue), "cli": ctx.wdb_argv_prefix,
                   "workRoot": str(ctx.work_root),
                   "log": str(evidence_path.parent / (evidence_path.stem + ".log"))},
        "preflight": preflight_info,
        # 可核性：本脚本创建过的每个 job id 都列出来（配合 queue/jobs/<id>/status.json 逐条复核）
        "jobs": sorted(shared.get("createdJobs", [])),
        # 防误读：单 case / --filter 跑出来的 JSON 不得被当成"全套通过"
        "caseSet": {
            "selected": len(selected),
            "expectedFullSet": expected_full_set,
            "isFullSet": len(selected) == expected_full_set,
            "staticOnly": bool(args.static),
            "day1": bool(args.day1),
            "day2": bool(args.day2),
            "filter": args.filter,
            "case": args.case,
            "selectedNames": [name for name, _nw, _d, _f, _g in selected],
        },
        "guarantees": {
            "neverStartsWorker": True,
            "neverAllowNonWindows": True,
            "neverShutdown": True,
            "neverKillsProcesses": "脚本不杀任何进程；树是否消失全部由 Windows 侧 ProcessProbe 只读判定",
            "writesOutsideWorkspace": "仅经 \\\\wsl.localhost UNC 写本次 work-root 内的 marker 文件",
            "touchesRealGalleryData": False,
            "guiVerified": False,
        },
        "totals": {"passed": passed, "failed": len(failed), "skipped": len(skipped), "selected": len(selected)},
        "cases": [{"name": r.name, "status": r.status, "reason": r.reason,
                   "durationSeconds": round(r.duration, 2), "evidence": r.evidence} for r in results],
        "notTested": NOT_TESTED,
    }
    _write_json_atomic(evidence_path, docs)

    log("-" * 78)
    log(f"结果：{result_code} pass={passed} fail={len(failed)} skip={len(skipped)} 用时={time.time() - started:.1f}s")
    log(f"证据：{evidence_path}")
    if failed:
        log("失败用例：" + ", ".join(r.name for r in failed))
    if ctx.clean:
        shutil.rmtree(ctx.work_root, ignore_errors=True)
        log(f"（已删除本次工作根 {ctx.work_root}）")
    else:
        log(f"（保留工作根取证：{ctx.work_root}；加 --clean 可删除）")

    if blocked is not None:
        return 2
    if failed:
        return 1
    if ctx.strict_skips and skipped:
        return 1
    return 0


def _write_json_atomic(path: Path, payload: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + f".tmp-{uuid.uuid4().hex[:6]}")
    tmp.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    os.replace(tmp, path)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
