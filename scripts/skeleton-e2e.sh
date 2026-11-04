#!/usr/bin/env bash
# 一键复现端到端骨架（Go 服务端 + Avalonia 客户端 + 媒体核心 DLL）。
#
#   scripts/skeleton-e2e.sh            # 纯无头回归：服务端测试 → 客户端编译 → 自检 → 双进程互通
#   scripts/skeleton-e2e.sh --gui      # 再加一步：真开一个 GUI 窗口并截图（需要桌面空闲！）
#
# 覆盖的验收点：
#   1. 服务端 `go build` / `go vet` / `go test ./...` 全绿；
#   2. 服务端启动后 stdout 打印邀请码与 bootstrap token（全新数据目录）；
#   3. 客户端只在 Windows 上**编译**（wdb build，不启进程），再用无窗口方式跑 console 模式：
#      DLL 18 导出 → pu_engine_create → pu_screen_start(synthetic) → 轮询到 PU_EVENT_STATE → destroy；
#      bootstrap + 邀请码注册 + 两条 WebSocket 互发消息 + history；
#   4. 两个独立 OS 进程（--duo 拉起 --role b 子进程）通过服务端互发消息；
#   5. --gui：真实窗口登录、进入频道、收到别的进程发来的实时消息（截图取证）。
#
# 无窗口约定：默认路径全程不创建任何 Windows 窗口——客户端用
#   `pwsh.exe -WindowStyle Hidden -File scripts/run-headless.ps1` 启动（Start-Process -NoNewWindow），
#   且 --selftest / --duo / --role send 在 Program.Main 里就返回，不会初始化 Avalonia。
#
# 退出码：0 全部通过；1 有失败；2 环境/参数问题。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERVER_DIR="$ROOT/server"
CLIENT_PROJECT="client/PartyUp.Client.csproj"
BIN="$SERVER_DIR/.cache/bin/partyup"
PORT="${PARTYUP_E2E_PORT:-18777}"
BASE="http://127.0.0.1:$PORT"
CHANNEL="${PARTYUP_E2E_CHANNEL:-interop}"
WITH_GUI=0

for arg in "$@"; do
  case "$arg" in
    --gui) WITH_GUI=1 ;;
    -h|--help)
      sed -n '2,22p' "$0"
      exit 0
      ;;
    *)
      echo "未知参数：$arg（可用：--gui）" >&2
      exit 2
      ;;
  esac
done

if [[ "$WITH_GUI" -eq 1 ]]; then
  printf '注意：--gui 会在 Windows 桌面真正弹出一个 Avalonia 窗口。\n' >&2
  printf '      只有确认桌面空闲、用户允许弹窗时才使用；日常回归用脚本默认路径（纯无头）。\n' >&2
fi

log()  { printf '\n== %s\n' "$*"; }
ok()   { printf 'PASS  %s\n' "$*"; }
die()  { printf 'FAIL  %s\n' "$*" >&2; exit 1; }

# Go 的模块/构建缓存必须落在工作区内：$HOME 在 WSL 里只读。
export GOMODCACHE="$SERVER_DIR/.cache/gomod"
export GOCACHE="$SERVER_DIR/.cache/go-build"
export GOPATH="$SERVER_DIR/.cache/gopath"
export GOFLAGS=-mod=readonly

# 调试桥快照排除项：仓库 3GB，不排除会把整棵 src/media-core 的中间产物搬去 Windows。
EXCLUDES=(
  --exclude third_party
  --exclude spikes
  --exclude server/.cache
  --exclude client/.cache
  --exclude src/media-core/out/partyup_media.pdb
  --exclude src/media-core/out/media_smoke.pdb
  --exclude src/media-core/out/media_signaling_smoke.pdb
  --exclude src/media-core/out/teardown_stress.pdb
  --exclude src/media-core/out/run-logs
)

DATA_DIR="$SERVER_DIR/.cache/e2e-data-$(date +%Y%m%d%H%M%S)"
SERVER_LOG="$SERVER_DIR/.cache/e2e-server.log"
SERVER_ERR="$SERVER_DIR/.cache/e2e-server.err"
SERVER_PID=""
CLIENT_EXE=""
CLIENT_LOG_WSL=""
CLIENT_LOG_WIN=""

