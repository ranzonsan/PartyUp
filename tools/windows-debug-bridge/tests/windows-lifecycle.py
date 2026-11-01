#!/usr/bin/env python3
"""Windows 调试桥 · 副 Worker 生命周期验收入口（WSL 侧驱动，真实 Windows 执行）。

用法：
    python3 tools/windows-debug-bridge/tests/windows-lifecycle.py \
        [--queue <主队列根>] [--main-pid <主 Worker PID>] [--timeout 1200] [--no-wait] [--json] [--dry-run]

主 Worker PID：默认从 <queue>/worker.json **现场读取**并核验（protocolVersion/pid/hostName/queueRoot 一致 +
心跳新鲜）；若显式传 --main-pid，则必须与现场 PID 相同，否则 fail closed 直接退出、不提交任何任务
（写死陈旧 PID 会让 safety 断言变成假证据）。

做什么：
    1) 在 <repo>/.wdb/lifecycle/<UTCstamp>/ 下新建**本次唯一**目录，并建 secondary-queue/；
    2) 用 wslpath -w 把它转成 \\\\wsl.localhost\\... 的 Windows 可见路径（探针的 UNC queue）；
    3) 通过 CLI 向**主队列**投一个 build-run 任务（主 Worker 会串行认领执行，不插队）；
       该任务在 Windows 上跑 fixtures/WorkerLifecycleProbe，探针再自行拉起专用副 Worker；
    4) 读该任务的 stdout.log，取出最后一行 WDB_LIFECYCLE 板子并打印。

不做什么（硬性）：
    * 绝不 stop / shutdown / kill 主 Worker，也不碰主队列里别人的任务目录；
    * 不直接调用任何 Windows 可执行文件；只用 CLI 的文件队列协议；
    * 只创建上面那个唯一目录，不删除/覆盖任何既有文件。
退出码：0 = 探针 verdict=PASS；1 = 探针 FAIL；2 = 驱动自身失败（未拿到板子）。
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

BOARD_PREFIX = "WDB_LIFECYCLE "
PROBE_PROJECT = "tools/windows-debug-bridge/tests/fixtures/WorkerLifecycleProbe/WorkerLifecycleProbe.csproj"  # 仓库相对路径


def sh(cmd: list[str], **kw) -> subprocess.CompletedProcess[str]:
    return subprocess.run(cmd, text=True, capture_output=True, **kw)  # noqa: PLW1510  (returncode 由调用方判定)


def repo_root() -> Path:
    here = Path(__file__).resolve()
    for parent in [here, *here.parents]:
        if (parent / "GalleriesManager.slnx").exists() or (parent / ".git").exists():
            return parent
    raise SystemExit("无法定位仓库根（未找到 GalleriesManager.slnx 或 .git）")


def to_windows_path(path: Path) -> str:
    """用 wslpath -w 转 Windows 路径；失败时按 \\\\wsl.localhost\\<distro>\\... 兜底。"""
    result = sh(["wslpath", "-w", str(path)])
    if result.returncode == 0 and result.stdout.strip():
        return result.stdout.strip()

    distro = os.environ.get("WSL_DISTRO_NAME", "")
    if not distro:
        raise SystemExit(f"wslpath -w 失败且无 WSL_DISTRO_NAME，无法得到 Windows 路径：{path}")
    return "\\\\wsl.localhost\\" + distro + str(path).replace("/", "\\")


class DriverError(Exception):
    """现场核验失败：一律 fail closed，不向队列写任何东西。"""


def heartbeat_age_seconds(value: str | None) -> float | None:
    if not value:
        return None
    try:
        text = value.strip().replace("Z", "+00:00")
        when = datetime.fromisoformat(text)
    except ValueError:
        return None
    if when.tzinfo is None:
        when = when.replace(tzinfo=timezone.utc)
    return (datetime.now(timezone.utc) - when).total_seconds()


def _norm_path(value: str) -> str:
    return value.replace("\\", "/").rstrip("/").lower()


def _strip_unc(value: str) -> str:
    """\\\\wsl.localhost\\<distro>\\home\\... → /home/...（与 WSL 侧路径可比）。"""
    value = _norm_path(value)
    if value.startswith("//"):
        parts = [x for x in value.split("/") if x]
        return "/" + "/".join(parts[2:])
    return value


def same_queue(worker_root: str, wsl_queue: Path) -> bool:
    if not worker_root:
        return False
    return _norm_path(worker_root) == _norm_path(str(wsl_queue)) or _strip_unc(worker_root) == _norm_path(str(wsl_queue))


def read_worker_or_fail(queue: Path, max_heartbeat_age_seconds: float = 120.0) -> dict:
    """只读核验现场主 Worker：缺文件/协议不符/pid 非法/队列不符/心跳失联 一律判失败。"""
    path = queue / "worker.json"
    if not path.is_file():
        raise DriverError(f"找不到主 Worker 心跳 {path}（Worker 未在跑？）")
    try:
        worker = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as ex:
        raise DriverError(f"主 Worker 心跳不可解析（{path}）：{ex}") from ex

    pid = worker.get("pid")
    if not isinstance(pid, int) or pid <= 0:
        raise DriverError(f"心跳里的 pid 非法：{pid!r}")
    if worker.get("protocolVersion") != 1:
        raise DriverError(f"心跳协议版本不匹配：{worker.get('protocolVersion')!r}（本驱动要求 1）")
    if not worker.get("hostName"):
        raise DriverError("心跳缺少 hostName")
    if not same_queue(str(worker.get("queueRoot", "")), queue):
        raise DriverError(f"心跳的 queueRoot 与本次队列不一致：worker={worker.get('queueRoot')!r} 驱动={str(queue)!r}")

    age = heartbeat_age_seconds(worker.get("heartbeatUtc"))
    if age is None:
        raise DriverError(f"心跳时间不可解析：{worker.get('heartbeatUtc')!r}")
    if age > max_heartbeat_age_seconds:
        raise DriverError(f"心跳已失联 {age:.1f}s（>{max_heartbeat_age_seconds:.0f}s）：拒绝向不健康的主 Worker 提交")

    worker["_heartbeatAgeSeconds"] = age
    return worker


def main() -> int:
    parser = argparse.ArgumentParser(add_help=True)
    parser.add_argument("--queue", default=None, help="主队列根（默认 <repo>/.wdb/queue）")
    parser.add_argument("--main-pid", type=int, default=0,
                        help="主 Worker PID：缺省自动从 <queue>/worker.json 现场读取；显式传入时必须与现场一致，否则 fail closed")
    parser.add_argument("--timeout", type=int, default=1200, help="任务硬超时秒数（Worker 侧）")
    parser.add_argument("--wait-timeout", type=int, default=1500, help="CLI 等待终态秒数")
    parser.add_argument("--no-wait", action="store_true", help="只投递，不等结果")
    parser.add_argument("--json", action="store_true", help="stdout 只输出板子 JSON")
    parser.add_argument("--dry-run", action="store_true", help="只核验现场并打印将要提交的命令，不向队列写任何东西")
    args = parser.parse_args()

    repo = repo_root()
    queue = Path(args.queue) if args.queue else repo / ".wdb" / "queue"

    try:
        worker = read_worker_or_fail(queue)
    except DriverError as ex:
        sys.stderr.write(f"[lifecycle] 现场核验失败（fail closed，未提交任何任务）：{ex}\n")
        return 2

    live_pid = int(worker["pid"])
    if args.main_pid and args.main_pid != live_pid:
        sys.stderr.write(
            f"[lifecycle] fail closed：显式 --main-pid={args.main_pid} 与现场 worker.json 的 pid={live_pid} 不一致，"
            "拒绝提交（陈旧 PID 会让 safety 断言变成假证据）。\n")
        return 2

    main_pid = args.main_pid or live_pid
    pid_source = "explicit(--main-pid 已与现场一致)" if args.main_pid else "worker.json(现场读取)"
    sys.stderr.write(
        f"[lifecycle] 现场主 Worker 核验通过：pid={main_pid} workerId={worker.get('workerId')} "
        f"host={worker.get('hostName')} heartbeatAge={worker['_heartbeatAgeSeconds']:.1f}s queueRootMatch=yes "
        f"来源={pid_source}\n")

    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    run_dir = repo / ".wdb" / "lifecycle" / stamp
    secondary_queue = run_dir / "secondary-queue"
    unc_queue = to_windows_path(secondary_queue)   # dry-run 也要先算出路径，但绝不落盘

    probe_args = ["--unc-queue", unc_queue,
                  "--start-timeout", "45", "--run-timeout", "120",
                  "--state-timeout", "60", "--gone-timeout", "45",
                  "--overall-timeout", "420",
                  "--main-pid", str(main_pid)]   # 探针侧强制要求现场 PID（缺失即 usage 拒绝执行）

    cmd = [
        "dotnet", "run", "--project", str(repo / "tools" / "windows-debug-bridge" / "Bridge.csproj"), "--",
        "build-run",
        "--project", PROBE_PROJECT,
        "--queue", str(queue),
        "--timeout", str(args.timeout),
        "--json",
        "--",
        *probe_args,
    ]
    if not args.no_wait:
        cmd.insert(cmd.index("--json"), "--wait")
        cmd.insert(cmd.index("--wait") + 1, "--wait-timeout")
        cmd.insert(cmd.index("--wait-timeout") + 1, str(args.wait_timeout))

    if args.dry_run:
        sys.stderr.write("[lifecycle] --dry-run：仅核验现场并打印命令，不向队列写入任何东西。\n")
        print(json.dumps({
            "dryRun": True,
            "mainPid": main_pid,
            "mainPidSource": pid_source,
            "worker": {k: v for k, v in worker.items() if k != "_heartbeatAgeSeconds"},
            "heartbeatAgeSeconds": round(worker["_heartbeatAgeSeconds"], 2),
            "uncSecondaryQueue": unc_queue,
            "runDir": str(run_dir),
            "cmd": cmd,
        }, ensure_ascii=False, indent=2))
        return 0

    secondary_queue.mkdir(parents=True, exist_ok=True)   # 只有真的要提交时才建本次唯一目录
    sys.stderr.write(f"[lifecycle] 唯一目录: {run_dir}\n")
    sys.stderr.write(f"[lifecycle] UNC queue: {unc_queue}\n")
    sys.stderr.write("[lifecycle] 提交主队列任务（串行执行，不插队、不停主 Worker）…\n")

    # 注意：CLI 的 --wait 会**透传任务的退出码**（探针 FAIL ⇒ 1），这不是驱动失败；
    # 只要还能解析出 job id，就去把板子取回来（否则真实失败原因会被"驱动失败"掩盖）。
    submitted = sh(cmd, cwd=str(repo))
    if submitted.returncode != 0:
        sys.stderr.write(f"[lifecycle] CLI 退出码 {submitted.returncode}（任务自身退出码会透传，继续尝试取板子）\n")

    job_id = None
    for line in submitted.stdout.splitlines():
        line = line.strip()
        if line.startswith("{"):
            try:
                payload = json.loads(line)
            except json.JSONDecodeError:
                continue
            job_id = payload.get("id") or payload.get("jobId") or job_id

    if job_id is None:
        match = re.search(r"([0-9]{8}T[0-9]{9}Z-[0-9a-f]{6})", submitted.stdout)
        job_id = match.group(1) if match else None

    if job_id is None:
        sys.stderr.write("[lifecycle] 未能解析任务 id：\n" + submitted.stdout)
        return 2

    stdout_log = queue / "jobs" / job_id / "stdout.log"
    board = None
    if stdout_log.exists():
        for line in stdout_log.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.startswith(BOARD_PREFIX):
                try:
                    board = json.loads(line[len(BOARD_PREFIX):])
                except json.JSONDecodeError:
                    continue

    if board is None:
        sys.stderr.write(f"[lifecycle] 任务 {job_id} 的 stdout.log 里没有板子（{stdout_log}）\n")
        sys.stderr.write(submitted.stdout[-2000:])
        return 2

    board["jobId"] = job_id
    board["queue"] = str(queue)
    board["uncSecondaryQueue"] = unc_queue
    board["localRunDir"] = str(run_dir)

    if args.json:
        print(json.dumps(board, ensure_ascii=False))
    else:
        print(f"jobId={job_id}")
        print(f"verdict={board.get('verdict')} host={board.get('host')}")
        for check in board.get("checks", []):
            print(f"  [{check.get('status','?').upper():4}] {check.get('name')} :: {check.get('detail','')}")
        print("phaseFacts=" + json.dumps(board.get("phaseFacts", {}), ensure_ascii=False, indent=2))

    return 0 if board.get("verdict") == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
