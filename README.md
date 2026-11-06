# Woodman docker tools

Two small Windows helpers of the [Woodman docker](https://mrwoodman.by/) for CorelDRAW, published as open source (MIT):

| Folder | What it is |
|---|---|
| `wood_server/` | `wood_server.exe`: local viewer/render helper, fallback HTTPS client of the docker (JSON API of the site) and the docker updater (verifies, unpacks and installs an archive that the docker has already downloaded). |
| `wood_install/` | `wood_install.exe`: installer of the docker (GUI and `--silent` mode used by the auto-updater), with backups and restore. |
| `third_party/miniz/` | [miniz](https://github.com/richgel999/miniz) (public domain / unlicense), used to unpack update archives. |

Both programs are plain Win32 C++ (no external dependencies except miniz). The docker itself (JS panel, VBA, DLLs, the 3D renderer code) is **not** part of this repository.

## Что здесь

- `wood_server` — вспомогательный exe докера: окно просмотра 3D-сборки, пакетный рендер, связь докера с сайтом (`api`) и автообновление (`update`). Команды и коды выхода — в [wood_server/README.md](wood_server/README.md).
- `wood_install` — установщик докера (окно и тихий режим `--silent`).

## Что программы делают в сети

`wood_server.exe` обращается только к `https://mrwoodman.by/` (`panel_api.php` как запасной путь докера и `share.php` для «Поделиться») и к `127.0.0.1` (свой сервер просмотра). Режим `update` сети не использует: архив скачивает сам CorelDRAW. Архив обновления перед установкой проверяется открытым ключом разработчика (`wood_server/pubkey_release.h`, RSA-SHA256): подпись неверна — установки нет. Закрытый ключ в репозиторий не входит.

## Сборка

Нужны Visual Studio (MSVC, Windows SDK) и, для подписи архивов, Node.js.

```bat
wood_server\build.bat           :: собирает build\wood_server.exe
msbuild wood_install\wood_install.vcxproj /p:Configuration=Release /p:Platform=Win32
```

Собственные ключи подписи архивов: `node wood_server\tools\genkey.js` (закрытый ключ ложится в `keys\`, его нет в git; открытый записывается в `wood_server\pubkey_release.h`). Подпись архива: `node wood_server\tools\sign_release.js archive.zip`.

Если вы собираете форк, замените адреса `https://mrwoodman.by/` в `wood_server\main.cpp` и `wood_server\update_mode.inc` и подпишите свои exe своим сертификатом.

## Лицензия

MIT, см. [LICENSE](LICENSE). miniz — public domain (unlicense), см. заголовок `third_party/miniz/miniz.c`.
