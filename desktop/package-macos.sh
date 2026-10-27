#!/usr/bin/env bash
# Универсальная (Apple Silicon + Intel) сборка LEDCAST Console в desktop/dist/.
#
#   desktop/package-macos.sh
#
# Подпись ad-hoc: без неё macOS считает скачанное приложение повреждённым.
# Подпись Developer ID и нотаризация подключаются переменными окружения
# APPLE_SIGNING_IDENTITY, APPLE_ID, APPLE_TEAM_ID и APPLE_APP_PASSWORD.
set -euo pipefail

cd "$(dirname "$0")"

version=$(sed -n 's/^version = "\(.*\)"$/\1/p' Cargo.toml | head -1)
targets=(aarch64-apple-darwin x86_64-apple-darwin)

rustup target add "${targets[@]}" >/dev/null
for target in "${targets[@]}"; do
    cargo build --release --locked --target "$target"
done

dist=dist
app="$dist/LEDCAST Console.app"
rm -rf "$dist"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
lipo -create -output "$app/Contents/MacOS/ledcast-console" \
    "target/aarch64-apple-darwin/release/ledcast-console" \
    "target/x86_64-apple-darwin/release/ledcast-console"
cp package/macos/Info.plist "$app/Contents/Info.plist"
plutil -replace CFBundleShortVersionString -string "$version" "$app/Contents/Info.plist"
plutil -replace CFBundleVersion -string "$version" "$app/Contents/Info.plist"

if [ -n "${APPLE_SIGNING_IDENTITY:-}" ]; then
    codesign --force --deep --options runtime --timestamp --sign "$APPLE_SIGNING_IDENTITY" "$app"
else
    codesign --force --deep --sign - "$app"
fi

archive="$dist/LEDCAST-Console-$version-macOS-universal.zip"
ditto -c -k --sequesterRsrc --keepParent "$app" "$archive"

if [ -n "${APPLE_SIGNING_IDENTITY:-}" ] && [ -n "${APPLE_ID:-}" ]; then
    xcrun notarytool submit "$archive" --wait \
        --apple-id "$APPLE_ID" --team-id "$APPLE_TEAM_ID" --password "$APPLE_APP_PASSWORD"
    xcrun stapler staple "$app"
    rm "$archive"
    ditto -c -k --sequesterRsrc --keepParent "$app" "$archive"
fi

(cd "$dist" && shasum -a 256 "$(basename "$archive")" > SHA256SUMS)
echo "$archive"
