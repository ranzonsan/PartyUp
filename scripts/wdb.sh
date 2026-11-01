#!/usr/bin/env bash
# 调试桥 CLI 的薄包装：首次调用时自动构建 wdb，之后直接转发参数。
# 用法: scripts/wdb.sh doctor | status | build-run --project <csproj> --follow | logs <id> --follow | stop <id>
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DLL="$ROOT/tools/windows-debug-bridge/bin/Release/net10.0/wdb.dll"

if [[ ! -f "$DLL" ]]; then
  echo "wdb 未构建，先编译一次…" >&2
  dotnet build "$ROOT/tools/windows-debug-bridge/Bridge.csproj" -c Release --nologo >&2
fi

cd "$ROOT"
exec dotnet "$DLL" "$@"
