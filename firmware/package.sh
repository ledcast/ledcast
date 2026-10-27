#!/usr/bin/env bash
# Собирает прошивку и раскладывает готовые к установке файлы в firmware/dist/.
#
#   . $IDF_PATH/export.sh
#   firmware/package.sh
#
# Результат (каталог в git не попадает):
#   ledcast-firmware-<версия>-first-install.bin  полный образ с адреса 0x0 — установка с нуля
#   ledcast-firmware-<версия>-ota.bin            только приложение — обновление из веб-интерфейса
#   manifest.json                                описание для ESP Web Tools (установка из браузера)
#   SHA256SUMS
#
# Полный образ стирает настройки: merge-bin заполняет промежуток до otadata,
# где лежит NVS (0x9000–0xE000), пустыми байтами, и плата поднимается на
# заводских настройках со своей точкой доступа. Поэтому он только для первой
# установки, а обновление — всегда OTA-образом: тот пишется в свободную
# OTA-партицию и NVS не касается.
set -euo pipefail

cd "$(dirname "$0")"

version=$(sed -n 's/^set(PROJECT_VER "\(.*\)")$/\1/p' CMakeLists.txt)
[ -n "$version" ] || { echo "PROJECT_VER не найден в CMakeLists.txt" >&2; exit 1; }

idf.py build

dist=dist
rm -rf "$dist"
mkdir -p "$dist"

install="ledcast-firmware-$version-first-install.bin"
ota="ledcast-firmware-$version-ota.bin"

idf.py merge-bin -o "$PWD/$dist/$install"
cp build/ledcast-node.bin "$dist/$ota"

cat > "$dist/manifest.json" <<EOF
{
  "name": "LEDCAST",
  "version": "$version",
  "new_install_prompt_erase": true,
  "builds": [
    {
      "chipFamily": "ESP32-S3",
      "parts": [{ "path": "$install", "offset": 0 }]
    }
  ]
}
EOF

if command -v sha256sum >/dev/null; then sha256=(sha256sum); else sha256=(shasum -a 256); fi
(cd "$dist" && "${sha256[@]}" "$install" "$ota" > SHA256SUMS)
ls -l "$dist"
