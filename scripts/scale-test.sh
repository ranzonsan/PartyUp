#!/usr/bin/env bash
# 规模压测：用 pion 假客户端（cmd/loadtest）对 SFU 中继打 N 个成员，同时在外部采样
# **服务端进程**的 CPU/RSS —— 客户端跑在同一台机器的另一个进程里，所以测出来的数字
# 是纯服务端的（不像 go test 那样把服务端和假客户端混在同一进程）。
#
# 用法：
#   scripts/scale-test.sh                      # 12 人单频道（默认）
#   MEMBERS=10 CHANNELS=5 scripts/scale-test.sh  # 想让 50 人时用这个参数组合
#
# 输出：$WORK（默认 /tmp/pu-scale）下 server.err / load.out / samples.txt，
# 以及终端上的容量表。
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
# 默认放工作区 .cache 下：DSH 的 /tmp 每次命令可能是隔离的，跑完就看不到证据了。
WORK=${WORK:-$ROOT/.cache/scale}
MEMBERS=${MEMBERS:-12}
CHANNELS=${CHANNELS:-1}
# SCREEN_MODE=direct 时服务端**不建视频房间**（纯语音基线）；
# SCREEN_MODE=relay 时每个订阅者都会有一条空跑视频 PC，若 SHARES>0 还有真实视频转发。
SCREEN_MODE=${SCREEN_MODE:-direct}
SHARES=${SHARES:-0}
DURATION=${DURATION:-20}
PORT=${PORT:-19001}
CHANNEL=${CHANNEL:-scale}
BIN=$WORK/bin

export GOCACHE=${GOCACHE:-$ROOT/server/.cache/gocache}
export GOPATH=${GOPATH:-$ROOT/server/.cache/gopath}
export GOFLAGS=${GOFLAGS:--mod=mod}

rm -rf "$WORK"
mkdir -p "$WORK/data" "$BIN"

echo "== build (server + loadtest)"
(
  cd "$ROOT/server"
  go build -o "$BIN/partyup" ./cmd/partyup
  go build -o "$BIN/loadtest" ./cmd/loadtest
)

echo "== start server (relay, port $PORT)"
"$BIN/partyup" serve --data "$WORK/data" --http "127.0.0.1:$PORT" --voice-mode relay --screen-mode "$SCREEN_MODE" \
  >"$WORK/server.out" 2>"$WORK/server.err" &
SERVER_PID=$!
trap 'kill "$SERVER_PID" 2>/dev/null || true' EXIT

for _ in $(seq 1 100); do
  grep -q '^READY' "$WORK/server.out" && break
  sleep 0.1
done
if ! grep -q '^READY' "$WORK/server.out"; then
  echo "server did not start"; tail -20 "$WORK/server.out" "$WORK/server.err"; exit 1
fi
BOOT=$(sed -n 's/^BOOTSTRAP_TOKEN=//p' "$WORK/server.out" | head -1)
echo "server pid=$SERVER_PID bootstrap=${BOOT:0:10}… members=$MEMBERS"

echo "== create admin + $((MEMBERS - 1)) members"
ADMIN_TOKEN=$(curl -sS -X POST "http://127.0.0.1:$PORT/api/bootstrap" \
  -H 'Content-Type: application/json' \
  -d "{\"token\":\"$BOOT\",\"username\":\"loadadmin\",\"password\":\"password-123\"}" |
  python3 -c 'import sys,json;print(json.load(sys.stdin)["token"])')

: >"$WORK/tokens.txt"
# admin 也当第 1 个成员用（建完频道后它的连接就关了，token 仍有效）。
echo "admin=$ADMIN_TOKEN" >>"$WORK/tokens.txt"
for i in $(seq 1 $((MEMBERS - 1))); do
  CODE=$("$BIN/partyup" invite --data "$WORK/data" | sed -n 's/^INVITE_CODE=//p')
  TOK=$(curl -sS -X POST "http://127.0.0.1:$PORT/api/invite/$CODE" \
    -H 'Content-Type: application/json' \
    -d "{\"username\":\"load$i\",\"password\":\"password-123\"}" |
    python3 -c 'import sys,json;print(json.load(sys.stdin)["token"])')
  echo "m$i=$TOK" >>"$WORK/tokens.txt"
done
echo "tokens ready: $(wc -l <"$WORK/tokens.txt")"

