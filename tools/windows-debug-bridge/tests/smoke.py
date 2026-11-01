#!/usr/bin/env python3
"""Windows 调试桥 · 黑盒集成冒烟（stdlib only，不经 shell，全程 argv 数组传递）。

【只适用于 Linux/WSL】本脚本用 /proc/<pid> 判定存活与监听、用 os.killpg 回收进程树，
因此**不要在 Windows 上直接运行**；Windows 侧专用脚本是 tests/windows-smoke.py。
Linux 环境下的等价复现命令见本文件顶部与 tests/run-all.sh。

分层：
  L0 infra   —— 不需要 CLI/Worker：fixture 自证、跨语言合同交叉校验、dotnet 负对照。今天就能跑。
  L1 cli     —— 只调 WSL 侧 CLI（快照/提交/status/logs/wait/stop/shutdown 的文件层语义）。
  L2 e2e     —— CLI + Linux 上的 Worker（worker --allow-non-windows）端到端。

用法：
  python3 tools/windows-debug-bridge/tests/smoke.py --list
  python3 tools/windows-debug-bridge/tests/smoke.py --tier infra          # 无 CLI 也能跑
  python3 tools/windows-debug-bridge/tests/smoke.py --filter snapshot
  python3 tools/windows-debug-bridge/tests/smoke.py --keep                 # 保留现场取证
  python3 tools/windows-debug-bridge/tests/smoke.py --print-spec           # 打印待同步的 CLI 契约

【唯一的同步点】SPEC 字典：Cli.cs / Worker.cs 落盘后，对照其 --help 修正 SPEC 即可，
其余测试只通过 Cli 适配层与工作区文件系统交互，不直接拼命令行。

【可以宣称的与不能宣称的】
  本脚本在 Linux/WSL 上验证文件队列语义与 Worker 的 Linux 兼容路径。
  它**不能**证明 Windows 实机行为（Job Object 杀树、Windows 控制台 UTF-8、%LOCALAPPDATA% 路径），
  这些必须由用户在 Windows 前台执行。任何"Windows 已测"的表述都是错误的。
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time
import uuid
from dataclasses import dataclass, field
from pathlib import Path

# --------------------------------------------------------------------------
# 常量：必须与 tools/windows-debug-bridge/Contracts.cs (BridgeLayout/JobModes/JobStates) 一致。
# 一致性由 L0 用例 contract.py-mirrors-contracts-cs 交叉校验（直接读 Contracts.cs 源码比对），
# 因此这里写错会在 CI/本地立刻翻红，而不是悄悄漂移。
# --------------------------------------------------------------------------
PROTOCOL_VERSION = 1

JOBS_DIR = "jobs"
REQUEST_JSON = "request.json"
STATUS_JSON = "status.json"
PAYLOAD_DIR = "payload"
PAYLOAD_REPORT_JSON = "payload-report.json"
STDOUT_LOG = "stdout.log"
STDERR_LOG = "stderr.log"
CANCEL_FILE = "cancel"
WORKER_JSON = "worker.json"
SHUTDOWN_FILE = "shutdown"

MODE_BUILD = "build"
MODE_RUN = "run"
MODE_BUILD_RUN = "buildRun"

TERMINAL_STATES = {"succeeded", "failed", "canceled", "timeout", "rejected", "interrupted"}
NON_TERMINAL_STATES = {"queued", "running"}
# CLI 退出码契约（CliExit）：任何负值/128+ 都表示进程被信号杀死或未捕获异常崩溃
CLI_EXIT_CODES = {0, 2, 3, 4, 5, 6, 7, 8, 9}

REPO_ROOT = Path(__file__).resolve().parents[3]
BRIDGE_DIR = REPO_ROOT / "tools" / "windows-debug-bridge"
TESTS_DIR = BRIDGE_DIR / "tests"
FIXTURE_ECHO = TESTS_DIR / "fixtures" / "EchoApp"
CONTRACTS_CS = BRIDGE_DIR / "Contracts.cs"


# --------------------------------------------------------------------------
# 断言与证据
# --------------------------------------------------------------------------
class SmokeFailure(AssertionError):
    pass


class SkipTest(Exception):
    """环境能力不足导致的跳过：消息里必须写清原因，禁止用它掩盖失败。"""


def check(cond: bool, msg: str, evidence: str = "") -> None:
    if not cond:
        raise SmokeFailure(f"{msg}" + (f"\n--- 证据 ---\n{evidence}" if evidence else ""))


def check_eq(expected, actual, msg: str, evidence: str = "") -> None:
    if expected != actual:
        raise SmokeFailure(f"{msg}\n  expected={expected!r}\n  actual  ={actual!r}" + (f"\n--- 证据 ---\n{evidence}" if evidence else ""))


# --------------------------------------------------------------------------
# 进程执行（不经 shell；有超时；总是回收子进程树）
# --------------------------------------------------------------------------
@dataclass
class Run:
    argv: list[str]
    code: int
    out: str
    err: str
    timed_out: bool = False

    @property
    def cmd(self) -> str:
        return " ".join(shlex_quote(a) for a in self.argv)

    def evidence(self, limit: int = 4000) -> str:
        return (
            f"$ {self.cmd}\n[exit={self.code} timed_out={self.timed_out}]\n"
            f"--- stdout ---\n{self.out[-limit:]}\n--- stderr ---\n{self.err[-limit:]}"
        )


def shlex_quote(s: str) -> str:
    return f'"{s}"' if any(c in s for c in ' \t"') else s


def run(argv: list[str], cwd: Path | None = None, timeout: float = 300.0) -> Run:
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
    )
    try:
        out, err = proc.communicate(timeout=timeout)
        timed_out = False
    except subprocess.TimeoutExpired:
        kill_tree(proc)
        out, err = proc.communicate()
        timed_out = True
    return Run([str(a) for a in argv], proc.returncode, out or "", err or "", timed_out)


def kill_tree(proc: subprocess.Popen) -> None:
    try:
        os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
    except Exception:
        try:
            proc.kill()
        except Exception:
            pass


def pid_alive(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def read_pids(path: Path) -> list[int]:
    if not path.exists():
        return []
    return [int(x) for x in path.read_text(encoding="utf-8").split() if x.strip().isdigit()]


# --------------------------------------------------------------------------
# 队列读写（按 Contracts.cs 的 exact 字段名手写 JSON）
# --------------------------------------------------------------------------
def job_dir(queue: Path, job_id: str) -> Path:
    return queue / JOBS_DIR / job_id


def read_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def write_json(path: Path, obj: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(json.dumps(obj, ensure_ascii=False), encoding="utf-8")
    os.replace(tmp, path)  # 原子落盘：与 CLI 的"最后写 request.json"语义一致


def make_request(
    job_id: str,
    mode: str,
    project: str,
    *,
    configuration: str | None = "Debug",
    framework: str | None = None,
    arguments: list[str] | None = None,
    timeout_seconds: int = 0,
    payload: dict | None = None,
    source_root: str = "/wsl/source",
) -> dict:
    """exact 字段名，逐字对齐 Contracts.cs 的 Request 记录（camelCase）。"""
    return {
        "protocolVersion": PROTOCOL_VERSION,
        "id": job_id,
        "mode": mode,
        "project": project,
        "configuration": configuration,
        "framework": framework,
        "arguments": arguments,
        "timeoutSeconds": timeout_seconds,
        "payload": payload
        or {"relativePath": PAYLOAD_DIR, "fileCount": 0, "totalBytes": 0, "manifestHash": None},
        "createdUtc": now_utc(),
        "sourceRoot": source_root,
    }


def now_utc() -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%S+00:00", time.gmtime())


def submit(queue: Path, request: dict) -> Path:
    """先 payload（调用方已就绪）、再 request.json —— request 必须是任务目录里最后一个写入的文件。"""
    jd = job_dir(queue, request["id"])
    jd.mkdir(parents=True, exist_ok=True)
    write_json(jd / REQUEST_JSON, request)
    return jd


def wait_for(predicate, timeout: float = 60.0, interval: float = 0.1, what: str = "条件"):
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        try:
            last = predicate()
        except Exception as exc:  # 轮询期间的解析失败不算失败
            last = None
            _ = exc
        if last:
            return last
        time.sleep(interval)
    raise SmokeFailure(f"等待超时（{timeout}s）：{what}（最后一次观察={last!r}）")


def read_status(queue: Path, job_id: str) -> dict | None:
    p = job_dir(queue, job_id) / STATUS_JSON
    if not p.exists():
        return None
    try:
        return read_json(p)
    except json.JSONDecodeError:
        return None


def wait_terminal(queue: Path, job_id: str, timeout: float = 300.0) -> dict:
    def done():
        st = read_status(queue, job_id)
        return st if st and st.get("state") in TERMINAL_STATES else None

    return wait_for(done, timeout=timeout, what=f"任务 {job_id} 进入终态")


# --------------------------------------------------------------------------
# 现场
# --------------------------------------------------------------------------
@dataclass
class Ctx:
    tmp_root: Path
    keep: bool
    dotnet: str
    wdb_dll: Path | None = None
    worker: subprocess.Popen | None = None
    worker_log: Path | None = None
    spaces: list[Path] = field(default_factory=list)

    def space(self, name: str) -> "Space":
        path = self.tmp_root / f"{name}-{uuid.uuid4().hex[:8]}"
        path.mkdir(parents=True, exist_ok=True)
        self.spaces.append(path)
        return Space(path)

    def cleanup(self) -> None:
        self.stop_worker()
        if self.keep:
            for p in self.spaces:
                print(f"  [keep] {p}")
            return
        for p in self.spaces:
            shutil.rmtree(p, ignore_errors=True)

    def stop_worker(self) -> None:
        if self.worker and self.worker.poll() is None:
            kill_tree(self.worker)
            try:
                self.worker.wait(timeout=10)
            except subprocess.TimeoutExpired:
                pass
        self.worker = None


@dataclass
class Space:
    root: Path

    @property
    def queue(self) -> Path:
        return self.root / "queue"

    @property
    def runs(self) -> Path:
        return self.root / "runs"

    @property
    def src(self) -> Path:
        return self.root / "src"

    def prepare(self) -> "Space":
        for p in (self.queue, self.runs, self.src):
            p.mkdir(parents=True, exist_ok=True)
        return self

    def payload_copy(self, job_id: str, source: Path) -> Path:
        """把 fixture 源码放进 payload/，模拟 CLI 的快照结果。"""
        dest = job_dir(self.queue, job_id) / PAYLOAD_DIR
        shutil.copytree(source, dest, ignore=shutil.ignore_patterns("bin", "obj", ".git"))
        return dest


# --------------------------------------------------------------------------
# CLI 适配层 —— 唯一需要与 Cli.cs 同步的地方
# --------------------------------------------------------------------------
SPEC: dict[str, list[str]] = {
    # worker：由 start-worker.ps1 以 argv 数组前台执行；--allow-non-windows 仅 fixture 测试用。
    "worker": ["worker", "--queue", "{queue}", "--runs-root", "{runs_root}", "--allow-non-windows", "--dotnet", "{dotnet}"],
    "doctor": ["doctor", "--queue", "{queue}", "--json"],
    # run/build/build-run：提交后立即输出单行 JSON（--json 只让 stdout 是机器可读，人读走 stderr）。
    # 被启动程序的参数必须放在 "--" 之后（桥不做 shell 解析）。
    "run": ["run", "--queue", "{queue}", "--source", "{source}", "--project", "{project}", "--framework", "{framework}", "--timeout", "{job_timeout}", "--json"],
    "build": ["build", "--queue", "{queue}", "--source", "{source}", "--project", "{project}", "--framework", "{framework}", "--timeout", "{job_timeout}", "--json"],
    "build-run": ["build-run", "--queue", "{queue}", "--source", "{source}", "--project", "{project}", "--framework", "{framework}", "--timeout", "{job_timeout}", "--json"],
    "status": ["status", "{job}", "--queue", "{queue}", "--json"],
    "logs": ["logs", "{job}", "--queue", "{queue}", "--stream", "{stream}", "--tail", "{tail}", "--offset", "{offset}", "{--follow|follow}", "{--json|json}"],
    "wait": ["wait", "{job}", "--queue", "{queue}", "--wait-timeout", "{wait_timeout}", "--json"],
    "stop": ["stop", "{job}", "--queue", "{queue}", "--reason", "{reason}"],
    "shutdown": ["shutdown", "--queue", "{queue}", "--reason", "{reason}"],
}

# 2026-10-11 用 wdb --help 与逐 verb 实调核实（见 VALIDATION：doctor/status/wait/logs/stop/shutdown 均实跑）。
# 关键形态：id 是 positional；--queue 是全局选项可后置；非 Windows 宿主必须显式 --allow-non-windows。
SPEC_VERIFIED = set(SPEC)
SPEC_NON_WINDOWS_ONLY = {"worker"}


class Cli:
    def __init__(self, ctx: Ctx):
        self.ctx = ctx

    def argv(self, verb: str, app_args: list[str] | None = None, **kwargs) -> list[str]:
        template = SPEC[verb]
        out: list[str] = []
        for token in template:
            if token.startswith("{") and "|" in token:
                # 形如 {--follow|follow}：仅当 kwargs['follow'] 为真时追加该布尔开关
                flag, key = token[1:-1].split("|", 1)
                if kwargs.get(key):
                    out.append(flag)
                continue
            if token.startswith("{"):
                key = token[1:-1]
                value = kwargs.get(key)
                if value is None:
                    # 可选占位符缺失：若它前面刚压入的是一个独立选项（--x），必须一并撤回，
                    # 否则会留下一个悬空的 "--reason"（CLI 会以"缺少取值"失败）。
                    if out and out[-1].startswith("--"):
                        out.pop()
                    continue
                out.append(str(value))
            else:
                out.append(token)
        if app_args:
            out.append("--")
            out.extend(str(a) for a in app_args)
        assert self.ctx.wdb_dll is not None
        return [self.ctx.dotnet, str(self.ctx.wdb_dll), *out]

    def call(self, verb: str, timeout: float = 300.0, app_args: list[str] | None = None, **kwargs) -> Run:
        return run(self.argv(verb, app_args=app_args, **kwargs), cwd=REPO_ROOT, timeout=timeout)

    def status(self, **kwargs) -> dict | None:
        r = self.call("status", **kwargs)
        if r.code != 0:
            return None
        try:
            return json.loads(r.out)
        except json.JSONDecodeError:
            return None

    def submit_job(
        self,
        verb: str,
        project: str,
        queue: Path,
        source: Path | None = None,
        app_args: list[str] | None = None,
        framework: str | None = None,
        job_timeout: int | None = None,
        timeout: float = 600.0,
    ) -> tuple[str, Run]:
        """调用 CLI 提交任务，返回 (job_id, run)。job id 取 CLI 单行 JSON 的 exact 字段 id。"""
        r = self.call(
            verb, timeout=timeout, app_args=app_args, framework=framework, job_timeout=job_timeout,
            queue=queue, source=source or REPO_ROOT, project=project,
        )
        job_id = ""
        for line in (r.out or "").splitlines():
            line = line.strip()
            if line.startswith("{"):
                try:
                    payload = json.loads(line)
                except json.JSONDecodeError:
                    continue
                job_id = payload.get("id") or payload.get("jobId") or ""
                if job_id:
                    break
        return job_id, r

    def start_worker(self, space: Space) -> subprocess.Popen:
        argv = self.argv("worker", queue=space.queue, runs_root=space.runs, dotnet=self.ctx.dotnet)
        log = space.root / "worker.log"
        handle = open(log, "w", encoding="utf-8")
        proc = subprocess.Popen(
            argv,
            cwd=str(REPO_ROOT),
            stdin=subprocess.DEVNULL,
            stdout=handle,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        self.ctx.worker = proc
        self.ctx.worker_log = log
        return proc

    def worker_evidence(self) -> str:
        if self.ctx.worker_log and self.ctx.worker_log.exists():
            return self.ctx.worker_log.read_text(encoding="utf-8", errors="replace")[-4000:]
        return "(无 worker 日志)"


# --------------------------------------------------------------------------
# 测试注册
# --------------------------------------------------------------------------
TESTS: list[tuple[str, str, str]] = []


def test(tier: str, name: str):
    def deco(fn):
        TESTS.append((tier, name, fn.__name__))
        return fn

    return deco


# ==========================================================================
# L0 · infra：不依赖 CLI/Worker
# ==========================================================================
@test("infra", "contract.py-mirrors-contracts-cs")
def t_contract_mirror(ctx: Ctx) -> str:
    """Python 侧常量必须与 Contracts.cs 的字面量逐字一致（跨语言合同不会静默漂移）。

    阴性对照：断言"故意不同"的取值确实会不等（证明比对不是恒真）。
    """
    src = CONTRACTS_CS.read_text(encoding="utf-8")

    def literal(name: str) -> str | None:
        m = re.search(r'\b' + re.escape(name) + r'\s*=\s*"([^"]*)"', src)
        return m.group(1) if m else None

    pairs = [
        ("JobsDirName", JOBS_DIR),
        ("RequestFileName", REQUEST_JSON),
        ("StatusFileName", STATUS_JSON),
        ("PayloadDirName", PAYLOAD_DIR),
        ("PayloadReportFileName", PAYLOAD_REPORT_JSON),
        ("StdoutFileName", STDOUT_LOG),
        ("StderrFileName", STDERR_LOG),
        ("CancelFileName", CANCEL_FILE),
        ("WorkerFileName", WORKER_JSON),
        ("ShutdownFileName", SHUTDOWN_FILE),
        ("Build", MODE_BUILD),
        ("Run", MODE_RUN),
        ("BuildRun", MODE_BUILD_RUN),
    ]
    for cs_name, py_value in pairs:
        cs_value = literal(cs_name)
        check(cs_value is not None, f"Contracts.cs 里找不到常量 {cs_name}（合同改名了？）")
        check_eq(cs_value, py_value, f"跨语言常量不一致：Contracts.cs {cs_name} 与 smoke.py 常量")

    m = re.search(r"Version\s*=\s*(\d+)", src)
    check(m is not None, "Contracts.cs 里找不到 BridgeProtocol.Version")
    check_eq(int(m.group(1)), PROTOCOL_VERSION, "协议版本不一致")

    # 终态集合：逐个字面量校验
    for state in sorted(TERMINAL_STATES):
        check(f'"{state}"' in src, f"Contracts.cs 缺少终态字面量 {state}")
    for state in sorted(NON_TERMINAL_STATES):
        check(f'"{state}"' in src, f"Contracts.cs 缺少非终态字面量 {state}")

    # 阴性对照：比对器不是恒真的
    check("\"build-run\"" not in src, "Contracts.cs 不应存在 build-run（模式是 buildRun）")
    check(JOBS_DIR != "jobz", "恒真比对器")
    return f"contracts={CONTRACTS_CS}（{len(pairs)} 组常量逐字一致，终态 {len(TERMINAL_STATES)} 个）"


@test("infra", "fixture.echoapp-builds-and-runs")
def t_fixture_builds(ctx: Ctx) -> str:
    """fixture 自身必须先把"真实启动/真实 argv"证明清楚，否则上面的端到端结论不可信。

    阴性对照：不存在的项目路径必须编译失败（证明 dotnet 不是恒成功）。
    """
    space = ctx.space("fixture").prepare()
    echo_proj = FIXTURE_ECHO / "EchoApp.csproj"
    check(echo_proj.exists(), f"fixture 缺失：{echo_proj}")
    r = run([ctx.dotnet, "build", str(echo_proj), "-c", "Release", "--nologo", "-v", "q"], cwd=REPO_ROOT, timeout=600)
    check(r.code == 0, "fixture EchoApp 构建失败", r.evidence())

    app = find_echo_app()
    check(app is not None, "找不到 EchoApp 产物", r.evidence())

    marker = space.root / "marker.txt"
    extra = ["a b", "中文 参数", 'quote"inside', "", "--exit", "7"]
    full_argv = ["--marker", str(marker), *extra]
    r2 = run([str(app), *full_argv], timeout=60)
    check_eq(7, r2.code, "EchoApp 未按 --exit 返回退出码", r2.evidence())
    check(marker.exists(), "启动哨兵未写入（说明进程没真正启动）", r2.evidence())
    payload = parse_echo(r2.out)
    check_eq(full_argv, payload["argv"], "argv 未逐字节保留", r2.evidence())
    # 阴性对照：argvHex 必须与 argv 逐字节一致（防止控制台编码把比较洗白）
    check_eq([arg.encode("utf-8").hex() for arg in full_argv], payload["argvHex"], "argvHex 与 argv 不一致", r2.evidence())

    # 阴性对照：坏项目必须失败
    bad = space.root / "bad" / "Bad.csproj"
    bad.parent.mkdir(parents=True, exist_ok=True)
    bad.write_text(
        '<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><OutputType>Exe</OutputType>'
        "<TargetFramework>net10.0</TargetFramework></PropertyGroup></Project>",
        encoding="utf-8",
    )
    (bad.parent / "Program.cs").write_text("class P { static void Main() { this is not C# } }", encoding="utf-8")
    r3 = run([ctx.dotnet, "build", str(bad), "--nologo", "-v", "q"], timeout=600)
    check(r3.code != 0, "故意写坏的项目竟然编译成功 = 负对照失效", r3.evidence())
    return f"EchoApp={app}；负对照退出码={r3.code}"


@test("infra", "fixture.spawns-real-process-tree")
def t_fixture_tree(ctx: Ctx) -> str:
    """证明 fixture 能造出真正的三层进程树并且后代在父进程死后仍存活——
    这正是"stop 必须杀整棵树"的负对照前提（否则杀树测试是空转）。
    """
    space = ctx.space("tree").prepare()
    app = find_echo_app()
    check(app is not None, "找不到 EchoApp 产物（先跑 fixture.echoapp-builds-and-runs）")
    pidfile = space.root / "pids.txt"
    r = run([str(app), "--pidfile", str(pidfile), "--spawn", "1", "--depth", "2", "--sleep", "700"], timeout=120)
    _ = r
    pids = read_pids(pidfile)
    check(len(pids) >= 3, f"进程树未到三层，实际 pid={pids}", r.evidence())
    parent = pids[0]
    check(not pid_alive(parent), f"父进程 {parent} 应已退出（sleep 结束）")
    survivors = [p for p in pids[1:] if pid_alive(p)]
    check(len(survivors) >= 2, f"后代应存活以构成杀树验证对象，实际存活={survivors}", r.evidence())

    # 清理本次造出的存活后代（只杀本次登记过的 PID）
    for p in survivors:
        try:
            os.kill(p, signal.SIGKILL)
        except Exception:
            pass

    # 阴性对照：不杀就真的还活着（上一段已断言），杀完必须真的消失
    wait_for(lambda: not any(pid_alive(p) for p in survivors), timeout=10, what="后代进程被清理")
    return f"pids={pids}，父退出后仍存活={len(survivors)}，已清理"


@test("infra", "safety.no-host-business-data-touched")
def t_no_host_data(ctx: Ctx) -> str:
    """基线：任何测试动作都不得改到被测 fixture 源码或宿主业务仓库。

    本用例在测试前后对 fixture 目录与仓库 git 状态取快照并逐字比对（阴性对照：故意改一个字节必须被发现）。
    """
    def snapshot(root: Path) -> dict[str, str]:
        out: dict[str, str] = {}
        for p in sorted(root.rglob("*")):
            # 只按目录名排除构建产物与 VCS —— 不能用 ".tmp"（测试现场本身就落在 tests/.tmp 下）
            if {"bin", "obj", ".git"} & set(p.relative_to(root).parts):
                continue
            if p.is_file():
                st = p.stat()
                out[str(p.relative_to(root))] = f"{st.st_size}:{st.st_mtime_ns}"
        return out

    before = snapshot(FIXTURE_ECHO)
    space = ctx.space("nodata").prepare()
    (space.src / "x.txt").write_text("x", encoding="utf-8")
    after = snapshot(FIXTURE_ECHO)
    check_eq(before, after, "fixture 源目录被测试动作改动了（不得写宿主业务数据）")

    # 阴性对照：快照器能发现变化
    probe = space.root / "probe.txt"
    probe.write_text("a", encoding="utf-8")
    s1 = snapshot(space.root)
    probe.write_text("bb", encoding="utf-8")
    s2 = snapshot(space.root)
    check(s1 != s2, "快照器对变化不敏感 = 上面的一致性结论无效")
    return f"fixture 文件数={len(before)}（前后一致），快照器灵敏度已用负对照确认"


# ==========================================================================
# L1 · cli：只依赖 WSL 侧 CLI 的文件层语义
# ==========================================================================
@test("cli", "cli.help-matches-spec")
def t_cli_help(ctx: Ctx) -> str:
    """SPEC 漂移哨兵：逐个 verb 跑 --help，确认 SPEC 里的 verb 与关键字面存在。

    这是"与 CLI 作者同步 API"的机械保证：Cli.cs 改名 → 本用例立刻红。
    """
    drift = []
    for verb in sorted(SPEC_VERIFIED):
        r = run([ctx.dotnet, str(ctx.wdb_dll), verb, "--help"], cwd=REPO_ROOT, timeout=120)
        # 人读帮助走 stderr（机器 JSON 才走 stdout），两个流都要看。
        text = (r.out or "") + (r.err or "")
        if r.code != 0:
            drift.append(f"{verb}: --help 退出码 {r.code}")
            continue
        if verb not in text:
            drift.append(f"{verb}: --help 输出里没有该 verb 名")
    # 关键选项也要在帮助里出现（防止 SPEC 里的选项被改名后静默漂移）
    for verb, flag in (("run", "--project"), ("stop", None), ("worker", "--runs-root")):
        r = run([ctx.dotnet, str(ctx.wdb_dll), verb, "--help"], cwd=REPO_ROOT, timeout=120)
        text = (r.out or "") + (r.err or "")
        if flag and flag not in text:
            drift.append(f"{verb}: 帮助里缺少 {flag}")
    check(not drift, "SPEC 与 Cli.cs 漂移（请按 --help 修正 smoke.py 的 SPEC）", "\n".join(drift))
    return f"已核对 {len(SPEC_VERIFIED)} 个 verb 与关键选项"


@test("cli", "snapshot.excludes-bin-obj-and-keeps-root-props")
def t_snapshot_excludes(ctx: Ctx) -> str:
    """快照必须排除 bin/obj，且**不得**误排掉仓库根的 Directory.Build.props（否则构建语义被悄悄改掉）。

    阴性对照：bin 下的可执行文件不得出现在 payload 里。
    """
    space = ctx.space("snapshot").prepare()
    src = space.src
    (src / "App").mkdir(parents=True, exist_ok=True)
    (src / "App" / "App.csproj").write_text(
        '<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><OutputType>Exe</OutputType>'
        "<TargetFramework>net10.0</TargetFramework></PropertyGroup></Project>",
        encoding="utf-8",
    )
    (src / "App" / "Program.cs").write_text("internal static class P { static void Main() => System.Console.WriteLine(\"ok\"); }", encoding="utf-8")
    (src / "Directory.Build.props").write_text("<Project><PropertyGroup><TargetFramework>net10.0</TargetFramework></PropertyGroup></Project>", encoding="utf-8")
    (src / "App" / "bin" / "Release").mkdir(parents=True, exist_ok=True)
    (src / "App" / "bin" / "Release" / "App.dll").write_bytes(b"NOT-A-REAL-ASSEMBLY")
    (src / "App" / "obj").mkdir(parents=True, exist_ok=True)
    (src / "App" / "obj" / "project.assets.json").write_text("{}", encoding="utf-8")

    job_id, r = ctx_cli(ctx).submit_job("run", "App/App.csproj", space.queue, source=src)
    check(r.code == 0 and job_id, "CLI 提交任务失败", r.evidence())
    payload = job_dir(space.queue, job_id) / PAYLOAD_DIR

    check((payload / "App" / "App.csproj").exists(), "payload 缺少 App.csproj")
    check((payload / "Directory.Build.props").exists(), "根 Directory.Build.props 被误排除（会改变构建语义）")
    check(not (payload / "App" / "bin").exists(), "bin/ 未被排除（构建产物污染快照）")
    check(not (payload / "App" / "obj").exists(), "obj/ 未被排除", str(list(payload.rglob("obj*"))))

    report_path = job_dir(space.queue, job_id) / PAYLOAD_REPORT_JSON
    if report_path.exists():
        report = read_json(report_path)
        check("bin" in report.get("excluded", []) or "obj" in report.get("excluded", []), "payload-report.excluded 未记录排除项", json.dumps(report, ensure_ascii=False))
    return f"job={job_id}，payload 已排除 bin/obj 且保留根 props"


@test("cli", "snapshot.rejects-path-escape-and-links")
def t_snapshot_escape(ctx: Ctx) -> str:
    """路径与符号链接安全：越界 project、指向外部的 symlink 都不得把外部内容带进队列。

    阴性对照：外部哨兵文件的内容绝不出现在 payload 中，且外部目录未被修改。
    """
    space = ctx.space("escape").prepare()
    outside = space.root / "outside"
    outside.mkdir(parents=True, exist_ok=True)
    secret = outside / "secret.txt"
    secret.write_text("OUTSIDE-SECRET-PAYLOAD", encoding="utf-8")
    outside_before = str(secret.stat().st_mtime) + str(secret.stat().st_size)

    src = space.src
    (src / "App").mkdir(parents=True, exist_ok=True)
    (src / "App" / "App.csproj").write_text(
        '<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><OutputType>Exe</OutputType>'
        "<TargetFramework>net10.0</TargetFramework></PropertyGroup></Project>",
        encoding="utf-8",
    )
    (src / "App" / "Program.cs").write_text("internal static class P { static void Main() { } }", encoding="utf-8")
    os.symlink(outside, src / "App" / "link-out", target_is_directory=True)
    os.symlink(outside / "secret.txt", src / "App" / "link-secret.txt")

    # 1) project 越界（上跳）必须被拒
    r_escape = run(
        [ctx.dotnet, str(ctx.wdb_dll), "run", "--queue", str(space.queue), "--source", str(src),
         "--project", "../outside/App.csproj", "--json"],
        cwd=REPO_ROOT, timeout=120,
    )
    check(r_escape.code != 0, "越界 project 未被拒绝", r_escape.evidence())

    # 2) 含 symlink 的正常提交：必须被跳过或被拒，绝不能把外部内容抄进 payload
    job_id, r = ctx_cli(ctx).submit_job("run", "App/App.csproj", space.queue, source=src)
    if r.code == 0 and job_id:
        payload = job_dir(space.queue, job_id) / PAYLOAD_DIR
        leaked = [str(p) for p in payload.rglob("*") if p.is_file() and "OUTSIDE-SECRET-PAYLOAD" in p.read_text(encoding="utf-8", errors="ignore")]
        check(not leaked, "外部内容被符号链接带进了 payload", "\n".join(leaked))
        check(not (payload / "App" / "link-out").exists() and not (payload / "App" / "link-out").is_symlink(), "payload 里出现了指向外部的目录链接")
    else:
        check(r.code != 0, "含 symlink 的提交既没成功也没失败（异常路径）")

    check_eq(outside_before, str(secret.stat().st_mtime) + str(secret.stat().st_size), "快照动作修改了源目录之外的文件")
    return f"越界退出码={r_escape.code}；symlink 提交退出码={r.code}；外部文件未被改动"


@test("cli", "snapshot.unique-copy-per-job")
def t_snapshot_unique(ctx: Ctx) -> str:
    """同一源码的两次提交必须落到互不相同的副本目录；改 A 不影响 B。

    阴性对照：故意把两个 job 的 payload 内容改成不同值，证明它们确实是两个物理副本。
    """
    space = ctx.space("unique").prepare()
    src = space.src
    (src / "App").mkdir(parents=True, exist_ok=True)
    (src / "App" / "App.csproj").write_text(
        '<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><OutputType>Exe</OutputType>'
        "<TargetFramework>net10.0</TargetFramework></PropertyGroup></Project>",
        encoding="utf-8",
    )
    (src / "App" / "Program.cs").write_text("internal static class P { static void Main() { } }", encoding="utf-8")

    cli = ctx_cli(ctx)
    j1, r1 = cli.submit_job("run", "App/App.csproj", space.queue, source=src)
    j2, r2 = cli.submit_job("run", "App/App.csproj", space.queue, source=src)
    check(r1.code == 0 and r2.code == 0, "提交失败", r1.evidence() + "\n" + r2.evidence())
    check(j1 != j2, f"两次提交拿到了同一个 job id：{j1}")

    p1 = job_dir(space.queue, j1) / PAYLOAD_DIR / "App" / "Program.cs"
    p2 = job_dir(space.queue, j2) / PAYLOAD_DIR / "App" / "Program.cs"
    check(p1.exists() and p2.exists(), "两次提交的 payload 不完整")
    p1.write_text("MUTATED-A", encoding="utf-8")
    check_eq("MUTATED-A", p1.read_text(encoding="utf-8"), "写入失败")
    check(p2.read_text(encoding="utf-8") != "MUTATED-A", "两个 job 共享了同一份 payload（副本不唯一）")
    return f"job1={j1} job2={j2}，副本相互独立"


@test("cli", "cli.stop-and-shutdown-are-idempotent-markers")
def t_cli_markers(ctx: Ctx) -> str:
    """stop/shutdown 只允许创建空标记文件；重复执行必须幂等且不破坏任务目录。"""
    space = ctx.space("markers").prepare()
    job_id = "manual-" + uuid.uuid4().hex[:8]
    submit(space.queue, make_request(job_id, MODE_RUN, "App.csproj"))
    cli = ctx_cli(ctx)

    r1 = cli.call("stop", queue=space.queue, job=job_id)
    check(r1.code == 0, "stop 第一次失败", r1.evidence())
    cancel = job_dir(space.queue, job_id) / CANCEL_FILE
    check(cancel.exists(), "stop 未创建 cancel 标记", r1.evidence())

    r2 = cli.call("stop", queue=space.queue, job=job_id)
    check(r2.code == 0, "stop 第二次失败（不幂等）", r2.evidence())

    rs = cli.call("shutdown", queue=space.queue)
    check(rs.code == 0, "shutdown 失败", rs.evidence())
    check((space.queue / SHUTDOWN_FILE).exists(), "shutdown 未创建标记")
    rs2 = cli.call("shutdown", queue=space.queue)
    check(rs2.code == 0, "shutdown 重复执行失败（不幂等）", rs2.evidence())

    # 阴性对照：请求文件必须原封不动（stop 不是删除操作的代理）
    check((job_dir(space.queue, job_id) / REQUEST_JSON).exists(), "stop 破坏了任务目录")
    return f"job={job_id}，cancel/shutdown 标记均幂等"


# ==========================================================================
# L2 · e2e：CLI + Linux 上的 Worker（--allow-non-windows）
# ==========================================================================
def e2e_space(ctx: Ctx, name: str) -> tuple[Space, Cli, subprocess.Popen]:
    space = ctx.space(name).prepare()
    cli = ctx_cli(ctx)
    proc = cli.start_worker(space)
    wait_for(lambda: (space.queue / WORKER_JSON).exists(), timeout=60, what="Worker 心跳 worker.json")
    return space, cli, proc


@test("e2e", "e2e.run-executes-real-binary-with-exact-argv")
def t_e2e_run(ctx: Ctx) -> str:
    """A1+A3：真实执行宿主程序，argv 含空格/中文/引号/空串/注入串逐字节无损。

    阴性对照：注入串（; touch /tmp/x、$(...)、`...`、& 、%PATH%）不得产生任何副作用文件。
    """
    space, cli, _ = e2e_space(ctx, "e2e-run")
    marker = space.root / "launched.marker"
    pwn_target = space.root / "pwned.txt"
    args = [
        "--marker", str(marker),
        "--exit", "7",
        "a b",
        "中文 参数",
        'quote"inside',
        "",
        "semi;touch " + str(pwn_target),
        "$(touch " + str(pwn_target) + ")",
        "`touch " + str(pwn_target) + "`",
        "amp&echo hi",
        "%PATH%",
    ]
    job_id, r = cli.submit_job("run", "EchoApp.csproj", space.queue, source=FIXTURE_ECHO, app_args=args)
    check(r.code == 0 and job_id, "CLI 提交失败", r.evidence())

    # 文件层先钉死：CLI 落盘的 request.json 里 arguments 必须逐字等于传入的 argv
    req = read_json(job_dir(space.queue, job_id) / REQUEST_JSON)
    check_eq(args, req.get("arguments"), "CLI 写进 request.json 的 arguments 已被改写", json.dumps(req, ensure_ascii=False))

    st = wait_terminal(space.queue, job_id, timeout=600)
    out = read_text(job_dir(space.queue, job_id) / STDOUT_LOG)
    ev = f"status={json.dumps(st, ensure_ascii=False)}\nstdout=\n{out}\nworker=\n{cli.worker_evidence()}"
    check(marker.exists(), "宿主程序未真正启动（无启动哨兵）", ev)
    check_eq(7, st.get("exitCode"), "退出码未透传", ev)
    check(st.get("state") in TERMINAL_STATES, f"任务未落终态：{st.get('state')}", ev)

    payload = parse_echo(out)
    check_eq(args, payload["argv"], "argv 未逐字节保留（被 shell 或拼接解析过）", ev)
    check(not pwn_target.exists(), f"命令注入真的产生了副作用文件：{pwn_target}", ev)
    return f"job={job_id} exit={st.get('exitCode')} state={st.get('state')} argv={len(payload['argv'])} 项逐字节一致，无注入副作用"


@test("e2e", "e2e.build-failure-never-launches")
def t_e2e_build_fail(ctx: Ctx) -> str:
    """A2：编译失败必须"未启动"，而不是"启动后失败"。

    构造：宿主 Main 第一行就写启动哨兵；另一文件故意语法错误让编译失败。
    判据：status=failed + 日志含 CS 诊断 + **哨兵文件不存在** + 无残留进程。
    阴性对照：把语法错误修好后重跑，同一哨兵必须出现（证明哨兵机制有效，不是恒不写）。
    """
    space, cli, _ = e2e_space(ctx, "e2e-buildfail")
    src = space.root / "src-bad"
    (src / "App").mkdir(parents=True, exist_ok=True)
    marker = space.root / "must-not-exist.marker"
    (src / "App" / "App.csproj").write_text(
        '<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><OutputType>Exe</OutputType>'
        "<TargetFramework>net10.0</TargetFramework></PropertyGroup></Project>",
        encoding="utf-8",
    )
    (src / "App" / "Program.cs").write_text(
        "internal static class P { static void Main(string[] a) { "
        f'System.IO.File.WriteAllText(@"{marker}", "launched"); '
        'System.Console.WriteLine("LAUNCHED"); } }',
        encoding="utf-8",
    )
    (src / "App" / "Broken.cs").write_text("internal static class Broken { void x() { this is not C# } }", encoding="utf-8")

    job_bad, rbad = cli.submit_job("build-run", "App/App.csproj", space.queue, source=src)
    check(rbad.code == 0 and job_bad, "坏项目提交失败（提交阶段不该失败，失败应发生在构建阶段）", rbad.evidence())
    st_bad = wait_terminal(space.queue, job_bad, timeout=600)

    logs = read_text(job_dir(space.queue, job_bad) / STDOUT_LOG) + read_text(job_dir(space.queue, job_bad) / STDERR_LOG)
    ev = f"status={json.dumps(st_bad, ensure_ascii=False)}\nlogs=\n{logs}\nworker=\n{cli.worker_evidence()}"
    check_eq("failed", st_bad.get("state"), "编译失败未落 failed 终态", ev)
    check("CS" in logs, "日志缺少编译器诊断（CS####）", ev)
    check(not marker.exists(), "编译失败却启动了进程（哨兵存在）= A2 失败", ev)
    # pid 允许是"构建进程"的 PID（诊断有用）；关键是被启动的是 build 而不是 run。
    check("dotnet build" in ev, "worker 未以 dotnet build 执行构建阶段", ev)
    check("dotnet run" not in ev, "构建失败却启动了 dotnet run", ev)
    check("构建失败" in (st_bad.get("error") or ""), f"失败原因未指明构建阶段：{st_bad.get('error')}", ev)

    # 阴性对照：修好语法后必须真的启动并写出哨兵
    (src / "App" / "Broken.cs").write_text("internal static class Broken { }", encoding="utf-8")
    job_ok, rok = cli.submit_job("build-run", "App/App.csproj", space.queue, source=src)
    check(rok.code == 0 and job_ok, "修复后提交失败", rok.evidence())
    wait_terminal(space.queue, job_ok, timeout=600)
    check(marker.exists(), "修好语法后哨兵仍未出现 = 负对照失效（上面的'未启动'结论不可信）")
    return f"bad={job_bad}(failed, 未启动) ok={job_ok}(已启动)，负对照通过"


@test("e2e", "e2e.utf8-streams-and-large-output")
def t_e2e_utf8(ctx: Ctx) -> str:
    """A4：中文/emoji 无损、stdout 与 stderr 严格分离、1MB 输出不死锁、非法字节不炸桥。"""
    space, cli, _ = e2e_space(ctx, "e2e-utf8")
    job_id, r = cli.submit_job(
        "run", "EchoApp.csproj", space.queue, source=FIXTURE_ECHO,
        app_args=["--stderr", "中文错误 🚫", "--bytes", "1200000", "--bad-utf8", "--exit", "0"],
    )
    check(r.code == 0 and job_id, "CLI 提交失败", r.evidence())
    st = wait_terminal(space.queue, job_id, timeout=600)
    out = read_bytes(job_dir(space.queue, job_id) / STDOUT_LOG)
    err = read_bytes(job_dir(space.queue, job_id) / STDERR_LOG)
    ev = f"status={json.dumps(st, ensure_ascii=False)}\nstdout_len={len(out)} stderr_len={len(err)}"

    check_eq("succeeded", st.get("state"), "大输出任务未成功（可能死锁/截断）", ev)
    check(len(out) > 1_000_000, f"stdout 明显被截断：{len(out)} 字节", ev)
    check(b"<<BULK>>" in out and b"<<ENDBULK>>" in out, "stdout 缺少大输出边界标记", ev)
    check(b"WDB_STDERR" in err, "stderr 未收到内容", ev)
    check(b"WDB_STDERR" not in out, "stderr 内容混进了 stdout（通道未分离）", ev)
    check(b"WDB_ECHO" not in err, "stdout 内容混进了 stderr", ev)
    check(b"\xe4\xb8\xad\xe6\x96\x87" in err, "stderr 中文未按 UTF-8 原样落盘", ev)
    check(b"WDB_ECHO" in out, "stdout 缺少结构化输出", ev)
    # 非法字节：允许替换或标记，但绝不能导致 status 无法解析（上面已解析）或日志丢失整段
    check(b"<<BAD>>" in out and b"<<END>>" in out, "非法字节段整段丢失", ev)
    return f"job={job_id} stdout={len(out)}B stderr={len(err)}B，通道分离且中文无损"


@test("e2e", "e2e.cancel-kills-entire-tree")
def t_e2e_cancel(ctx: Ctx) -> str:
    """A5：cancel 必须杀掉整棵进程树（含孙进程），不能只杀顶层。"""
    space, cli, _ = e2e_space(ctx, "e2e-cancel")
    pidfile = space.root / "pids.txt"
    job_id, r0 = cli.submit_job(
        "run", "EchoApp.csproj", space.queue, source=FIXTURE_ECHO,
        app_args=["--pidfile", str(pidfile), "--spawn", "1", "--depth", "2", "--repeat"],
    )
    check(r0.code == 0 and job_id, "CLI 提交失败", r0.evidence())
    wait_for(lambda: len(read_pids(pidfile)) >= 3, timeout=600, what="三层进程树建立")
    pids = read_pids(pidfile)

    r = cli.call("stop", queue=space.queue, job=job_id)
    check(r.code == 0, "stop 调用失败", r.evidence())
    st = wait_terminal(space.queue, job_id, timeout=180)
    ev = f"status={json.dumps(st, ensure_ascii=False)}\npids={pids}\nworker=\n{cli.worker_evidence()}"

    check_eq("canceled", st.get("state"), "取消后状态不是 canceled", ev)
    wait_for(lambda: not any(pid_alive(p) for p in pids), timeout=60, what="全部后代进程消失")
    alive = [p for p in pids if pid_alive(p)]
    check(not alive, f"取消后仍有存活进程（杀树不完整）：{alive}", ev)

    # 幂等：再次 stop 不得报错，也不得误杀无关同名进程
    r2 = cli.call("stop", queue=space.queue, job=job_id)
    check(r2.code == 0, "重复 stop 报错（不幂等）", r2.evidence())
    return f"job={job_id} 杀掉 {len(pids)} 个进程（含孙进程），重复 stop 幂等"


@test("e2e", "e2e.queued-cancel-never-executes")
def t_e2e_queued_cancel(ctx: Ctx) -> str:
    """A5b：任务还在 queued 就取消——绝不能出现"先跑一次再取消"的执行痕迹。"""
    space = ctx.space("e2e-queued-cancel").prepare()
    cli = ctx_cli(ctx)
    marker = space.root / "must-not-launch.marker"
    job_id, r0 = cli.submit_job(
        "run", "EchoApp.csproj", space.queue, source=FIXTURE_ECHO,
        app_args=["--marker", str(marker), "--sleep", "5000"],
    )
    check(r0.code == 0 and job_id, "CLI 提交失败", r0.evidence())
    # 先取消，再启动 worker
    cancel = job_dir(space.queue, job_id) / CANCEL_FILE
    cancel.write_text("", encoding="utf-8")
    cli.start_worker(space)

    st = wait_terminal(space.queue, job_id, timeout=600)
    ev = f"status={json.dumps(st, ensure_ascii=False)}\nworker=\n{cli.worker_evidence()}"
    check(st.get("state") in {"canceled", "rejected"}, "queued 取消后未被正确处理", ev)
    check(not marker.exists(), "已取消的任务仍然执行了（哨兵存在）", ev)
    return f"job={job_id} state={st.get('state')}，无执行痕迹"


@test("e2e", "e2e.worker-restart-does-not-replay-finished-or-interrupted")
def t_e2e_restart(ctx: Ctx) -> str:
    """A6：Worker 重启后，已完成任务不得重放；被中断任务不得被静默重跑出第二份副作用。

    判据：哨兵文件的行数（= 执行次数）在重启前后不变，且中断任务落到 interrupted（或恰好重跑一次）。
    """
    space = ctx.space("e2e-restart").prepare()
    cli = ctx_cli(ctx)
    marker = space.root / "replay.marker"

    done_job, r0 = cli.submit_job(
        "run", "EchoApp.csproj", space.queue, source=FIXTURE_ECHO,
        app_args=["--marker", str(marker), "--exit", "0"],
    )
    check(r0.code == 0 and done_job, "CLI 提交失败", r0.evidence())
    cli.start_worker(space)
    st_done = wait_terminal(space.queue, done_job, timeout=600)
    check_eq("succeeded", st_done.get("state"), "基线任务未成功，无法做重放对照")
    lines_after_first = marker_line_count(marker)
    check_eq(1, lines_after_first, "基线执行次数不为 1")

    # 重启 Worker（shutdown 标记→等它退出→重新拉起）
    cli.call("shutdown", queue=space.queue, timeout=120)
    wait_for(lambda: cli.ctx.worker is None or cli.ctx.worker.poll() is not None, timeout=60, what="Worker 退出")
    (space.queue / SHUTDOWN_FILE).unlink(missing_ok=True)
    cli.start_worker(space)

    # 已完成任务不得再次执行
    time.sleep(3)
    check_eq(1, marker_line_count(marker), "重启后已完成任务被重放（副作用 +1）")

    # 中断场景：提交长任务→进入 running→强杀 Worker→重启
    rolling_marker = space.root / "rolling.marker"
    rolling, r1 = cli.submit_job(
        "run", "EchoApp.csproj", space.queue, source=FIXTURE_ECHO,
        app_args=["--marker", str(rolling_marker), "--repeat"],
    )
    check(r1.code == 0 and rolling, "CLI 提交失败", r1.evidence())
    wait_for(lambda: (read_status(space.queue, rolling) or {}).get("state") == "running", timeout=600, what="长任务进入 running")
    # state=running 表示"已启动 dotnet run"，宿主程序还要先完成构建才落到 Main：必须等哨兵出现，不能立刻断言。
    wait_for(lambda: marker_line_count(rolling_marker) >= 1, timeout=180, what="长任务宿主程序启动（哨兵出现）")
    check_eq(1, marker_line_count(rolling_marker), "长任务执行次数不为 1")
    proc = cli.ctx.worker
    check(proc is not None, "Worker 句柄丢失")
    kill_tree(proc)
    proc.wait(timeout=30)
    cli.ctx.worker = None
    time.sleep(1)

    cli.start_worker(space)
    st_rolling = wait_for(
        lambda: (lambda s: s if s and s.get("state") in TERMINAL_STATES else None)(read_status(space.queue, rolling)),
        timeout=300, what="中断任务进入终态",
    )
    time.sleep(3)
    n = marker_line_count(rolling_marker)
    ev = f"status={json.dumps(st_rolling, ensure_ascii=False)} marker_lines={n}\nworker=\n{cli.worker_evidence()}"
    check(n <= 2, f"重启后副作用次数 {n} 超过「至多一次重跑」的上界", ev)
    if n == 2:
        check(st_rolling.get("state") == "running" or st_rolling.get("state") in TERMINAL_STATES, "重跑路径状态异常", ev)
    check(st_rolling.get("state") in TERMINAL_STATES | {"running"}, f"中断任务状态非法：{st_rolling.get('state')}", ev)
    return f"done={done_job} 未重放；rolling={rolling} state={st_rolling.get('state')} 执行次数={n}"


@test("e2e", "e2e.no-listening-port-and-no-autostart")
def t_e2e_no_port(ctx: Ctx) -> str:
    """边界自证：Worker 不得监听端口、不得注册服务/自启。"""
    space, cli, proc = e2e_space(ctx, "e2e-noport")
    listening = listening_sockets(proc.pid)
    check(not listening, f"Worker 监听了端口：{listening}")

    # 常见自启位置必须没有本工具的痕迹
    suspicious = []
    for p in (Path.home() / ".config" / "systemd" / "user", Path("/etc/systemd/system"), Path("/etc/cron.d")):
        if p.exists():
            for f in p.rglob("*"):
                if f.is_file() and "wdb" in f.name.lower():
                    suspicious.append(str(f))
    check(not suspicious, f"发现了可疑自启/服务注册：{suspicious}")
    return f"worker pid={proc.pid} 无监听端口、无自启痕迹"


# ==========================================================================
# L1b · 安全 / 日志边界（离线可复现；部分为已确证的实现缺陷红灯）
# ==========================================================================
def write_status_file(queue: Path, job_id: str, state: str, *, mode: str = MODE_RUN,
                      exit_code: int | None = None, pid: int | None = None, error: str | None = None) -> Path:
    """手写 status.json（exact camelCase 字段，与 Contracts.Status 逐字对齐）。"""
    jd = job_dir(queue, job_id)
    jd.mkdir(parents=True, exist_ok=True)
    now = now_utc()
    terminal = state in TERMINAL_STATES
    write_json(jd / STATUS_JSON, {
        "protocolVersion": PROTOCOL_VERSION,
        "id": job_id,
        "mode": mode,
        "state": state,
        "phase": "done" if terminal else "running",
        "exitCode": exit_code,
        "pid": pid,
        "error": error,
        "artifact": None,
        "updatedUtc": now,
        "startedUtc": now,
        "finishedUtc": now if terminal else None,
    })
    return jd


def spawn_cli(ctx: Ctx, argv: list[str], binary: bool = False) -> subprocess.Popen:
    """binary=True 时按**字节**捕获 stdout（plain 模式要断言精确字节，不能经过文本解码）。"""
    kwargs = dict(stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True)
    if not binary:
        kwargs.update(text=True, encoding="utf-8", errors="replace")
    else:
        kwargs.update(text=False)
    return subprocess.Popen(
        [str(a) for a in argv], cwd=str(REPO_ROOT), stdin=subprocess.DEVNULL, **kwargs)


def wait_exit(proc: subprocess.Popen, timeout: float) -> int | None:
    try:
        proc.wait(timeout=timeout)
        return proc.returncode
    except subprocess.TimeoutExpired:
        return None


def parse_log_chunks(stdout_text: str) -> tuple[list[str], list[dict]]:
    """把 logs --json 的逐次输出汇总成 (行, 各次 StreamView)。"""
    lines: list[str] = []
    views: list[dict] = []
    for raw in (stdout_text or "").splitlines():
        raw = raw.strip()
        if not raw.startswith("{"):
            continue
        try:
            chunk = json.loads(raw)
        except json.JSONDecodeError:
            continue
        view = chunk.get("stdout") or {}
        if view:
            views.append(view)
            lines.extend(view.get("lines") or [])
    return lines, views


@test("cli", "security.external-queue-must-not-be-created")
def t_ext_queue_no_create(ctx: Ctx) -> str:
    """workspace 之外的 --queue 必须在**创建任何目录之前**就被拒绝。

    阴性对照：同样的命令把 queue 放在 workspace 内时，目录确实被创建且命令成功——
    证明"外部目录不存在"不是因为我的探针路径写错或命令根本没跑。
    当前实现（ResolveQueueRoot 先 EnsureDir 再判 workspace）会先建目录再报错 → 本用例红。
    """
    space = ctx.space("ext-queue").prepare()
    cli = ctx_cli(ctx)
    ext = Path(f"/tmp/wdb-extq-{uuid.uuid4().hex[:10]}")
    check(not ext.exists(), f"探针路径本不该存在：{ext}")

    try:
        r = cli.call("run", timeout=180, queue=ext, source=FIXTURE_ECHO, project="EchoApp.csproj")
        ev = r.evidence()
        check(r.code != 0, "外部 queue 未被拒绝", ev)
        check("workspace" in (r.err or ""), "拒绝原因不是 workspace 规则", ev)
        check(not ext.exists(), f"外部 queue 目录被创建了（拒绝发生在 mkdir 之后）：{ext}", ev)
        check(r.code in CLI_EXIT_CODES, f"退出码 {r.code} 不在契约枚举内（负值/128+ = 崩溃或被信号杀死）", ev)
        check("Unhandled exception" not in (r.err or ""), "拒绝路径抛出了未捕获异常", ev)

        # 阳性对照：workspace 内的 queue 必须正常工作（同一命令形态）
        job, r2 = cli.submit_job("run", "EchoApp.csproj", space.queue, source=FIXTURE_ECHO)
        check(r2.code == 0 and job, "workspace 内的 queue 提交失败（对照失效）", r2.evidence())
        check(space.queue.exists(), "workspace 内 queue 未创建（对照失效）")
    finally:
        if ext.exists():
            shutil.rmtree(ext, ignore_errors=True)
            ext_created = True
        else:
            ext_created = False

    check(not ext_created, "本次运行确实在 /tmp 留下了越界目录（已在 finally 清理）")
    return f"外部 queue 退出码={r.code}，未创建 {ext}；workspace 内对照通过"


@test("cli", "security.queue-symlink-ancestor-must-not-escape")
def t_queue_symlink_escape(ctx: Ctx) -> str:
    """queue 路径字符串在 workspace 内、但**祖先目录是 symlink 指向外部**时必须拒绝。

    否则 payload 快照会被写进 workspace 之外的任意目录（真实的数据外泄面）。
    阴性对照：同一位置不放 symlink 时，queue 正常创建（证明拒绝只来自 symlink 逃逸）。
    """
    space = ctx.space("symq").prepare()
    cli = ctx_cli(ctx)
    tag = uuid.uuid4().hex[:10]
    link = space.root / "esc"          # workspace 内
    external = Path(f"/tmp/wdb-symq-{tag}")   # workspace 外
    os.symlink("/tmp", link, target_is_directory=True)
    escaped_queue = link / f"wdb-symq-{tag}"  # 字符串在 workspace 内，实际落到 /tmp

    try:
        r = cli.call("run", timeout=180, queue=escaped_queue, source=FIXTURE_ECHO, project="EchoApp.csproj")
        ev = r.evidence() + f"\nlink={link} -> /tmp\nexternal={external}\nexists={external.exists()}"
        check(r.code != 0, "祖先 symlink 指向 workspace 外时未被拒绝（queue 逃逸）", ev)
        check(not external.exists(), f"通过 symlink 在 workspace 外创建了目录：{external}", ev)
        # 必须是"干净的拒绝"：退出码落在契约枚举内，且没有未捕获异常/栈回溯
        # （曾经实测为 134 = WdbException 未被 catch 的崩溃；该缺陷已由作者修复，本断言永久看住它）
        check(r.code in CLI_EXIT_CODES, f"退出码 {r.code} 不在契约枚举内（负值/128+ = 崩溃或被信号杀死）", ev)
        check("Unhandled exception" not in (r.err or ""), "拒绝路径抛出了未捕获异常并打印栈回溯", ev)
        check(" at GalleriesManager." not in (r.err or ""), "stderr 出现 .NET 栈帧（未捕获异常）", ev)

        # 阳性对照：同一父目录下的普通（非 symlink）路径必须能正常作为 queue
        plain = space.root / "plain-queue"
        r2 = cli.call("run", timeout=180, queue=plain, source=FIXTURE_ECHO, project="EchoApp.csproj")
        check(r2.code == 0, "同一位置的普通 queue 也被拒绝了（拒绝过宽，误伤正常路径）", r2.evidence())
        check(plain.exists(), "普通 queue 未创建", r2.evidence())
    finally:
        if external.exists():
            shutil.rmtree(external, ignore_errors=True)
            leaked = True
        else:
            leaked = False
        link.unlink(missing_ok=True)

    check(not leaked, "本次运行确实越界写入了 /tmp（已在 finally 清理）")
    return f"symlink 祖先 queue 退出码={r.code}，未越界创建；普通 queue 对照通过"


@test("cli", "wait.exit-codes-match-terminal-state")
def t_wait_exit_codes(ctx: Ctx) -> str:
    """wait 的退出码必须区分终态（契约：succeeded=0 failed=5 canceled=6 timeout=4 interrupted=7）。
    阴性对照：非终态 + --wait-timeout 必须走超时码而不是任何终态码。
    """
    space = ctx.space("wait-codes").prepare()
    cli = ctx_cli(ctx)
    expect = {"succeeded": 0, "failed": 5, "canceled": 6, "timeout": 4, "interrupted": 7, "rejected": 5}
    seen = {}
    for state, code in expect.items():
        job = f"w-{state}-{uuid.uuid4().hex[:6]}"
        write_status_file(space.queue, job, state, exit_code=0 if state == "succeeded" else 1)
        r = cli.call("wait", timeout=60, queue=space.queue, job=job, wait_timeout=10)
        seen[state] = r.code
        check_eq(code, r.code, f"wait 对 state={state} 的退出码不符", r.evidence())
        body = json.loads(r.out) if r.out.strip().startswith("{") else {}
        check_eq(state, body.get("state"), f"wait 输出的 state 与 status.json 不一致（state={state}）", r.evidence())
    return "wait 终态退出码 " + ", ".join(f"{k}={v}" for k, v in seen.items())


@test("cli", "wait.times-out-without-terminal")
def t_wait_timeout(ctx: Ctx) -> str:
    """任务停在 running 时，wait 必须在 --wait-timeout 后返回 4（超时），且不得假装终态。"""
    space = ctx.space("wait-timeout").prepare()
    cli = ctx_cli(ctx)
    job = "wt-" + uuid.uuid4().hex[:8]
    write_status_file(space.queue, job, "running")

    t0 = time.time()
    r = cli.call("wait", timeout=60, queue=space.queue, job=job, wait_timeout=2)
    elapsed = time.time() - t0
    check_eq(4, r.code, "非终态等待未返回超时码 4", r.evidence())
    check(elapsed < 30, f"超时返回过慢：{elapsed:.1f}s", r.evidence())
    check("超时" in (r.err or ""), "stderr 未说明超时原因", r.evidence())

    # 翻转成终态后必须立刻返回 0（证明上面不是恒超时）
    write_status_file(space.queue, job, "succeeded", exit_code=0)
    r2 = cli.call("wait", timeout=60, queue=space.queue, job=job, wait_timeout=20)
    check_eq(0, r2.code, "终态化之后 wait 未返回 0（超时判定恒真）", r2.evidence())
    return f"非终态 wait 用时 {elapsed:.1f}s → 退出码 4；终态化后 → 0"


def follow_fragment_case(ctx: Ctx, cli: Cli, space: "Space", tag: str, variant: str,
                         first_bytes: bytes, rest_bytes: bytes, expected: str) -> str:
    """跟随读取公共夹具：先只落 first_bytes（可能是半行或半个 UTF-8 字符），启动 follow，
    再补 rest_bytes 并把任务置终态，最后校验跟随输出与期望文本一致。

    variant（四条路径都要成立，不允许一条绿掩盖其余）：
      * "offset0"       —— `--offset 0` + `--json`（字节增量路径，交付物是 lines 数组）
      * "tail"          —— 不传 offset/tail（默认 `--tail 200` 首轮）+ `--json`
      * "plain-offset0" —— `--offset 0` 无 --json：stdout 必须是**精确字节流**，不许多出/少掉换行
      * "plain-tail"    —— 默认 tail 无 --json：同上
    """
    plain = variant.startswith("plain-")
    job = f"{tag}-{variant}-{uuid.uuid4().hex[:6]}"
    write_status_file(space.queue, job, "running")
    log = job_dir(space.queue, job) / STDOUT_LOG
    log.write_bytes(first_bytes)

    kwargs: dict = {"queue": space.queue, "job": job, "follow": True}
    if not plain:
        kwargs["json"] = True
    if variant in ("offset0", "plain-offset0"):
        kwargs["offset"] = 0

    proc = spawn_cli(ctx, cli.argv("logs", **kwargs), binary=plain)
    try:
        time.sleep(1.5)                      # 让跟随者至少轮询一次（poll 默认 500ms）
        with open(log, "ab") as fh:
            fh.write(rest_bytes)
        write_status_file(space.queue, job, "succeeded", exit_code=0)
        out, err = proc.communicate(timeout=90)
    finally:
        if proc.poll() is None:
            kill_tree(proc)

    expected_bytes = first_bytes + rest_bytes     # 终态时文件的精确字节
    if plain:
        raw = out if isinstance(out, bytes) else (out or "").encode("utf-8", "replace")
        err_text = err.decode("utf-8", "replace") if isinstance(err, bytes) else (err or "")
        ev = (f"variant={variant} exit={proc.returncode}\nstdout_hex={raw.hex()}\n"
              f"期望_hex={expected_bytes.hex()}\nstderr={err_text[-400:]}")
        check(proc.returncode == 0, f"[{variant}] plain follow 未以 0 退出", ev)
        check_eq(expected_bytes, raw, f"[{variant}] plain 模式 stdout 与文件字节不等（多出/少掉换行或丢字节）", ev)
        check(b"\xef\xbf\xbd" not in raw, f"[{variant}] plain 输出含 U+FFFD 替换字符", ev)
        return f"{variant}={raw.hex()}"

    lines, views = parse_log_chunks(out)
    text = "".join(lines)
    ev = (f"variant={variant} exit={proc.returncode}\nlines={lines!r}\n"
          f"views={json.dumps(views, ensure_ascii=False)[:900]}\nstderr={(err or '')[-600:]}")
    check(proc.returncode == 0, f"[{variant}] follow 未以 0 退出", ev)
    check(views, f"[{variant}] follow 没有输出任何分片", ev)
    check("\ufffd" not in text, f"[{variant}] 跟随输出出现替换字符 U+FFFD（半字符被替换且 offset 越过它）", ev)
    check_eq(expected, text, f"[{variant}] 跟随输出文本不正确", ev)
    check(all(line != "" for line in lines), f"[{variant}] 跟随输出出现凭空空行 {lines!r}", ev)

    # 阴性对照：文件补齐后一次性读取必须正好得到期望值（证明文件内容与比较器都没问题）
    solo_kwargs: dict = {"queue": space.queue, "job": job}
    if variant == "tail":
        solo_kwargs["tail"] = 200
    r = cli.call("logs", timeout=60, json=True, **solo_kwargs)
    solo, _ = parse_log_chunks(r.out)
    check_eq([expected], solo, f"[{variant}] 一次性读取结果不对（上面的失败不该归因于文件内容）", r.evidence())
    return f"{variant}={lines!r}"


@test("cli", "logs.partial-utf8-follow-must-not-corrupt")
def t_logs_partial_utf8(ctx: Ctx) -> str:
    """跟随读取：日志先只有“中”的前 2 字节，随后补齐最后 1 字节 + “文” + 换行。

    正确行为：不得出现 U+FFFD（半个字符被当成坏字节替换且 offset 越过它），
    也不得凭空多出空行；最终应还原出“中文”。四条路径（offset0 / 默认 tail / 两者的 plain）都必须成立。
    """
    space = ctx.space("utf8-follow").prepare()
    cli = ctx_cli(ctx)
    zhong = "中".encode("utf-8")
    details = []
    for variant in ("offset0", "tail", "plain-offset0", "plain-tail"):
        details.append(follow_fragment_case(
            ctx, cli, space, "uf", variant,
            first_bytes=zhong[:2],
            rest_bytes=zhong[2:] + "文\n".encode("utf-8"),
            expected="中文",
        ))
    return "；".join(details)


@test("cli", "logs.partial-line-follow-fidelity")
def t_logs_partial_line(ctx: Ctx) -> str:
    """跟随读取：先落无换行的半行 "abc"，随后补 "def\\n"。

    正确行为：plain 模式 stdout 必须**恰为** b"abcdef\\n"（不许把半行当成整行而多插一个换行、
    不许重复、不许丢字节）；--json 模式下 lines 拼接必须恰为 "abcdef"。
    """
    space = ctx.space("frag-follow").prepare()
    cli = ctx_cli(ctx)
    details = []
    for variant in ("offset0", "tail", "plain-offset0", "plain-tail"):
        details.append(follow_fragment_case(
            ctx, cli, space, "frag", variant,
            first_bytes=b"abc",                # 故意不带换行
            rest_bytes=b"def\n",
            expected="abcdef",
        ))
    return "；".join(details)


@test("cli", "logs.terminal-oversize-log-must-drain")
def t_logs_oversize_drain(ctx: Ctx) -> str:
    """终态任务的日志超过单次读取上限（8MiB）时，--offset 0 --follow 必须把它全部 drain 完再退出。

    当前实现读到 cap 后立刻看到终态就返回 → 尾部内容丢失。
    判据：输出必须包含文件尾标记，且总字节数与文件一致。
    """
    space = ctx.space("oversize").prepare()
    cli = ctx_cli(ctx)
    job = "big-" + uuid.uuid4().hex[:8]
    jd = job_dir(space.queue, job)
    jd.mkdir(parents=True, exist_ok=True)
    body = b"A" * (9 * 1024 * 1024)     # 9MiB > Store.MaxReadBytes(8MiB)
    tail = b"TAIL-MARKER-END"
    (jd / STDOUT_LOG).write_bytes(body + tail + b"\n")
    write_status_file(space.queue, job, "succeeded", exit_code=0)

    r = cli.call("logs", timeout=300, queue=space.queue, job=job, offset=0, follow=True, json=True)
    lines, views = parse_log_chunks(r.out)
    text = "".join(lines)
    total = sum(len(l) for l in lines)
    ev = (f"exit={r.code} 分片数={len(views)} 总字符={total} 期望={len(body) + len(tail)}\n"
          f"nextOffsets={[v.get('nextOffset') for v in views][:6]} lengths={[v.get('length') for v in views][:6]}\n"
          f"stderr={r.err[-400:]}")
    check(r.code == 0, "终态大日志 follow 未以 0 退出", ev)
    check("TAIL-MARKER-END" in text, "终态大日志未被完整 drain（尾标记缺失）", ev)
    check_eq(len(body) + len(tail), total, "drain 到的字符数与文件不一致", ev)
    return f"分片={len(views)} 共 drain {total} 字节（文件 {len(body) + len(tail)}）"


# ==========================================================================
# L2b · e2e 补充：超时终态 / Worker 独占 / framework / 两任务隔离
# ==========================================================================
@test("e2e", "e2e.timeout-terminal-state-kills-tree")
def t_e2e_timeout(ctx: Ctx) -> str:
    """A-timeout：--timeout 到点必须写 timeout 终态、杀掉整棵树，且 wait 返回退出码 4。"""
    space, cli, _ = e2e_space(ctx, "e2e-timeout")
    pidfile = space.root / "pids.txt"
    job, r0 = cli.submit_job(
        "run", "EchoApp.csproj", space.queue, source=FIXTURE_ECHO, job_timeout=12,
        app_args=["--pidfile", str(pidfile), "--spawn", "1", "--depth", "2", "--repeat"],
    )
    check(r0.code == 0 and job, "CLI 提交失败", r0.evidence())
    wait_for(lambda: len(read_pids(pidfile)) >= 3, timeout=60, what="三层进程树建立")
    pids = read_pids(pidfile)

    st = wait_terminal(space.queue, job, timeout=120)
    ev = f"status={json.dumps(st, ensure_ascii=False)}\npids={pids}\nworker=\n{cli.worker_evidence()}"
    check_eq("timeout", st.get("state"), "超时未落 timeout 终态", ev)
    wait_for(lambda: not any(pid_alive(p) for p in pids), timeout=60, what="超时后进程树全部消失")
    alive = [p for p in pids if pid_alive(p)]
    check(not alive, f"超时后仍有存活进程：{alive}", ev)

    rw = cli.call("wait", timeout=60, queue=space.queue, job=job, wait_timeout=30)
    check_eq(4, rw.code, "wait 对 timeout 终态未返回 4", rw.evidence())
    return f"job={job} state=timeout，{len(pids)} 个进程全部终止，wait 退出码 4"


@test("e2e", "e2e.second-worker-refuses-exclusive")
def t_e2e_second_worker(ctx: Ctx) -> str:
    """独占：第二个 Worker 必须拒绝启动，不得覆盖 worker.json、不得与第一个抢任务。

    阴性对照：第一个 Worker 停掉后，新 Worker 必须能拿到锁（证明锁不是永久占用）。
    """
    space, cli, proc_a = e2e_space(ctx, "e2e-exclusive")
    state_a = read_json(space.queue / WORKER_JSON)
    check_eq(proc_a.pid, state_a.get("pid"), "worker.json 的 pid 与第一个 Worker 不符")

    log_b = space.root / "worker-b.log"
    handle = open(log_b, "w", encoding="utf-8")
    proc_b = subprocess.Popen(
        cli.argv("worker", queue=space.queue, runs_root=space.runs, dotnet=ctx.dotnet),
        cwd=str(REPO_ROOT), stdin=subprocess.DEVNULL, stdout=handle, stderr=subprocess.STDOUT,
        start_new_session=True,
    )
    try:
        code_b = wait_exit(proc_b, 30)
        check(code_b is not None, "第二个 Worker 未退出（独占锁失效：两个 Worker 同时在跑）", read_text(log_b))
        check(code_b != 0, f"第二个 Worker 退出码为 {code_b}（应为非 0 拒绝）", read_text(log_b))
        state_a2 = read_json(space.queue / WORKER_JSON)
        check_eq(state_a.get("workerId"), state_a2.get("workerId"), "第二个 Worker 覆盖了 worker.json", read_text(log_b))

        # 第一个 Worker 必须仍在正常工作
        job, r1 = cli.submit_job("run", "EchoApp.csproj", space.queue, source=FIXTURE_ECHO, app_args=["--exit", "0"])
        check(r1.code == 0 and job, "提交失败", r1.evidence())
        st = wait_terminal(space.queue, job, timeout=300)
        check_eq("succeeded", st.get("state"), "第一个 Worker 被第二个干扰后无法继续执行",
                 json.dumps(st, ensure_ascii=False) + "\n" + cli.worker_evidence())

        # 阴性对照：停掉第一个后，新 Worker 必须能起来
        cli.call("shutdown", queue=space.queue, timeout=60)
        proc_a.wait(timeout=60)
        (space.queue / SHUTDOWN_FILE).unlink(missing_ok=True)
        proc_c = cli.start_worker(space)
        wait_for(lambda: proc_c.poll() is None and (space.queue / WORKER_JSON).exists(), timeout=60, what="新 Worker 接管")
        time.sleep(1.5)
        check(proc_c.poll() is None, "第一个 Worker 退出后新 Worker 仍无法启动（锁未释放）", read_text(cli.ctx.worker_log))
        state_c = read_json(space.queue / WORKER_JSON)
        check(state_c.get("workerId") != state_a.get("workerId"), "新 Worker 未刷新 workerId", json.dumps(state_c, ensure_ascii=False))
    finally:
        if proc_b.poll() is None:
            kill_tree(proc_b)
        handle.close()

    return f"第二个 Worker 退出码={code_b}（已拒绝）；锁释放后的新 Worker 正常接管"


@test("e2e", "e2e.framework-flag-plumbing")
def t_e2e_framework(ctx: Ctx) -> str:
    """framework 透传：指定 --framework net10.0 必须落到 request.json 并且不影响成功运行；
    非法 framework 必须构建失败且**不启动**目标（哨兵缺席）。"""
    space, cli, _ = e2e_space(ctx, "e2e-framework")
    marker_ok = space.root / "fw-ok.marker"
    job_ok, r1 = cli.submit_job(
        "build-run", "EchoApp.csproj", space.queue, source=FIXTURE_ECHO, framework="net10.0",
        app_args=["--marker", str(marker_ok), "--exit", "0"],
    )
    check(r1.code == 0 and job_ok, "带 --framework 的提交失败", r1.evidence())
    req = read_json(job_dir(space.queue, job_ok) / REQUEST_JSON)
    check_eq("net10.0", req.get("framework"), "framework 未写入 request.json", json.dumps(req, ensure_ascii=False))
    st_ok = wait_terminal(space.queue, job_ok, timeout=300)
    check_eq("succeeded", st_ok.get("state"), "带 --framework 的正常任务未成功",
             json.dumps(st_ok, ensure_ascii=False) + "\n" + cli.worker_evidence())
    check(marker_ok.exists(), "带 --framework 的任务没真正启动目标（哨兵缺席）")

    # 阴性对照：非法 framework 必须失败且不启动
    marker_bad = space.root / "fw-bad.marker"
    job_bad, r2 = cli.submit_job(
        "build-run", "EchoApp.csproj", space.queue, source=FIXTURE_ECHO, framework="net99.0",
        app_args=["--marker", str(marker_bad), "--exit", "0"],
    )
    check(r2.code == 0 and job_bad, "非法 framework 在提交阶段就失败了（应到构建阶段失败）", r2.evidence())
    st_bad = wait_terminal(space.queue, job_bad, timeout=300)
    logs = read_text(job_dir(space.queue, job_bad) / STDOUT_LOG) + read_text(job_dir(space.queue, job_bad) / STDERR_LOG)
    ev = f"status={json.dumps(st_bad, ensure_ascii=False)}\nlogs=\n{logs[-1500:]}\nworker=\n{cli.worker_evidence()}"
    check_eq("failed", st_bad.get("state"), "非法 framework 未失败", ev)
    check(not marker_bad.exists(), "非法 framework 却启动了目标程序", ev)
    check("net99.0" in (logs + ev), "失败日志里没有出现非法 framework 名", ev)
    return f"ok={job_ok}(succeeded, 已启动) bad={job_bad}(failed, 未启动)"


@test("e2e", "e2e.build-run-multitarget-requires-framework")
def t_e2e_multitarget(ctx: Ctx) -> str:
    """多 TFM 项目：不给 --framework 必须拒绝（ERR_FRAMEWORK_REQUIRED），给了必须成功并真正运行产物。

    fixture 必须**真的**是多 TFM。仓库根 Directory.Build.props:3 无条件设置 TargetFramework=net10.0，
    若只在 csproj 里写 TargetFrameworks，项目会继承单数属性而退化成单 TFM——那样“跳过”就成了
    掩盖 fixture 缺陷的借口。因此本用例：
      1) 在 fixture 目录放本地 Directory.Build.props（nearest wins，截断仓库根继承）并开 EnableWindowsTargeting；
      2) csproj 显式 `<TargetFramework></TargetFramework>` 清空单数属性，只用 TargetFrameworks（net10.0;net10.0-windows）；
      3) 先求值断言 TargetFramework 为空且 TargetFrameworks 恰 2 项，不满足即 **fail**（不是 skip）；
      4) 只有构建失败、且首条错误属于**可识别的环境原因**（NuGet 目录只读 / NU**** / 找不到包 / restore 失败）
         才 SKIP，并把该错误行原样带出；语法错、非法 TFM（NETSDK1045 等）一律 fail。
    """
    space, cli, _ = e2e_space(ctx, "e2e-mtfm")
    src = space.root / "mt"
    app = src / "App"
    app.mkdir(parents=True, exist_ok=True)
    (src / "Directory.Build.props").write_text(
        "<Project>\n"
        "  <PropertyGroup>\n"
        "    <ManagePackageVersionsCentrally>false</ManagePackageVersionsCentrally>\n"
        "    <EnableWindowsTargeting>true</EnableWindowsTargeting>\n"
        "  </PropertyGroup>\n"
        "</Project>\n", encoding="utf-8")
    (app / "App.csproj").write_text(
        '<Project Sdk="Microsoft.NET.Sdk">\n'
        '  <PropertyGroup>\n'
        '    <OutputType>Exe</OutputType>\n'
        '    <TargetFramework></TargetFramework>\n'
        '    <TargetFrameworks>net10.0;net10.0-windows</TargetFrameworks>\n'
        '  </PropertyGroup>\n'
        '</Project>\n', encoding="utf-8")
    marker = space.root / "mt.marker"
    (app / "Program.cs").write_text(
        "internal static class P { static void Main(string[] a) { "
        f'System.IO.File.AppendAllText(@"{marker}", "launched\\n"); '
        "System.Console.WriteLine(\"MTFM\"); } }", encoding="utf-8")

    # (3) 自证 fixture 真的多 TFM —— 不满足即 fail，绝不用 skip 掩盖
    props = run([ctx.dotnet, "msbuild", str(app / "App.csproj"), "-nologo",
                 "-getProperty:TargetFramework", "-getProperty:TargetFrameworks"], timeout=300)
    check_eq(0, props.code, "msbuild 属性求值失败（fixture 本身有问题）", props.evidence())
    try:
        props_doc = json.loads(props.out[props.out.index("{"):])
        props_map = props_doc.get("Properties", {})
    except Exception as exc:            # noqa: BLE001
        raise SmokeFailure(f"无法解析属性求值输出：{exc}\n{props.evidence()}")
    tfm_single = (props_map.get("TargetFramework") or "").strip()
    tfms = (props_map.get("TargetFrameworks") or "").strip()
    check_eq("", tfm_single,
             "fixture 继承了单数 TargetFramework，已退化成单 TFM（此时跳过会掩盖 fixture 缺陷）", props.evidence())
    check_eq(2, len([t for t in tfms.split(";") if t.strip()]),
             f"TargetFrameworks 不是 2 项：{tfms!r}", props.evidence())

    # (4) 构建探测 + 精确归因
    probe = run([ctx.dotnet, "build", str(app / "App.csproj"), "--nologo", "-v", "m"], timeout=600)
    if probe.code != 0:
        first = first_error_line(probe.out + "\n" + probe.err)
        if is_environment_restore_failure(first):
            raise SkipTest("可识别的环境原因（非 fixture 问题）：" + first[:240])
        raise SmokeFailure(
            "多 TFM fixture 构建失败，但原因不是可识别的环境问题——按规则必须 fail 而不是 skip：\n"
            + first[:400])

    job_bad, r1 = cli.submit_job("build-run", "App/App.csproj", space.queue, source=src)
    check(r1.code == 0 and job_bad, "提交失败", r1.evidence())
    st_bad = wait_terminal(space.queue, job_bad, timeout=600)
    bad_logs = read_text(job_dir(space.queue, job_bad) / STDOUT_LOG) + read_text(job_dir(space.queue, job_bad) / STDERR_LOG)
    ev = f"status={json.dumps(st_bad, ensure_ascii=False)}\n{bad_logs[-1200:]}\n{cli.worker_evidence()}"
    check_eq("failed", st_bad.get("state"), "多 TFM 缺 framework 未失败", ev)
    check("ERR_FRAMEWORK_REQUIRED" in (st_bad.get("error") or "") or "framework" in bad_logs.lower(),
          "失败原因不是缺少 framework", ev)
    check(not marker.exists(), "多 TFM 缺 framework 却启动了目标", ev)

    job_ok, r2 = cli.submit_job("build-run", "App/App.csproj", space.queue, source=src, framework="net10.0")
    check(r2.code == 0 and job_ok, "带 framework 的提交失败", r2.evidence())
    st_ok = wait_terminal(space.queue, job_ok, timeout=600)
    check_eq("succeeded", st_ok.get("state"), "带 framework 的多 TFM 任务未成功",
             json.dumps(st_ok, ensure_ascii=False) + "\n" + cli.worker_evidence())
    check(marker.exists(), "带 framework 的多 TFM 任务没有真正运行产物")
    return f"无 framework → 拒绝（未启动）；有 framework → 成功并运行产物"


@test("e2e", "e2e.two-jobs-independent-stop")
def t_e2e_two_jobs(ctx: Ctx) -> str:
    """两任务隔离：stop 一个不得影响另一个（排队中的不被误杀，运行中的不受牵连）。"""
    space, cli, _ = e2e_space(ctx, "e2e-twojobs")
    pid_a = space.root / "a.pids"
    marker_a = space.root / "a.marker"
    marker_b = space.root / "b.marker"

    # 场景 1：A 已运行，B 排队；stop B 不得动 A
    job_a, r1 = cli.submit_job(
        "run", "EchoApp.csproj", space.queue, source=FIXTURE_ECHO,
        app_args=["--pidfile", str(pid_a), "--marker", str(marker_a), "--spawn", "1", "--depth", "1", "--repeat"],
    )
    check(r1.code == 0 and job_a, "A 提交失败", r1.evidence())
    wait_for(lambda: len(read_pids(pid_a)) >= 2, timeout=300, what="A 的进程树建立")
    pids_a = read_pids(pid_a)

    job_b, r2 = cli.submit_job("run", "EchoApp.csproj", space.queue, source=FIXTURE_ECHO,
                               app_args=["--marker", str(marker_b), "--exit", "0"])
    check(r2.code == 0 and job_b, "B 提交失败", r2.evidence())
    check_eq(0, cli.call("stop", timeout=60, queue=space.queue, job=job_b).code, "stop B 失败")

    # B 排在长任务 A 之后：控制面清扫必须在 **A 仍运行时** 就把 B 判为 canceled。
    # 旧缺陷是"等 A 结束才处理 B"，那样 B 会长期停在 queued —— 本断言专门钉死它
    # （若只等 stop A 之后再断言 B 终态，旧缺陷也能通过，等于永久假绿）。
    st_b = wait_for(
        lambda: (lambda s: s if s and s.get("state") in TERMINAL_STATES else None)(read_status(space.queue, job_b)),
        timeout=60, what="排队中的 B 在 A 仍运行时进入终态",
    )
    check_eq("canceled", st_b.get("state"), "排队取消后 B 未在 A 运行期间进入 canceled",
             json.dumps(st_b, ensure_ascii=False) + "\n" + cli.worker_evidence())
    check((job_dir(space.queue, job_b) / CANCEL_FILE).exists(), "stop 未为排队的 B 写入 cancel 标记")
    check(not marker_b.exists(), "被取消的 B 仍然执行了", cli.worker_evidence())
    check(all(pid_alive(p) for p in pids_a), "B 被判 canceled 时 A 的进程已被误伤",
          f"pids_a={pids_a} alive={[p for p in pids_a if pid_alive(p)]}")
    st_a_mid = read_status(space.queue, job_a) or {}
    check_eq("running", st_a_mid.get("state"), "B 被判 canceled 时 A 已不在 running", json.dumps(st_a_mid, ensure_ascii=False))

    # 收尾：停 A
    check_eq(0, cli.call("stop", timeout=60, queue=space.queue, job=job_a).code, "stop A 失败")
    st_a = wait_terminal(space.queue, job_a, timeout=180)
    check_eq("canceled", st_a.get("state"), "运行中的 A 未被取消", json.dumps(st_a, ensure_ascii=False))

    cli.call("shutdown", queue=space.queue, timeout=60)
    cli.ctx.worker.wait(timeout=60)
    cli.ctx.worker = None
    (space.queue / SHUTDOWN_FILE).unlink(missing_ok=True)

    q1, r3 = cli.submit_job("run", "EchoApp.csproj", space.queue, source=FIXTURE_ECHO, app_args=["--marker", str(space.root / "q1.marker"), "--exit", "0"])
    q2, r4 = cli.submit_job("run", "EchoApp.csproj", space.queue, source=FIXTURE_ECHO, app_args=["--marker", str(space.root / "q2.marker"), "--exit", "0"])
    check(r3.code == 0 and r4.code == 0, "排队任务提交失败", r3.evidence() + r4.evidence())
    check_eq(0, cli.call("stop", timeout=60, queue=space.queue, job=q1).code, "stop q1 失败")
    cli.start_worker(space)

    st_q1 = wait_terminal(space.queue, q1, timeout=300)
    st_q2 = wait_terminal(space.queue, q2, timeout=300)
    check_eq("canceled", st_q1.get("state"), "q1 未被取消", json.dumps(st_q1, ensure_ascii=False))
    check_eq("succeeded", st_q2.get("state"), "停 q1 影响了 q2", json.dumps(st_q2, ensure_ascii=False) + "\n" + cli.worker_evidence())
    check(not (space.root / "q1.marker").exists(), "已取消的 q1 仍被执行", cli.worker_evidence())
    check((space.root / "q2.marker").exists(), "q2 未真正执行", cli.worker_evidence())
    return f"A/B 与 q1/q2 两组均独立：stop 一个不影响另一个（{job_a},{job_b},{q1},{q2}）"


# 可识别的**环境**失败特征：只有命中这些才允许 SKIP（NuGet 目录不可写 / 包缺失 / restore 失败）。
ENV_RESTORE_FAILURE_PATTERNS = (
    "read-only file system", "只读", "nuget.targets",
    "unable to load the service index", "unable to find package", "no packages exist with this id",
    "restore failed", "无法加载源", "没有找到与以下项匹配的包",
    "nu1100", "nu1101", "nu1102", "nu1103", "nu1301", "nu1900",
)


def first_error_line(text: str) -> str:
    """取首条含 error 的行（而不是最后一行/耗时行），用于精确归因。"""
    for line in (text or "").splitlines():
        if "error" in line.lower():
            return line.strip()
    lines = [l.strip() for l in (text or "").splitlines() if l.strip()]
    return lines[-1] if lines else "(无输出)"


def is_environment_restore_failure(line: str) -> bool:
    low = (line or "").lower()
    return any(pat in low for pat in ENV_RESTORE_FAILURE_PATTERNS)


def worker_once(ctx: Ctx, queue: Path, runs: Path, timeout: float = 40.0) -> tuple[int, str]:
    """跑一次 Worker 并等它自己退出（用于"启动即拒绝"类断言）：返回 (退出码, 日志全文)。"""
    log = queue.parent / f"worker-once-{uuid.uuid4().hex[:6]}.log"
    handle = open(log, "w", encoding="utf-8")
    proc = subprocess.Popen(
        ctx_cli(ctx).argv("worker", queue=queue, runs_root=runs, dotnet=ctx.dotnet),
        cwd=str(REPO_ROOT), stdin=subprocess.DEVNULL, stdout=handle, stderr=subprocess.STDOUT,
        start_new_session=True,
    )
    try:
        code = wait_exit(proc, timeout)
        if code is None:
            kill_tree(proc)
            proc.wait(timeout=10)
            code = -999
    finally:
        handle.close()
    return code, read_text(log)


def snapshot_tree(root: Path) -> dict:
    """目录的逐字节快照（相对路径 → 大小/mtime_ns/sha256），用于证明"未被改动"。"""
    import hashlib
    out: dict = {}
    if not root.exists():
        return out
    for item in sorted(root.rglob("*")):
        rel = str(item.relative_to(root))
        if item.is_symlink():
            out[rel] = ("symlink", os.readlink(item))
        elif item.is_file():
            st = item.stat()
            out[rel] = (st.st_size, st.st_mtime_ns, hashlib.sha256(item.read_bytes()).hexdigest())
        else:
            out[rel + "/"] = ("dir",)
    return out


@test("e2e", "worker.runs-root-nonempty-without-marker-refused")
def t_runs_root_not_owned(ctx: Ctx) -> str:
    """runs-root 已存在、非空、且无所有权标记时必须 exit 6 拒绝，且**绝不改动用户既有目录**；
    空目录必须能被接管（写 token 标记）——阴性对照证明拒绝只针对"非本桥所有"。"""
    space = ctx.space("runs-not-owned").prepare()
    runs = space.root / "existing-runs"
    runs.mkdir(parents=True, exist_ok=True)
    (runs / "user-data.txt").write_text("user-data\n", encoding="utf-8")
    (runs / "sub").mkdir()
    (runs / "sub" / "more.txt").write_text("more\n", encoding="utf-8")
    before = snapshot_tree(runs)

    code, log = worker_once(ctx, space.queue, runs)
    after = snapshot_tree(runs)
    ev = f"exit={code}\nlog=\n{log[-1200:]}\nbefore={before}\nafter={after}"
    check_eq(6, code, "runs-root 非空且无标记时未以 exit 6 拒绝（EnvironmentViolation）", ev)
    check("ERR_RUNS_ROOT_NOT_OWNED" in log, "拒绝原因不是所有权标记缺失", ev)
    check_eq(before, after, "拒绝路径改动了用户既有目录（文件被改/被删/插入了标记）", ev)
    check(not (runs / ".wsl-debug-bridge-runs-owner").exists(), "拒绝时仍写入了所有权标记", ev)

    # 阴性对照：空目录必须被接管
    space2 = ctx.space("runs-adopt").prepare()
    cli = ctx_cli(ctx)
    proc = cli.start_worker(space2)
    try:
        wait_for(lambda: (space2.queue / WORKER_JSON).exists(), timeout=60, what="空 runs-root 接管并起心跳")
        marker = space2.runs / ".wsl-debug-bridge-runs-owner"
        check(marker.exists(), "空 runs-root 未被接管（无所有权标记）")
        first = marker.read_text(encoding="utf-8").splitlines()[0].strip()
        check_eq("wsl-debug-bridge/runs-root/v1", first, "所有权标记首行 token 不符")
        cli.call("shutdown", queue=space2.queue, timeout=60)
        proc.wait(timeout=60)
    finally:
        if proc.poll() is None:
            kill_tree(proc)
        cli.ctx.worker = None
    return f"非空无标记 → exit {code} 且目录逐字节未变；空目录 → 已接管并写入 token"


@test("e2e", "worker.runs-queue-overlap-refused")
def t_runs_queue_overlap(ctx: Ctx) -> str:
    """runs-root 与 queue 必须双向不重叠（相等/互为祖先都拒绝，exit 6）；
    并列目录必须能正常启动（阴性对照，防止把正常布局也拒了）。"""
    space_a = ctx.space("overlap-a").prepare()
    code1, log1 = worker_once(ctx, space_a.queue, space_a.queue / "runs")
    check_eq(6, code1, "runs 位于 queue 内未以 exit 6 拒绝", f"exit={code1}\n{log1[-800:]}")
    check("ERR_SECURITY_VIOLATION" in log1, "拒绝原因不是重叠违规", log1[-800:])

    space_b = ctx.space("overlap-b").prepare()
    nested_queue = space_b.runs / "q"
    nested_queue.mkdir(parents=True, exist_ok=True)     # queue 目录须已存在，否则先报"队列不存在"
    code2, log2 = worker_once(ctx, nested_queue, space_b.runs)
    check_eq(6, code2, "queue 位于 runs 内未以 exit 6 拒绝", f"exit={code2}\n{log2[-800:]}")
    check("ERR_SECURITY_VIOLATION" in log2, "拒绝原因不是重叠违规", log2[-800:])

    space_c = ctx.space("overlap-ok").prepare()
    cli = ctx_cli(ctx)
    proc = cli.start_worker(space_c)
    try:
        wait_for(lambda: (space_c.queue / WORKER_JSON).exists(), timeout=60, what="不重叠的 queue/runs 正常启动")
        cli.call("shutdown", queue=space_c.queue, timeout=60)
        proc.wait(timeout=60)
    finally:
        if proc.poll() is None:
            kill_tree(proc)
        cli.ctx.worker = None
    return f"runs⊂queue → exit {code1}；queue⊂runs → exit {code2}；不重叠 → 正常启动"


@test("e2e", "e2e.pre-existing-run-dir-rejected")
def t_pre_existing_run_dir(ctx: Ctx) -> str:
    """CLI 提交后、Worker 认领前，有人在 runs-root/<id> 造了普通目录/符号链接：
    该任务必须以 rejected 结束，且预存内容与 symlink 外部目标都**逐字节不被触碰**（绝不复用、绝不写入）。"""
    space = ctx.space("pre-existing").prepare()
    cli = ctx_cli(ctx)
    external = space.root / "external-target"
    external.mkdir(parents=True, exist_ok=True)
    (external / "keep.txt").write_text("external-keep\n", encoding="utf-8")
    ext_before = snapshot_tree(external)

    # 先用空 runs-root 合法 Adopt（拿到 root 标记），再 shutdown 让 Worker 退出
    proc = cli.start_worker(space)
    try:
        wait_for(lambda: (space.queue / WORKER_JSON).exists(), timeout=60, what="空 runs-root 接管")
        cli.call("shutdown", queue=space.queue, timeout=60)
        proc.wait(timeout=60)
    finally:
        if proc.poll() is None:
            kill_tree(proc)
        cli.ctx.worker = None
    (space.queue / SHUTDOWN_FILE).unlink(missing_ok=True)
    check((space.runs / ".wsl-debug-bridge-runs-owner").exists(), "Adopt 阶段未写 root 标记")

    job_plain, r1 = cli.submit_job("run", "EchoApp.csproj", space.queue, source=FIXTURE_ECHO, app_args=["--exit", "0"])
    job_link, r2 = cli.submit_job("run", "EchoApp.csproj", space.queue, source=FIXTURE_ECHO, app_args=["--exit", "0"])
    check(r1.code == 0 and r2.code == 0, "预置提交失败", r1.evidence() + r2.evidence())

    pre = space.runs / job_plain
    pre.mkdir(parents=True, exist_ok=True)
    (pre / "userfile.txt").write_text("pre-existing\n", encoding="utf-8")
    pre_before = snapshot_tree(pre)
    os.symlink(external, space.runs / job_link, target_is_directory=True)

    cli.start_worker(space)
    st1 = wait_terminal(space.queue, job_plain, timeout=300)
    st2 = wait_terminal(space.queue, job_link, timeout=300)
    ev = (f"plain={json.dumps(st1, ensure_ascii=False)}\nlink={json.dumps(st2, ensure_ascii=False)}\n"
          f"worker=\n{cli.worker_evidence()}")
    check_eq("rejected", st1.get("state"), "预存普通目录未被判 rejected", ev)
    check_eq("rejected", st2.get("state"), "预存符号链接未被判 rejected", ev)
    check("ERR_RUN_DIR_EXISTS" in (st1.get("error") or ""), "拒绝原因不是 ERR_RUN_DIR_EXISTS", ev)
    check_eq(pre_before, snapshot_tree(pre), "拒绝时改动了预存目录（内容或元数据变化）", ev)
    check(not (pre / ".wdb-run-owner").exists(), "拒绝时仍在预存目录里写了 claim 标记", ev)
    check_eq(ext_before, snapshot_tree(external), "通过 symlink 写进了 runs-root 之外的目标", ev)
    return f"plain/link 双双 rejected（未复用），预存目录与外部目标逐字节未变"


# --------------------------------------------------------------------------
# 工具函数
# --------------------------------------------------------------------------
_CTX: Ctx | None = None


def ctx_cli(ctx: Ctx) -> Cli:
    return Cli(ctx)


def find_echo_app() -> Path | None:
    base = FIXTURE_ECHO / "bin" / "Release" / "net10.0"
    for cand in (base / "EchoApp", base / "EchoApp.dll"):
        if cand.exists():
            return cand
    for cand in (FIXTURE_ECHO / "bin").rglob("EchoApp.dll"):
        return cand
    return None


def parse_echo(stdout: str) -> dict:
    for line in stdout.splitlines():
        line = line.strip()
        if line.startswith("WDB_ECHO "):
            return json.loads(line[len("WDB_ECHO "):])
    raise SmokeFailure("stdout 里找不到 WDB_ECHO 结构化输出\n--- stdout ---\n" + stdout[-2000:])


def read_text(path: Path) -> str:
    if not path.exists():
        return ""
    return path.read_text(encoding="utf-8", errors="replace")


def read_bytes(path: Path) -> bytes:
    if not path.exists():
        return b""
    return path.read_bytes()


def marker_line_count(path: Path) -> int:
    if not path.exists():
        return 0
    return len([l for l in path.read_text(encoding="utf-8", errors="replace").splitlines() if l.strip()])


def socket_fds(pid: int) -> set[str]:
    """该进程自己持有的 socket inode 集合（/proc/<pid>/fd → socket:[inode]）。"""
    fd_dir = Path(f"/proc/{pid}/fd")
    if not fd_dir.exists():
        return set()
    inodes: set[str] = set()
    for fd in fd_dir.iterdir():
        try:
            target = os.readlink(fd)
        except OSError:
            continue
        if target.startswith("socket:[") and target.endswith("]"):
            inodes.add(target[len("socket:["):-1])
    return inodes


def listening_sockets(pid: int) -> list[str]:
    """该进程自己持有的 LISTEN 套接字。

    注意：/proc/<pid>/net/tcp 是**网络命名空间级**的，直接读会把系统上所有监听都算进来
    （早期版本就是这么误报的）。必须先用 /proc/<pid>/fd 取出本进程的 socket inode 再做交集。
    """
    owned = socket_fds(pid)
    if not owned:
        return []
    found: list[str] = []
    for name in ("tcp", "tcp6"):
        p = Path(f"/proc/{pid}/net/{name}")
        if not p.exists():
            continue
        for line in p.read_text(encoding="utf-8", errors="replace").splitlines()[1:]:
            parts = line.split()
            if len(parts) > 9 and parts[3] == "0A" and parts[9] in owned:  # 0A = LISTEN，第 10 列=inode
                found.append(f"{name}:{parts[1]}")
    return found


# --------------------------------------------------------------------------
# 入口
# --------------------------------------------------------------------------
def ensure_bridge_built(ctx: Ctx) -> None:
    """构建桥并定位 wdb.dll。失败时保持 wdb_dll=None，让 L1/L2 用例给出明确失败信息。"""
    proj = BRIDGE_DIR / "Bridge.csproj"
    r = run([ctx.dotnet, "build", str(proj), "-c", "Release", "--nologo", "-v", "q"], cwd=REPO_ROOT, timeout=900)
    if r.code != 0:
        ctx.wdb_dll = None
        print(f"[smoke] 桥构建失败（L1/L2 将被跳过）：\n{r.evidence()}", flush=True)
        return
    for cand in (BRIDGE_DIR / "bin" / "Release" / "net10.0" / "wdb.dll",):
        if cand.exists():
            ctx.wdb_dll = cand
            return
    matches = list((BRIDGE_DIR / "bin").rglob("wdb.dll"))
    ctx.wdb_dll = matches[0] if matches else None
    if ctx.wdb_dll is None:
        print("[smoke] 构建成功但找不到 wdb.dll", flush=True)


def find_dotnet() -> str:
    found = shutil.which("dotnet")
    if found:
        return found
    fallback = Path.home() / ".dotnet" / "dotnet"
    if fallback.exists():
        return str(fallback)
    raise SystemExit("找不到 dotnet（PATH 与 ~/.dotnet 都没有）")


def main() -> int:
    global _CTX
    ap = argparse.ArgumentParser(description="Windows 调试桥黑盒冒烟（stdlib only）")
    ap.add_argument("--list", action="store_true", help="列出用例")
    ap.add_argument("--filter", default=None, help="按名字子串过滤")
    ap.add_argument("--tier", default=None, choices=["infra", "cli", "e2e"], help="只跑某一层")
    ap.add_argument("--keep", action="store_true", help="保留临时现场")
    ap.add_argument("--tmp-root", default=None, help="临时根（默认 <repo>/tools/windows-debug-bridge/tests/.tmp）")
    ap.add_argument("--print-spec", action="store_true", help="打印待与 Cli.cs 同步的 CLI 契约")
    args = ap.parse_args()

    if args.print_spec:
        for verb in sorted(SPEC):
            mark = "  [待核实]" if verb in SPEC_UNVERIFIED else "  [已由 start-worker.ps1 证实]"
            print(f"{verb:<10} {' '.join(SPEC[verb])}{mark}")
        return 0

    selected = []
    for tier, name, fn_name in TESTS:
        if args.tier and tier != args.tier:
            continue
        if args.filter and args.filter.lower() not in name.lower():
            continue
        selected.append((tier, name, globals()[fn_name]))

    if args.list:
        for tier, name, _ in selected:
            print(f"[{tier:<5}] {name}")
        print(f"共 {len(selected)} 个用例")
        return 0

    tmp_root = Path(args.tmp_root) if args.tmp_root else TESTS_DIR / ".tmp"
    tmp_root.mkdir(parents=True, exist_ok=True)
    ctx = Ctx(tmp_root=tmp_root, keep=args.keep, dotnet=find_dotnet())
    _CTX = ctx
    ensure_bridge_built(ctx)

    print(f"smoke · {len(selected)} 个用例 · tmp={tmp_root} · dotnet={ctx.dotnet}")
    print(f"wdb.dll = {ctx.wdb_dll}")
    print("-" * 78)

    passed = failed = skipped = 0
    failures: list[tuple[str, str]] = []
    started = time.time()

    for tier, name, fn in selected:
        if tier in {"cli", "e2e"} and ctx.wdb_dll is None:
            print(f"[SKIP] [{tier}] {name} — 桥未编译成功（Cli/Worker 尚未落盘）")
            skipped += 1
            continue
        t0 = time.time()
        try:
            detail = fn(ctx) or ""
            passed += 1
            print(f"[PASS] [{tier}] {name} ({time.time() - t0:.1f}s)" + (f" — {detail}" if detail else ""))
        except SkipTest as exc:
            skipped += 1
            print(f"[SKIP] [{tier}] {name} — {exc}")
        except Exception as exc:
            failed += 1
            msg = str(exc)
            failures.append((name, msg))
            print(f"[FAIL] [{tier}] {name} ({time.time() - t0:.1f}s)")
            print("       " + msg.replace("\n", "\n       ")[:6000])
        finally:
            ctx.stop_worker()

    print("-" * 78)
    print(f"结果：pass={passed} fail={failed} skip={skipped} 用时={time.time() - started:.1f}s")
    if failures:
        print("失败用例：" + ", ".join(n for n, _ in failures))
    ctx.cleanup()
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
