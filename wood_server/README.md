# wood_server.exe

Вспомогательный exe докера Woodman для CorelDRAW. Win32 + Winsock2 + WinHTTP, без сторонних библиотек (кроме miniz для распаковки архивов обновления). Лежит в `wpanel\bin\wood_server.exe` докера. Лицензия — MIT (см. `../LICENSE`).

Код 3D-рендера (папка `wpanel\render` докера) в этот репозиторий не входит: команды `serve`, `batch`, `view` ищут его через `--code`, переменную `WOOD_RENDER_CODE` или папку `<exe>\..\render`.

## Команды

```
wood_server.exe serve  [--code D] [--work D] [--port N|0] [--token T] [--portfile f] [--verbose]
wood_server.exe batch  [--code D] [--work D]
wood_server.exe view   [--viewer-port N]
wood_server.exe update ...
wood_server.exe api    --in <req.json> --out <resp.json> [--timeout 25]
```

**serve** — сервер на 127.0.0.1 для окна просмотра. Защита: проверка Host/Origin, токен (`?k=` -> cookie), `SO_EXCLUSIVEADDRUSE`, безопасные пути. Выходит, когда закрылся `CorelDRW.exe`, через 20 с без пингов страницы или 3 мин без открытия страницы, а также по `GET /quit`. Умеет хранить сохранённые сборки (`/saves`) и ретранслировать «Поделиться» на `https://mrwoodman.by/share.php` (почта и серийник берутся из `work\share_auth.txt`, в страницу не попадают).

**batch** — пакетный рендер: сервер на случайном порту с токеном, headless Chrome/Edge открывает `batch.html`, страница читает `config.js` и `input\*.svg`, шлёт PNG/STL/GLB на `/file`; `/done` завершает. Браузер: `browser.txt` рядом с кодом, `CHROME_PATH`, Chrome, Edge, Chromium. Коды выхода: 0 — ок, 1 — ошибка страницы, 20 — нет браузера, 21 — браузер не запустился/упал, 22 — завис, 50 — нет `viewer.html`.

**view** — окно просмотра по последнему рендеру (свой сервер просмотра отдельным процессом, порт 47653, токен в `work\viewer.session`).

**update** — автообновление докера:

```
wood_server.exe update --url <zip> [--sig-url <sig> | --sig-b64 <подпись>] --addons "<...\Addons>"
                       [--corel-pid N] [--corel-exe путь] [--corel-args s] [--work D] [--lang ru|en] [--no-ui] [--result файл]
wood_server.exe update --download-only --url <zip> (--sig-b64 <подпись> | --sig-url <sig>) --stage-dir "<папка>" --stage-id "<метка версии>"
wood_server.exe update --from-stage --stage-dir "<папка>" --addons ...
```

Порядок: копия себя во временную папку, скачивание архива и `.sig` (WinHTTP, архив только с `https://mrwoodman.by/`), проверка подписи открытым ключом из `pubkey_release.h`, распаковка (miniz, защита от `../`), остановка сервера просмотра, закрытие Corel (`WM_CLOSE`, Corel сам спросит про сохранение) и ожидание освобождения DLL, тихий `wood_install.exe --silent` (один запрос UAC), повторный запуск Corel. Всё, что может не получиться, происходит до закрытия Corel. Коды выхода: 0 — ок, 60 — скачать не удалось, 61 — подпись неверна, 62 — негодный архив, 63 — Corel не закрылся/отмена, 64 — установщик не отработал, 65 — прочее. `--download-only` только скачивает и проверяет архив (фоновая загрузка), `--from-stage` ставит из этой папки, подпись проверяется заново.

Подпись архива: `.sig` = base64(RSA-2048, SHA-256, PKCS#1 v1.5 от байтов zip). Создание: `node tools/sign_release.js archive.zip`, ключи: `node tools/genkey.js`.

**api** — единственный выход докера к сайту: POST тела из файла на `https://mrwoodman.by/panel_api.php`. Тело должно начинаться с `{"a":"<действие>"`, действие — из закрытого списка (`update_check`, `key`, `update_link`, `update_get`, `stat`, ...). Ответ: `{"net":"ok|timeout|dns|connect|tls|err","http":N,"body":{...}}`. Код выхода: 0 — сайт ответил, 1 — сбой связи, 2 — запрос отвергнут.

Общие флаги: `--nowindow` (не открывать окно), `--verbose`. Неизвестные аргументы пропускаются.

## Сборка

`build.bat` из «x64 Native Tools Command Prompt» (или после `vcvars64.bat`; для x86 — `vcvars32.bat`). Результат — `..\build\wood_server.exe`. `wood_server.rc` и манифест (`asInvoker`) обязательны: сведения о версии и авторе нужны, чтобы антивирусы не видели безымянный файл.

## Правка под свой сервер

Адреса `https://mrwoodman.by/...` заданы в `main.cpp` (`share.php`, переменная `WOOD_SHARE_URL` заменяет адрес) и в `update_mode.inc` (`panel_api.php`, архивы). Замените их и сгенерируйте свою пару ключей (`tools/genkey.js`).
