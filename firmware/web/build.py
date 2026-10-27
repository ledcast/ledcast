#!/usr/bin/env python3
"""Сборка веб-интерфейса LEDCAST в один сжатый файл.

Интерфейс (React + prime-ui-kit) собирает Vite в одну HTML-страницу со встроенными
стилями и скриптом: внешних запросов страница не делает, на площадке сети нет.
Этот скрипт запускает сборку и сжимает результат для прошивки.

    npm ci                        # один раз, в корне репозитория
    python3 firmware/web/build.py

Результат:
    firmware/web/dist/index.html        страница, для проверки в браузере
    firmware/web/dist/index.html.gz     то, что отдаёт прошивка через EMBED_FILES
"""

import argparse
import gzip
import pathlib
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent
DIST = ROOT / "dist"

TARGET_NAME = "index.html.gz"
# React и компоненты кита — около 270 КБ в gzip; раздел приложения — 4 МБ.
BUDGET_BYTES = 400 * 1024


def build(verbose: bool = True) -> pathlib.Path:
    npm = shutil.which("npm")
    if not npm:
        sys.exit("Не найден npm: веб-интерфейс собирается Node.js (https://nodejs.org)")
    if not (ROOT.parent.parent / "node_modules").is_dir():
        sys.exit("Нет зависимостей веб-интерфейса: выполните `npm ci` в корне репозитория")

    result = subprocess.run(
        [npm, "run", "build"],
        cwd=ROOT,
        stdout=None if verbose else subprocess.DEVNULL,
        stderr=None,
    )
    if result.returncode != 0:
        sys.exit("Сборка веб-интерфейса не прошла")

    raw = (DIST / "index.html").read_bytes()
    # mtime=0 — чтобы одинаковый исходник давал одинаковый файл и лишний раз
    # не перезаливался в устройство.
    packed = gzip.compress(raw, compresslevel=9, mtime=0)
    archive = DIST / TARGET_NAME
    archive.write_bytes(packed)

    if verbose:
        print(f"страница  : {len(raw):>7} байт")
        print(f"сжато     : {len(packed):>7} байт  ({len(packed) * 100 // BUDGET_BYTES}% бюджета)")
        print(f"файл      : {archive}")

    if len(packed) > BUDGET_BYTES:
        sys.exit(f"интерфейс не влезает в бюджет: {len(packed)} > {BUDGET_BYTES} байт")

    return archive


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Собрать веб-интерфейс LEDCAST")
    parser.add_argument("--quiet", action="store_true", help="без вывода размеров")
    args = parser.parse_args()
    build(verbose=not args.quiet)
