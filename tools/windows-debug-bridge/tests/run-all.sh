#!/usr/bin/env bash
# Windows 调试桥 · 一键离线测试入口（纯 BCL harness + 黑盒冒烟）
#
#   bash tools/windows-debug-bridge/tests/run-all.sh            # 全部
#   bash tools/windows-debug-bridge/tests/run-all.sh --infra    # 只跑不依赖 CLI/Worker 的一层
#
# 退出码：任一套件失败即非 0（便于 CI 与父代理判定）。
# 注意：本脚本只证明 Linux/WSL 侧语义；Windows 实机项一律未测，不得据此宣称已实机通过。
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
DOTNET="${DOTNET:-$(command -v dotnet || echo "$HOME/.dotnet/dotnet")}"

mode="all"
case "${1:-}" in
  --infra) mode="infra" ;;
  --cli) mode="cli" ;;
  --e2e) mode="e2e" ;;
  "" ) ;;
  *) echo "未知参数：$1（可用 --infra/--cli/--e2e）" >&2; exit 2 ;;
esac

rc=0

if [[ "$mode" == "infra" || "$mode" == "cli" || "$mode" == "e2e" ]]; then
  # 黑盒冒烟：infra 层今天即可跑；cli/e2e 需要 Bridge.csproj 能编译
  echo "=== smoke.py (--tier $mode) ==="
  python3 "$HERE/smoke.py" --tier "$mode" || rc=1
else
  echo "=== Wdb.Tests (纯 BCL 单元/合同 harness) ==="
  "$DOTNET" run --project "$HERE/Wdb.Tests.csproj" -c Release || rc=1

  echo
  echo "=== smoke.py (L0 infra) ==="
  python3 "$HERE/smoke.py" --tier infra || rc=1

  echo
  echo "=== smoke.py (L1+L2：需要 Bridge 可编译 + Worker) ==="
  python3 "$HERE/smoke.py" --tier cli || rc=1
  python3 "$HERE/smoke.py" --tier e2e || rc=1
fi

exit "$rc"
