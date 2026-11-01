#!/usr/bin/env bash
# 把工作区源码镜像到 Windows 可见目录，供 Windows 工具链构建。
# 用法: scripts/win-sync.sh [源] [目标]
set -euo pipefail

src="${1:-/home/ranzon/PartyUp}"
dst="${2:-/mnt/c/src/PartyUp}"

mkdir -p "$dst"
rsync -a --delete \
  --exclude='.git/' \
  --exclude='bin/' \
  --exclude='obj/' \
  --exclude='artifacts/' \
  --exclude='.dsh-memory/' \
  "$src/" "$dst/"

printf 'synced: %s -> %s\n' "$src" "$dst"
