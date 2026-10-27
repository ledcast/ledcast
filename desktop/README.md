# LEDCAST Console

Приложение для macOS и Windows: находит LEDCAST-строки в локальной сети,
назначает каждой строке NDI-источник и отправляет ей кадры по протоколу
LEDCAST Frame v6 (см. [docs/architecture.md](../docs/architecture.md)).

## Возможности

- автоматический поиск строк через mDNS (`_ledcast._tcp`);
- подключение строки паролем её веб-интерфейса один раз, дальше — своим ключом;
- карточка строки: превью, состояние, адрес, размер, RSSI, FPS и поток в Мбит/с;
- веб-интерфейс строки в отдельном окне Console;
- маппинг NDI → строка; один источник можно назначить нескольким строкам;
- вписывание `cover` / `contain` / `stretch`, поворот 0/90/180/270°, отражение
  по горизонтали;
- яркость и включение автономного показа текста прямо из карточки;
- суммарный сетевой поток в шапке, общий «Старт» / «Стоп»;
- общая задержка показа: все строки меняют кадр одновременно по часам Console.

Интерфейс — React + prime-ui-kit в `web/`, как и у веб-интерфейса контроллера.
`cargo build` сам собирает его в `web/dist` (нужен Node.js и один раз `npm ci` в
корне репозитория). В браузере без Tauri: `npm run dev -w desktop/web` — с моком
бэкенда и тремя строками-примерами.

## NDI

NDI Runtime загружается динамически. Подойдёт [NDI Tools](https://ndi.video/tools/)
или библиотека, установленная вместе с Resolume Arena/Avenue. Без неё
приложение запускается, но NDI-источники недоступны. Windows-архив уже содержит
`Processing.NDI.Lib.x64.dll`.

## Сборка

Нужен Rust 1.82 или новее.

```sh
cargo run --release
```

### Пакеты для релиза

Результат попадает в `desktop/dist/` (в git не хранится), рядом — `SHA256SUMS`.
Те же скрипты запускает workflow релиза.

macOS — универсальный `.app` (Apple Silicon + Intel) в zip. По умолчанию подпись
ad-hoc; подпись Developer ID и нотаризация включаются переменными
`APPLE_SIGNING_IDENTITY`, `APPLE_ID`, `APPLE_TEAM_ID`, `APPLE_APP_PASSWORD`:

```sh
./package-macos.sh        # → dist/LEDCAST-Console-<версия>-macOS-universal.zip
```

Windows (PowerShell) — exe со статическим CRT, NDI runtime и README в zip.
Путь к DLL берётся из `NDI_RUNTIME_DLL`, установленного NDI SDK или Resolume:

```powershell
.\package-windows.ps1     # → dist\LEDCAST-Console-<версия>-Windows-x64.zip
```

## Проверка

```sh
cargo fmt --check
cargo clippy --all-targets -- -D warnings
cargo test --all-targets
npm test        # из корня репозитория: логика интерфейса
```

## Сеть

- HTTP TCP 80 до контроллеров — состояние и настройки;
- UDP 4048 до контроллеров — видео, обратно — обратная связь приёмника;
- UDP 5353 — mDNS;
- NDI Discovery и NDI-видео в локальной сети.