cleanup() {
  if [[ -n "$SERVER_PID" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT INT TERM

# 一次性问 Windows 要 TEMP / LOCALAPPDATA（只读取环境变量，不启窗口）。
resolve_windows_paths() {
  command -v pwsh.exe >/dev/null 2>&1 || die "找不到 pwsh.exe（Windows PowerShell 7）；无窗口运行客户端需要它"
  local temp_win localappdata_win
  temp_win="$(pwsh.exe -NoProfile -WindowStyle Hidden -Command '$env:TEMP' 2>/dev/null | tr -d '\r' | tail -1)"
  localappdata_win="$(pwsh.exe -NoProfile -WindowStyle Hidden -Command '$env:LOCALAPPDATA' 2>/dev/null | tr -d '\r' | tail -1)"
  [[ -n "$temp_win" ]] || die "无法取得 Windows %TEMP%"
  [[ -n "$localappdata_win" ]] || die "无法取得 Windows %LOCALAPPDATA%"
  CLIENT_LOG_WIN="$temp_win\\partyup-e2e-client.log"
  CLIENT_LOG_WSL="$(wslpath -u "$CLIENT_LOG_WIN")"
  WIN_RUNS_ROOT="$localappdata_win\\WslDebugBridge\\runs"
}

# 只编译，不运行：这一步不产生任何窗口。
build_client() {
  local json id state
  json="$(cd "$ROOT" && ./scripts/wdb.sh build \
    --project "$CLIENT_PROJECT" --wait --json "${EXCLUDES[@]}" 2>&1 | tail -1)"
  id="$(jq -r '.id // empty' <<<"$json")"
  state="$(jq -r '.state // empty' <<<"$json")"
  [[ -n "$id" ]] || die "客户端编译任务提交失败：$json"
  [[ "$state" == "succeeded" ]] || die "客户端编译失败（job=$id，state=$state）"
  CLIENT_EXE="$WIN_RUNS_ROOT\\$id\\src\\client\\bin\\Debug\\net10.0\\win-x64\\PartyUp.Client.exe"
  ok "客户端编译通过（job=$id，未启动进程）"
}

# 用无窗口方式跑客户端 console 模式；stdout 落到 Windows 临时文件再读回。
# 用法：run_headless --selftest --server http://...
run_headless() {
  local joined encoded launch
  joined="$(IFS=,; echo "$*")"
  # -File 的参数解析会把以 -- 开头的值当成参数名，所以参数列表用 base64 传。
  encoded="$(printf '%s' "$joined" | base64 -w0)"
  # 不删旧日志文件：/mnt/c 在 WSL 侧只读，且 RedirectStandardOutput 会截断重写。
  # 脚本在 UNC 路径上，PowerShell 默认执行策略会拒绝运行，必须显式 Bypass。
  if ! launch="$(pwsh.exe -NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass \
      -File "$(wslpath -w "$ROOT/scripts/run-headless.ps1")" \
      -Exe "$CLIENT_EXE" -LogPath "$CLIENT_LOG_WIN" -ArgumentsBase64 "$encoded" 2>&1)"; then
    printf '%s\n' "$launch" >&2
    die "无窗口启动客户端失败（exe=$CLIENT_EXE）"
  fi
  [[ -f "$CLIENT_LOG_WSL" ]] || { printf '%s\n' "$launch" >&2; die "客户端没有产生输出（$CLIENT_LOG_WSL）"; }
  tr -d '\r' < "$CLIENT_LOG_WSL"
}

log "1/5 服务端构建与测试（go build / go vet / go test）"
(
  cd "$SERVER_DIR"
  go build ./...
  go vet ./...
  go test -count=1 -timeout 120s ./... >/dev/null
  go build -o "$BIN" ./cmd/partyup
)
ok "go build / go vet / go test ./... 通过，二进制 $BIN"

log "2/5 客户端编译（wdb build，不启动进程）"
resolve_windows_paths
build_client

log "3/5 启动服务端（全新数据目录 $DATA_DIR）"
mkdir -p "$SERVER_DIR/.cache"
: > "$SERVER_LOG"
: > "$SERVER_ERR"
"$BIN" serve --data "$DATA_DIR" --http "127.0.0.1:$PORT" --log-level info >"$SERVER_LOG" 2>"$SERVER_ERR" &
SERVER_PID=$!

for _ in $(seq 1 100); do
  grep -q '^READY$' "$SERVER_LOG" 2>/dev/null && break
  sleep 0.1
done
grep -q '^READY$' "$SERVER_LOG" || { cat "$SERVER_LOG" "$SERVER_ERR"; die "服务端 10 秒内没有 READY"; }

BOOTSTRAP_TOKEN="$(sed -n 's/^BOOTSTRAP_TOKEN=//p' "$SERVER_LOG" | head -1)"
INVITE_CODE="$(sed -n 's/^INVITE_CODE=//p' "$SERVER_LOG" | head -1)"
[[ -n "$BOOTSTRAP_TOKEN" ]] || die "服务端没有打印 BOOTSTRAP_TOKEN"
[[ -n "$INVITE_CODE" ]] || die "服务端没有创建邀请码"
ok "服务端监听 $BASE，已创建邀请码 INVITE_CODE=$INVITE_CODE"

log "4/5 客户端自检（无窗口：媒体核心 + 两个连接互发消息）"
SELFTEST_LOG="$(run_headless --selftest --server "$BASE" \
  --bootstrap-token "$BOOTSTRAP_TOKEN" --invite-code "$INVITE_CODE")"
if grep -q '^SELFTEST_RESULT=PASS$' <<<"$SELFTEST_LOG"; then
  ok "SELFTEST_RESULT=PASS"
else
  grep -E 'FAIL|SELFTEST_RESULT' <<<"$SELFTEST_LOG" || true
  die "客户端自检失败（完整日志：$CLIENT_LOG_WSL）"
fi
ALICE_TOKEN="$(sed -n 's/^SELFTEST_ALICE_TOKEN=//p' <<<"$SELFTEST_LOG" | head -1)"
BOB_TOKEN="$(sed -n 's/^SELFTEST_BOB_TOKEN=//p' <<<"$SELFTEST_LOG" | head -1)"
[[ -n "$ALICE_TOKEN" && -n "$BOB_TOKEN" ]] || die "自检没有输出两个会话 token"
ok "自检输出 alice/bob 会话 token，可复用于下一步"

log "5/5 双进程互通（--duo 在进程内拉起独立 --role b 子进程）"
DUO_LOG="$(run_headless --duo --server "$BASE" --channel "$CHANNEL" \
  --token-a "$ALICE_TOKEN" --token-b "$BOB_TOKEN")"
if grep -q '^INTEROP_RESULT=PASS$' <<<"$DUO_LOG"; then
  ok "INTEROP_RESULT=PASS（INTEROP_PROCESSES=2）"
else
  grep -E 'FAIL|INTEROP' <<<"$DUO_LOG" || true
  die "双进程互通失败（完整日志：$CLIENT_LOG_WSL）"
fi

if [[ "$WITH_GUI" -eq 1 ]]; then
  log "附加：GUI 取证（登录 → 进频道 → 收实时消息 → 截图）"
  printf '提示：本步骤会真开窗口；脚本用 build-run 启动 GUI。\n'
  GUI_JSON="$(cd "$ROOT" && ./scripts/wdb.sh build-run \
    --project "$CLIENT_PROJECT" --json "${EXCLUDES[@]}" -- \
    --server "$BASE" --token "$ALICE_TOKEN" 2>&1 | tail -1)"
  GUI_ID="$(jq -r '.id // empty' <<<"$GUI_JSON")"
  [[ -n "$GUI_ID" ]] || die "GUI 任务提交失败：$GUI_JSON"

  # 等窗口起来并完成自动登录（媒体引擎初始化约 2 秒，登录再加 1 秒）。
  sleep 20

  # 另一个 OS 进程（bob）发一条消息，GUI 应该实时显示出来。
  GUI_EXE="$WIN_RUNS_ROOT\\$GUI_ID\\src\\client\\bin\\Debug\\net10.0\\win-x64\\PartyUp.Client.exe"
  HELPER_ARGS="--role,send,--server,$BASE,--channel,$CHANNEL,--token,$BOB_TOKEN,--text,live message for GUI evidence"
  HELPER_B64="$(printf '%s' "$HELPER_ARGS" | base64 -w0)"
  pwsh.exe -NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass \
    -File "$(wslpath -w "$ROOT/scripts/run-headless.ps1")" \
    -Exe "$GUI_EXE" -LogPath "$CLIENT_LOG_WIN" -ArgumentsBase64 "$HELPER_B64" \
    >/dev/null 2>&1 || true
  sleep 2

  GUI_LOG="$(cd "$ROOT" && ./scripts/wdb.sh logs "$GUI_ID" --tail 2000 2>&1 | tr -d '\r')"
  grep -E 'gui: (connected as|opened #|message seq=)' <<<"$GUI_LOG" | tail -8 || true
  grep -q 'gui: connected as ' <<<"$GUI_LOG" || die "GUI 没有完成自动登录"
  grep -q 'gui: opened #' <<<"$GUI_LOG" || die "GUI 没有进入频道"
  if ! grep -q 'author=bob' <<<"$GUI_LOG"; then
    die "GUI 没有收到 helper 进程发来的实时消息"
  fi
  ok "GUI 日志：已登录、已进频道、收到另一进程的实时消息"

  SCREENSHOT="$ROOT/client/.cache/gui-evidence.png"
  mkdir -p "$(dirname "$SCREENSHOT")"
  # 按进程名把窗口最大化提到前台再截主屏，避免截到别的窗口。
  pwsh.exe -NoProfile -ExecutionPolicy Bypass \
    -File "$(wslpath -w "$ROOT/scripts/capture-window.ps1")" \
    -OutPath "$(wslpath -w "$SCREENSHOT")" -ProcessName 'PartyUp.Client' >/dev/null 2>&1 || true
  [[ -f "$SCREENSHOT" ]] && ok "GUI 截图：$SCREENSHOT"

  (cd "$ROOT" && ./scripts/wdb.sh stop "$GUI_ID" --reason "e2e 脚本取证完成" >/dev/null 2>&1 || true)
  ok "GUI 任务已关闭"
fi

log "全部通过"
printf '服务端：%s（数据目录 %s）\n' "$BASE" "$DATA_DIR"
printf '邀请码：%s\n' "$INVITE_CODE"
printf '证据：.docs/spikes/2026-11-02-skeleton.md\n'
