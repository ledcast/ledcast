#!/usr/bin/env bash
# Проверяет, что версия одна во всех местах, а при передаче тега — что она
# совпадает с ним.
#
#   scripts/check-version.sh            # только согласованность
#   scripts/check-version.sh v0.1.0     # и совпадение с тегом
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"

firmware=$(sed -n 's/^set(PROJECT_VER "\(.*\)")$/\1/p' "$root/firmware/CMakeLists.txt")
cargo=$(sed -n 's/^version = "\(.*\)"$/\1/p' "$root/desktop/Cargo.toml" | head -1)
tauri=$(sed -n 's/^  "version": "\(.*\)",$/\1/p' "$root/desktop/tauri.conf.json")

echo "firmware/CMakeLists.txt  $firmware"
echo "desktop/Cargo.toml       $cargo"
echo "desktop/tauri.conf.json  $tauri"

if [ -z "$firmware" ] || [ "$firmware" != "$cargo" ] || [ "$firmware" != "$tauri" ]; then
    echo "версии расходятся" >&2
    exit 1
fi

if [ $# -gt 0 ] && [ "$1" != "v$firmware" ]; then
    echo "тег $1 не совпадает с версией v$firmware" >&2
    exit 1
fi