echo "== run load for ${DURATION}s"
"$BIN/loadtest" -server "http://127.0.0.1:$PORT" -admin-token "$ADMIN_TOKEN" \
  -channel-name "$CHANNEL" -channels "$CHANNELS" -tokens-file "$WORK/tokens.txt" -members "$MEMBERS" \
  -share "$SHARES" \
  -duration "${DURATION}s" >"$WORK/load.out" 2>"$WORK/load.err" &
LOAD_PID=$!

# 采样服务端进程：utime/stime（jiffies，USER_HZ=100）+ VmRSS(kB)。
: >"$WORK/samples.txt"
while kill -0 "$LOAD_PID" 2>/dev/null; do
  read -r _ _ _ _ _ _ _ _ _ _ _ _ _ UT ST _ </proc/"$SERVER_PID"/stat || break
  RSS=$(awk '/VmRSS/{print $2}' /proc/"$SERVER_PID"/status 2>/dev/null || echo 0)
  echo "$(date +%s.%N) $UT $ST $RSS" >>"$WORK/samples.txt"
  sleep 1
done
wait "$LOAD_PID" || true

echo
echo "== capacity table"
MEMBERS="$MEMBERS" DURATION="$DURATION" WORK="$WORK" python3 - "$WORK" "$MEMBERS" <<'PY'
import json, os, re, sys

work, members = sys.argv[1], int(sys.argv[2])
samples = []
for line in open(os.path.join(work, "samples.txt")):
    p = line.split()
    if len(p) != 4:
        continue
    samples.append((float(p[0]), int(p[1]) + int(p[2]), int(p[3])))

# 前 12 秒是爬坡段（50 个客户端建连 + 进语音，若有共享还有共享建立），不算稳态。
SKIP_S = 12.0
base = samples[0][0] if samples else 0.0
cpus, rsss, all_cpus = [], [], []
for (t0, t0c, _), (t1, t1c, r1) in zip(samples, samples[1:]):
    if t1 - t0 <= 0:
        continue
    pct = (t1c - t0c) / 100.0 / (t1 - t0) * 100.0
    all_cpus.append(pct)
    if t0 - base < SKIP_S:
        continue
    cpus.append(pct)
    rsss.append(r1)

peak_rss = max((s[2] for s in samples), default=0) / 1024.0
avg_cpu = sum(cpus) / len(cpus) if cpus else 0.0
max_cpu = max(cpus) if cpus else 0.0
peak_cpu = max(all_cpus) if all_cpus else 0.0

summary = None
raw = open(os.path.join(work, "load.out")).read()
m = re.search(r"SUMMARY_JSON_BEGIN\n(.*?)\nSUMMARY_JSON_END", raw, re.S)
if m:
    summary = json.loads(m.group(1))

print(f"members        : {members}")
print(f"screen_mode    : {os.environ.get('SCREEN_MODE', 'direct')}；shares={os.environ.get('SHARES', '0')}")
print(f"channels       : {summary['channels'] if summary else '?'}（每频道 {members // max(1, CHANNELS if (CHANNELS := int(os.environ.get('CHANNELS', '1'))) else 1)} 人）")
print(f"server 平均 CPU : {avg_cpu:6.1f} %（稳态峰值 {max_cpu:.1f} %，含建连/共享建立的全程峰值 {peak_cpu:.1f} %）")
print(f"server 峰值 RSS : {peak_rss:6.1f} MB")
if summary:
    print(f"每端远端音轨 : {summary['min_tracks']} / 期望 {summary['expect_tracks']}（各频道期望最小值）")
    print(f"每端平均收包 : {summary['avg_pps']} pkt/s")
    print(f"服务端总转发 : {summary['total_pps']} pkt/s")
    print(f"共享者/观看者 : {summary['sharers']} 路 / {summary['video_viewers']} 人")
    print(f"视频转发     : 每观看者 {summary['avg_video_pps']} pkt/s，合计 {summary['total_video_pps']} pkt/s")
    bad = [p for p in summary["peers"] if p["tracks"] < summary["expect_tracks"]]
    if bad:
        print("劣化的成员   : " + ", ".join(f"{p['name']}(tracks={p['tracks']})" for p in bad))
    else:
        print("劣化         : 无（每端都收到全部 N-1 路）")

# 服务端日志里的告警（槽位不足、连接失败等）。
err = open(os.path.join(work, "server.err")).read()
no_slot = len(re.findall(r"no free slot", err))
fails = len(re.findall(r"level=ERROR", err))
print(f"槽位不足警告 : {no_slot}")
print(f"服务端 ERROR : {fails}")
print(f"证据         : {work}/ (samples.txt, server.err, load.out)")
PY
